/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_rdc

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <drivers/motor/hpmicro_rdc.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#include <hpm_common.h>
#include <hpm_rdc_drv.h>

BUILD_ASSERT((int)HPM_RDC_OUTPUT_DAC == (int)rdc_output_dac);
BUILD_ASSERT((int)HPM_RDC_OUTPUT_PWM == (int)rdc_output_pwm);
BUILD_ASSERT((int)HPM_RDC_RECTIFY_EXCITATION_0 == (int)rdc_rectify_signal_exc_0_ph);
BUILD_ASSERT((int)HPM_RDC_RECTIFY_EXCITATION_90 == (int)rdc_rectify_signal_exc_90_ph);
BUILD_ASSERT((int)HPM_RDC_RECTIFY_EXCITATION_180 == (int)rdc_rectify_signal_exc_180_ph);
BUILD_ASSERT((int)HPM_RDC_RECTIFY_EXCITATION_270 == (int)rdc_rectify_signal_exc_270_ph);
BUILD_ASSERT((int)HPM_RDC_RECTIFY_EXTERNAL == (int)rdc_rectify_signal_external);
BUILD_ASSERT((int)HPM_RDC_RECTIFY_EXTERNAL_INVERTED == (int)rdc_rectify_signal_external_invert);
BUILD_ASSERT((int)HPM_RDC_STAMP_END == (int)rdc_acc_stamp_end_of_acc);
BUILD_ASSERT((int)HPM_RDC_STAMP_START == (int)rdc_acc_stamp_start_of_acc);
BUILD_ASSERT((int)HPM_RDC_STAMP_CENTER == (int)rdc_acc_stamp_center_of_acc);
BUILD_ASSERT(HPM_RDC_EVENT_ACC_VALID_I == acc_vld_i_stat);
BUILD_ASSERT(HPM_RDC_EVENT_ACC_VALID_Q == acc_vld_q_stat);
BUILD_ASSERT(HPM_RDC_EVENT_RISING_DELAY_I == rising_delay_i_stat);
BUILD_ASSERT(HPM_RDC_EVENT_FALLING_DELAY_I == falling_delay_i_stat);
BUILD_ASSERT(HPM_RDC_EVENT_RISING_DELAY_Q == rising_delay_q_stat);
BUILD_ASSERT(HPM_RDC_EVENT_FALLING_DELAY_Q == falling_delay_q_stat);
BUILD_ASSERT(HPM_RDC_EVENT_SAMPLE_RISING_I == sample_rising_i_stat);
BUILD_ASSERT(HPM_RDC_EVENT_SAMPLE_FALLING_I == sample_falling_i_stat);
BUILD_ASSERT(HPM_RDC_EVENT_SAMPLE_RISING_Q == sample_rising_q_stat);
BUILD_ASSERT(HPM_RDC_EVENT_SAMPLE_FALLING_Q == sample_falling_q_stat);
BUILD_ASSERT(HPM_RDC_EVENT_ACC_OVERFLOW_I == acc_vld_i_ovh_stat);
BUILD_ASSERT(HPM_RDC_EVENT_ACC_OVERFLOW_Q == acc_vld_q_ovh_stat);
BUILD_ASSERT(HPM_RDC_EVENT_ACC_UNDERFLOW_I == acc_vld_i_ovl_stat);
BUILD_ASSERT(HPM_RDC_EVENT_ACC_UNDERFLOW_Q == acc_vld_q_ovl_stat);
BUILD_ASSERT(HPM_RDC_EVENT_AMPLITUDE_OVERFLOW == acc_amp_ovh_stat);
BUILD_ASSERT(HPM_RDC_EVENT_AMPLITUDE_UNDERFLOW == acc_amp_ovl_stat);

struct hpm_rdc_config {
	RDC_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	void (*irq_config_func)(const struct device *dev);
};

struct hpm_rdc_data {
	struct k_spinlock lock;
	uint32_t event_mask;
	hpm_rdc_event_callback_t callback;
	void *callback_user_data;
};

static bool hpm_rdc_channel_is_valid(enum hpm_rdc_channel channel)
{
	return channel <= HPM_RDC_CHANNEL_Q;
}

static bool hpm_rdc_trigger_is_valid(enum hpm_rdc_trigger trigger)
{
	return trigger <= HPM_RDC_TRIGGER_1;
}

static int hpm_rdc_drv_configure_output(const struct device *dev,
					const struct hpm_rdc_output_config *config)
{
	const struct hpm_rdc_config *rdc = dev->config;
	struct hpm_rdc_data *data = dev->data;
	rdc_output_cfg_t sdk_config = {0};
	k_spinlock_key_t key;

	if ((config == NULL) || (config->mode > HPM_RDC_OUTPUT_PWM) ||
	    (config->excitation_period_cycles == 0U) ||
	    (config->excitation_points > HPM_RDC_EXCITATION_1024_POINTS) ||
	    (config->pwm_period_samples > 15U)) {
		return -EINVAL;
	}

	sdk_config.mode = (rdc_output_mode_t)config->mode;
	sdk_config.excitation_period_cycle = config->excitation_period_cycles;
	sdk_config.excitation_precision = (rdc_output_precision_t)config->excitation_points;
	sdk_config.pwm_period = (rdc_output_pwm_period_t)config->pwm_period_samples;
	sdk_config.output_swap = config->output_swap;
	sdk_config.amp_offset = config->amplitude_offset;
	sdk_config.amp_man = config->amplitude_mantissa;
	sdk_config.amp_exp = config->amplitude_exponent;
	sdk_config.pwm_dither_enable = config->pwm_dither;
	sdk_config.pwm_exc_p_low_active = config->pwm_p_active_low;
	sdk_config.pwm_exc_n_low_active = config->pwm_n_active_low;
	sdk_config.trig_by_hw = config->hardware_trigger;
	sdk_config.hw_trig_delay = config->hardware_trigger_delay;
	sdk_config.dac_chn_i_sel = config->dac_channel_i;
	sdk_config.dac_chn_q_sel = config->dac_channel_q;
	sdk_config.pwm_deadzone_p = config->pwm_deadzone_p;
	sdk_config.pwm_deadzone_n = config->pwm_deadzone_n;

	key = k_spin_lock(&data->lock);
	rdc_output_config(rdc->base, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_rdc_drv_configure_input(const struct device *dev,
				       const struct hpm_rdc_input_config *config)
{
	const struct hpm_rdc_config *rdc = dev->config;
	struct hpm_rdc_data *data = dev->data;
	rdc_input_cfg_t sdk_config = {0};
	k_spinlock_key_t key;

	if ((config == NULL) || (config->rectify_signal > HPM_RDC_RECTIFY_EXTERNAL_INVERTED) ||
	    (config->stamp > HPM_RDC_STAMP_CENTER) || (config->adc_channel_i > 15U) ||
	    (config->adc_channel_q > 15U) || (config->adc_port_i > 1U) ||
	    (config->adc_port_q > 1U)) {
		return -EINVAL;
	}

	sdk_config.rectify_signal_sel = (rdc_rectify_signal_t)config->rectify_signal;
	sdk_config.acc_cycle_len = config->accumulation_cycles;
	sdk_config.acc_stamp = (rdc_acc_stamp_time_t)config->stamp;
	sdk_config.acc_input_chn_i = config->adc_channel_i;
	sdk_config.acc_input_port_i = config->adc_port_i;
	sdk_config.acc_input_chn_q = config->adc_channel_q;
	sdk_config.acc_input_port_q = config->adc_port_q;

	key = k_spin_lock(&data->lock);
	rdc_input_config(rdc->base, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_rdc_drv_configure_accumulation(const struct device *dev,
					      const struct hpm_rdc_accumulation_config *config)
{
	const struct hpm_rdc_config *rdc = dev->config;
	struct hpm_rdc_data *data = dev->data;
	rdc_acc_cfg_t sdk_config = {0};
	k_spinlock_key_t key;

	if ((config == NULL) || (config->filter_edges > 7U) || (config->edge_distance > 63U) ||
	    (config->right_shift > 31U) || (config->amplitude_min > config->amplitude_max)) {
		return -EINVAL;
	}

	sdk_config.continue_edge_num = config->filter_edges;
	sdk_config.edge_distance = config->edge_distance;
	sdk_config.right_shift_without_sign = config->right_shift;
	sdk_config.error_data_remove = config->discard_invalid;
	sdk_config.exc_carrier_period = config->excitation_carrier_period;
	sdk_config.sync_delay_i = config->sync_delay_i;
	sdk_config.sync_delay_q = config->sync_delay_q;
	sdk_config.amp_max = config->amplitude_max;
	sdk_config.amp_min = config->amplitude_min;

	key = k_spin_lock(&data->lock);
	rdc_set_acc_config(rdc->base, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_rdc_drv_set_channel_threshold(const struct device *dev, enum hpm_rdc_channel channel,
					     int32_t threshold)
{
	const struct hpm_rdc_config *rdc = dev->config;
	struct hpm_rdc_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_rdc_channel_is_valid(channel)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	rdc_set_edge_detection_offset(rdc->base, (rdc_input_acc_chn_t)channel, threshold);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_rdc_drv_set_channel_sync_delay(const struct device *dev,
					      enum hpm_rdc_channel channel, uint32_t delay)
{
	const struct hpm_rdc_config *rdc = dev->config;
	struct hpm_rdc_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_rdc_channel_is_valid(channel)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	rdc_set_acc_sync_delay(rdc->base, (rdc_input_acc_chn_t)channel, delay);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_rdc_drv_configure_trigger(const struct device *dev, enum hpm_rdc_trigger trigger,
					 int32_t offset, bool enable)
{
	const struct hpm_rdc_config *rdc = dev->config;
	struct hpm_rdc_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_rdc_trigger_is_valid(trigger)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	rdc_output_trig_offset_config(rdc->base, (rdc_output_trig_chn_t)trigger, offset);
	if (enable) {
		rdc_output_trig_enable(rdc->base, (rdc_output_trig_chn_t)trigger);
	} else {
		rdc_output_trig_disable(rdc->base, (rdc_output_trig_chn_t)trigger);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_rdc_drv_excitation_enable(const struct device *dev, bool enable)
{
	const struct hpm_rdc_config *rdc = dev->config;
	struct hpm_rdc_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	if (enable) {
		rdc_exc_enable(rdc->base);
	} else {
		rdc_exc_disable(rdc->base);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_rdc_drv_software_trigger(const struct device *dev)
{
	const struct hpm_rdc_config *rdc = dev->config;
	struct hpm_rdc_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	rdc_output_trig_sw(rdc->base);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_rdc_drv_accumulation_enable(const struct device *dev, bool enable)
{
	const struct hpm_rdc_config *rdc = dev->config;
	struct hpm_rdc_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	if (enable) {
		rdc_acc_enable(rdc->base);
	} else {
		rdc_acc_disable(rdc->base);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_rdc_drv_get_result(const struct device *dev, struct hpm_rdc_result *result)
{
	const struct hpm_rdc_config *rdc = dev->config;
	struct hpm_rdc_data *data = dev->data;
	k_spinlock_key_t key;

	if (result == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	result->i = rdc_get_acc_avl(rdc->base, rdc_acc_chn_i);
	result->q = rdc_get_acc_avl(rdc->base, rdc_acc_chn_q);
	result->max_i = rdc_get_i_maxval(rdc->base);
	result->min_i = rdc_get_i_minval(rdc->base);
	result->max_q = rdc_get_q_maxval(rdc->base);
	result->min_q = rdc_get_q_minval(rdc->base);
	result->rise_delay_i = rdc_get_rise_delay_i(rdc->base);
	result->fall_delay_i = rdc_get_fall_delay_i(rdc->base);
	result->rise_delay_q = rdc_get_rise_delay_q(rdc->base);
	result->fall_delay_q = rdc_get_fall_delay_q(rdc->base);
	result->interrupt_status = get_interrupt_status(rdc->base);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_rdc_drv_clear_extrema(const struct device *dev)
{
	const struct hpm_rdc_config *rdc = dev->config;
	struct hpm_rdc_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	rdc_clear_i_maxval(rdc->base);
	rdc_clear_i_minval(rdc->base);
	rdc_clear_q_maxval(rdc->base);
	rdc_clear_q_minval(rdc->base);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_rdc_drv_get_events(const struct device *dev, uint32_t *events)
{
	const struct hpm_rdc_config *rdc = dev->config;
	struct hpm_rdc_data *data = dev->data;
	k_spinlock_key_t key;

	if (events == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	*events = get_interrupt_status(rdc->base) & HPM_RDC_EVENT_ALL;
	rdc->base->ADC_INT_STATE = *events;
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_rdc_drv_clear_events(const struct device *dev, uint32_t events)
{
	const struct hpm_rdc_config *rdc = dev->config;
	struct hpm_rdc_data *data = dev->data;
	k_spinlock_key_t key;

	if ((events & ~HPM_RDC_EVENT_ALL) != 0U) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	rdc->base->ADC_INT_STATE = events;
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_rdc_drv_set_event_callback(const struct device *dev, uint32_t event_mask,
					  hpm_rdc_event_callback_t callback, void *user_data)
{
	const struct hpm_rdc_config *rdc = dev->config;
	struct hpm_rdc_data *data = dev->data;
	k_spinlock_key_t key;

	if ((event_mask & ~HPM_RDC_EVENT_ALL) != 0U) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	rdc_interrupt_reset_config(rdc->base, HPM_RDC_EVENT_ALL);
	rdc_interrupt_disable(rdc->base);
	rdc->base->ADC_INT_STATE = HPM_RDC_EVENT_ALL;
	data->event_mask = event_mask;
	data->callback = callback;
	data->callback_user_data = user_data;
	if ((callback != NULL) && (event_mask != 0U)) {
		rdc_interrupt_config(rdc->base, event_mask);
		rdc_interrupt_enable(rdc->base);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static void hpm_rdc_isr(const struct device *dev)
{
	const struct hpm_rdc_config *rdc = dev->config;
	struct hpm_rdc_data *data = dev->data;
	hpm_rdc_event_callback_t callback;
	void *user_data;
	k_spinlock_key_t key;
	uint32_t events;

	key = k_spin_lock(&data->lock);
	events = get_interrupt_status(rdc->base) & data->event_mask;
	if (events != 0U) {
		rdc->base->ADC_INT_STATE = events;
	}
	callback = data->callback;
	user_data = data->callback_user_data;
	k_spin_unlock(&data->lock, key);

	if ((callback != NULL) && (events != 0U)) {
		callback(dev, events, user_data);
	}
}

static DEVICE_API(hpm_rdc, hpm_rdc_api) = {
	.configure_output = hpm_rdc_drv_configure_output,
	.configure_input = hpm_rdc_drv_configure_input,
	.configure_accumulation = hpm_rdc_drv_configure_accumulation,
	.set_channel_threshold = hpm_rdc_drv_set_channel_threshold,
	.set_channel_sync_delay = hpm_rdc_drv_set_channel_sync_delay,
	.configure_trigger = hpm_rdc_drv_configure_trigger,
	.excitation_enable = hpm_rdc_drv_excitation_enable,
	.software_trigger = hpm_rdc_drv_software_trigger,
	.accumulation_enable = hpm_rdc_drv_accumulation_enable,
	.get_result = hpm_rdc_drv_get_result,
	.clear_extrema = hpm_rdc_drv_clear_extrema,
	.get_events = hpm_rdc_drv_get_events,
	.clear_events = hpm_rdc_drv_clear_events,
	.set_event_callback = hpm_rdc_drv_set_event_callback,
};

static int hpm_rdc_init(const struct device *dev)
{
	const struct hpm_rdc_config *config = dev->config;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		return ret;
	}

	config->irq_config_func(dev);

	return 0;
}

#define HPM_RDC_IRQ_CONFIG(n)                                                                      \
	static void hpm_rdc_irq_config_##n(const struct device *dev)                               \
	{                                                                                          \
		ARG_UNUSED(dev);                                                                   \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_rdc_isr,                \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}

#define HPM_RDC_INIT(n)                                                                            \
	HPM_RDC_IRQ_CONFIG(n)                                                                      \
	static const struct hpm_rdc_config hpm_rdc_config_##n = {                                  \
		.base = (RDC_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.irq_config_func = hpm_rdc_irq_config_##n,                                         \
	};                                                                                         \
                                                                                                   \
	static struct hpm_rdc_data hpm_rdc_data_##n;                                               \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_rdc_init, NULL, &hpm_rdc_data_##n, &hpm_rdc_config_##n,       \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &hpm_rdc_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_RDC_INIT)
