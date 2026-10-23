/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_ewdg

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

#include <hpm_clock_drv.h>
#include <hpm_ewdg_drv.h>

LOG_MODULE_REGISTER(wdt_hpmicro, CONFIG_WDT_LOG_LEVEL);

#define HPM_EWDG_COUNTER_HZ              32768U
#define HPM_EWDG_CALLBACK_RESET_DELAY_US 1000U

struct hpm_ewdg_config {
	EWDG_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	void (*irq_config_func)(const struct device *dev);
};

struct hpm_ewdg_data {
	wdt_callback_t callback;
	uint32_t timeout_ms;
	uint8_t flags;
	bool timeout_installed;
	bool enabled;
};

static int hpm_ewdg_status_to_errno(hpm_stat_t status)
{
	switch (status) {
	case status_success:
		return 0;
	case status_invalid_argument:
	case status_ewdg_tick_out_of_range:
	case status_ewdg_div_out_of_range:
		return -EINVAL;
	case status_ewdg_feature_unsupported:
		return -ENOTSUP;
	default:
		return -EIO;
	}
}

static int hpm_ewdg_install_timeout(const struct device *dev, const struct wdt_timeout_cfg *cfg)
{
	struct hpm_ewdg_data *data = dev->data;

	if (cfg == NULL) {
		return -EINVAL;
	}

	if (data->enabled) {
		return -EBUSY;
	}

	if (data->timeout_installed) {
		return -ENOMEM;
	}

	if ((cfg->window.max == 0U) || (cfg->window.min != 0U)) {
		LOG_ERR("Only a single non-window timeout is supported");
		return -ENOTSUP;
	}

	switch (cfg->flags & WDT_FLAG_RESET_MASK) {
	case WDT_FLAG_RESET_NONE:
		if (cfg->callback == NULL) {
			LOG_ERR("A non-reset timeout requires a callback");
			return -EINVAL;
		}
		break;
	case WDT_FLAG_RESET_SOC:
		break;
	default:
		LOG_ERR("Only SoC reset is supported");
		return -ENOTSUP;
	}

	data->callback = cfg->callback;
	data->timeout_ms = cfg->window.max;
	data->flags = cfg->flags;
	data->timeout_installed = true;

	return 0;
}

static int hpm_ewdg_configure(const struct device *dev, uint8_t options)
{
	const struct hpm_ewdg_config *config = dev->config;
	struct hpm_ewdg_data *data = dev->data;
	ewdg_config_t ewdg_config;
	uint64_t timeout_us;
	uint64_t reset_us;
	hpm_stat_t status;

	timeout_us = (uint64_t)data->timeout_ms * 1000U;
	if (timeout_us > UINT32_MAX) {
		return -EINVAL;
	}

	reset_us = timeout_us;
	if (data->callback != NULL) {
		reset_us += HPM_EWDG_CALLBACK_RESET_DELAY_US;
		if (reset_us > UINT32_MAX) {
			return -EINVAL;
		}
	}

	ewdg_get_default_config(config->base, &ewdg_config);
	ewdg_config.cnt_src_freq = HPM_EWDG_COUNTER_HZ;
	ewdg_config.enable_watchdog = true;
	ewdg_config.ctrl_config.cnt_clk_sel = ewdg_cnt_src_clk_32k;
	ewdg_config.ctrl_config.use_lowlevel_timeout = false;
	ewdg_config.ctrl_config.keep_running_in_debug_mode =
		(options & WDT_OPT_PAUSE_HALTED_BY_DBG) == 0U;
	ewdg_config.ctrl_config.low_power_mode = (options & WDT_OPT_PAUSE_IN_SLEEP) != 0U
							 ? ewdg_low_power_mode_halt
							 : ewdg_low_power_mode_work_clock_normal;
	ewdg_config.ctrl_config.timeout_interrupt_us = (uint32_t)timeout_us;
	ewdg_config.ctrl_config.timeout_reset_us = (uint32_t)reset_us;
	ewdg_config.int_rst_config.enable_timeout_interrupt = data->callback != NULL;
	ewdg_config.int_rst_config.enable_timeout_reset =
		(data->flags & WDT_FLAG_RESET_MASK) == WDT_FLAG_RESET_SOC;

	status = ewdg_init(config->base, &ewdg_config);

	return hpm_ewdg_status_to_errno(status);
}

static int hpm_ewdg_setup(const struct device *dev, uint8_t options)
{
	struct hpm_ewdg_data *data = dev->data;
	int ret;

	if (!data->timeout_installed) {
		return -EINVAL;
	}

	if (data->enabled) {
		return -EBUSY;
	}

	if ((options & ~(WDT_OPT_PAUSE_IN_SLEEP | WDT_OPT_PAUSE_HALTED_BY_DBG)) != 0U) {
		return -ENOTSUP;
	}

	ret = hpm_ewdg_configure(dev, options);
	if (ret == 0) {
		data->enabled = true;
	}

	return ret;
}

static int hpm_ewdg_disable(const struct device *dev)
{
	const struct hpm_ewdg_config *config = dev->config;
	struct hpm_ewdg_data *data = dev->data;

	if (!data->enabled) {
		return -EFAULT;
	}

	ewdg_disable(config->base);
	data->enabled = false;
	data->timeout_installed = false;

	return 0;
}

static int hpm_ewdg_feed(const struct device *dev, int channel_id)
{
	const struct hpm_ewdg_config *config = dev->config;
	struct hpm_ewdg_data *data = dev->data;

	if ((channel_id != 0) || !data->timeout_installed) {
		return -EINVAL;
	}

	if (!data->enabled) {
		return -EFAULT;
	}

	return hpm_ewdg_status_to_errno(ewdg_refresh(config->base));
}

static void hpm_ewdg_isr(const struct device *dev)
{
	const struct hpm_ewdg_config *config = dev->config;
	struct hpm_ewdg_data *data = dev->data;
	uint32_t status_flags;

	status_flags = ewdg_get_status_flags(config->base);

	if ((status_flags & EWDG_EVENT_TIMEOUT_INTERRUPT) != 0U) {
		/*
		 * The timeout event is cleared by refreshing the watchdog. Do not
		 * refresh here, otherwise the configured SoC reset would be lost.
		 */
		ewdg_disable_interrupt(config->base, EWDG_INT_TIMEOUT);
		if (data->callback != NULL) {
			data->callback(dev, 0);
		}
	}

	ewdg_clear_status_flags(config->base, status_flags & ~EWDG_EVENT_TIMEOUT_INTERRUPT);
}

static DEVICE_API(wdt, hpm_ewdg_api) = {
	.install_timeout = hpm_ewdg_install_timeout,
	.setup = hpm_ewdg_setup,
	.disable = hpm_ewdg_disable,
	.feed = hpm_ewdg_feed,
};

#define HPM_EWDG_IRQ_CONFIG(n)                                                                     \
	static void hpm_ewdg_irq_config_##n(const struct device *dev)                              \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_ewdg_isr,               \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}

#define HPM_EWDG_INIT(n)                                                                           \
	HPM_EWDG_IRQ_CONFIG(n)                                                                     \
                                                                                                   \
	static struct hpm_ewdg_data hpm_ewdg_data_##n;                                             \
                                                                                                   \
	static const struct hpm_ewdg_config hpm_ewdg_config_##n = {                                \
		.base = (EWDG_Type *)DT_INST_REG_ADDR(n),                                          \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.irq_config_func = hpm_ewdg_irq_config_##n,                                        \
	};                                                                                         \
                                                                                                   \
	static int hpm_ewdg_init_##n(const struct device *dev)                                     \
	{                                                                                          \
		const struct hpm_ewdg_config *config = dev->config;                                \
		int ret;                                                                           \
                                                                                                   \
		if (!device_is_ready(config->clock_dev)) {                                         \
			return -ENODEV;                                                            \
		}                                                                                  \
                                                                                                   \
		ret = clock_control_on(config->clock_dev, config->clock_subsys);                   \
		if (ret < 0) {                                                                     \
			return ret;                                                                \
		}                                                                                  \
                                                                                                   \
		config->irq_config_func(dev);                                                      \
                                                                                                   \
		return 0;                                                                          \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_ewdg_init_##n, NULL, &hpm_ewdg_data_##n,                      \
			      &hpm_ewdg_config_##n, POST_KERNEL,                                   \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &hpm_ewdg_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_EWDG_INIT)
