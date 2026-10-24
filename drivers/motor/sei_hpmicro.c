/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_sei

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <drivers/motor/hpmicro_sei.h>
#include <drivers/motor/hpmicro_synt.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#include <hpm_common.h>
#include <hpm_sei_drv.h>
#include <hpm_synt_drv.h>

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
BUILD_ASSERT(HPM_SEI_EVENT_ALL ==
	     (SEI_CTRL_IRQ_INT_FLAG_STALL_MASK | SEI_CTRL_IRQ_INT_FLAG_EXCEPT_MASK |
	      SEI_CTRL_IRQ_INT_FLAG_WDOG_MASK | SEI_CTRL_IRQ_INT_FLAG_PTR0_ST_MASK |
	      SEI_CTRL_IRQ_INT_FLAG_PTR1_ST_MASK | SEI_CTRL_IRQ_INT_FLAG_INSTR0_ST_MASK |
	      SEI_CTRL_IRQ_INT_FLAG_INSTR1_ST_MASK | SEI_CTRL_IRQ_INT_FLAG_PTR0_END_MASK |
	      SEI_CTRL_IRQ_INT_FLAG_PTR1_END_MASK | SEI_CTRL_IRQ_INT_FLAG_INSTR0_END_MASK |
	      SEI_CTRL_IRQ_INT_FLAG_INSTR1_END_MASK | SEI_CTRL_IRQ_INT_FLAG_TRX_ERR_MASK |
	      SEI_CTRL_IRQ_INT_FLAG_TIMEOUT_MASK | SEI_CTRL_IRQ_INT_FLAG_LATCH0_MASK |
	      SEI_CTRL_IRQ_INT_FLAG_LATCH1_MASK | SEI_CTRL_IRQ_INT_FLAG_LATCH2_MASK |
	      SEI_CTRL_IRQ_INT_FLAG_LATCH3_MASK | SEI_CTRL_IRQ_INT_FLAG_SMP_ERR_MASK |
	      SEI_CTRL_IRQ_INT_FLAG_TRIGER0_MASK | SEI_CTRL_IRQ_INT_FLAG_TRIGER1_MASK |
	      SEI_CTRL_IRQ_INT_FLAG_TRIGER2_MASK | SEI_CTRL_IRQ_INT_FLAG_TRIGER3_MASK |
	      SEI_CTRL_IRQ_INT_FLAG_TRG_ERR0_MASK | SEI_CTRL_IRQ_INT_FLAG_TRG_ERR1_MASK |
	      SEI_CTRL_IRQ_INT_FLAG_TRG_ERR2_MASK | SEI_CTRL_IRQ_INT_FLAG_TRG_ERR3_MASK));

struct hpm_sei_config {
	SEI_Type *base;
	const struct device *clock_dev;
	const struct device *timestamp_dev;
	clock_control_subsys_t clock_subsys;
	void (*irq_config_func)(const struct device *dev);
};

struct hpm_sei_data {
	struct k_spinlock lock;
	uint32_t clock_frequency;
	enum hpm_sei_timestamp_source timestamp_source;
	const void *owner;
	enum hpm_sei_mode mode[HPM_SEI_CHANNEL_1 + 1U];
	uint32_t event_mask[HPM_SEI_CHANNEL_1 + 1U];
	hpm_sei_event_callback_t callback[HPM_SEI_CHANNEL_1 + 1U];
	void *user_data[HPM_SEI_CHANNEL_1 + 1U];
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

static int hpm_sei_drv_claim(const struct device *dev, const void *owner)
{
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;
	int ret = 0;

	if (owner == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	if ((data->owner != NULL) && (data->owner != owner)) {
		ret = -EBUSY;
	} else {
		data->owner = owner;
	}
	k_spin_unlock(&data->lock, key);

	return ret;
}

static int hpm_sei_drv_release(const struct device *dev, const void *owner)
{
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;
	int ret = 0;

	if (owner == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	if (data->owner != owner) {
		ret = -EINVAL;
	} else {
		data->owner = NULL;
	}
	k_spin_unlock(&data->lock, key);

	return ret;
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
	if (status == status_success) {
		data->mode[channel] = config->mode;
	}
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

static int hpm_sei_drv_configure_command(const struct device *dev,
					 enum hpm_sei_channel channel,
					 const struct hpm_sei_data_format *format)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	sei_data_format_config_t sdk_format = {0};
	k_spinlock_key_t key;
	hpm_stat_t status;

	if (!hpm_sei_channel_is_valid(channel) || (format == NULL) ||
	    (format->mode != HPM_SEI_DATA_MODE_DATA) || !hpm_sei_bit_config_is_valid(format)) {
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

	key = k_spin_lock(&data->lock);
	status = sei_cmd_data_format_config_init(sei->base, SEI_SELECT_CMD, channel,
						 &sdk_format);
	k_spin_unlock(&data->lock, key);

	return status == status_success ? 0 : -EIO;
}

static int hpm_sei_drv_get_command(const struct device *dev, enum hpm_sei_channel channel,
				   uint32_t *value)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_sei_channel_is_valid(channel) || (value == NULL)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	*value = sei_get_command_value(sei->base, channel);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_sei_drv_set_command(const struct device *dev, enum hpm_sei_channel channel,
				   uint32_t value)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_sei_channel_is_valid(channel)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	sei->base->CTRL[channel].CMD.CMD = value;
	k_spin_unlock(&data->lock, key);

	return 0;
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
#if defined(HPM_IP_FEATURE_SEI_ERRATA_E00047) && HPM_IP_FEATURE_SEI_ERRATA_E00047
	if (!config->disable_rx_check ||
	    ((data->mode[channel] == HPM_SEI_MODE_SYNC_SLAVE) && !config->disable_clock_check)) {
		k_spin_unlock(&data->lock, key);
		return -ENOTSUP;
	}
#endif
	status = sei_state_transition_config_init(sei->base, channel, latch, transition,
						  &sdk_config);
	k_spin_unlock(&data->lock, key);

	return status == status_success ? 0 : -EIO;
}

static int hpm_sei_drv_configure_latch_output(
	const struct device *dev, enum hpm_sei_channel channel, uint8_t latch,
	const struct hpm_sei_latch_output_config *config)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	sei_state_transition_latch_config_t sdk_config = {0};
	k_spinlock_key_t key;
	hpm_stat_t status;

	if (!hpm_sei_channel_is_valid(channel) || (latch >= HPM_SEI_LATCH_COUNT) ||
	    (config == NULL) || (config->output_select >= HPM_SEI_TRANSITION_COUNT)) {
		return -EINVAL;
	}

	sdk_config.enable = config->enable;
	sdk_config.output_select = config->output_select;
	sdk_config.delay = config->delay;

	key = k_spin_lock(&data->lock);
	status = sei_state_transition_latch_config_init(sei->base, channel, latch, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return status == status_success ? 0 : -EIO;
}

static int
hpm_sei_drv_configure_trigger_input(const struct device *dev, enum hpm_sei_channel channel,
				    const struct hpm_sei_trigger_input_config *config)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	sei_trigger_input_config_t sdk_config = {0};
	k_spinlock_key_t key;
	hpm_stat_t status;

	if (!hpm_sei_channel_is_valid(channel) || (config == NULL) ||
	    (config->input0_select > 7U) || (config->input1_select > 7U) ||
	    (config->periodic_sync_select > 7U) ||
	    (config->periodic_enable &&
	     ((config->periodic_cycles == 0U) || (config->periodic_cycles == UINT32_MAX)))) {
		return -EINVAL;
	}

	sdk_config.trig_in0_enable = config->input0_enable;
	sdk_config.trig_in0_select = config->input0_select;
	sdk_config.trig_in1_enable = config->input1_enable;
	sdk_config.trig_in1_select = config->input1_select;
	sdk_config.trig_period_enable = config->periodic_enable;
	sdk_config.trig_period_arming_mode = config->periodic_arm_on_trigger
						     ? sei_arming_wait_trigger
						     : sei_arming_direct_exec;
	sdk_config.trig_period_sync_enable = config->periodic_sync;
	sdk_config.trig_period_sync_select = config->periodic_sync_select;
	sdk_config.trig_period_time = config->periodic_cycles;
	/* errata E00048: the pinned HAL applies the required baud-rate workaround. */

	key = k_spin_lock(&data->lock);
	status = sei_trigger_input_config_init(sei->base, channel, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return status == status_success ? 0 : -EIO;
}

static int
hpm_sei_drv_configure_trigger_output(const struct device *dev, enum hpm_sei_channel channel,
				     uint8_t output,
				     const struct hpm_sei_trigger_output_config *config)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	sei_trigger_output_config_t sdk_config = {0};
	k_spinlock_key_t key;
	hpm_stat_t status;

	if (!hpm_sei_channel_is_valid(channel) || (output >= HPM_SEI_TRIGGER_OUTPUTS) ||
	    (config == NULL) || (config->latch >= HPM_SEI_LATCH_COUNT)) {
		return -EINVAL;
	}

	/* The SDK field name is misleading: it selects the OUTx field, not a latch. */
	sdk_config.src_latch_select = output;
	sdk_config.trig_out_enable = config->enable;
	sdk_config.trig_out_select = config->latch;

	key = k_spin_lock(&data->lock);
	status = sei_trigger_output_config_init(sei->base, channel, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return status == status_success ? 0 : -EIO;
}

static int hpm_sei_drv_set_trigger_command(const struct device *dev,
					   enum hpm_sei_channel channel,
					   enum hpm_sei_trigger_command source, uint32_t command)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_sei_channel_is_valid(channel) ||
	    (source > HPM_SEI_TRIGGER_COMMAND_SOFTWARE)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	sei_set_trig_input_command_value(sei->base, channel, (sei_trig_in_type_t)source, command);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_sei_drv_get_trigger_status(const struct device *dev,
					  enum hpm_sei_channel channel,
					  struct hpm_sei_trigger_status *status)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	uint32_t value;
	k_spinlock_key_t key;
	uint8_t source;

	if (!hpm_sei_channel_is_valid(channel) || (status == NULL)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	value = sei->base->CTRL[channel].TRG.PRD_STS;
	status->periodic_triggered = SEI_CTRL_TRG_PRD_STS_TRIGERED_GET(value) != 0U;
	status->periodic_armed = SEI_CTRL_TRG_PRD_STS_ARMED_GET(value) != 0U;
	status->periodic_count =
		SEI_CTRL_TRG_PRD_CNT_PERIOD_CNT_GET(sei->base->CTRL[channel].TRG.PRD_CNT);
	for (source = 0U; source <= HPM_SEI_TRIGGER_COMMAND_SOFTWARE; source++) {
		status->trigger_time[source] =
			SEI_CTRL_TRG_TABLE_TIME_TRIGGER0_TIME_GET(
				sei->base->CTRL[channel].TRG_TABLE.TIME[source]);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
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

static int hpm_sei_drv_get_crc_value(const struct device *dev,
				     enum hpm_sei_data_register register_index, uint32_t *value)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if ((register_index < HPM_SEI_DATA_REGISTER_2) ||
	    (register_index > HPM_SEI_DATA_REGISTER_9) || (value == NULL)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	*value = sei_get_crc_value(sei->base, register_index);
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

static int hpm_sei_drv_get_clock_frequency(const struct device *dev, uint32_t *frequency)
{
	const struct hpm_sei_data *data = dev->data;

	if (frequency == NULL) {
		return -EINVAL;
	}

	*frequency = data->clock_frequency;

	return 0;
}

static int hpm_sei_drv_get_timing(const struct device *dev, enum hpm_sei_channel channel,
				  struct hpm_sei_timing *timing)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_sei_channel_is_valid(channel) || (timing == NULL)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	timing->rx_point = SEI_CTRL_XCVR_DATA_CFG_RXD_POINT_GET(
		sei->base->CTRL[channel].XCVR.DATA_CFG);
	timing->tx_point = SEI_CTRL_XCVR_DATA_CFG_TXD_POINT_GET(
		sei->base->CTRL[channel].XCVR.DATA_CFG);
	timing->ck0_point = sei_get_xcvr_ck0_point(sei->base, channel);
	timing->ck1_point = sei_get_xcvr_ck1_point(sei->base, channel);
	timing->baud_div = sei_get_xcvr_baud_div(sei->base, channel);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_sei_drv_set_data_points(const struct device *dev, enum hpm_sei_channel channel,
				       uint16_t rx_point, uint16_t tx_point)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_sei_channel_is_valid(channel) || (rx_point == 0U) || (tx_point == 0U)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	sei_set_xcvr_rx_point(sei->base, channel, rx_point);
	sei_set_xcvr_tx_point(sei->base, channel, tx_point);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_sei_drv_rewind(const struct device *dev, enum hpm_sei_channel channel,
			      uint32_t data_register_mask)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;
	uint8_t register_index;

	if (!hpm_sei_channel_is_valid(channel) ||
	    (data_register_mask & ~BIT_MASK(HPM_SEI_DATA_REGISTERS)) != 0U) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	if ((sei->base->CTRL[channel].ENGINE.CTRL & SEI_CTRL_ENGINE_CTRL_ENABLE_MASK) != 0U) {
		k_spin_unlock(&data->lock, key);
		return -EBUSY;
	}
	sei_set_engine_rewind(sei->base, channel);
	sei_set_command_rewind(sei->base, channel);
	for (register_index = 0U; register_index < HPM_SEI_DATA_REGISTERS; register_index++) {
		if ((data_register_mask & BIT(register_index)) != 0U) {
			sei_set_data_rewind(sei->base, register_index);
		}
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_sei_drv_restart_async(const struct device *dev, enum hpm_sei_channel channel)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_sei_channel_is_valid(channel) || (data->mode[channel] != HPM_SEI_MODE_ASYNC)) {
		return -ENOTSUP;
	}

	key = k_spin_lock(&data->lock);
	if ((sei->base->CTRL[channel].ENGINE.CTRL & SEI_CTRL_ENGINE_CTRL_ENABLE_MASK) != 0U) {
		k_spin_unlock(&data->lock, key);
		return -EBUSY;
	}
	sei_restart_asynchronous_xcvr(sei->base, channel);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_sei_drv_get_latch_time(const struct device *dev, enum hpm_sei_channel channel,
				      uint8_t latch, uint32_t *time)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_sei_channel_is_valid(channel) || (latch >= HPM_SEI_LATCH_COUNT) ||
	    (time == NULL)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	*time = sei_get_latch_time(sei->base, channel, latch);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_sei_drv_set_timestamp_source(const struct device *dev,
					    enum hpm_sei_timestamp_source source)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;
	int ret;

	if (source > HPM_SEI_TIMESTAMP_SOURCE_SYNT) {
		return -EINVAL;
	}
	if ((source == HPM_SEI_TIMESTAMP_SOURCE_SYNT) &&
	    (!IS_ENABLED(CONFIG_SYNT_HPMICRO) || (sei->timestamp_dev == NULL) ||
	     !device_is_ready(sei->timestamp_dev))) {
		return -ENODEV;
	}

	key = k_spin_lock(&data->lock);
	ret = 0;
#if defined(CONFIG_SYNT_HPMICRO)
	if (((source == HPM_SEI_TIMESTAMP_SOURCE_SYNT) ||
	     (data->timestamp_source == HPM_SEI_TIMESTAMP_SOURCE_SYNT)) &&
	    (sei->timestamp_dev != NULL)) {
		ret = hpm_synt_timestamp_enable(sei->timestamp_dev,
						source == HPM_SEI_TIMESTAMP_SOURCE_SYNT);
	}
#endif
	if (ret == 0) {
		data->timestamp_source = source;
	}
	k_spin_unlock(&data->lock, key);

	return ret;
}

static int hpm_sei_drv_get_timestamp_source(const struct device *dev,
					    enum hpm_sei_timestamp_source *source)
{
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if (source == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	*source = data->timestamp_source;
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_sei_drv_get_status(const struct device *dev, enum hpm_sei_channel channel,
				  struct hpm_sei_status *status)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_sei_channel_is_valid(channel) || (status == NULL)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	status->irq_status = sei->base->CTRL[channel].IRQ.INT_STS;
	status->irq_enable = sei->base->CTRL[channel].IRQ.INT_EN;
	status->sample_valid = sei->base->CTRL[channel].POS.SMP_VAL;
	status->update_status = sei->base->CTRL[channel].POS.UPD_STS;
	status->sample_occurred =
		SEI_CTRL_POS_SMP_STS_OCCUR_GET(sei->base->CTRL[channel].POS.SMP_STS) != 0U;
	status->update_error = SEI_CTRL_POS_UPD_STS_UPD_ERR_GET(status->update_status) != 0U;
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_sei_drv_get_events(const struct device *dev, enum hpm_sei_channel channel,
				  uint32_t *events)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_sei_channel_is_valid(channel) || (events == NULL)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	*events = sei_get_irq_flag(sei->base, channel) & HPM_SEI_EVENT_ALL;
	if (*events != 0U) {
		sei_clear_irq_flag(sei->base, channel, *events);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_sei_drv_clear_events(const struct device *dev, enum hpm_sei_channel channel,
				    uint32_t events)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_sei_channel_is_valid(channel) || ((events & ~HPM_SEI_EVENT_ALL) != 0U)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	sei_clear_irq_flag(sei->base, channel, events);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_sei_drv_set_event_callback(const struct device *dev,
					  enum hpm_sei_channel channel, uint32_t event_mask,
					  hpm_sei_event_callback_t callback, void *user_data)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_sei_channel_is_valid(channel) || ((event_mask & ~HPM_SEI_EVENT_ALL) != 0U)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	sei_set_irq_enable(sei->base, channel, HPM_SEI_EVENT_ALL, false);
	sei_clear_irq_flag(sei->base, channel, HPM_SEI_EVENT_ALL);
	data->event_mask[channel] = event_mask;
	data->callback[channel] = callback;
	data->user_data[channel] = user_data;
	if ((callback != NULL) && (event_mask != 0U)) {
		sei_set_irq_enable(sei->base, channel, event_mask, true);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static void hpm_sei_isr(const struct device *dev, enum hpm_sei_channel channel)
{
	const struct hpm_sei_config *sei = dev->config;
	struct hpm_sei_data *data = dev->data;
	hpm_sei_event_callback_t callback;
	void *user_data;
	k_spinlock_key_t key;
	uint32_t events;

	key = k_spin_lock(&data->lock);
	events = sei_get_irq_flag(sei->base, channel) & data->event_mask[channel];
	if (events != 0U) {
		sei_clear_irq_flag(sei->base, channel, events);
	}
	callback = data->callback[channel];
	user_data = data->user_data[channel];
	k_spin_unlock(&data->lock, key);

	if ((callback != NULL) && (events != 0U)) {
		callback(dev, channel, events, user_data);
	}
}

static void hpm_sei_isr_0(const struct device *dev)
{
	hpm_sei_isr(dev, HPM_SEI_CHANNEL_0);
}

static void hpm_sei_isr_1(const struct device *dev)
{
	hpm_sei_isr(dev, HPM_SEI_CHANNEL_1);
}

static DEVICE_API(hpm_sei, hpm_sei_api) = {
	.configure_transceiver = hpm_sei_drv_configure_transceiver,
	.claim = hpm_sei_drv_claim,
	.release = hpm_sei_drv_release,
	.configure_data = hpm_sei_drv_configure_data,
	.configure_command = hpm_sei_drv_configure_command,
	.set_instruction = hpm_sei_drv_set_instruction,
	.configure_latch = hpm_sei_drv_configure_latch,
	.configure_latch_output = hpm_sei_drv_configure_latch_output,
	.configure_sample = hpm_sei_drv_configure_sample,
	.configure_update = hpm_sei_drv_configure_update,
	.configure_engine = hpm_sei_drv_configure_engine,
	.configure_trigger_input = hpm_sei_drv_configure_trigger_input,
	.configure_trigger_output = hpm_sei_drv_configure_trigger_output,
	.set_trigger_command = hpm_sei_drv_set_trigger_command,
	.get_trigger_status = hpm_sei_drv_get_trigger_status,
	.enable = hpm_sei_drv_enable,
	.software_trigger = hpm_sei_drv_software_trigger,
	.get_data = hpm_sei_drv_get_data,
	.get_crc_value = hpm_sei_drv_get_crc_value,
	.set_data = hpm_sei_drv_set_data,
	.get_command = hpm_sei_drv_get_command,
	.set_command = hpm_sei_drv_set_command,
	.get_clock_frequency = hpm_sei_drv_get_clock_frequency,
	.get_timing = hpm_sei_drv_get_timing,
	.set_data_points = hpm_sei_drv_set_data_points,
	.rewind = hpm_sei_drv_rewind,
	.restart_async = hpm_sei_drv_restart_async,
	.get_latch_time = hpm_sei_drv_get_latch_time,
	.set_timestamp_source = hpm_sei_drv_set_timestamp_source,
	.get_timestamp_source = hpm_sei_drv_get_timestamp_source,
	.get_status = hpm_sei_drv_get_status,
	.get_events = hpm_sei_drv_get_events,
	.clear_events = hpm_sei_drv_clear_events,
	.set_event_callback = hpm_sei_drv_set_event_callback,
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

	if (data->clock_frequency == 0U) {
		return -EINVAL;
	}

	config->irq_config_func(dev);

	return 0;
}

#define HPM_SEI_IRQ_CONFIG(n)                                                                      \
	static void hpm_sei_irq_config_##n(const struct device *dev)                               \
	{                                                                                          \
		ARG_UNUSED(dev);                                                                   \
		IRQ_CONNECT(DT_INST_IRQN_BY_NAME(n, channel0),                                    \
			    DT_INST_IRQ_BY_NAME(n, channel0, priority),                            \
			    hpm_sei_isr_0, DEVICE_DT_INST_GET(n), 0);                              \
		IRQ_CONNECT(DT_INST_IRQN_BY_NAME(n, channel1),                                    \
			    DT_INST_IRQ_BY_NAME(n, channel1, priority),                            \
			    hpm_sei_isr_1, DEVICE_DT_INST_GET(n), 0);                              \
		irq_enable(DT_INST_IRQN_BY_NAME(n, channel0));                                     \
		irq_enable(DT_INST_IRQN_BY_NAME(n, channel1));                                     \
	}

#if defined(CONFIG_SYNT_HPMICRO)
#define HPM_SEI_TIMESTAMP_DEV(n)                                                                   \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(n, synt), (DEVICE_DT_GET(DT_INST_PHANDLE(n, synt))),     \
		    (NULL))
#else
#define HPM_SEI_TIMESTAMP_DEV(n) NULL
#endif

#define HPM_SEI_INIT(n)                                                                            \
	BUILD_ASSERT(DT_INST_NUM_IRQS(n) == 2, "SEI requires one IRQ per channel");                 \
	BUILD_ASSERT(DT_INST_IRQ_HAS_NAME(n, channel0) && DT_INST_IRQ_HAS_NAME(n, channel1),        \
		     "SEI interrupts must be named channel0 and channel1");                            \
	HPM_SEI_IRQ_CONFIG(n)                                                                      \
	static const struct hpm_sei_config hpm_sei_config_##n = {                                  \
		.base = (SEI_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.timestamp_dev = HPM_SEI_TIMESTAMP_DEV(n),                                         \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.irq_config_func = hpm_sei_irq_config_##n,                                         \
	};                                                                                         \
                                                                                                   \
	static struct hpm_sei_data hpm_sei_data_##n;                                               \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_sei_init, NULL, &hpm_sei_data_##n, &hpm_sei_config_##n,       \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &hpm_sei_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_SEI_INIT)
