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
	int32_t temperature_raw;
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

static DEVICE_API(sensor, hpm_tsns_api) = {
	.sample_fetch = hpm_tsns_sample_fetch,
	.channel_get = hpm_tsns_channel_get,
};

static int hpm_tsns_init(const struct device *dev)
{
	const struct hpm_tsns_config *config = dev->config;
	struct hpm_tsns_data *data = dev->data;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		return ret;
	}

	tsns_disable(config->base);
	tsns_set_speed(config->base, config->speed);
	tsns_set_average(config->base, config->averaging);
	if (config->sample_interval_cycles != 0U) {
		tsns_set_validity(config->base, config->sample_interval_cycles);
	}
	tsns_enable_continuous_mode(config->base);
	tsns_enable(config->base);

	k_mutex_init(&data->lock);

	return 0;
}

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
	SENSOR_DEVICE_DT_INST_DEFINE(n, hpm_tsns_init, NULL, &hpm_tsns_data_##n,                   \
				     &hpm_tsns_config_##n, POST_KERNEL,                            \
				     CONFIG_SENSOR_INIT_PRIORITY, &hpm_tsns_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_TSNS_INIT)
