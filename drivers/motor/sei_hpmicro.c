/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_sei

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <drivers/motor/hpmicro_sei.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/kernel.h>

#include <hpm_common.h>
#include <hpm_sei_drv.h>

BUILD_ASSERT((int)HPM_SEI_MODE_SYNC_MASTER == (int)sei_synchronous_master_mode);
BUILD_ASSERT((int)HPM_SEI_MODE_SYNC_SLAVE == (int)sei_synchronous_slave_mode);
BUILD_ASSERT((int)HPM_SEI_MODE_ASYNC == (int)sei_asynchronous_mode);
BUILD_ASSERT((int)HPM_SEI_IDLE_LOW == (int)sei_idle_low_state);
BUILD_ASSERT((int)HPM_SEI_IDLE_HIGH == (int)sei_idle_high_state);
BUILD_ASSERT((int)HPM_SEI_DATA_MODE_DATA == (int)sei_data_mode);
BUILD_ASSERT((int)HPM_SEI_DATA_MODE_CHECK == (int)sei_check_mode);
BUILD_ASSERT((int)HPM_SEI_DATA_MODE_CRC == (int)sei_crc_mode);
BUILD_ASSERT((int)HPM_SEI_BIT_ORDER_LSB_FIRST == (int)sei_bit_lsb_first);
BUILD_ASSERT((int)HPM_SEI_BIT_ORDER_MSB_FIRST == (int)sei_bit_msb_first);
BUILD_ASSERT((int)HPM_SEI_WORD_ORDER_NORMAL == (int)sei_word_nonreverse);
BUILD_ASSERT((int)HPM_SEI_WORD_ORDER_REVERSED == (int)sei_word_reverse);
BUILD_ASSERT((int)HPM_SEI_TRANSITION_HIGH_MATCH == (int)sei_state_tran_condition_high_match);
BUILD_ASSERT((int)HPM_SEI_TRANSITION_LOW_DISMATCH == (int)sei_state_tran_condition_low_dismatch);
BUILD_ASSERT((int)HPM_SEI_TRANSITION_RISE_ENTRY == (int)sei_state_tran_condition_rise_entry);
BUILD_ASSERT((int)HPM_SEI_TRANSITION_FALL_LEAVE == (int)sei_state_tran_condition_fall_leave);

struct hpm_sei_config {
	SEI_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
};

struct hpm_sei_data {
	struct k_spinlock lock;
	uint32_t clock_frequency;
};

static bool hpm_sei_channel_is_valid(enum hpm_sei_channel channel)
{
	return channel <= HPM_SEI_CHANNEL_1;
}

static bool hpm_sei_register_is_valid(enum hpm_sei_data_register register_index)
{
	return register_index <= HPM_SEI_DATA_REGISTER_9;
}

static bool hpm_sei_instruction_register_is_valid(enum hpm_sei_data_register register_index)
{
	return (register_index <= HPM_SEI_DATA_REGISTER_9) ||
	       (register_index == HPM_SEI_DATA_REGISTER_CONST_0) ||
	       (register_index == HPM_SEI_DATA_REGISTER_CONST_1);
}

static bool hpm_sei_bit_config_is_valid(const struct hpm_sei_data_format *format)
{
	return format->word_length >= 1U && format->word_length <= 32U &&
	       format->first_bit <= 31U && format->last_bit <= 31U && format->min_bit <= 31U &&
	       format->max_bit <= 31U && format->crc_length <= 32U;
}

static int hpm_sei_drv_configure_transceiver(const struct device *dev, enum hpm_sei_channel channel,
					     const struct hpm_sei_transceiver_config *config)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	sei_transceiver_config_t xcvr = {0};
	k_spinlock_key_t key;
	hpm_stat_t status;

	if (!hpm_sei_channel_is_valid(channel) || (config == NULL) || (config->baudrate == 0U) ||
	    (config->mode > HPM_SEI_MODE_ASYNC) || (config->data.idle_state > HPM_SEI_IDLE_HIGH) ||
	    (config->clock.idle_state > HPM_SEI_IDLE_HIGH)) {
		return -EINVAL;
	}

	xcvr.mode = (sei_transceiver_mode_t)config->mode;
	xcvr.tri_sample = config->tri_sample;
	xcvr.src_clk_freq = data->clock_frequency;

	switch (xcvr.mode) {
	case sei_synchronous_master_mode:
		xcvr.synchronous_master_config.data_idle_high_z = config->data.idle_high_z;
		xcvr.synchronous_master_config.data_idle_state =
			(sei_idle_state_t)config->data.idle_state;
		xcvr.synchronous_master_config.clock_idle_high_z = config->clock.idle_high_z;
		xcvr.synchronous_master_config.clock_idle_state =
			(sei_idle_state_t)config->clock.idle_state;
		xcvr.synchronous_master_config.baudrate = config->baudrate;
		break;
	case sei_synchronous_slave_mode:
		xcvr.synchronous_slave_config.data_idle_high_z = config->data.idle_high_z;
		xcvr.synchronous_slave_config.data_idle_state =
			(sei_idle_state_t)config->data.idle_state;
		xcvr.synchronous_slave_config.clock_idle_high_z = config->clock.idle_high_z;
		xcvr.synchronous_slave_config.clock_idle_state =
			(sei_idle_state_t)config->clock.idle_state;
		xcvr.synchronous_slave_config.max_baudrate = config->baudrate;
		xcvr.synchronous_slave_config.ck0_timeout_us = config->ck0_timeout_us;
		xcvr.synchronous_slave_config.ck1_timeout_us = config->ck1_timeout_us;
		break;
	case sei_asynchronous_mode:
		if ((config->async_data_bits == 0U) || (config->async_data_bits > 32U) ||
		    (config->async_wait_bits > 32U)) {
			return -EINVAL;
		}
		xcvr.asynchronous_config.wait_len = config->async_wait_bits;
		xcvr.asynchronous_config.data_len = config->async_data_bits;
		xcvr.asynchronous_config.parity_enable = config->async_parity_enable;
		xcvr.asynchronous_config.parity = config->async_parity_odd
							  ? sei_asynchronous_parity_odd
							  : sei_asynchronous_parity_even;
		xcvr.asynchronous_config.data_idle_high_z = config->data.idle_high_z;
		xcvr.asynchronous_config.data_idle_state =
			(sei_idle_state_t)config->data.idle_state;
		xcvr.asynchronous_config.baudrate = config->baudrate;
		break;
	default:
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	status = sei_transceiver_config_init(sei->base, channel, &xcvr);
	k_spin_unlock(&data->lock, key);

	return status == status_success ? 0 : -EIO;
}

static int hpm_sei_drv_configure_data(const struct device *dev,
				      enum hpm_sei_data_register register_index,
				      const struct hpm_sei_data_format *format)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	sei_data_format_config_t sdk_format = {0};
	k_spinlock_key_t key;
	hpm_stat_t status;

	if (!hpm_sei_register_is_valid(register_index) || (format == NULL) ||
	    (format->mode > HPM_SEI_DATA_MODE_CRC) || !hpm_sei_bit_config_is_valid(format)) {
		return -EINVAL;
	}

	sdk_format.mode = (sei_data_mode_t)format->mode;
	sdk_format.signed_flag = format->is_signed;
	sdk_format.bit_order = (sei_data_bit_order_t)format->bit_order;
	sdk_format.word_order = (sei_data_word_order_t)format->word_order;
	sdk_format.word_len = format->word_length;
	sdk_format.crc_invert = format->crc_invert;
	sdk_format.crc_shift_mode = format->crc_shift;
	sdk_format.crc_len = format->crc_length;
	sdk_format.first_bit = format->first_bit;
	sdk_format.last_bit = format->last_bit;
	sdk_format.min_bit = format->min_bit;
	sdk_format.max_bit = format->max_bit;
	sdk_format.gold_value = format->gold_value;
	sdk_format.crc_init_value = format->crc_initial_value;
	sdk_format.crc_poly = format->crc_polynomial;

	key = k_spin_lock(&data->lock);
	status = sei_cmd_data_format_config_init(sei->base, SEI_SELECT_DATA, register_index,
						 &sdk_format);
	k_spin_unlock(&data->lock, key);

	return status == status_success ? 0 : -EIO;
}

static int hpm_sei_drv_set_instruction(const struct device *dev, uint8_t index,
				       const struct hpm_sei_instruction *instruction)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if ((index >= HPM_SEI_MAX_INSTRUCTIONS) || (instruction == NULL) ||
	    (instruction->operation > HPM_SEI_INSTRUCTION_RECEIVE) ||
	    !hpm_sei_instruction_register_is_valid(instruction->crc_register) ||
	    !hpm_sei_instruction_register_is_valid(instruction->data_register)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	sei_set_instr(sei->base, index, instruction->operation, instruction->clock,
		      instruction->crc_register, instruction->data_register, instruction->operand);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_sei_drv_configure_latch(const struct device *dev, enum hpm_sei_channel channel,
				       uint8_t latch, uint8_t transition,
				       const struct hpm_sei_latch_transition *config)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	sei_state_transition_config_t sdk_config = {0};
	k_spinlock_key_t key;
	hpm_stat_t status;

	if (!hpm_sei_channel_is_valid(channel) || (latch >= HPM_SEI_LATCH_COUNT) ||
	    (transition >= 4U) || (config == NULL)) {
		return -EINVAL;
	}

	sdk_config.disable_instr_ptr_check = config->disable_instr_ptr_check;
	sdk_config.instr_ptr_cfg = (sei_state_tran_condition_t)config->instr_ptr_cfg;
	sdk_config.instr_ptr_value = config->instr_ptr_value;
	sdk_config.disable_clk_check = config->disable_clock_check;
	sdk_config.clk_cfg = (sei_state_tran_condition_t)config->clock_cfg;
	sdk_config.disable_txd_check = config->disable_tx_check;
	sdk_config.txd_cfg = (sei_state_tran_condition_t)config->tx_cfg;
	sdk_config.disable_rxd_check = config->disable_rx_check;
	sdk_config.rxd_cfg = (sei_state_tran_condition_t)config->rx_cfg;
	sdk_config.disable_timeout_check = config->disable_timeout_check;
	sdk_config.timeout_cfg = (sei_state_tran_condition_t)config->timeout_cfg;

	key = k_spin_lock(&data->lock);
	status = sei_state_transition_config_init(sei->base, channel, latch, transition,
						  &sdk_config);
	k_spin_unlock(&data->lock, key);

	return status == status_success ? 0 : -EIO;
}

static int hpm_sei_drv_configure_sample(const struct device *dev, enum hpm_sei_channel channel,
					const struct hpm_sei_sample_config *config)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	sei_sample_config_t sdk_config = {0};
	k_spinlock_key_t key;
	hpm_stat_t status;

	if (!hpm_sei_channel_is_valid(channel) || (config == NULL) ||
	    (config->latch >= HPM_SEI_LATCH_COUNT) ||
	    !hpm_sei_register_is_valid(config->acc_data_register) ||
	    !hpm_sei_register_is_valid(config->speed_data_register) ||
	    !hpm_sei_register_is_valid(config->revolution_data_register) ||
	    !hpm_sei_register_is_valid(config->position_data_register)) {
		return -EINVAL;
	}

	sdk_config.acc_data_idx = config->acc_data_register;
	sdk_config.spd_data_idx = config->speed_data_register;
	sdk_config.rev_data_idx = config->revolution_data_register;
	sdk_config.pos_data_idx = config->position_data_register;
	sdk_config.acc_data_use_rx = config->use_acc_rx;
	sdk_config.spd_data_use_rx = config->use_speed_rx;
	sdk_config.rev_data_use_rx = config->use_revolution_rx;
	sdk_config.pos_data_use_rx = config->use_position_rx;
	sdk_config.latch_select = config->latch;
	sdk_config.sample_once = config->sample_once;
	sdk_config.sample_window = config->window;
	sdk_config.data_register_select = config->data_register_select;

	key = k_spin_lock(&data->lock);
	status = sei_sample_config_init(sei->base, channel, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return status == status_success ? 0 : -EIO;
}

static int hpm_sei_drv_configure_update(const struct device *dev, enum hpm_sei_channel channel,
					const struct hpm_sei_update_config *config)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	sei_update_config_t sdk_config = {0};
	k_spinlock_key_t key;
	hpm_stat_t status;

	if (!hpm_sei_channel_is_valid(channel) || (config == NULL) ||
	    (config->latch >= HPM_SEI_LATCH_COUNT) ||
	    !hpm_sei_register_is_valid(config->acc_data_register) ||
	    !hpm_sei_register_is_valid(config->speed_data_register) ||
	    !hpm_sei_register_is_valid(config->revolution_data_register) ||
	    !hpm_sei_register_is_valid(config->position_data_register)) {
		return -EINVAL;
	}

	sdk_config.acc_data_idx = config->acc_data_register;
	sdk_config.spd_data_idx = config->speed_data_register;
	sdk_config.rev_data_idx = config->revolution_data_register;
	sdk_config.pos_data_idx = config->position_data_register;
	sdk_config.acc_data_use_rx = config->use_acc_rx;
	sdk_config.spd_data_use_rx = config->use_speed_rx;
	sdk_config.rev_data_use_rx = config->use_revolution_rx;
	sdk_config.pos_data_use_rx = config->use_position_rx;
	sdk_config.time_use_override = config->use_time_override;
	sdk_config.update_on_err = config->update_on_error;
	sdk_config.latch_select = config->latch;
	sdk_config.data_register_select = config->data_register_select;

	key = k_spin_lock(&data->lock);
	status = sei_update_config_init(sei->base, channel, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return status == status_success ? 0 : -EIO;
}

static int hpm_sei_drv_configure_engine(const struct device *dev, enum hpm_sei_channel channel,
					const struct hpm_sei_engine_config *config)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	sei_engine_config_t sdk_config = {0};
	k_spinlock_key_t key;
	hpm_stat_t status;

	if (!hpm_sei_channel_is_valid(channel) || (config == NULL) ||
	    (config->initial_instruction >= HPM_SEI_MAX_INSTRUCTIONS) ||
	    (config->watchdog_instruction >= HPM_SEI_MAX_INSTRUCTIONS) ||
	    !hpm_sei_register_is_valid(config->command_data_register) ||
	    !hpm_sei_register_is_valid(config->data_base_register)) {
		return -EINVAL;
	}

	sdk_config.arming_mode =
		config->arm_on_trigger ? sei_arming_wait_trigger : sei_arming_direct_exec;
	sdk_config.data_cdm_idx = config->command_data_register;
	sdk_config.data_base_idx = config->data_base_register;
	sdk_config.init_instr_idx = config->initial_instruction;
	sdk_config.wdg_enable = config->watchdog_enable;
	sdk_config.wdg_action =
		config->watchdog_exception ? sei_wdg_exec_exception_instr : sei_wdg_exec_next_instr;
	sdk_config.wdg_instr_idx = config->watchdog_instruction;
	sdk_config.wdg_time = config->watchdog_time;

	key = k_spin_lock(&data->lock);
	status = sei_engine_config_init(sei->base, channel, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return status == status_success ? 0 : -EIO;
}

static int hpm_sei_drv_enable(const struct device *dev, enum hpm_sei_channel channel, bool enable)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_sei_channel_is_valid(channel)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	sei_set_engine_enable(sei->base, channel, enable);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_sei_drv_software_trigger(const struct device *dev, enum hpm_sei_channel channel)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_sei_channel_is_valid(channel)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	sei_set_trig_input_soft_enable(sei->base, channel);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_sei_drv_get_data(const struct device *dev, enum hpm_sei_data_register register_index,
				uint32_t *value)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_sei_register_is_valid(register_index) || (value == NULL)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	*value = sei_get_data_value(sei->base, register_index);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_sei_drv_set_data(const struct device *dev, enum hpm_sei_data_register register_index,
				uint32_t value)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_sei_register_is_valid(register_index)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	sei_set_data_value(sei->base, register_index, value);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_sei_drv_get_status(const struct device *dev, enum hpm_sei_channel channel,
				  struct hpm_sei_status *status)
{
	const struct hpm_sei_config *sei = dev->config;

	if (!hpm_sei_channel_is_valid(channel) || (status == NULL)) {
		return -EINVAL;
	}

	status->irq_status = sei->base->CTRL[channel].IRQ.INT_STS;
	status->sample_valid = sei->base->CTRL[channel].POS.SMP_VAL;
	status->update_status = sei->base->CTRL[channel].POS.UPD_STS;
	status->sample_occurred =
		SEI_CTRL_POS_SMP_STS_OCCUR_GET(sei->base->CTRL[channel].POS.SMP_STS) != 0U;
	status->update_error = SEI_CTRL_POS_UPD_STS_UPD_ERR_GET(status->update_status) != 0U;

	return 0;
}

static DEVICE_API(hpm_sei, hpm_sei_api) = {
	.configure_transceiver = hpm_sei_drv_configure_transceiver,
	.configure_data = hpm_sei_drv_configure_data,
	.set_instruction = hpm_sei_drv_set_instruction,
	.configure_latch = hpm_sei_drv_configure_latch,
	.configure_sample = hpm_sei_drv_configure_sample,
	.configure_update = hpm_sei_drv_configure_update,
	.configure_engine = hpm_sei_drv_configure_engine,
	.enable = hpm_sei_drv_enable,
	.software_trigger = hpm_sei_drv_software_trigger,
	.get_data = hpm_sei_drv_get_data,
	.set_data = hpm_sei_drv_set_data,
	.get_status = hpm_sei_drv_get_status,
};

static int hpm_sei_init(const struct device *dev)
{
	const struct hpm_sei_config *config = dev->config;
	struct hpm_sei_data *data = dev->data;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		return ret;
	}

	ret = clock_control_get_rate(config->clock_dev, config->clock_subsys,
				     &data->clock_frequency);
	if (ret < 0) {
		return ret;
	}

	return data->clock_frequency == 0U ? -EINVAL : 0;
}

#define HPM_SEI_INIT(n)                                                                            \
	static const struct hpm_sei_config hpm_sei_config_##n = {                                  \
		.base = (SEI_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
	};                                                                                         \
                                                                                                   \
	static struct hpm_sei_data hpm_sei_data_##n;                                               \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_sei_init, NULL, &hpm_sei_data_##n, &hpm_sei_config_##n,       \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &hpm_sei_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_SEI_INIT)
