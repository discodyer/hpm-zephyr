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
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hpm_clock_drv.h>
#include <hpm_pwm_drv.h>

LOG_MODULE_REGISTER(pwm_hpmicro, CONFIG_PWM_LOG_LEVEL);

#if defined(PWM_SOC_PWM_MAX_COUNT)
#define HPM_PWM_CHANNELS PWM_SOC_PWM_MAX_COUNT
#else
#define HPM_PWM_CHANNELS 8U
#endif

#define HPM_PWM_MAX_PERIOD (PWM_RLD_RLD_MASK >> PWM_RLD_RLD_SHIFT)

struct hpm_pwm_config {
	PWM_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	const struct pinctrl_dev_config *pincfg;
};

struct hpm_pwm_data {
	struct k_mutex lock;
	uint32_t frequency;
};

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

static int hpm_pwm_get_cycles_per_sec(const struct device *dev, uint32_t channel, uint64_t *cycles)
{
	struct hpm_pwm_data *data = dev->data;

	if (channel >= HPM_PWM_CHANNELS) {
		return -EINVAL;
	}

	if (cycles == NULL) {
		return -EINVAL;
	}

	*cycles = data->frequency;

	return 0;
}

static DEVICE_API(pwm, hpm_pwm_api) = {
	.set_cycles = hpm_pwm_set_cycles,
	.get_cycles_per_sec = hpm_pwm_get_cycles_per_sec,
};

#define HPM_PWM_INIT(n)                                                                            \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
                                                                                                   \
	static struct hpm_pwm_data hpm_pwm_data_##n;                                               \
                                                                                                   \
	static const struct hpm_pwm_config hpm_pwm_config_##n = {                                  \
		.base = (PWM_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
	};                                                                                         \
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
		k_mutex_init(&data->lock);                                                         \
		pwm_stop_counter(config->base);                                                    \
		pwm_deinit(config->base);                                                          \
                                                                                                   \
		return 0;                                                                          \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_pwm_init_##n, NULL, &hpm_pwm_data_##n, &hpm_pwm_config_##n,   \
			      POST_KERNEL, CONFIG_PWM_INIT_PRIORITY, &hpm_pwm_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_PWM_INIT)
