/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>

#include <drivers/motor/hpmicro_sei.h>
#include <drivers/motor/hpmicro_sei_bissc.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#define HPM_SEI_BISSC_CHANNEL_COUNT (HPM_SEI_CHANNEL_1 + 1U)
#define HPM_SEI_BISSC_DATA_MASK                                                                    \
	(BIT(HPM_SEI_DATA_REGISTER_2) | BIT(HPM_SEI_DATA_REGISTER_3) |                             \
	 BIT(HPM_SEI_DATA_REGISTER_4) | BIT(HPM_SEI_DATA_REGISTER_5))

enum hpm_sei_bissc_instruction {
	HPM_SEI_BISSC_INSTRUCTION_WAIT_ACK = 0,
	HPM_SEI_BISSC_INSTRUCTION_WAIT_START,
	HPM_SEI_BISSC_INSTRUCTION_RECEIVE_CDS,
	HPM_SEI_BISSC_INSTRUCTION_RECEIVE_MULTI_TURN,
	HPM_SEI_BISSC_INSTRUCTION_RECEIVE_SINGLE_TURN,
	HPM_SEI_BISSC_INSTRUCTION_RECEIVE_STATUS,
	HPM_SEI_BISSC_INSTRUCTION_RECEIVE_CRC,
	HPM_SEI_BISSC_INSTRUCTION_RECEIVE_TIMEOUT,
	HPM_SEI_BISSC_INSTRUCTION_WAIT,
	HPM_SEI_BISSC_INSTRUCTION_HALT,
	HPM_SEI_BISSC_INSTRUCTION_COUNT,
};

BUILD_ASSERT(HPM_SEI_BISSC_INSTRUCTION_COUNT <= HPM_SEI_MAX_INSTRUCTIONS);

struct hpm_sei_bissc_channel_state {
	const struct device *dev;
	hpm_sei_event_callback_t user_callback;
	void *user_data;
	uint32_t multi_turn_mask;
	uint32_t single_turn_mask;
	uint32_t crc_mask;
	uint8_t single_turn_shift;
	bool configured;
	atomic_t update_seen;
	atomic_t error_seen;
};

static struct hpm_sei_bissc_channel_state state[HPM_SEI_BISSC_CHANNEL_COUNT];
static K_MUTEX_DEFINE(bissc_lock);

static const struct hpm_sei_latch_transition bissc_latch_disabled = {
	.disable_instr_ptr_check = true,
	.disable_clock_check = true,
	.disable_tx_check = true,
	.disable_rx_check = true,
	.disable_timeout_check = true,
};

static bool hpm_sei_bissc_channel_is_valid(enum hpm_sei_channel channel)
{
	return channel <= HPM_SEI_CHANNEL_1;
}

static bool hpm_sei_bissc_device_is_valid(const struct device *dev)
{
	return (dev != NULL) && device_is_ready(dev) && DEVICE_API_IS(hpm_sei, dev);
}

static uint32_t hpm_sei_bissc_mask(uint8_t bits)
{
	return bits == 32U ? UINT32_MAX : (1U << bits) - 1U;
}

static void hpm_sei_bissc_event_callback(const struct device *dev, enum hpm_sei_channel channel,
					 uint32_t events, void *user_data)
{
	struct hpm_sei_bissc_channel_state *channel_state = user_data;

	if ((!hpm_sei_bissc_channel_is_valid(channel)) || (channel_state == NULL) ||
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
		(void)hpm_sei_rewind(dev, channel, HPM_SEI_BISSC_DATA_MASK);
		(void)hpm_sei_enable(dev, channel, true);
	}
	if (channel_state->user_callback != NULL) {
		channel_state->user_callback(dev, channel, events, channel_state->user_data);
	}
}

static int bissc_configure_data(const struct device *dev, enum hpm_sei_data_register register_index,
				uint8_t bits, uint8_t first_bit, uint8_t last_bit, uint8_t min_bit,
				uint8_t max_bit)
{
	const struct hpm_sei_data_format format = {
		.mode = HPM_SEI_DATA_MODE_DATA,
		.bit_order = HPM_SEI_BIT_ORDER_MSB_FIRST,
		.word_order = HPM_SEI_WORD_ORDER_NORMAL,
		.word_length = bits,
		.first_bit = first_bit,
		.last_bit = last_bit,
		.min_bit = min_bit,
		.max_bit = max_bit,
	};

	return hpm_sei_configure_data(dev, register_index, &format);
}

static int bissc_configure_crc_data(const struct device *dev,
				    const struct hpm_sei_bissc_config *config)
{
	const struct hpm_sei_data_format format = {
		.mode = HPM_SEI_DATA_MODE_CRC,
		.bit_order = HPM_SEI_BIT_ORDER_MSB_FIRST,
		.word_order = HPM_SEI_WORD_ORDER_NORMAL,
		.word_length = config->crc_bits,
		.crc_invert = config->crc_invert,
		.crc_length = config->crc_bits,
		.first_bit = config->crc_bits - 1U,
		.last_bit = 0U,
		.min_bit = 0U,
		.max_bit = config->crc_bits - 1U,
		.crc_initial_value = config->crc_initial_value,
		.crc_polynomial = config->crc_polynomial,
	};

	return hpm_sei_configure_data(dev, HPM_SEI_DATA_REGISTER_5, &format);
}

static int bissc_set_instruction(const struct device *dev, uint8_t index,
				 enum hpm_sei_instruction_operation operation,
				 enum hpm_sei_master_clock clock,
				 enum hpm_sei_data_register crc_register,
				 enum hpm_sei_data_register data_register, uint8_t operand)
{
	const struct hpm_sei_instruction instruction = {
		.operation = operation,
		.clock = clock,
		.crc_register = crc_register,
		.data_register = data_register,
		.operand = operand,
	};

	return hpm_sei_set_instruction(dev, index, &instruction);
}

static int bissc_configure_instructions(const struct device *dev,
					const struct hpm_sei_bissc_config *config)
{
	int ret;

	ret = bissc_set_instruction(dev, HPM_SEI_BISSC_INSTRUCTION_WAIT_ACK,
				    HPM_SEI_INSTRUCTION_WAIT, HPM_SEI_MASTER_CLOCK_FALL_RISE,
				    HPM_SEI_DATA_REGISTER_0, HPM_SEI_DATA_REGISTER_CONST_0, 1U);
	if (ret < 0) {
		return ret;
	}
	ret = bissc_set_instruction(dev, HPM_SEI_BISSC_INSTRUCTION_WAIT_START,
				    HPM_SEI_INSTRUCTION_WAIT, HPM_SEI_MASTER_CLOCK_FALL_RISE,
				    HPM_SEI_DATA_REGISTER_0, HPM_SEI_DATA_REGISTER_CONST_1, 1U);
	if (ret < 0) {
		return ret;
	}
	ret = bissc_set_instruction(dev, HPM_SEI_BISSC_INSTRUCTION_RECEIVE_CDS,
				    HPM_SEI_INSTRUCTION_RECEIVE, HPM_SEI_MASTER_CLOCK_FALL_RISE,
				    HPM_SEI_DATA_REGISTER_0, HPM_SEI_DATA_REGISTER_0, 1U);
	if (ret < 0) {
		return ret;
	}
	ret = bissc_set_instruction(dev, HPM_SEI_BISSC_INSTRUCTION_RECEIVE_MULTI_TURN,
				    HPM_SEI_INSTRUCTION_RECEIVE, HPM_SEI_MASTER_CLOCK_FALL_RISE,
				    HPM_SEI_DATA_REGISTER_5, HPM_SEI_DATA_REGISTER_2,
				    config->multi_turn_bits);
	if (ret < 0) {
		return ret;
	}
	ret = bissc_set_instruction(dev, HPM_SEI_BISSC_INSTRUCTION_RECEIVE_SINGLE_TURN,
				    HPM_SEI_INSTRUCTION_RECEIVE, HPM_SEI_MASTER_CLOCK_FALL_RISE,
				    HPM_SEI_DATA_REGISTER_5, HPM_SEI_DATA_REGISTER_3,
				    config->single_turn_bits);
	if (ret < 0) {
		return ret;
	}
	ret = bissc_set_instruction(dev, HPM_SEI_BISSC_INSTRUCTION_RECEIVE_STATUS,
				    HPM_SEI_INSTRUCTION_RECEIVE, HPM_SEI_MASTER_CLOCK_FALL_RISE,
				    HPM_SEI_DATA_REGISTER_5, HPM_SEI_DATA_REGISTER_4,
				    HPM_SEI_BISSC_STATUS_BITS);
	if (ret < 0) {
		return ret;
	}
	ret = bissc_set_instruction(dev, HPM_SEI_BISSC_INSTRUCTION_RECEIVE_CRC,
				    HPM_SEI_INSTRUCTION_RECEIVE, HPM_SEI_MASTER_CLOCK_FALL_RISE,
				    HPM_SEI_DATA_REGISTER_0, HPM_SEI_DATA_REGISTER_5,
				    config->crc_bits);
	if (ret < 0) {
		return ret;
	}
	ret = bissc_set_instruction(dev, HPM_SEI_BISSC_INSTRUCTION_RECEIVE_TIMEOUT,
				    HPM_SEI_INSTRUCTION_RECEIVE, HPM_SEI_MASTER_CLOCK_HIGH,
				    HPM_SEI_DATA_REGISTER_0, HPM_SEI_DATA_REGISTER_CONST_0, 0U);
	if (ret < 0) {
		return ret;
	}
	ret = bissc_set_instruction(dev, HPM_SEI_BISSC_INSTRUCTION_WAIT, HPM_SEI_INSTRUCTION_WAIT,
				    HPM_SEI_MASTER_CLOCK_HIGH, HPM_SEI_DATA_REGISTER_0,
				    HPM_SEI_DATA_REGISTER_CONST_1, 0U);
	if (ret < 0) {
		return ret;
	}

	return bissc_set_instruction(dev, HPM_SEI_BISSC_INSTRUCTION_HALT, HPM_SEI_INSTRUCTION_HALT,
				     HPM_SEI_MASTER_CLOCK_LOW, HPM_SEI_DATA_REGISTER_0,
				     HPM_SEI_DATA_REGISTER_0, 0U);
}

static int bissc_configure_latches(const struct device *dev, enum hpm_sei_channel channel,
				   uint16_t sample_delay_cycles)
{
	const struct hpm_sei_latch_transition latch0_start = {
		.disable_instr_ptr_check = true,
		.disable_clock_check = false,
		.clock_cfg = HPM_SEI_TRANSITION_FALL_LEAVE,
		.disable_tx_check = true,
		.disable_rx_check = true,
		.disable_timeout_check = true,
	};
	const struct hpm_sei_latch_transition latch0_middle = {
		.disable_instr_ptr_check = true,
		.disable_clock_check = false,
		.clock_cfg = HPM_SEI_TRANSITION_RISE_ENTRY,
		.disable_tx_check = true,
		.disable_rx_check = true,
		.disable_timeout_check = true,
	};
	const struct hpm_sei_latch_transition latch0_end = {
		.disable_instr_ptr_check = false,
		.instr_ptr_cfg = HPM_SEI_TRANSITION_FALL_LEAVE,
		.instr_ptr_value = HPM_SEI_BISSC_INSTRUCTION_WAIT,
		.disable_clock_check = true,
		.disable_tx_check = true,
		.disable_rx_check = true,
		.disable_timeout_check = true,
	};
	const struct hpm_sei_latch_transition latch1_start = {
		.disable_instr_ptr_check = false,
		.instr_ptr_cfg = HPM_SEI_TRANSITION_FALL_LEAVE,
		.instr_ptr_value = HPM_SEI_BISSC_INSTRUCTION_RECEIVE_CRC,
		.disable_clock_check = true,
		.disable_tx_check = true,
		.disable_rx_check = true,
		.disable_timeout_check = true,
	};
	const struct hpm_sei_latch_output_config latch0_output = {
		.enable = true,
		.output_select = 1U,
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
	ret = hpm_sei_configure_latch(dev, channel, 0U, 1U, &latch0_middle);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch(dev, channel, 0U, 2U, &bissc_latch_disabled);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch(dev, channel, 0U, 3U, &latch0_end);
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
	ret = hpm_sei_configure_latch(dev, channel, 1U, 1U, &bissc_latch_disabled);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch(dev, channel, 1U, 2U, &bissc_latch_disabled);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch(dev, channel, 1U, 3U, &bissc_latch_disabled);
	if (ret < 0) {
		return ret;
	}

	return hpm_sei_configure_latch_output(dev, channel, 1U, &latch1_output);
}

static int bissc_configure_channel(const struct device *dev, enum hpm_sei_channel channel,
				   uint32_t period_cycles, uint32_t baud_div,
				   const struct hpm_sei_bissc_config *config,
				   struct hpm_sei_bissc_channel_state *channel_state)
{
	const struct hpm_sei_transceiver_config transceiver = {
		.mode = HPM_SEI_MODE_SYNC_MASTER,
		.tri_sample = false,
		.baudrate = config->baudrate,
		.data = {.idle_state = HPM_SEI_IDLE_HIGH},
		.clock = {.idle_state = HPM_SEI_IDLE_HIGH},
	};
	const struct hpm_sei_sample_config sample = {
		.latch = 0U,
	};
	const struct hpm_sei_update_config update = {
		.latch = 1U,
		.update_on_error = false,
		.revolution_data_register = HPM_SEI_DATA_REGISTER_2,
		.position_data_register = HPM_SEI_DATA_REGISTER_3,
		.use_revolution_rx = true,
		.use_position_rx = true,
		.data_register_select = BIT(2) | BIT(3),
	};
	const struct hpm_sei_engine_config engine = {
		.arm_on_trigger = true,
		.command_data_register = HPM_SEI_DATA_REGISTER_0,
		.data_base_register = HPM_SEI_DATA_REGISTER_0,
		.initial_instruction = HPM_SEI_BISSC_INSTRUCTION_WAIT_ACK,
		.watchdog_enable = config->watchdog_time != 0U,
		.watchdog_exception = true,
		.watchdog_instruction = HPM_SEI_BISSC_INSTRUCTION_HALT,
		.watchdog_time = config->watchdog_time,
	};
	const struct hpm_sei_trigger_input_config trigger = {
		.periodic_enable = true,
		.periodic_cycles = period_cycles,
	};
	struct hpm_sei_timing timing;
	uint8_t single_turn_first_bit = 32U - config->single_turn_bits;
	int ret;

	ret = hpm_sei_set_event_callback(dev, channel, 0U, NULL, NULL);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_transceiver(dev, channel, &transceiver);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_get_timing(dev, channel, &timing);
	if (ret < 0) {
		return ret;
	}
	if (((uint32_t)timing.baud_div + 1U != baud_div) || (timing.ck0_point == 0U) ||
	    (timing.ck1_point == 0U)) {
		return -EIO;
	}
	ret = hpm_sei_set_data_points(dev, channel, timing.ck0_point, timing.ck1_point);
	if (ret < 0) {
		return ret;
	}

	ret = bissc_configure_data(dev, HPM_SEI_DATA_REGISTER_2, config->multi_turn_bits,
				   config->multi_turn_bits - 1U, 0U, 0U,
				   config->multi_turn_bits - 1U);
	if (ret < 0) {
		return ret;
	}
	ret = bissc_configure_data(dev, HPM_SEI_DATA_REGISTER_3, config->single_turn_bits, 31U,
				   single_turn_first_bit, single_turn_first_bit, 31U);
	if (ret < 0) {
		return ret;
	}
	ret = bissc_configure_data(dev, HPM_SEI_DATA_REGISTER_4, HPM_SEI_BISSC_STATUS_BITS, 1U, 0U,
				   0U, 1U);
	if (ret < 0) {
		return ret;
	}
	ret = bissc_configure_crc_data(dev, config);
	if (ret < 0) {
		return ret;
	}
	ret = bissc_configure_instructions(dev, config);
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
	ret = bissc_configure_latches(dev, channel, config->sample_delay_cycles);
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
	channel_state->multi_turn_mask = hpm_sei_bissc_mask(config->multi_turn_bits);
	channel_state->single_turn_mask = hpm_sei_bissc_mask(config->single_turn_bits);
	channel_state->crc_mask = hpm_sei_bissc_mask(config->crc_bits);
	channel_state->single_turn_shift = 32U - config->single_turn_bits;
	channel_state->user_callback = config->callback;
	channel_state->user_data = config->user_data;
	ret = hpm_sei_set_event_callback(dev, channel,
					 HPM_SEI_EVENT_LATCH1 | HPM_SEI_EVENT_TRANSFER_ERROR |
						 HPM_SEI_EVENT_EXCEPTION | HPM_SEI_EVENT_WATCHDOG,
					 hpm_sei_bissc_event_callback, channel_state);
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
	channel_state->configured = false;

	return ret;
}

int hpm_sei_bissc_configure(const struct device *dev, enum hpm_sei_channel channel,
			    const struct hpm_sei_bissc_config *config)
{
	struct hpm_sei_bissc_channel_state *channel_state;
	uint32_t clock_hz;
	uint32_t period_cycles;
	uint32_t divider;
	bool claimed = false;
	int ret;

	if (!hpm_sei_bissc_device_is_valid(dev)) {
		return -ENODEV;
	}
	if ((config == NULL) || !hpm_sei_bissc_channel_is_valid(channel) ||
	    (config->baudrate == 0U) || (config->multi_turn_bits == 0U) ||
	    (config->multi_turn_bits > 32U) || (config->single_turn_bits == 0U) ||
	    (config->single_turn_bits > 32U) || (config->crc_bits == 0U) ||
	    (config->crc_bits > 32U) || (config->poll_period_us == 0U)) {
		return -EINVAL;
	}
	ret = hpm_sei_bissc_check_timing(config->baudrate, config->poll_period_us,
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

	k_mutex_lock(&bissc_lock, K_FOREVER);
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
	ret = hpm_sei_rewind(dev, channel, HPM_SEI_BISSC_DATA_MASK);
	if (ret < 0) {
		goto fail_release;
	}
	ret = bissc_configure_channel(dev, channel, period_cycles, divider, config, channel_state);
	if (ret == 0) {
		channel_state->dev = dev;
	}

fail_release:
	if ((ret < 0) && claimed) {
		(void)hpm_sei_release(dev, channel_state);
	}

out:
	k_mutex_unlock(&bissc_lock);
	return ret;
}

int hpm_sei_bissc_deconfigure(const struct device *dev, enum hpm_sei_channel channel)
{
	const struct hpm_sei_trigger_input_config trigger_disabled = {0};
	struct hpm_sei_bissc_channel_state *channel_state;
	int ret;

	if (!hpm_sei_bissc_device_is_valid(dev) || !hpm_sei_bissc_channel_is_valid(channel)) {
		return -EINVAL;
	}

	k_mutex_lock(&bissc_lock, K_FOREVER);
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
	k_mutex_unlock(&bissc_lock);
	return ret;
}

int hpm_sei_bissc_get_result(const struct device *dev, enum hpm_sei_channel channel,
			     struct hpm_sei_bissc_result *result)
{
	struct hpm_sei_bissc_channel_state *channel_state;
	struct hpm_sei_status status;
	uint32_t events;
	uint32_t value;
	int ret;

	if (!hpm_sei_bissc_device_is_valid(dev)) {
		return -ENODEV;
	}
	if ((result == NULL) || !hpm_sei_bissc_channel_is_valid(channel)) {
		return -EINVAL;
	}

	k_mutex_lock(&bissc_lock, K_FOREVER);
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
	ret = hpm_sei_get_data(dev, HPM_SEI_DATA_REGISTER_2, &value);
	if (ret < 0) {
		goto out;
	}
	result->multi_turn = value & channel_state->multi_turn_mask;
	ret = hpm_sei_get_data(dev, HPM_SEI_DATA_REGISTER_3, &value);
	if (ret < 0) {
		goto out;
	}
	result->single_turn =
		(value >> channel_state->single_turn_shift) & channel_state->single_turn_mask;
	ret = hpm_sei_get_data(dev, HPM_SEI_DATA_REGISTER_4, &value);
	if (ret < 0) {
		goto out;
	}
	result->error = (value & BIT(1)) != 0U;
	result->warning = (value & BIT(0)) != 0U;
	ret = hpm_sei_get_crc_value(dev, HPM_SEI_DATA_REGISTER_5, &value);
	if (ret < 0) {
		goto out;
	}
	result->crc = value & channel_state->crc_mask;
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
	k_mutex_unlock(&bissc_lock);
	return ret;
}
