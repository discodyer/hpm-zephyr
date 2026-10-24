/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>

#include <drivers/motor/hpmicro_sei.h>
#include <drivers/motor/hpmicro_sei_ssi.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#define HPM_SEI_SSI_CHANNEL_COUNT (HPM_SEI_CHANNEL_1 + 1U)

enum hpm_sei_ssi_instruction {
	HPM_SEI_SSI_INSTRUCTION_RECEIVE_IDLE = 0,
	HPM_SEI_SSI_INSTRUCTION_RECEIVE_START,
	HPM_SEI_SSI_INSTRUCTION_RECEIVE_MULTI_TURN,
	HPM_SEI_SSI_INSTRUCTION_RECEIVE_SINGLE_TURN,
	HPM_SEI_SSI_INSTRUCTION_RECEIVE_TIMEOUT,
	HPM_SEI_SSI_INSTRUCTION_WAIT,
	HPM_SEI_SSI_INSTRUCTION_HALT,
	HPM_SEI_SSI_INSTRUCTION_COUNT,
};

BUILD_ASSERT(HPM_SEI_SSI_INSTRUCTION_COUNT <= HPM_SEI_MAX_INSTRUCTIONS);

struct hpm_sei_ssi_channel_state {
	const struct device *dev;
	hpm_sei_event_callback_t user_callback;
	void *user_data;
	uint32_t multi_turn_mask;
	uint32_t single_turn_mask;
	bool configured;
	atomic_t update_seen;
	atomic_t error_seen;
};

static struct hpm_sei_ssi_channel_state ssi_state[HPM_SEI_SSI_CHANNEL_COUNT];
static K_MUTEX_DEFINE(ssi_lock);

static const struct hpm_sei_latch_transition ssi_latch_disabled = {
	.disable_instr_ptr_check = true,
	.disable_clock_check = true,
	.disable_tx_check = true,
	.disable_rx_check = true,
	.disable_timeout_check = true,
};

static bool hpm_sei_ssi_channel_is_valid(enum hpm_sei_channel channel)
{
	return channel <= HPM_SEI_CHANNEL_1;
}

static bool hpm_sei_ssi_device_is_valid(const struct device *dev)
{
	return (dev != NULL) && device_is_ready(dev) && DEVICE_API_IS(hpm_sei, dev);
}

static uint32_t hpm_sei_ssi_mask(uint8_t bits)
{
	return bits == 32U ? UINT32_MAX : (1U << bits) - 1U;
}

static void hpm_sei_ssi_event_callback(const struct device *dev, enum hpm_sei_channel channel,
				       uint32_t events, void *user_data)
{
	struct hpm_sei_ssi_channel_state *state = user_data;

	if ((!hpm_sei_ssi_channel_is_valid(channel)) || (state == NULL) || (dev == NULL)) {
		return;
	}

	if ((events & HPM_SEI_EVENT_LATCH1) != 0U) {
		atomic_set(&state->update_seen, 1);
	}
	if ((events & HPM_SEI_EVENT_TRANSFER_ERROR) != 0U) {
		atomic_set(&state->error_seen, 1);
	}
	if (state->user_callback != NULL) {
		state->user_callback(dev, channel, events, state->user_data);
	}
}

static int hpm_sei_ssi_configure_data(const struct device *dev, uint8_t bits,
				      enum hpm_sei_data_register reg)
{
	const struct hpm_sei_data_format format = {
		.mode = HPM_SEI_DATA_MODE_DATA,
		.is_signed = false,
		.bit_order = HPM_SEI_BIT_ORDER_MSB_FIRST,
		.word_order = HPM_SEI_WORD_ORDER_NORMAL,
		.word_length = bits,
		.first_bit = bits - 1U,
		.last_bit = 0U,
		.min_bit = 0U,
		.max_bit = bits - 1U,
	};

	return hpm_sei_configure_data(dev, reg, &format);
}

static int hpm_sei_ssi_set_instruction(const struct device *dev, uint8_t index,
				       enum hpm_sei_instruction_operation operation,
				       enum hpm_sei_master_clock clock,
				       enum hpm_sei_data_register data_register, uint8_t operand)
{
	const struct hpm_sei_instruction instruction = {
		.operation = operation,
		.clock = clock,
		.crc_register = HPM_SEI_DATA_REGISTER_0,
		.data_register = data_register,
		.operand = operand,
	};

	return hpm_sei_set_instruction(dev, index, &instruction);
}

static int hpm_sei_ssi_configure_instructions(const struct device *dev,
					      const struct hpm_sei_ssi_config *config)
{
	int ret;

	ret = hpm_sei_ssi_set_instruction(
		dev, HPM_SEI_SSI_INSTRUCTION_RECEIVE_IDLE, HPM_SEI_INSTRUCTION_RECEIVE,
		HPM_SEI_MASTER_CLOCK_FALL_RISE, HPM_SEI_DATA_REGISTER_0, 1U);
	if (ret < 0) {
		return ret;
	}

	ret = hpm_sei_ssi_set_instruction(
		dev, HPM_SEI_SSI_INSTRUCTION_RECEIVE_START, HPM_SEI_INSTRUCTION_RECEIVE,
		HPM_SEI_MASTER_CLOCK_FALL_RISE, HPM_SEI_DATA_REGISTER_CONST_1, 1U);
	if (ret < 0) {
		return ret;
	}

	ret = hpm_sei_ssi_set_instruction(
		dev, HPM_SEI_SSI_INSTRUCTION_RECEIVE_MULTI_TURN, HPM_SEI_INSTRUCTION_RECEIVE,
		HPM_SEI_MASTER_CLOCK_FALL_RISE, HPM_SEI_DATA_REGISTER_2, config->multi_turn_bits);
	if (ret < 0) {
		return ret;
	}

	ret = hpm_sei_ssi_set_instruction(
		dev, HPM_SEI_SSI_INSTRUCTION_RECEIVE_SINGLE_TURN, HPM_SEI_INSTRUCTION_RECEIVE,
		HPM_SEI_MASTER_CLOCK_FALL_RISE, HPM_SEI_DATA_REGISTER_3, config->single_turn_bits);
	if (ret < 0) {
		return ret;
	}

	ret = hpm_sei_ssi_set_instruction(dev, HPM_SEI_SSI_INSTRUCTION_RECEIVE_TIMEOUT,
					  HPM_SEI_INSTRUCTION_RECEIVE, HPM_SEI_MASTER_CLOCK_HIGH,
					  HPM_SEI_DATA_REGISTER_CONST_0, 0U);
	if (ret < 0) {
		return ret;
	}

	ret = hpm_sei_ssi_set_instruction(dev, HPM_SEI_SSI_INSTRUCTION_WAIT,
					  HPM_SEI_INSTRUCTION_WAIT, HPM_SEI_MASTER_CLOCK_HIGH,
					  HPM_SEI_DATA_REGISTER_CONST_1, 0U);
	if (ret < 0) {
		return ret;
	}

	return hpm_sei_ssi_set_instruction(dev, HPM_SEI_SSI_INSTRUCTION_HALT,
					   HPM_SEI_INSTRUCTION_HALT, HPM_SEI_MASTER_CLOCK_LOW,
					   HPM_SEI_DATA_REGISTER_0, 0U);
}

static int hpm_sei_ssi_configure_latches(const struct device *dev, enum hpm_sei_channel channel)
{
	const struct hpm_sei_latch_transition latch0_start = {
		.disable_instr_ptr_check = true,
		.disable_clock_check = false,
		.clock_cfg = HPM_SEI_TRANSITION_FALL_LEAVE,
		.disable_tx_check = true,
		.disable_rx_check = true,
		.disable_timeout_check = true,
	};
	const struct hpm_sei_latch_transition latch0_end = {
		.disable_instr_ptr_check = false,
		.instr_ptr_cfg = HPM_SEI_TRANSITION_FALL_LEAVE,
		.instr_ptr_value = HPM_SEI_SSI_INSTRUCTION_WAIT,
		.disable_clock_check = true,
		.disable_tx_check = true,
		.disable_rx_check = true,
		.disable_timeout_check = true,
	};
	const struct hpm_sei_latch_transition latch1_start = {
		.disable_instr_ptr_check = false,
		.instr_ptr_cfg = HPM_SEI_TRANSITION_FALL_LEAVE,
		.instr_ptr_value = HPM_SEI_SSI_INSTRUCTION_RECEIVE_SINGLE_TURN,
		.disable_clock_check = true,
		.disable_tx_check = true,
		.disable_rx_check = true,
		.disable_timeout_check = true,
	};
	const struct hpm_sei_latch_output_config output = {
		.enable = true,
		.output_select = 0U,
		.delay = 0U,
	};
	int ret;

	ret = hpm_sei_configure_latch(dev, channel, 0U, 0U, &latch0_start);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch(dev, channel, 0U, 1U, &ssi_latch_disabled);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch(dev, channel, 0U, 2U, &ssi_latch_disabled);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch(dev, channel, 0U, 3U, &latch0_end);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch_output(dev, channel, 0U, &output);
	if (ret < 0) {
		return ret;
	}

	ret = hpm_sei_configure_latch(dev, channel, 1U, 0U, &latch1_start);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch(dev, channel, 1U, 1U, &ssi_latch_disabled);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch(dev, channel, 1U, 2U, &ssi_latch_disabled);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_configure_latch(dev, channel, 1U, 3U, &ssi_latch_disabled);
	if (ret < 0) {
		return ret;
	}

	return hpm_sei_configure_latch_output(dev, channel, 1U, &output);
}

static int hpm_sei_ssi_configure_channel(const struct device *dev, enum hpm_sei_channel channel,
					 uint32_t period_cycles, uint32_t baud_div,
					 const struct hpm_sei_ssi_config *config,
					 struct hpm_sei_ssi_channel_state *state)
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
		.initial_instruction = HPM_SEI_SSI_INSTRUCTION_RECEIVE_IDLE,
		.watchdog_enable = false,
	};
	const struct hpm_sei_trigger_input_config trigger = {
		.periodic_enable = true,
		.periodic_arm_on_trigger = false,
		.periodic_sync = false,
		.periodic_cycles = period_cycles,
	};
	struct hpm_sei_timing timing;
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
	if ((uint32_t)timing.baud_div + 1U != baud_div) {
		return -EIO;
	}
	ret = hpm_sei_set_data_points(dev, channel, timing.ck0_point, timing.ck1_point);
	if (ret < 0) {
		return ret;
	}

	ret = hpm_sei_ssi_configure_data(dev, config->multi_turn_bits, HPM_SEI_DATA_REGISTER_2);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_ssi_configure_data(dev, config->single_turn_bits, HPM_SEI_DATA_REGISTER_3);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_ssi_configure_instructions(dev, config);
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
	ret = hpm_sei_ssi_configure_latches(dev, channel);
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

	atomic_clear(&state->update_seen);
	atomic_clear(&state->error_seen);
	state->multi_turn_mask = hpm_sei_ssi_mask(config->multi_turn_bits);
	state->single_turn_mask = hpm_sei_ssi_mask(config->single_turn_bits);
	state->user_callback = config->callback;
	state->user_data = config->user_data;
	if (config->callback != NULL) {
		ret = hpm_sei_set_event_callback(
			dev, channel, HPM_SEI_EVENT_LATCH1 | HPM_SEI_EVENT_TRANSFER_ERROR,
			hpm_sei_ssi_event_callback, state);
		if (ret < 0) {
			goto fail;
		}
	}

	ret = hpm_sei_enable(dev, channel, true);
	if (ret < 0) {
		goto fail;
	}
	ret = hpm_sei_configure_trigger_input(dev, channel, &trigger);
	if (ret < 0) {
		goto fail;
	}

	state->configured = true;

	return 0;

fail:
	(void)hpm_sei_enable(dev, channel, false);
	(void)hpm_sei_set_event_callback(dev, channel, 0U, NULL, NULL);
	state->configured = false;

	return ret;
}

int hpm_sei_ssi_configure(const struct device *dev, enum hpm_sei_channel channel,
			  const struct hpm_sei_ssi_config *config)
{
	struct hpm_sei_ssi_channel_state *state;
	uint32_t clock_hz;
	uint32_t period_cycles;
	uint32_t baud_div;
	int ret;

	if (!hpm_sei_ssi_device_is_valid(dev)) {
		return -ENODEV;
	}
	if ((config == NULL) || !hpm_sei_ssi_channel_is_valid(channel) ||
	    (config->baudrate == 0U) || (config->multi_turn_bits == 0U) ||
	    (config->multi_turn_bits > 32U) || (config->single_turn_bits == 0U) ||
	    (config->single_turn_bits > 32U) || (config->poll_period_us == 0U)) {
		return -EINVAL;
	}

	ret = hpm_sei_get_clock_frequency(dev, &clock_hz);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_ssi_baud_to_divider(clock_hz, config->baudrate, &baud_div);
	if (ret < 0) {
		return ret;
	}
	ret = hpm_sei_ssi_period_to_cycles(clock_hz, config->poll_period_us, &period_cycles);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&ssi_lock, K_FOREVER);
	state = &ssi_state[channel];
	if (state->configured && (state->dev != dev)) {
		ret = -EBUSY;
		goto out;
	}

	ret = hpm_sei_enable(dev, channel, false);
	if (ret < 0) {
		goto out;
	}
	state->configured = false;
	(void)hpm_sei_clear_events(dev, channel, HPM_SEI_EVENT_ALL);
	ret = hpm_sei_rewind(dev, channel,
			     BIT(HPM_SEI_DATA_REGISTER_2) | BIT(HPM_SEI_DATA_REGISTER_3));
	if (ret < 0) {
		goto out;
	}
	ret = hpm_sei_ssi_configure_channel(dev, channel, period_cycles, baud_div, config, state);
	if (ret == 0) {
		state->dev = dev;
	}

out:
	k_mutex_unlock(&ssi_lock);
	return ret;
}

int hpm_sei_ssi_get_result(const struct device *dev, enum hpm_sei_channel channel,
			   struct hpm_sei_ssi_result *result)
{
	struct hpm_sei_ssi_channel_state *state;
	struct hpm_sei_status status;
	uint32_t events;
	uint32_t value;
	int ret;

	if (!hpm_sei_ssi_device_is_valid(dev)) {
		return -ENODEV;
	}
	if ((result == NULL) || !hpm_sei_ssi_channel_is_valid(channel)) {
		return -EINVAL;
	}

	k_mutex_lock(&ssi_lock, K_FOREVER);
	state = &ssi_state[channel];
	if (!state->configured || (state->dev != dev)) {
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
		atomic_set(&state->update_seen, 1);
	}
	if (((events & HPM_SEI_EVENT_TRANSFER_ERROR) != 0U) || status.update_error) {
		atomic_set(&state->error_seen, 1);
	}
	if (atomic_get(&state->error_seen) != 0) {
		atomic_clear(&state->error_seen);
		atomic_clear(&state->update_seen);
		ret = -EIO;
		goto out;
	}
	if (atomic_get(&state->update_seen) == 0) {
		ret = -EAGAIN;
		goto out;
	}
	ret = hpm_sei_get_data(dev, HPM_SEI_DATA_REGISTER_2, &value);
	if (ret < 0) {
		goto out;
	}
	result->multi_turn = value & state->multi_turn_mask;
	ret = hpm_sei_get_data(dev, HPM_SEI_DATA_REGISTER_3, &value);
	if (ret < 0) {
		goto out;
	}
	result->single_turn = value & state->single_turn_mask;
	ret = hpm_sei_get_latch_time(dev, channel, 0U, &result->sample_time);
	if (ret < 0) {
		goto out;
	}
	ret = hpm_sei_get_latch_time(dev, channel, 1U, &result->update_time);
	if (ret < 0) {
		goto out;
	}
	result->delay_ticks = result->update_time - result->sample_time;
	atomic_clear(&state->update_seen);
out:
	k_mutex_unlock(&ssi_lock);
	return ret;
}
