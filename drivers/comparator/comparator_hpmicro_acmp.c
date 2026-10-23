/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_acmp_comp

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/comparator.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <hpm_acmp_drv.h>

LOG_MODULE_REGISTER(comparator_hpmicro_acmp, CONFIG_COMPARATOR_LOG_LEVEL);

struct hpm_acmp_config {
	ACMP_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	const struct pinctrl_dev_config *pincfg;
	acmp_channel_config_t channel_cfg;
	uint8_t channel;
	uint8_t dac_value;
};

struct hpm_acmp_data {
	struct k_spinlock lock;
	comparator_callback_t callback;
	void *user_data;
	atomic_t trigger_pending;
};

static uint8_t hpm_acmp_filter_mode(uint8_t mode)
{
	switch (mode) {
	case 1U:
		return ACMP_FILTER_MODE_CHANGE_IMMEDIATELY;
	case 2U:
		return ACMP_FILTER_MODE_CHANGE_AFTER_FILTER;
	case 3U:
		return ACMP_FILTER_MODE_STABLE_LOW;
	case 4U:
		return ACMP_FILTER_MODE_STABLE_HIGH;
	case 0U:
	default:
		return ACMP_FILTER_MODE_BYPASS;
	}
}

static int hpm_acmp_get_output(const struct device *dev)
{
	ARG_UNUSED(dev);

	/*
	 * The ACMP status register exposes only edge flags. It has no live
	 * output-level bit, so deriving a level from those flags would be
	 * incorrect.
	 */
	return -ENOTSUP;
}

static void hpm_acmp_disable_trigger(const struct device *dev)
{
	const struct hpm_acmp_config *config = dev->config;

	acmp_channel_enable_irq(config->base, config->channel,
				ACMP_EVENT_RISING_EDGE | ACMP_EVENT_FALLING_EDGE, false);
	acmp_channel_clear_status(config->base, config->channel,
				  ACMP_EVENT_RISING_EDGE | ACMP_EVENT_FALLING_EDGE);
}

static int hpm_acmp_set_trigger(const struct device *dev, enum comparator_trigger trigger)
{
	const struct hpm_acmp_config *config = dev->config;
	struct hpm_acmp_data *data = dev->data;
	uint32_t mask;
	k_spinlock_key_t key;

	switch (trigger) {
	case COMPARATOR_TRIGGER_NONE:
		mask = 0U;
		break;
	case COMPARATOR_TRIGGER_RISING_EDGE:
		mask = ACMP_EVENT_RISING_EDGE;
		break;
	case COMPARATOR_TRIGGER_FALLING_EDGE:
		mask = ACMP_EVENT_FALLING_EDGE;
		break;
	case COMPARATOR_TRIGGER_BOTH_EDGES:
		mask = ACMP_EVENT_RISING_EDGE | ACMP_EVENT_FALLING_EDGE;
		break;
	default:
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	hpm_acmp_disable_trigger(dev);
	atomic_clear(&data->trigger_pending);
	if (mask != 0U) {
		acmp_channel_enable_irq(config->base, config->channel, mask, true);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_acmp_set_trigger_callback(const struct device *dev, comparator_callback_t callback,
					 void *user_data)
{
	struct hpm_acmp_data *data = dev->data;
	bool notify_pending = false;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	data->callback = callback;
	data->user_data = user_data;
	if ((callback != NULL) && atomic_cas(&data->trigger_pending, 1, 0)) {
		notify_pending = true;
	}
	k_spin_unlock(&data->lock, key);

	if (notify_pending) {
		callback(dev, user_data);
	}

	return 0;
}

static int hpm_acmp_trigger_is_pending(const struct device *dev)
{
	struct hpm_acmp_data *data = dev->data;

	return atomic_cas(&data->trigger_pending, 1, 0) ? 1 : 0;
}

static void hpm_acmp_isr(const struct device *dev)
{
	const struct hpm_acmp_config *config = dev->config;
	struct hpm_acmp_data *data = dev->data;
	uint32_t status;
	comparator_callback_t callback;
	void *user_data;
	k_spinlock_key_t key;

	status = acmp_channel_get_status(config->base, config->channel) &
		 (ACMP_EVENT_RISING_EDGE | ACMP_EVENT_FALLING_EDGE);
	if (status == 0U) {
		return;
	}

	acmp_channel_clear_status(config->base, config->channel, status);

	key = k_spin_lock(&data->lock);
	callback = data->callback;
	user_data = data->user_data;
	if (callback == NULL) {
		atomic_set(&data->trigger_pending, 1);
	}
	k_spin_unlock(&data->lock, key);

	if (callback != NULL) {
		callback(dev, user_data);
	}
}

static DEVICE_API(comparator, hpm_acmp_api) = {
	.get_output = hpm_acmp_get_output,
	.set_trigger = hpm_acmp_set_trigger,
	.set_trigger_callback = hpm_acmp_set_trigger_callback,
	.trigger_is_pending = hpm_acmp_trigger_is_pending,
};

#define HPM_ACMP_INIT(n)                                                                           \
	BUILD_ASSERT(DT_INST_REG_ADDR(n) <= 1U, "invalid ACMP channel");                           \
	BUILD_ASSERT(DT_INST_PROP(n, dac_value) <= 0xFFU, "DAC value exceeds 8 bits");             \
	BUILD_ASSERT(DT_INST_PROP(n, filter_length) <= 0xFFFU, "filter length exceeds 12 bits");   \
                                                                                                   \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
                                                                                                   \
	static struct hpm_acmp_data hpm_acmp_data_##n;                                             \
                                                                                                   \
	static const struct hpm_acmp_config hpm_acmp_config_##n = {                                \
		.base = (ACMP_Type *)DT_REG_ADDR(DT_INST_PARENT(n)),                               \
		.clock_dev = DEVICE_DT_GET(DT_CLOCKS_CTLR(DT_INST_PARENT(n))),                     \
		.clock_subsys = (clock_control_subsys_t)DT_CLOCKS_CELL(DT_INST_PARENT(n), name),   \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
		.channel_cfg =                                                                     \
			{                                                                          \
				.plus_input = DT_INST_PROP(n, positive_input),                     \
				.minus_input = DT_INST_PROP(n, negative_input),                    \
				.filter_mode = DT_INST_ENUM_IDX(n, filter_mode),                   \
				.hyst_level = DT_INST_PROP(n, hysteresis),                         \
				.enable_cmp_output = DT_INST_PROP(n, enable_output_pin),           \
				.enable_window_mode = false,                                       \
				.invert_output = DT_INST_PROP(n, invert_output),                   \
				.bypass_filter = DT_INST_ENUM_IDX(n, filter_mode) == 0U,           \
				.enable_dac =                                                      \
					(DT_INST_PROP(n, positive_input) == ACMP_INPUT_DAC_OUT) || \
					(DT_INST_PROP(n, negative_input) == ACMP_INPUT_DAC_OUT),   \
				.enable_hpmode = DT_INST_PROP(n, high_performance_mode),           \
				.filter_length = DT_INST_PROP(n, filter_length),                   \
			},                                                                         \
		.channel = DT_INST_REG_ADDR(n),                                                    \
		.dac_value = DT_INST_PROP(n, dac_value),                                           \
	};                                                                                         \
                                                                                                   \
	static int hpm_acmp_init_##n(const struct device *dev)                                     \
	{                                                                                          \
		const struct hpm_acmp_config *config = dev->config;                                \
		struct hpm_acmp_data *data = dev->data;                                            \
		acmp_channel_config_t channel_cfg = config->channel_cfg;                           \
                                                                                                   \
		channel_cfg.filter_mode = hpm_acmp_filter_mode(channel_cfg.filter_mode);           \
		hpm_stat_t status;                                                                 \
		int ret;                                                                           \
                                                                                                   \
		if (!device_is_ready(config->clock_dev)) {                                         \
			return -ENODEV;                                                            \
		}                                                                                  \
                                                                                                   \
		if (DT_INST_NODE_HAS_PROP(n, pinctrl_0)) {                                         \
			ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);          \
			if (ret < 0) {                                                             \
				return ret;                                                        \
			}                                                                          \
		}                                                                                  \
                                                                                                   \
		ret = clock_control_on(config->clock_dev, config->clock_subsys);                   \
		if (ret < 0) {                                                                     \
			return ret;                                                                \
		}                                                                                  \
                                                                                                   \
		atomic_clear(&data->trigger_pending);                                              \
                                                                                                   \
		if (channel_cfg.enable_dac) {                                                      \
			acmp_channel_config_dac(config->base, config->channel, config->dac_value); \
		}                                                                                  \
                                                                                                   \
		status = acmp_channel_config(config->base, config->channel, &channel_cfg, true);   \
		if (status != status_success) {                                                    \
			return -EIO;                                                               \
		}                                                                                  \
                                                                                                   \
		hpm_acmp_disable_trigger(dev);                                                     \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_acmp_isr,               \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
                                                                                                   \
		return 0;                                                                          \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_acmp_init_##n, NULL, &hpm_acmp_data_##n,                      \
			      &hpm_acmp_config_##n, POST_KERNEL, CONFIG_COMPARATOR_INIT_PRIORITY,  \
			      &hpm_acmp_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_ACMP_INIT)
