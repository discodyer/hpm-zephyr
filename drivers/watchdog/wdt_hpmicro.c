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
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <hpm_clock_drv.h>
#include <hpm_ewdg_drv.h>
#include <hpm_plic_drv.h>

LOG_MODULE_REGISTER(wdt_hpmicro, CONFIG_WDT_LOG_LEVEL);

#define HPM_EWDG_COUNTER_HZ              32768U
#define HPM_EWDG_CALLBACK_RESET_DELAY_US 1000U
#define HPM_EWDG_REFRESH_TIMEOUT_US      10000U

struct hpm_ewdg_config {
	EWDG_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	void (*irq_config_func)(const struct device *dev);
};

struct hpm_ewdg_data {
	wdt_callback_t callback;
	uint32_t timeout_ms;
	uint32_t window_min_ms;
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

	if (cfg->window.max == 0U) {
		return -EINVAL;
	}

	if ((cfg->window.min != 0U) && (cfg->window.min >= cfg->window.max)) {
		return -EINVAL;
	}

	switch (cfg->flags & WDT_FLAG_RESET_MASK) {
	case WDT_FLAG_RESET_NONE:
		if (cfg->callback == NULL) {
			LOG_ERR("A non-reset timeout requires a callback");
			return -EINVAL;
		}
		if (cfg->window.min != 0U) {
			LOG_ERR("EWDG window mode requires a SoC reset");
			return -ENOTSUP;
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
	data->window_min_ms = cfg->window.min;
	data->flags = cfg->flags;
	data->timeout_installed = true;

	return 0;
}

static hpm_stat_t hpm_ewdg_configure_interrupt_reset(const struct device *dev, bool interrupt)
{
	const struct hpm_ewdg_config *config = dev->config;
	struct hpm_ewdg_data *data = dev->data;
	ewdg_interrupt_reset_config_t int_rst_config = {
		.enable_timeout_interrupt = interrupt && (data->callback != NULL),
		.enable_timeout_reset = (data->flags & WDT_FLAG_RESET_MASK) == WDT_FLAG_RESET_SOC,
		.enable_refresh_violation_reset =
			(data->window_min_ms != 0U) &&
			((data->flags & WDT_FLAG_RESET_MASK) == WDT_FLAG_RESET_SOC),
	};

	/* Rebuild CTRL1 so its parity bit is recalculated from the new settings. */
	return ewdg_init_interrupt_reset(config->base, &int_rst_config);
}

static bool hpm_ewdg_counter_advanced(EWDG_Type *base, uint32_t *previous)
{
	/* Reading the refresh register returns the live counter for diagnostics. */
	uint32_t count = base->WDT_REFRESH_REG;
	bool advanced = count > *previous;

	*previous = count;
	return advanced;
}

static int hpm_ewdg_wait_running(const struct device *dev)
{
	const struct hpm_ewdg_config *config = dev->config;
	uint32_t count = config->base->WDT_REFRESH_REG;

	/* Do not refresh until enable has reached the counter clock domain. */
	if (!WAIT_FOR(hpm_ewdg_counter_advanced(config->base, &count), HPM_EWDG_REFRESH_TIMEOUT_US,
		      k_busy_wait(1))) {
		return -ETIMEDOUT;
	}

	return 0;
}

static int hpm_ewdg_refresh(const struct device *dev)
{
	const struct hpm_ewdg_config *config = dev->config;
	hpm_stat_t status = ewdg_refresh(config->base);

	if (status != status_success) {
		return hpm_ewdg_status_to_errno(status);
	}

	/* The refresh crosses into the counter clock domain before clearing timeout. */
	if (!WAIT_FOR((ewdg_get_status_flags(config->base) &
		       (EWDG_EVENT_TIMEOUT_INTERRUPT | EWDG_EVENT_TIMEOUT_RESET)) == 0U,
		      HPM_EWDG_REFRESH_TIMEOUT_US, k_busy_wait(1))) {
		return -ETIMEDOUT;
	}

	return 0;
}

static int hpm_ewdg_configure(const struct device *dev, uint8_t options)
{
	const struct hpm_ewdg_config *config = dev->config;
	struct hpm_ewdg_data *data = dev->data;
	ewdg_config_t ewdg_config;
	uint64_t timeout_us;
	uint64_t reset_us;
	uint64_t interrupt_ticks;
	uint64_t reset_ticks;
	uint32_t divider = 0U;
	hpm_stat_t status;
	int ret;

	timeout_us = (uint64_t)data->timeout_ms * 1000U;
	reset_us = timeout_us;
	if (data->callback != NULL) {
		reset_us += HPM_EWDG_CALLBACK_RESET_DELAY_US;
	}

	/* The SDK's high-level conversion swaps the clock and timeout arguments. */
	interrupt_ticks = DIV_ROUND_UP(timeout_us * HPM_EWDG_COUNTER_HZ, 1000000U);
	reset_ticks = DIV_ROUND_UP(reset_us * HPM_EWDG_COUNTER_HZ, 1000000U);
	while (reset_ticks > EWDG_OT_RST_VAL_OT_RST_VAL_MASK) {
		if (divider == EWDG_SOC_CLK_DIV_VAL_MAX) {
			return -EINVAL;
		}
		divider++;
		interrupt_ticks = DIV_ROUND_UP(interrupt_ticks, 2U);
		reset_ticks = DIV_ROUND_UP(reset_ticks, 2U);
	}
	if ((data->flags & WDT_FLAG_RESET_MASK) == WDT_FLAG_RESET_NONE) {
		reset_ticks = EWDG_OT_RST_VAL_OT_RST_VAL_MASK;
	}

	ewdg_get_default_config(config->base, &ewdg_config);
	ewdg_config.cnt_src_freq = HPM_EWDG_COUNTER_HZ;
	ewdg_config.enable_watchdog = true;
	ewdg_config.ctrl_config.cnt_clk_sel = ewdg_cnt_src_clk_32k;
	ewdg_config.ctrl_config.use_lowlevel_timeout = true;
	ewdg_config.ctrl_config.clock_div_by_power_of_2 = divider;
	ewdg_config.ctrl_config.keep_running_in_debug_mode =
		(options & WDT_OPT_PAUSE_HALTED_BY_DBG) == 0U;
	/* HPM5300 uses EN_LP=3 for full speed; the SDK's value of 1 selects /4. */
	ewdg_config.ctrl_config.low_power_mode = (options & WDT_OPT_PAUSE_IN_SLEEP) != 0U
							 ? ewdg_low_power_mode_halt
							 : (ewdg_low_power_mode_t)3U;
	ewdg_config.ctrl_config.timeout_interrupt_val = (uint32_t)interrupt_ticks;
	ewdg_config.ctrl_config.timeout_reset_val = (uint32_t)reset_ticks;

	if (data->window_min_ms != 0U) {
		uint64_t min_ratio_16 =
			DIV_ROUND_UP((uint64_t)data->window_min_ms * 16U, reset_us / 1000U);

		if (min_ratio_16 <= 8U) {
			ewdg_config.ctrl_config.window_lower_limit =
				ewdg_window_lower_timeout_period_8_div_16;
		} else if (min_ratio_16 <= 10U) {
			ewdg_config.ctrl_config.window_lower_limit =
				ewdg_window_lower_timeout_period_10_div_16;
		} else if (min_ratio_16 <= 12U) {
			ewdg_config.ctrl_config.window_lower_limit =
				ewdg_window_lower_timeout_period_12_div_16;
		} else if (min_ratio_16 <= 14U) {
			ewdg_config.ctrl_config.window_lower_limit =
				ewdg_window_lower_timeout_period_14_div_16;
		} else {
			return -EINVAL;
		}

		/* An upper-limit value of zero means no limit before reset. */
		ewdg_config.ctrl_config.window_upper_limit =
			ewdg_window_upper_timeout_period_8_div_16;
	}

	/* Keep IRQ/reset sources off until refresh clears the previous timeout. */
	status = ewdg_init(config->base, &ewdg_config);
	if (status != status_success) {
		return hpm_ewdg_status_to_errno(status);
	}
	ret = hpm_ewdg_wait_running(dev);
	if (ret == 0) {
		ret = hpm_ewdg_refresh(dev);
	}
	if (ret < 0) {
		ewdg_disable(config->base);
		return ret;
	}
	ewdg_clear_status_flags(config->base, ewdg_get_status_flags(config->base));
	if (data->window_min_ms != 0U) {
		/* Establish the initial count before enforcing the refresh window. */
		ewdg_config.ctrl_config.enable_window_mode = true;
		status = ewdg_init_ctrl_func(config->base, &ewdg_config.ctrl_config,
					     HPM_EWDG_COUNTER_HZ);
		if (status != status_success) {
			ewdg_disable(config->base);
			return hpm_ewdg_status_to_errno(status);
		}
	}
	status = hpm_ewdg_configure_interrupt_reset(dev, true);
	if (status != status_success) {
		ewdg_disable(config->base);
	}

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
	data->window_min_ms = 0U;

	return 0;
}

static int hpm_ewdg_feed(const struct device *dev, int channel_id)
{
	const struct hpm_ewdg_config *config = dev->config;
	struct hpm_ewdg_data *data = dev->data;
	unsigned int key;
	int ret;

	if ((channel_id != 0) || !data->timeout_installed) {
		return -EINVAL;
	}

	if (!data->enabled) {
		return -EFAULT;
	}

	key = irq_lock();
	ret = 0;
	if ((data->flags & WDT_FLAG_RESET_MASK) == WDT_FLAG_RESET_NONE) {
		ewdg_enable(config->base);
		ret = hpm_ewdg_wait_running(dev);
	}
	if (ret == 0) {
		ret = hpm_ewdg_refresh(dev);
	}
	if ((ret == 0) && (data->callback != NULL)) {
		ret = hpm_ewdg_status_to_errno(hpm_ewdg_configure_interrupt_reset(dev, true));
	}
	if ((ret < 0) && ((data->flags & WDT_FLAG_RESET_MASK) == WDT_FLAG_RESET_NONE)) {
		ewdg_disable(config->base);
	}
	irq_unlock(key);

	return ret;
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
		(void)hpm_ewdg_configure_interrupt_reset(dev, false);
		if ((data->flags & WDT_FLAG_RESET_MASK) == WDT_FLAG_RESET_NONE) {
			/* Avoid reaching the terminal reset count while waiting for a feed. */
			ewdg_disable(config->base);
		}
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

static void hpm_ewdg_set_irq_trigger(uint32_t irq)
{
#if defined(EWDG_TIMEOUT_INTERRUPT_REQUIRE_EDGE_TRIGGER) &&                                        \
	(EWDG_TIMEOUT_INTERRUPT_REQUIRE_EDGE_TRIGGER == 1)
	__plic_enable_irq_edge_trigger(HPM_PLIC_BASE, irq);
#else
	ARG_UNUSED(irq);
#endif
}

#define HPM_EWDG_IRQ_CONFIG(n)                                                                     \
	static void hpm_ewdg_irq_config_##n(const struct device *dev)                              \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_ewdg_isr,               \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		hpm_ewdg_set_irq_trigger(DT_INST_IRQ(n, irq));                                     \
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
