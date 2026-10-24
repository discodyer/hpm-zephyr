/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_tsns

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hpm_tsns_drv.h>

LOG_MODULE_REGISTER(tsns_hpmicro, CONFIG_SENSOR_LOG_LEVEL);

struct hpm_tsns_config {
	TSNS_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	uint32_t sample_interval_cycles;
	uint8_t averaging;
	uint8_t speed;
};

struct hpm_tsns_data {
	struct k_mutex lock;
	struct k_work work;
	const struct device *dev;
	const struct sensor_trigger *trigger;
	sensor_trigger_handler_t handler;
	int32_t temperature_raw;
	int32_t lower_limit_raw;
	int32_t upper_limit_raw;
	bool lower_limit_set;
	bool upper_limit_set;
};

static int hpm_tsns_wait_valid(const struct hpm_tsns_config *config)
{
	uint32_t elapsed_us = 0U;

	while (!tsns_temperature_is_valid(config->base)) {
		if (elapsed_us >= CONFIG_TSNS_HPMICRO_TIMEOUT_MS * 1000U) {
			return -ETIMEDOUT;
		}

		k_busy_wait(10U);
		elapsed_us += 10U;
	}

	return 0;
}

static int hpm_tsns_configure_limits(const struct device *dev)
{
	const struct hpm_tsns_config *config = dev->config;
	struct hpm_tsns_data *data = dev->data;

	if (data->lower_limit_set && data->upper_limit_set &&
	    (data->lower_limit_raw >= data->upper_limit_raw)) {
		return -EINVAL;
	}

	tsns_disable_compare_max(config->base);
	tsns_disable_compare_min(config->base);

	if (data->upper_limit_set) {
		tsns_set_irq_threshold_high(config->base, data->upper_limit_raw);
		tsns_enable_compare_max(config->base);
	}
	if (data->lower_limit_set) {
		tsns_set_irq_threshold_low(config->base, data->lower_limit_raw);
		tsns_enable_compare_min(config->base);
	}

	if ((data->handler != NULL) && (data->lower_limit_set || data->upper_limit_set)) {
		tsns_enable_limit_trigger_irq(config->base);
	} else {
		/* The pinned HAL clears RST_EN here instead of IRQ_EN. */
		config->base->CONFIG &= ~TSNS_CONFIG_IRQ_EN_MASK;
	}

	return 0;
}

static int hpm_tsns_attr_set(const struct device *dev, enum sensor_channel chan,
			     enum sensor_attribute attr, const struct sensor_value *val)
{
	struct hpm_tsns_data *data = dev->data;
	int64_t raw;
	int32_t old_raw;
	bool old_set;
	int ret;

	if ((chan != SENSOR_CHAN_ALL) && (chan != SENSOR_CHAN_DIE_TEMP)) {
		return -ENOTSUP;
	}

	if (val == NULL) {
		return -EINVAL;
	}

	switch (attr) {
	case SENSOR_ATTR_LOWER_THRESH:
	case SENSOR_ATTR_UPPER_THRESH:
		raw = DIV_ROUND_CLOSEST(sensor_value_to_micro(val) * TSNS_TEMP_SCALE, 1000000);
		if ((raw < INT32_MIN) || (raw > INT32_MAX)) {
			return -EINVAL;
		}
		break;
	default:
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	if (attr == SENSOR_ATTR_LOWER_THRESH) {
		old_raw = data->lower_limit_raw;
		old_set = data->lower_limit_set;
		data->lower_limit_raw = (int32_t)raw;
		data->lower_limit_set = true;
	} else {
		old_raw = data->upper_limit_raw;
		old_set = data->upper_limit_set;
		data->upper_limit_raw = (int32_t)raw;
		data->upper_limit_set = true;
	}
	ret = hpm_tsns_configure_limits(dev);
	if (ret < 0) {
		if (attr == SENSOR_ATTR_LOWER_THRESH) {
			data->lower_limit_raw = old_raw;
			data->lower_limit_set = old_set;
		} else {
			data->upper_limit_raw = old_raw;
			data->upper_limit_set = old_set;
		}
		(void)hpm_tsns_configure_limits(dev);
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

static int hpm_tsns_attr_get(const struct device *dev, enum sensor_channel chan,
			     enum sensor_attribute attr, struct sensor_value *val)
{
	const struct hpm_tsns_data *data = dev->data;
	int32_t raw;

	if ((chan != SENSOR_CHAN_ALL) && (chan != SENSOR_CHAN_DIE_TEMP)) {
		return -ENOTSUP;
	}

	if (val == NULL) {
		return -EINVAL;
	}

	switch (attr) {
	case SENSOR_ATTR_LOWER_THRESH:
		raw = data->lower_limit_raw;
		if (!data->lower_limit_set) {
			return -ENODATA;
		}
		break;
	case SENSOR_ATTR_UPPER_THRESH:
		raw = data->upper_limit_raw;
		if (!data->upper_limit_set) {
			return -ENODATA;
		}
		break;
	default:
		return -ENOTSUP;
	}

	return sensor_value_from_micro(val,
				       DIV_ROUND_CLOSEST((int64_t)raw * 1000000, TSNS_TEMP_SCALE));
}

static void hpm_tsns_work_handler(struct k_work *work)
{
	struct hpm_tsns_data *data = CONTAINER_OF(work, struct hpm_tsns_data, work);

	if ((data->handler != NULL) && (data->trigger != NULL)) {
		data->handler(data->dev, data->trigger);
	}
}

static int hpm_tsns_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
				sensor_trigger_handler_t handler)
{
	struct hpm_tsns_data *data = dev->data;
	int ret;

	if ((trig == NULL) ||
	    ((trig->chan != SENSOR_CHAN_ALL) && (trig->chan != SENSOR_CHAN_DIE_TEMP)) ||
	    (trig->type != SENSOR_TRIG_THRESHOLD)) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	data->dev = dev;
	data->trigger = trig;
	data->handler = handler;
	ret = hpm_tsns_configure_limits(dev);
	k_mutex_unlock(&data->lock);

	return ret;
}

static int hpm_tsns_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct hpm_tsns_config *config = dev->config;
	struct hpm_tsns_data *data = dev->data;
	int ret;

	if ((chan != SENSOR_CHAN_ALL) && (chan != SENSOR_CHAN_DIE_TEMP)) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = hpm_tsns_wait_valid(config);
	if (ret == 0) {
		data->temperature_raw = (int32_t)TSNS_T_T_GET(config->base->T);
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

static int hpm_tsns_channel_get(const struct device *dev, enum sensor_channel chan,
				struct sensor_value *value)
{
	const struct hpm_tsns_data *data = dev->data;
	int32_t remainder;

	if (chan != SENSOR_CHAN_DIE_TEMP) {
		return -ENOTSUP;
	}

	if (value == NULL) {
		return -EINVAL;
	}

	value->val1 = data->temperature_raw / TSNS_TEMP_SCALE;
	remainder = data->temperature_raw % TSNS_TEMP_SCALE;
	value->val2 = (remainder * 1000000) / TSNS_TEMP_SCALE;

	return 0;
}

static void hpm_tsns_isr(const struct device *dev)
{
	const struct hpm_tsns_config *config = dev->config;
	struct hpm_tsns_data *data = dev->data;
	uint32_t flags = tsns_get_flag_value(config->base);

	if ((flags & (TSNS_FLAG_UNDER_TEMP_MASK | TSNS_FLAG_OVER_TEMP_MASK)) == 0U) {
		tsns_clear_with_mask(config->base, tsns_clear_irq);
		return;
	}

	tsns_clear_with_mask(config->base, (tsns_clear_type_mask_t)(TSNS_FLAG_UNDER_TEMP_MASK |
								    TSNS_FLAG_OVER_TEMP_MASK |
								    TSNS_FLAG_IRQ_MASK));
	k_work_submit(&data->work);
}

static DEVICE_API(sensor, hpm_tsns_api) = {
	.sample_fetch = hpm_tsns_sample_fetch,
	.channel_get = hpm_tsns_channel_get,
	.attr_set = hpm_tsns_attr_set,
	.attr_get = hpm_tsns_attr_get,
	.trigger_set = hpm_tsns_trigger_set,
};

#define HPM_TSNS_INIT(n)                                                                           \
	BUILD_ASSERT(DT_INST_PROP_OR(n, averaging, 3) <= 7U, "averaging must be 0..7");            \
	BUILD_ASSERT(DT_INST_PROP_OR(n, measurement_speed, 96) >= 24U,                             \
		     "measurement speed must be at least 24 cycles");                              \
	BUILD_ASSERT(DT_INST_PROP_OR(n, measurement_speed, 96) <= 255U,                            \
		     "measurement speed must not exceed 255 cycles");                              \
                                                                                                   \
	static struct hpm_tsns_data hpm_tsns_data_##n;                                             \
                                                                                                   \
	static const struct hpm_tsns_config hpm_tsns_config_##n = {                                \
		.base = (TSNS_Type *)DT_INST_REG_ADDR(n),                                          \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.sample_interval_cycles = DT_INST_PROP_OR(n, sample_interval_cycles, 0),           \
		.averaging = DT_INST_PROP_OR(n, averaging, 3),                                     \
		.speed = DT_INST_PROP_OR(n, measurement_speed, 96),                                \
	};                                                                                         \
                                                                                                   \
	static int hpm_tsns_init_##n(const struct device *dev)                                     \
	{                                                                                          \
		const struct hpm_tsns_config *config = dev->config;                                \
		struct hpm_tsns_data *data = dev->data;                                            \
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
		tsns_disable(config->base);                                                        \
		tsns_set_speed(config->base, config->speed);                                       \
		tsns_set_average(config->base, config->averaging);                                 \
		if (config->sample_interval_cycles != 0U) {                                        \
			tsns_set_validity(config->base, config->sample_interval_cycles);           \
		}                                                                                  \
		tsns_enable_continuous_mode(config->base);                                         \
		tsns_enable(config->base);                                                         \
                                                                                                   \
		k_mutex_init(&data->lock);                                                         \
		k_work_init(&data->work, hpm_tsns_work_handler);                                   \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_tsns_isr,               \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
                                                                                                   \
		return 0;                                                                          \
	}                                                                                          \
                                                                                                   \
	SENSOR_DEVICE_DT_INST_DEFINE(n, hpm_tsns_init_##n, NULL, &hpm_tsns_data_##n,               \
				     &hpm_tsns_config_##n, POST_KERNEL,                            \
				     CONFIG_SENSOR_INIT_PRIORITY, &hpm_tsns_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_TSNS_INIT)
