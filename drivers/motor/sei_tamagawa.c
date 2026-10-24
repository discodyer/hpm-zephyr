/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>

#include <drivers/motor/hpmicro_sei.h>
#include <drivers/motor/hpmicro_sei_tamagawa.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#define HPM_SEI_TAMAGAWA_CHANNEL_COUNT (HPM_SEI_CHANNEL_1 + 1U)
#define HPM_SEI_TAMAGAWA_DATA_MASK                                                                 \
	(BIT(HPM_SEI_DATA_REGISTER_2) | BIT(HPM_SEI_DATA_REGISTER_3) |                             \
	 BIT(HPM_SEI_DATA_REGISTER_4) | BIT(HPM_SEI_DATA_REGISTER_5) |                             \
	 BIT(HPM_SEI_DATA_REGISTER_6) | BIT(HPM_SEI_DATA_REGISTER_7) |                             \
	 BIT(HPM_SEI_DATA_REGISTER_8) | BIT(HPM_SEI_DATA_REGISTER_9))

enum hpm_sei_tamagawa_instruction {
	HPM_SEI_TAMAGAWA_INSTRUCTION_SEND_COMMAND = 0,
	HPM_SEI_TAMAGAWA_INSTRUCTION_RECEIVE_CF,
	HPM_SEI_TAMAGAWA_INSTRUCTION_RECEIVE_SF,
	HPM_SEI_TAMAGAWA_INSTRUCTION_RECEIVE_POSITION,
	HPM_SEI_TAMAGAWA_INSTRUCTION_RECEIVE_ENCODER_ID,
	HPM_SEI_TAMAGAWA_INSTRUCTION_RECEIVE_REVOLUTION,
	HPM_SEI_TAMAGAWA_INSTRUCTION_RECEIVE_ALARM,
	HPM_SEI_TAMAGAWA_INSTRUCTION_RECEIVE_CRC,
	HPM_SEI_TAMAGAWA_INSTRUCTION_HALT,
	HPM_SEI_TAMAGAWA_INSTRUCTION_COUNT,
};

BUILD_ASSERT(HPM_SEI_TAMAGAWA_INSTRUCTION_COUNT <= HPM_SEI_MAX_INSTRUCTIONS);

struct hpm_sei_tamagawa_channel_state {
	const struct device *dev;
	hpm_sei_event_callback_t user_callback;
	void *user_data;
	uint8_t expected_encoder_id;
	bool configured;
	atomic_t update_seen;
	atomic_t error_seen;
};

static struct hpm_sei_tamagawa_channel_state state[HPM_SEI_TAMAGAWA_CHANNEL_COUNT];
static K_MUTEX_DEFINE(tamagawa_lock);

static const struct hpm_sei_latch_transition tamagawa_latch_disabled = {
	.disable_instr_ptr_check = true,
	.disable_clock_check = true,
	.disable_tx_check = true,
	.disable_rx_check = true,
	.disable_timeout_check = true,
};

static bool hpm_sei_tamagawa_channel_is_valid(enum hpm_sei_channel channel)
{
	return channel <= HPM_SEI_CHANNEL_1;
}

static bool hpm_sei_tamagawa_device_is_valid(const struct device *dev)
{
	return (dev != NULL) && device_is_ready(dev) && DEVICE_API_IS(hpm_sei, dev);
}

static void hpm_sei_tamagawa_event_callback(const struct device *dev, enum hpm_sei_channel channel,
					    uint32_t events, void *user_data)
{
	struct hpm_sei_tamagawa_channel_state *channel_state = user_data;

	if ((!hpm_sei_tamagawa_channel_is_valid(channel)) || (channel_state == NULL) ||
	    (dev == NULL)) {
		return;
	}

	if ((events & HPM_SEI_EVENT_LATCH1) != 0U) {
		atomic_set(&channel_state->update_seen, 1);
	}
	if ((events & (HPM_SEI_EVENT_TRANSFER_ERROR | HPM_SEI_EVENT_EXCEPTION |
		       HPM_SEI_EVENT_WATCHDOG)) != 0U) {
		atomic_set(&channel_state->error_seen, 1);
	}
	if ((events & HPM_SEI_EVENT_WATCHDOG) != 0U) {
		(void)hpm_sei_enable(dev, channel, false);
		(void)hpm_sei_rewind(dev, channel, HPM_SEI_TAMAGAWA_DATA_MASK);
		(void)hpm_sei_restart_async(dev, channel);
		(void)hpm_sei_enable(dev, channel, true);
	}
	if (channel_state->user_callback != NULL) {
		channel_state->user_callback(dev, channel, events, channel_state->user_data);
	}
}

static int tamagawa_configure_data(const struct device *dev,
				   enum hpm_sei_data_register register_index, uint8_t bits)
{
	const struct hpm_sei_data_format format = {
		.mode = HPM_SEI_DATA_MODE_DATA,
		.bit_order = HPM_SEI_BIT_ORDER_LSB_FIRST,
		.word_order = HPM_SEI_WORD_ORDER_NORMAL,
		.word_length = bits,
		.first_bit = 0U,
		.last_bit = bits - 1U,
		.min_bit = 0U,
		.max_bit = bits - 1U,
	};

	return hpm_sei_configure_data(dev, register_index, &format);
}

static int tamagawa_configure_crc_data(const struct device *dev)
{
	const struct hpm_sei_data_format format = {
		.mode = HPM_SEI_DATA_MODE_CRC,
		.bit_order = HPM_SEI_BIT_ORDER_LSB_FIRST,
		.word_order = HPM_SEI_WORD_ORDER_NORMAL,
		.word_length = 8U,
		.crc_length = 8U,
		.first_bit = 0U,
		.last_bit = 7U,
		.min_bit = 0U,
		.max_bit = 7U,
		.crc_polynomial = 1U,
	};

	return hpm_sei_configure_data(dev, HPM_SEI_DATA_REGISTER_9, &format);
}

static int tamagawa_set_instruction(const struct device *dev, uint8_t index,
				    enum hpm_sei_instruction_operation operation,
				    enum hpm_sei_data_register crc_register,
				    enum hpm_sei_data_register data_register, uint8_t operand)
{
	const struct hpm_sei_instruction instruction = {
		.operation = operation,
		.crc_register = crc_register,
		.data_register = data_register,
		.operand = operand,
	};

	return hpm_sei_set_instruction(dev, index, &instruction);
}

static int tamagawa_configure_instructions(const struct device *dev)
{
	int ret;

	ret = tamagawa_set_instruction(dev, HPM_SEI_TAMAGAWA_INSTRUCTION_SEND_COMMAND,
				       HPM_SEI_INSTRUCTION_SEND, HPM_SEI_DATA_REGISTER_0,
				       HPM_SEI_DATA_REGISTER_2, 8U);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_set_instruction(dev, HPM_SEI_TAMAGAWA_INSTRUCTION_RECEIVE_CF,
				       HPM_SEI_INSTRUCTION_RECEIVE_WATCHDOG,
				       HPM_SEI_DATA_REGISTER_9, HPM_SEI_DATA_REGISTER_3, 8U);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_set_instruction(dev, HPM_SEI_TAMAGAWA_INSTRUCTION_RECEIVE_SF,
				       HPM_SEI_INSTRUCTION_RECEIVE_WATCHDOG,
				       HPM_SEI_DATA_REGISTER_9, HPM_SEI_DATA_REGISTER_4, 8U);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_set_instruction(dev, HPM_SEI_TAMAGAWA_INSTRUCTION_RECEIVE_POSITION,
				       HPM_SEI_INSTRUCTION_RECEIVE_WATCHDOG,
				       HPM_SEI_DATA_REGISTER_9, HPM_SEI_DATA_REGISTER_5, 24U);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_set_instruction(dev, HPM_SEI_TAMAGAWA_INSTRUCTION_RECEIVE_ENCODER_ID,
				       HPM_SEI_INSTRUCTION_RECEIVE_WATCHDOG,
				       HPM_SEI_DATA_REGISTER_9, HPM_SEI_DATA_REGISTER_6, 8U);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_set_instruction(dev, HPM_SEI_TAMAGAWA_INSTRUCTION_RECEIVE_REVOLUTION,
				       HPM_SEI_INSTRUCTION_RECEIVE_WATCHDOG,
				       HPM_SEI_DATA_REGISTER_9, HPM_SEI_DATA_REGISTER_7, 24U);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_set_instruction(dev, HPM_SEI_TAMAGAWA_INSTRUCTION_RECEIVE_ALARM,
				       HPM_SEI_INSTRUCTION_RECEIVE_WATCHDOG,
				       HPM_SEI_DATA_REGISTER_9, HPM_SEI_DATA_REGISTER_8, 8U);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_set_instruction(dev, HPM_SEI_TAMAGAWA_INSTRUCTION_RECEIVE_CRC,
				       HPM_SEI_INSTRUCTION_RECEIVE_WATCHDOG,
				       HPM_SEI_DATA_REGISTER_0, HPM_SEI_DATA_REGISTER_9, 8U);
	if (ret < 0) {
		return ret;
	}

	return tamagawa_set_instruction(dev, HPM_SEI_TAMAGAWA_INSTRUCTION_HALT,
					HPM_SEI_INSTRUCTION_HALT, HPM_SEI_DATA_REGISTER_0,
					HPM_SEI_DATA_REGISTER_0, 0U);
}

static int tamagawa_configure_latches(const struct device *dev, enum hpm_sei_channel channel,
				      uint16_t sample_delay_cycles)
{
	const struct hpm_sei_latch_transition latch0_start = {
		.disable_instr_ptr_check = false,
		.instr_ptr_cfg = HPM_SEI_TRANSITION_FALL_LEAVE,
		.instr_ptr_value = HPM_SEI_TAMAGAWA_INSTRUCTION_SEND_COMMAND,
		.disable_clock_check = true,
		.disable_tx_check = true,
		.disable_rx_check = true,
		.disable_timeout_check = true,
	};
	const struct hpm_sei_latch_transition latch1_start = {
		.disable_instr_ptr_check = false,
		.instr_ptr_cfg = HPM_SEI_TRANSITION_FALL_LEAVE,
		.instr_ptr_value = HPM_SEI_TAMAGAWA_INSTRUCTION_RECEIVE_CRC,
		.disable_clock_check = true,
		.disable_tx_check = true,
		.disable_rx_check = true,
		.disable_timeout_check = true,
	};
	const struct hpm_sei_latch_output_config latch0_output = {
		.enable = true,
		.output_select = 0U,
		.delay = sample_delay_cycles,
	};
	const struct hpm_sei_latch_output_config latch1_output = {
		.enable = true,
		.output_select = 0U,
	};
	int ret;

	ret = hpm_sei_configure_latch(dev, channel, 0U, 0U, &latch0_start);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch(dev, channel, 0U, 1U, &tamagawa_latch_disabled);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch(dev, channel, 0U, 2U, &tamagawa_latch_disabled);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch(dev, channel, 0U, 3U, &tamagawa_latch_disabled);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch_output(dev, channel, 0U, &latch0_output);
	if (ret < 0) {
		return ret;
	}

	ret = hpm_sei_configure_latch(dev, channel, 1U, 0U, &latch1_start);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch(dev, channel, 1U, 1U, &tamagawa_latch_disabled);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch(dev, channel, 1U, 2U, &tamagawa_latch_disabled);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch(dev, channel, 1U, 3U, &tamagawa_latch_disabled);
	if (ret < 0) {
		return ret;
	}

	return hpm_sei_configure_latch_output(dev, channel, 1U, &latch1_output);
}

static int tamagawa_configure_channel(const struct device *dev, enum hpm_sei_channel channel,
				      uint32_t period_cycles,
				      const struct hpm_sei_tamagawa_config *config,
				      struct hpm_sei_tamagawa_channel_state *channel_state)
{
	const struct hpm_sei_transceiver_config transceiver = {
		.mode = HPM_SEI_MODE_ASYNC,
		.baudrate = config->baudrate,
		.data = {.idle_state = HPM_SEI_IDLE_HIGH},
		.async_data_bits = 8U,
	};
	const struct hpm_sei_sample_config sample = {
		.latch = 0U,
	};
	const struct hpm_sei_update_config update = {
		.latch = 1U,
		.update_on_error = false,
		.revolution_data_register = HPM_SEI_DATA_REGISTER_7,
		.position_data_register = HPM_SEI_DATA_REGISTER_5,
		.use_revolution_rx = true,
		.use_position_rx = true,
		.data_register_select = BIT(5) | BIT(7),
	};
	const struct hpm_sei_engine_config engine = {
		.arm_on_trigger = true,
		.initial_instruction = HPM_SEI_TAMAGAWA_INSTRUCTION_SEND_COMMAND,
		.watchdog_enable = config->watchdog_time != 0U,
		.watchdog_exception = true,
		.watchdog_instruction = HPM_SEI_TAMAGAWA_INSTRUCTION_HALT,
		.watchdog_time = config->watchdog_time,
	};
	const struct hpm_sei_trigger_input_config trigger = {
		.periodic_enable = true,
		.periodic_cycles = period_cycles,
	};
	int ret;

	ret = hpm_sei_set_event_callback(dev, channel, 0U, NULL, NULL);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_transceiver(dev, channel, &transceiver);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_configure_data(dev, HPM_SEI_DATA_REGISTER_2, 8U);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_set_data(dev, HPM_SEI_DATA_REGISTER_2, HPM_SEI_TAMAGAWA_DATA_ID_3);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_configure_data(dev, HPM_SEI_DATA_REGISTER_3, 8U);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_configure_data(dev, HPM_SEI_DATA_REGISTER_4, 8U);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_configure_data(dev, HPM_SEI_DATA_REGISTER_5, HPM_SEI_TAMAGAWA_ST_BITS);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_configure_data(dev, HPM_SEI_DATA_REGISTER_6, 8U);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_configure_data(dev, HPM_SEI_DATA_REGISTER_7, HPM_SEI_TAMAGAWA_MT_BITS);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_configure_data(dev, HPM_SEI_DATA_REGISTER_8, 8U);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_configure_crc_data(dev);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_configure_instructions(dev);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_sample(dev, channel, &sample);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_update(dev, channel, &update);
	if (ret < 0) {
		return ret;
	}
	ret = tamagawa_configure_latches(dev, channel, config->sample_delay_cycles);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_engine(dev, channel, &engine);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_set_timestamp_source(dev, HPM_SEI_TIMESTAMP_SOURCE_SYNT);
	if (ret < 0) {
		return ret;
	}

	atomic_clear(&channel_state->update_seen);
	atomic_clear(&channel_state->error_seen);
	channel_state->user_callback = config->callback;
	channel_state->user_data = config->user_data;
	channel_state->expected_encoder_id = config->encoder_id;
	ret = hpm_sei_set_event_callback(dev, channel,
					 HPM_SEI_EVENT_LATCH1 | HPM_SEI_EVENT_TRANSFER_ERROR |
						 HPM_SEI_EVENT_EXCEPTION | HPM_SEI_EVENT_WATCHDOG,
					 hpm_sei_tamagawa_event_callback, channel_state);
	if (ret < 0) {
		goto fail;
	}
	ret = hpm_sei_configure_trigger_input(dev, channel, &trigger);
	if (ret < 0) {
		goto fail;
	}
	ret = hpm_sei_enable(dev, channel, true);
	if (ret < 0) {
		goto fail;
	}

	channel_state->configured = true;
	return 0;

fail:
	(void)hpm_sei_enable(dev, channel, false);
	(void)hpm_sei_set_event_callback(dev, channel, 0U, NULL, NULL);
	return ret;
}

int hpm_sei_tamagawa_configure(const struct device *dev, enum hpm_sei_channel channel,
			       const struct hpm_sei_tamagawa_config *config)
{
	struct hpm_sei_tamagawa_channel_state *channel_state;
	uint32_t clock_hz;
	uint32_t period_cycles;
	uint32_t divider;
	bool claimed = false;
	int ret;

	if (!hpm_sei_tamagawa_device_is_valid(dev)) {
		return -ENODEV;
	}
	if ((config == NULL) || !hpm_sei_tamagawa_channel_is_valid(channel) ||
	    (config->baudrate == 0U) || (config->poll_period_us == 0U)) {
		return -EINVAL;
	}
	ret = hpm_sei_tamagawa_check_timing(config->baudrate, config->poll_period_us,
					    config->watchdog_time);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_get_clock_frequency(dev, &clock_hz);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_baud_to_divider(clock_hz, config->baudrate, &divider);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_period_us_to_cycles(clock_hz, config->poll_period_us, &period_cycles);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&tamagawa_lock, K_FOREVER);
	channel_state = &state[channel];
	for (uint8_t i = 0U; i <= HPM_SEI_CHANNEL_1; i++) {
		if ((i != channel) && state[i].configured && (state[i].dev == dev)) {
			ret = -EBUSY;
			goto out;
		}
	}
	if (channel_state->configured && (channel_state->dev != dev)) {
		ret = -EBUSY;
		goto out;
	}
	ret = hpm_sei_claim(dev, channel_state);
	if (ret < 0) {
		goto out;
	}
	claimed = true;
	ret = hpm_sei_enable(dev, channel, false);
	if (ret < 0) {
		goto fail_release;
	}
	channel_state->configured = false;
	(void)hpm_sei_clear_events(dev, channel, HPM_SEI_EVENT_ALL);
	ret = hpm_sei_rewind(dev, channel, HPM_SEI_TAMAGAWA_DATA_MASK);
	if (ret < 0) {
		goto fail_release;
	}
	ret = tamagawa_configure_channel(dev, channel, period_cycles, config, channel_state);
	if (ret == 0) {
		channel_state->dev = dev;
	}

fail_release:
	if ((ret < 0) && claimed) {
		(void)hpm_sei_release(dev, channel_state);
	}

out:
	k_mutex_unlock(&tamagawa_lock);
	return ret;
}

int hpm_sei_tamagawa_deconfigure(const struct device *dev, enum hpm_sei_channel channel)
{
	const struct hpm_sei_trigger_input_config trigger_disabled = {0};
	struct hpm_sei_tamagawa_channel_state *channel_state;
	int ret;

	if (!hpm_sei_tamagawa_device_is_valid(dev) || !hpm_sei_tamagawa_channel_is_valid(channel)) {
		return -EINVAL;
	}

	k_mutex_lock(&tamagawa_lock, K_FOREVER);
	channel_state = &state[channel];
	if (!channel_state->configured || (channel_state->dev != dev)) {
		ret = -EINVAL;
		goto out;
	}
	ret = hpm_sei_enable(dev, channel, false);
	if (ret < 0) {
		goto out;
	}
	(void)hpm_sei_configure_trigger_input(dev, channel, &trigger_disabled);
	(void)hpm_sei_set_event_callback(dev, channel, 0U, NULL, NULL);
	(void)hpm_sei_release(dev, channel_state);
	channel_state->configured = false;
	channel_state->dev = NULL;

out:
	k_mutex_unlock(&tamagawa_lock);
	return ret;
}

int hpm_sei_tamagawa_get_result(const struct device *dev, enum hpm_sei_channel channel,
				struct hpm_sei_tamagawa_result *result)
{
	struct hpm_sei_tamagawa_channel_state *channel_state;
	struct hpm_sei_status status;
	uint32_t events;
	uint32_t value;
	int ret;

	if (!hpm_sei_tamagawa_device_is_valid(dev)) {
		return -ENODEV;
	}
	if ((result == NULL) || !hpm_sei_tamagawa_channel_is_valid(channel)) {
		return -EINVAL;
	}

	k_mutex_lock(&tamagawa_lock, K_FOREVER);
	channel_state = &state[channel];
	if (!channel_state->configured || (channel_state->dev != dev)) {
		ret = -EINVAL;
		goto out;
	}
	ret = hpm_sei_get_events(dev, channel, &events);
	if (ret < 0) {
		goto out;
	}
	ret = hpm_sei_get_status(dev, channel, &status);
	if (ret < 0) {
		goto out;
	}
	if ((events & HPM_SEI_EVENT_LATCH1) != 0U) {
		atomic_set(&channel_state->update_seen, 1);
	}
	if (((events & (HPM_SEI_EVENT_TRANSFER_ERROR | HPM_SEI_EVENT_EXCEPTION |
			HPM_SEI_EVENT_WATCHDOG)) != 0U) ||
	    status.update_error) {
		atomic_set(&channel_state->error_seen, 1);
	}
	if (atomic_get(&channel_state->error_seen) != 0) {
		atomic_clear(&channel_state->error_seen);
		atomic_clear(&channel_state->update_seen);
		ret = -EIO;
		goto out;
	}
	if (atomic_get(&channel_state->update_seen) == 0) {
		ret = -EAGAIN;
		goto out;
	}
	ret = hpm_sei_get_data(dev, HPM_SEI_DATA_REGISTER_3, &value);
	if (ret < 0) {
		goto out;
	}
	result->control_field = value & UINT8_MAX;
	ret = hpm_sei_get_data(dev, HPM_SEI_DATA_REGISTER_4, &value);
	if (ret < 0) {
		goto out;
	}
	result->status_field = value & UINT8_MAX;
	ret = hpm_sei_get_data(dev, HPM_SEI_DATA_REGISTER_5, &value);
	if (ret < 0) {
		goto out;
	}
	result->position = value & GENMASK(HPM_SEI_TAMAGAWA_ST_BITS - 1U, 0U);
	ret = hpm_sei_get_data(dev, HPM_SEI_DATA_REGISTER_6, &value);
	if (ret < 0) {
		goto out;
	}
	result->encoder_id = value & UINT8_MAX;
	if ((channel_state->expected_encoder_id != 0U) &&
	    (result->encoder_id != channel_state->expected_encoder_id)) {
		atomic_clear(&channel_state->error_seen);
		atomic_clear(&channel_state->update_seen);
		ret = -EIO;
		goto out;
	}
	ret = hpm_sei_get_data(dev, HPM_SEI_DATA_REGISTER_7, &value);
	if (ret < 0) {
		goto out;
	}
	result->revolution = value & GENMASK(HPM_SEI_TAMAGAWA_MT_BITS - 1U, 0U);
	ret = hpm_sei_get_data(dev, HPM_SEI_DATA_REGISTER_8, &value);
	if (ret < 0) {
		goto out;
	}
	result->alarm = value & UINT8_MAX;
	ret = hpm_sei_get_crc_value(dev, HPM_SEI_DATA_REGISTER_9, &value);
	if (ret < 0) {
		goto out;
	}
	result->crc = value & UINT8_MAX;
	ret = hpm_sei_get_latch_time(dev, channel, 0U, &result->sample_time);
	if (ret < 0) {
		goto out;
	}
	ret = hpm_sei_get_latch_time(dev, channel, 1U, &result->update_time);
	if (ret < 0) {
		goto out;
	}
	result->delay_ticks = result->update_time - result->sample_time;
	atomic_clear(&channel_state->update_seen);

out:
	k_mutex_unlock(&tamagawa_lock);
	return ret;
}
