/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_pwm

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <drivers/pwm/hpmicro_pwm.h>

#include <hpm_clock_drv.h>
#include <hpm_pwm_drv.h>

LOG_MODULE_REGISTER(pwm_hpmicro, CONFIG_PWM_LOG_LEVEL);

#if defined(PWM_SOC_PWM_MAX_COUNT)
#define HPM_PWM_CHANNELS PWM_SOC_PWM_MAX_COUNT
#else
#define HPM_PWM_CHANNELS 8U
#endif

#define HPM_PWM_MAX_PERIOD          (PWM_RLD_RLD_MASK >> PWM_RLD_RLD_SHIFT)
#define HPM_PWM_CAPTURE_MAX_XRLD    (PWM_RLD_XRLD_MASK >> PWM_RLD_XRLD_SHIFT)
#define HPM_PWM_CAPTURE_COUNT_SHIFT PWM_CNT_CNT_SHIFT

struct hpm_pwm_config {
	PWM_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	const struct pinctrl_dev_config *pincfg;
#ifdef CONFIG_PWM_CAPTURE
	void (*irq_config_func)(const struct device *dev);
#endif
};

struct hpm_pwm_data {
	struct k_mutex lock;
	uint32_t frequency;
	bool output_configured;
#ifdef CONFIG_PWM_CAPTURE
	pwm_capture_callback_handler_t capture_callback;
	void *capture_user_data;
	uint32_t capture_last_same_edge;
	pwm_flags_t capture_flags;
	uint8_t capture_channel;
	atomic_t capture_active;
	bool capture_configured;
	bool capture_have_edge;
#endif
};

static bool hpm_pwm_capture_is_active(const struct hpm_pwm_data *data)
{
#ifdef CONFIG_PWM_CAPTURE
	return atomic_get(&data->capture_active) != 0;
#else
	ARG_UNUSED(data);
	return false;
#endif
}

static int hpm_pwm_set_cycles(const struct device *dev, uint32_t channel, uint32_t period_cycles,
			      uint32_t pulse_cycles, pwm_flags_t flags)
{
	const struct hpm_pwm_config *config = dev->config;
	struct hpm_pwm_data *data = dev->data;
	pwm_cmp_config_t cmp_config;
	pwm_config_t pwm_config;
	uint32_t reload;
	uint32_t compare;
	bool inverted;
	int ret = 0;

	if (channel >= HPM_PWM_CHANNELS) {
		return -EINVAL;
	}

	if ((flags & ~PWM_POLARITY_MASK) != 0U) {
		return -ENOTSUP;
	}

	if ((period_cycles == 0U) || (pulse_cycles > period_cycles)) {
		return -EINVAL;
	}

	/*
	 * A zero pulse is encoded as a compare value beyond the reload value.
	 * Keep one count available for that encoding. A full pulse compares at
	 * zero, matching the SDK's edge-aligned waveform convention.
	 */
	if (period_cycles > HPM_PWM_MAX_PERIOD) {
		return -ENOTSUP;
	}

	reload = period_cycles - 1U;
	inverted = (flags & PWM_POLARITY_INVERTED) != 0U;
	compare = pulse_cycles == 0U ? period_cycles : period_cycles - pulse_cycles;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (hpm_pwm_capture_is_active(data)) {
		ret = -EBUSY;
		goto out;
	}
	data->output_configured = true;

	pwm_stop_counter(config->base);
	pwm_set_start_count(config->base, 0U, 0U);
	pwm_set_reload(config->base, 0U, reload);

	pwm_get_default_pwm_config(config->base, &pwm_config);
	pwm_config.enable_output = true;
	pwm_config.invert_output = inverted;

	pwm_get_default_cmp_config(config->base, &cmp_config);
	cmp_config.cmp = compare;
	cmp_config.mode = pwm_cmp_mode_output_compare;
	cmp_config.update_trigger = pwm_shadow_register_update_on_modify;

	if (pwm_setup_waveform(config->base, channel, &pwm_config, channel, &cmp_config, 1U) !=
	    status_success) {
		ret = -EIO;
		goto out;
	}

	pwm_issue_shadow_register_lock_event(config->base);
	pwm_start_counter(config->base);

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int hpm_pwm_set_complementary_impl(const struct device *dev, uint32_t channel,
					  uint32_t period_cycles, uint32_t pulse_cycles,
					  pwm_flags_t flags, uint32_t dead_zone_half_cycles)
{
	const struct hpm_pwm_config *config = dev->config;
	struct hpm_pwm_data *data = dev->data;
	pwm_cmp_config_t cmp_config[2];
	pwm_pair_config_t pair_config;
	uint32_t reload;
	uint32_t compare;
	bool inverted;
	int ret = 0;

	if ((channel >= HPM_PWM_CHANNELS) || ((channel & 1U) != 0U)) {
		return -EINVAL;
	}

	if ((flags & ~PWM_POLARITY_MASK) != 0U) {
		return -ENOTSUP;
	}

	if ((period_cycles == 0U) || (pulse_cycles > period_cycles) ||
	    (period_cycles > HPM_PWM_MAX_PERIOD) ||
	    (dead_zone_half_cycles > HPM_PWM_DEAD_ZONE_MAX)) {
		return -EINVAL;
	}

	reload = period_cycles - 1U;
	compare = pulse_cycles == 0U ? period_cycles : period_cycles - pulse_cycles;
	inverted = (flags & PWM_POLARITY_INVERTED) != 0U;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (hpm_pwm_capture_is_active(data)) {
		ret = -EBUSY;
		goto out;
	}
	data->output_configured = true;

	pwm_stop_counter(config->base);
	pwm_set_start_count(config->base, 0U, 0U);
	pwm_set_reload(config->base, 0U, reload);

	pwm_get_default_cmp_config(config->base, &cmp_config[0]);
	cmp_config[0].cmp = compare;
	cmp_config[0].mode = pwm_cmp_mode_output_compare;
	cmp_config[0].update_trigger = pwm_shadow_register_update_on_modify;
	cmp_config[1] = cmp_config[0];

	pwm_get_default_pwm_pair_config(config->base, &pair_config);
	pair_config.pwm[0].enable_output = true;
	pair_config.pwm[0].invert_output = inverted;
	pair_config.pwm[0].dead_zone_in_half_cycle = dead_zone_half_cycles;
	pair_config.pwm[1].enable_output = true;
	pair_config.pwm[1].invert_output = inverted;
	pair_config.pwm[1].dead_zone_in_half_cycle = dead_zone_half_cycles;

	if (pwm_setup_waveform_in_pair(config->base, channel, &pair_config, channel, cmp_config,
				       ARRAY_SIZE(cmp_config)) != status_success) {
		ret = -EIO;
		goto out;
	}

	pwm_issue_shadow_register_lock_event(config->base);
	pwm_start_counter(config->base);

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int hpm_pwm_set_reference_cycles_impl(const struct device *dev, uint8_t comparator,
					     uint8_t reference, uint32_t period_cycles,
					     uint32_t pulse_cycles, pwm_flags_t flags)
{
	const struct hpm_pwm_config *config = dev->config;
	struct hpm_pwm_data *data = dev->data;
	pwm_output_channel_t ch_config;
	pwm_cmp_config_t cmp_config;
	uint32_t reload;
	uint32_t compare;

	if ((comparator >= HPM_PWM_COMPARATOR_COUNT) ||
	    (reference < HPM_PWM_REFERENCE_OUTPUT_FIRST) ||
	    (reference > HPM_PWM_REFERENCE_OUTPUT_LAST) || (period_cycles == 0U) ||
	    (pulse_cycles > period_cycles) || (period_cycles > HPM_PWM_MAX_PERIOD)) {
		return -EINVAL;
	}
	if ((flags & ~PWM_POLARITY_MASK) != 0U) {
		return -ENOTSUP;
	}

	reload = period_cycles - 1U;
	compare = pulse_cycles == 0U ? period_cycles : period_cycles - pulse_cycles;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (hpm_pwm_capture_is_active(data)) {
		k_mutex_unlock(&data->lock);
		return -EBUSY;
	}
	data->output_configured = true;
	pwm_stop_counter(config->base);
	pwm_set_start_count(config->base, 0U, 0U);
	pwm_set_reload(config->base, 0U, reload);

	pwm_get_default_cmp_config(config->base, &cmp_config);
	cmp_config.cmp = compare;
	cmp_config.mode = pwm_cmp_mode_output_compare;
	cmp_config.update_trigger = pwm_shadow_register_update_on_modify;
	pwm_config_cmp(config->base, comparator, &cmp_config);

	pwm_get_default_output_channel_config(config->base, &ch_config);
	ch_config.cmp_start_index = comparator;
	ch_config.cmp_end_index = comparator;
	ch_config.invert_output = (flags & PWM_POLARITY_INVERTED) != 0U;
	pwm_config_output_channel(config->base, reference, &ch_config);

	pwm_issue_shadow_register_lock_event(config->base);
	pwm_start_counter(config->base);
	k_mutex_unlock(&data->lock);

	return 0;
}

static int hpm_pwm_configure_fault_impl(const struct device *dev,
					const struct hpm_pwm_fault_config *config)
{
	const struct hpm_pwm_config *pwm = dev->config;
	struct hpm_pwm_data *data = dev->data;
	pwm_fault_source_config_t source = {0};
	uint32_t source_mask;
	pwm_config_t output_config;
	uint32_t channel;

	if ((config == NULL) ||
	    ((config->source_mask &
	      ~(HPM_PWM_FAULT_SOURCE_INTERNAL0 | HPM_PWM_FAULT_SOURCE_INTERNAL1 |
		HPM_PWM_FAULT_SOURCE_INTERNAL2 | HPM_PWM_FAULT_SOURCE_INTERNAL3 |
		HPM_PWM_FAULT_SOURCE_EXTERNAL0 | HPM_PWM_FAULT_SOURCE_EXTERNAL1 |
		HPM_PWM_FAULT_SOURCE_DEBUG)) != 0U) ||
	    (config->output_mode > HPM_PWM_FAULT_FORCE_HIGH_Z) ||
	    (config->recovery > HPM_PWM_FAULT_RECOVER_ON_CLEAR)) {
		return -EINVAL;
	}

	source_mask = config->source_mask;
	source.source_mask = 0U;
	if ((source_mask & HPM_PWM_FAULT_SOURCE_INTERNAL0) != 0U) {
		source.source_mask |= pwm_fault_source_internal_0;
	}
	if ((source_mask & HPM_PWM_FAULT_SOURCE_INTERNAL1) != 0U) {
		source.source_mask |= pwm_fault_source_internal_1;
	}
	if ((source_mask & HPM_PWM_FAULT_SOURCE_INTERNAL2) != 0U) {
		source.source_mask |= pwm_fault_source_internal_2;
	}
	if ((source_mask & HPM_PWM_FAULT_SOURCE_INTERNAL3) != 0U) {
		source.source_mask |= pwm_fault_source_internal_3;
	}
	if ((source_mask & HPM_PWM_FAULT_SOURCE_EXTERNAL0) != 0U) {
		source.source_mask |= pwm_fault_source_external_0;
	}
	if ((source_mask & HPM_PWM_FAULT_SOURCE_EXTERNAL1) != 0U) {
		source.source_mask |= pwm_fault_source_external_1;
	}
	if ((source_mask & HPM_PWM_FAULT_SOURCE_DEBUG) != 0U) {
		source.source_mask |= pwm_fault_source_debug;
	}

	source.fault_recover_at_rising_edge = config->recover_at_rising_edge;
	source.fault_external_0_active_low = config->external0_active_low;
	source.fault_external_1_active_low = config->external1_active_low;
	source.fault_output_recovery_trigger = config->recovery;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (hpm_pwm_capture_is_active(data)) {
		k_mutex_unlock(&data->lock);
		return -EBUSY;
	}
	data->output_configured = true;
	pwm_config_fault_source(pwm->base, &source);

	for (channel = 0U; channel < HPM_PWM_CHANNELS; channel++) {
		output_config.enable_output = true;
		output_config.invert_output = false;
		output_config.force_cmd_shadow_update_trigger =
			pwm_shadow_register_update_on_modify;
		output_config.fault_mode = config->output_mode;
		output_config.fault_recovery_trigger = config->recovery;
		output_config.force_source = pwm_force_source_software;
		output_config.dead_zone_in_half_cycle =
			PWM_PWMCFG_DEADAREA_GET(pwm->base->PWMCFG[channel]);
		pwm_config_pwm(pwm->base, channel, &output_config,
			       PWM_PWMCFG_PAIR_GET(pwm->base->PWMCFG[channel]) != 0U);
	}
	k_mutex_unlock(&data->lock);

	return 0;
}

static int hpm_pwm_clear_fault_impl(const struct device *dev)
{
	const struct hpm_pwm_config *config = dev->config;
	struct hpm_pwm_data *data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	pwm_clear_fault(config->base);
	k_mutex_unlock(&data->lock);

	return 0;
}

#ifdef CONFIG_PWM_CAPTURE
static void hpm_pwm_capture_isr(const struct device *dev)
{
	const struct hpm_pwm_config *config = dev->config;
	struct hpm_pwm_data *data = dev->data;
	uint32_t mask = PWM_IRQ_CMP(data->capture_channel);
	uint32_t status = pwm_get_status(config->base);
	pwm_capture_callback_handler_t callback;
	void *user_data;
	uint32_t same_edge;
	uint32_t opposite_edge;
	uint32_t period = 0U;
	uint32_t pulse = 0U;
	uint32_t edge;

	if (((status & mask) == 0U) || (atomic_get(&data->capture_active) == 0)) {
		return;
	}

	pwm_clear_status(config->base, mask);
	edge = (data->capture_flags & PWM_POLARITY_MASK) == PWM_POLARITY_INVERTED
		       ? pwm_counter_type_capture_falling_edge
		       : pwm_counter_type_capture_rising_edge;
	pwm_get_captured_count(config->base, &same_edge, edge, data->capture_channel, 1U);
	pwm_get_captured_count(config->base, &opposite_edge, !edge, data->capture_channel, 1U);

	if (!data->capture_have_edge) {
		data->capture_last_same_edge = same_edge;
		data->capture_have_edge = true;
		return;
	}

	/* Subtract raw encodings before shifting to handle a full-counter wrap. */
	if ((data->capture_flags & PWM_CAPTURE_TYPE_PERIOD) != 0U) {
		period = (same_edge - data->capture_last_same_edge) >> HPM_PWM_CAPTURE_COUNT_SHIFT;
	}
	if ((data->capture_flags & PWM_CAPTURE_TYPE_PULSE) != 0U) {
		pulse = (opposite_edge - data->capture_last_same_edge) >>
			HPM_PWM_CAPTURE_COUNT_SHIFT;
	}
	data->capture_last_same_edge = same_edge;

	callback = data->capture_callback;
	user_data = data->capture_user_data;
	if ((data->capture_flags & PWM_CAPTURE_MODE_MASK) == PWM_CAPTURE_MODE_SINGLE) {
		pwm_disable_irq(config->base, mask);
		pwm_stop_counter(config->base);
		atomic_clear(&data->capture_active);
	}
	if (callback != NULL) {
		callback(dev, data->capture_channel, period, pulse, 0, user_data);
	}
}

static int hpm_pwm_configure_capture_api_impl(const struct device *dev, uint32_t channel,
					      pwm_flags_t flags,
					      pwm_capture_callback_handler_t callback,
					      void *user_data)
{
	const struct hpm_pwm_config *config = dev->config;
	struct hpm_pwm_data *data = dev->data;
	pwm_cmp_config_t compare = {0};
	uint32_t irq_mask;
	pwm_flags_t type = flags & PWM_CAPTURE_TYPE_MASK;

	if ((channel >= HPM_PWM_COMPARATOR_COUNT) ||
	    ((type != PWM_CAPTURE_TYPE_PERIOD) && (type != PWM_CAPTURE_TYPE_PULSE) &&
	     (type != PWM_CAPTURE_TYPE_BOTH)) ||
	    ((flags & ~(PWM_POLARITY_MASK | PWM_CAPTURE_TYPE_MASK | PWM_CAPTURE_MODE_MASK)) !=
	     0U)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	if (atomic_get(&data->capture_active) != 0) {
		k_mutex_unlock(&data->lock);
		return -EBUSY;
	}
	if ((callback != NULL) && data->output_configured) {
		k_mutex_unlock(&data->lock);
		return -EBUSY;
	}

	/* NULL is used by the blocking helpers to release their stack callback. */
	if (callback != NULL) {
		irq_mask = PWM_IRQ_CMP(channel);
		pwm_disable_irq(config->base, irq_mask);
		pwm_clear_status(config->base, irq_mask);
		pwm_get_default_cmp_config(config->base, &compare);
		compare.mode = pwm_cmp_mode_input_capture;
		pwm_config_cmp(config->base, channel, &compare);
		if ((flags & PWM_POLARITY_MASK) == PWM_POLARITY_INVERTED) {
			pwm_set_capture_irq_falling_edge(config->base, irq_mask);
		} else {
			pwm_set_capture_irq_rising_edge(config->base, irq_mask);
		}
	}

	data->capture_callback = callback;
	data->capture_user_data = user_data;
	data->capture_flags = flags;
	data->capture_channel = (uint8_t)channel;
	data->capture_configured = true;
	data->capture_have_edge = false;
	k_mutex_unlock(&data->lock);

	return 0;
}

static int hpm_pwm_enable_capture_impl(const struct device *dev, uint32_t channel)
{
	const struct hpm_pwm_config *config = dev->config;
	struct hpm_pwm_data *data = dev->data;
	uint32_t irq_mask;

	if (channel >= HPM_PWM_COMPARATOR_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	if (!data->capture_configured || (data->capture_channel != channel) ||
	    (data->capture_callback == NULL)) {
		k_mutex_unlock(&data->lock);
		return -EINVAL;
	}
	if (atomic_get(&data->capture_active) != 0) {
		k_mutex_unlock(&data->lock);
		return -EBUSY;
	}
	if (data->output_configured) {
		k_mutex_unlock(&data->lock);
		return -EBUSY;
	}

	irq_mask = PWM_IRQ_CMP(channel);
	/* Full 28-bit free-running timebase; raw captures have four reserved low bits. */
	pwm_stop_counter(config->base);
	pwm_set_start_count(config->base, 0U, 0U);
	pwm_set_reload(config->base, HPM_PWM_CAPTURE_MAX_XRLD, HPM_PWM_MAX_PERIOD);
	pwm_issue_shadow_register_lock_event(config->base);
	if ((data->capture_flags & PWM_POLARITY_MASK) == PWM_POLARITY_INVERTED) {
		pwm_set_capture_irq_falling_edge(config->base, irq_mask);
	} else {
		pwm_set_capture_irq_rising_edge(config->base, irq_mask);
	}
	pwm_clear_status(config->base, irq_mask);
	data->capture_have_edge = false;
	atomic_set(&data->capture_active, 1);
	pwm_start_counter(config->base);
	pwm_enable_irq(config->base, irq_mask);
	k_mutex_unlock(&data->lock);

	return 0;
}

static int hpm_pwm_disable_capture_impl(const struct device *dev, uint32_t channel)
{
	const struct hpm_pwm_config *config = dev->config;
	struct hpm_pwm_data *data = dev->data;
	uint32_t irq_mask;
	bool was_active;

	if (channel >= HPM_PWM_COMPARATOR_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	if (!data->capture_configured || (data->capture_channel != channel)) {
		k_mutex_unlock(&data->lock);
		return -EINVAL;
	}

	irq_mask = PWM_IRQ_CMP(channel);
	was_active = atomic_clear(&data->capture_active) != 0;
	pwm_disable_irq(config->base, irq_mask);
	pwm_clear_status(config->base, irq_mask);
	if (was_active) {
		pwm_stop_counter(config->base);
	}
	k_mutex_unlock(&data->lock);

	return 0;
}
#endif /* CONFIG_PWM_CAPTURE */

static int hpm_pwm_configure_capture_impl(const struct device *dev, uint8_t comparator,
					  enum hpm_pwm_capture_edge edge)
{
	const struct hpm_pwm_config *config = dev->config;
	struct hpm_pwm_data *data = dev->data;
	pwm_cmp_config_t cmp_config;
	uint32_t irq_mask;

	if ((comparator >= HPM_PWM_COMPARATOR_COUNT) ||
	    ((edge != HPM_PWM_CAPTURE_RISING) && (edge != HPM_PWM_CAPTURE_FALLING))) {
		return -EINVAL;
	}

	irq_mask = PWM_IRQ_CMP(comparator);

	k_mutex_lock(&data->lock, K_FOREVER);
	if (hpm_pwm_capture_is_active(data)) {
		k_mutex_unlock(&data->lock);
		return -EBUSY;
	}
	pwm_get_default_cmp_config(config->base, &cmp_config);
	cmp_config.mode = pwm_cmp_mode_input_capture;
	pwm_config_cmp(config->base, comparator, &cmp_config);

	if (edge == HPM_PWM_CAPTURE_FALLING) {
		pwm_set_capture_irq_falling_edge(config->base, irq_mask);
	} else {
		pwm_set_capture_irq_rising_edge(config->base, irq_mask);
	}

	pwm_clear_status(config->base, irq_mask);
	k_mutex_unlock(&data->lock);

	return 0;
}

static int hpm_pwm_get_capture_impl(const struct device *dev, uint8_t comparator, uint32_t *capture)
{
	const struct hpm_pwm_config *config = dev->config;
	struct hpm_pwm_data *data = dev->data;

	if ((comparator >= HPM_PWM_COMPARATOR_COUNT) || (capture == NULL)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	/* The raw extension returns the latest rising-edge timestamp. */
	pwm_get_captured_count(config->base, capture, pwm_counter_type_capture_rising_edge,
			       comparator, 1U);
	k_mutex_unlock(&data->lock);

	return 0;
}

static int hpm_pwm_get_cycles_per_sec(const struct device *dev, uint32_t channel, uint64_t *cycles)
{
	struct hpm_pwm_data *data = dev->data;
	uint32_t channels =
		IS_ENABLED(CONFIG_PWM_CAPTURE) ? HPM_PWM_COMPARATOR_COUNT : HPM_PWM_CHANNELS;

	if (channel >= channels) {
		return -EINVAL;
	}

	if (cycles == NULL) {
		return -EINVAL;
	}

	*cycles = data->frequency;

	return 0;
}

static void hpm_pwm_init_data(struct hpm_pwm_data *data)
{
	k_mutex_init(&data->lock);
	data->output_configured = false;
#ifdef CONFIG_PWM_CAPTURE
	atomic_clear(&data->capture_active);
	data->capture_configured = false;
#endif
}

#ifdef CONFIG_PWM_CAPTURE
#define HPM_PWM_CAPTURE_API_FIELDS                                                                 \
	.configure_capture = hpm_pwm_configure_capture_api_impl,                                   \
	.enable_capture = hpm_pwm_enable_capture_impl,                                             \
	.disable_capture = hpm_pwm_disable_capture_impl,
#else
#define HPM_PWM_CAPTURE_API_FIELDS
#endif

static DEVICE_API(hpm_pwm, hpm_pwm_api) = {
	.pwm_api = {.set_cycles = hpm_pwm_set_cycles,
		    .get_cycles_per_sec = hpm_pwm_get_cycles_per_sec,
		    HPM_PWM_CAPTURE_API_FIELDS},
	.set_complementary = hpm_pwm_set_complementary_impl,
	.configure_fault = hpm_pwm_configure_fault_impl,
	.clear_fault = hpm_pwm_clear_fault_impl,
	.configure_capture = hpm_pwm_configure_capture_impl,
	.get_capture = hpm_pwm_get_capture_impl,
	.set_reference_cycles = hpm_pwm_set_reference_cycles_impl,
};

#ifdef CONFIG_PWM_CAPTURE
#define HPM_PWM_IRQ_CONFIG(n)                                                                      \
	static void hpm_pwm_irq_config_##n(const struct device *dev)                               \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_pwm_capture_isr,        \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}
#define HPM_PWM_IRQ_CONFIG_FIELD(n) .irq_config_func = hpm_pwm_irq_config_##n,
#define HPM_PWM_IRQ_INIT(dev, config)                                                              \
	do {                                                                                       \
		(config)->irq_config_func(dev);                                                    \
	} while (false)
#else
#define HPM_PWM_IRQ_CONFIG(n)
#define HPM_PWM_IRQ_CONFIG_FIELD(n)
#define HPM_PWM_IRQ_INIT(dev, config)
#endif

#define HPM_PWM_INIT(n)                                                                            \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	HPM_PWM_IRQ_CONFIG(n)                                                                      \
                                                                                                   \
	static struct hpm_pwm_data hpm_pwm_data_##n;                                               \
                                                                                                   \
	static const struct hpm_pwm_config hpm_pwm_config_##n = {                                  \
		.base = (PWM_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
		HPM_PWM_IRQ_CONFIG_FIELD(n)};                                                      \
                                                                                                   \
	static int hpm_pwm_init_##n(const struct device *dev)                                      \
	{                                                                                          \
		const struct hpm_pwm_config *config = dev->config;                                 \
		struct hpm_pwm_data *data = dev->data;                                             \
		int ret;                                                                           \
                                                                                                   \
		if (!device_is_ready(config->clock_dev)) {                                         \
			return -ENODEV;                                                            \
		}                                                                                  \
                                                                                                   \
		ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);                  \
		if (ret < 0) {                                                                     \
			return ret;                                                                \
		}                                                                                  \
                                                                                                   \
		ret = clock_control_on(config->clock_dev, config->clock_subsys);                   \
		if (ret < 0) {                                                                     \
			return ret;                                                                \
		}                                                                                  \
                                                                                                   \
		ret = clock_control_get_rate(config->clock_dev, config->clock_subsys,              \
					     &data->frequency);                                    \
		if (ret < 0) {                                                                     \
			return ret;                                                                \
		}                                                                                  \
                                                                                                   \
		hpm_pwm_init_data(data);                                                           \
		pwm_stop_counter(config->base);                                                    \
		pwm_deinit(config->base);                                                          \
		HPM_PWM_IRQ_INIT(dev, config);                                                     \
                                                                                                   \
		return 0;                                                                          \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_pwm_init_##n, NULL, &hpm_pwm_data_##n, &hpm_pwm_config_##n,   \
			      POST_KERNEL, CONFIG_PWM_INIT_PRIORITY, &hpm_pwm_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_PWM_INIT)
