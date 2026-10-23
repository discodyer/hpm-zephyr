/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_rng

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hpm_rng_drv.h>

LOG_MODULE_REGISTER(entropy_hpmicro_rng, CONFIG_ENTROPY_LOG_LEVEL);

struct hpm_rng_config {
	RNG_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	bool self_test;
};

struct hpm_rng_data {
	struct k_mutex lock;
};

static int hpm_rng_status_to_errno(hpm_stat_t status)
{
	switch (status) {
	case status_success:
		return 0;
	case status_invalid_argument:
		return -EINVAL;
	case status_rng_busy:
		return -EBUSY;
	case status_rng_not_available:
		return -EAGAIN;
	case status_rng_selftest_failed:
	default:
		return -EIO;
	}
}

static int hpm_rng_get_entropy(const struct device *dev, uint8_t *buffer, uint16_t length)
{
	const struct hpm_rng_config *config = dev->config;
	struct hpm_rng_data *data = dev->data;
	uint32_t random_word;
	uint16_t offset = 0U;
	hpm_stat_t status;
	int ret = 0;

	if ((buffer == NULL) && (length != 0U)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	while (offset < length) {
		size_t chunk = MIN((size_t)(length - offset), sizeof(random_word));

		status = rng_rand_wait(config->base, &random_word, sizeof(random_word));
		ret = hpm_rng_status_to_errno(status);
		if (ret < 0) {
			break;
		}

		memcpy(&buffer[offset], &random_word, chunk);
		offset += chunk;
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

static int hpm_rng_init(const struct device *dev)
{
	const struct hpm_rng_config *config = dev->config;
	struct hpm_rng_data *data = dev->data;
	hpm_stat_t status;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		return ret;
	}

	status = rng_init(config->base);
	if (status != status_success) {
		LOG_ERR("%s: RNG initialization failed (0x%x)", dev->name, status);
		return hpm_rng_status_to_errno(status);
	}

	if (config->self_test) {
		status = rng_run_selftest(config->base);
		if (status != status_success) {
			LOG_ERR("%s: RNG self-test failed (0x%x)", dev->name, status);
			return hpm_rng_status_to_errno(status);
		}
	}

	k_mutex_init(&data->lock);

	return 0;
}

static DEVICE_API(entropy, hpm_rng_api) = {
	.get_entropy = hpm_rng_get_entropy,
};

#define HPM_RNG_INIT(n)                                                                            \
	static struct hpm_rng_data hpm_rng_data_##n;                                               \
                                                                                                   \
	static const struct hpm_rng_config hpm_rng_config_##n = {                                  \
		.base = (RNG_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.self_test = DT_INST_PROP(n, self_test),                                           \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_rng_init, NULL, &hpm_rng_data_##n, &hpm_rng_config_##n,       \
			      POST_KERNEL, CONFIG_ENTROPY_INIT_PRIORITY, &hpm_rng_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_RNG_INIT)
