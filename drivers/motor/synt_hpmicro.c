/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_synt

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <drivers/motor/hpmicro_synt.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/kernel.h>

#include <hpm_common.h>
#include <hpm_synt_drv.h>

struct hpm_synt_config {
	SYNT_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
};

struct hpm_synt_data {
	struct k_spinlock lock;
};

static int hpm_synt_drv_set_reload(const struct device *dev, uint32_t reload)
{
	const struct hpm_synt_config *config = dev->config;
	struct hpm_synt_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	synt_set_reload(config->base, reload);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_synt_drv_set_comparator(const struct device *dev, uint8_t channel, uint32_t value)
{
	const struct hpm_synt_config *config = dev->config;
	struct hpm_synt_data *data = dev->data;
	k_spinlock_key_t key;
	hpm_stat_t status;

	key = k_spin_lock(&data->lock);
	status = synt_set_comparator(config->base, channel, value);
	k_spin_unlock(&data->lock, key);

	return status == status_success ? 0 : -EINVAL;
}

static int hpm_synt_drv_counter_enable(const struct device *dev, bool enable)
{
	const struct hpm_synt_config *config = dev->config;
	struct hpm_synt_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	synt_enable_counter(config->base, enable);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_synt_drv_counter_reset(const struct device *dev)
{
	const struct hpm_synt_config *config = dev->config;
	struct hpm_synt_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	synt_reset_counter(config->base);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_synt_drv_get_counter(const struct device *dev, uint32_t *value)
{
	const struct hpm_synt_config *config = dev->config;

	if (value == NULL) {
		return -EINVAL;
	}

	*value = synt_get_current_count(config->base);

	return 0;
}

static int hpm_synt_drv_counter_debug_stop(const struct device *dev, bool enable)
{
	const struct hpm_synt_config *config = dev->config;
	struct hpm_synt_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	if (enable) {
		config->base->GCR |= SYNT_GCR_COUNTER_DEBUG_EN_MASK;
	} else {
		config->base->GCR &= ~SYNT_GCR_COUNTER_DEBUG_EN_MASK;
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_synt_drv_timestamp_enable(const struct device *dev, bool enable)
{
	const struct hpm_synt_config *config = dev->config;
	struct hpm_synt_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	synt_enable_timestamp(config->base, enable);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_synt_drv_timestamp_reset(const struct device *dev)
{
	const struct hpm_synt_config *config = dev->config;
	struct hpm_synt_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	synt_reset_timestamp(config->base);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_synt_drv_timestamp_update(const struct device *dev,
					 enum hpm_synt_timestamp_update operation, uint32_t value)
{
	const struct hpm_synt_config *config = dev->config;
	struct hpm_synt_data *data = dev->data;
	k_spinlock_key_t key;

	if (operation > HPM_SYNT_TIMESTAMP_DECREMENT) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	synt_set_timestamp_new_value(config->base, value);
	if (operation == HPM_SYNT_TIMESTAMP_SET) {
		synt_update_timestamp_new(config->base);
	} else if (operation == HPM_SYNT_TIMESTAMP_INCREMENT) {
		synt_update_timestamp_inc(config->base);
	} else {
		synt_update_timestamp_dec(config->base);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_synt_drv_get_timestamp(const struct device *dev, uint32_t *value)
{
	const struct hpm_synt_config *config = dev->config;

	if (value == NULL) {
		return -EINVAL;
	}

	*value = synt_get_timestamp_current_value(config->base);

	return 0;
}

static int hpm_synt_drv_get_saved_timestamp(const struct device *dev, uint32_t *value)
{
	const struct hpm_synt_config *config = dev->config;

	if (value == NULL) {
		return -EINVAL;
	}

	*value = synt_get_timestamp_save_value(config->base);

	return 0;
}

static int hpm_synt_drv_timestamp_debug_stop(const struct device *dev, bool enable)
{
	const struct hpm_synt_config *config = dev->config;
	struct hpm_synt_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	synt_enable_timestamp_debug_stop(config->base, enable);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static DEVICE_API(hpm_synt, hpm_synt_api) = {
	.set_reload = hpm_synt_drv_set_reload,
	.set_comparator = hpm_synt_drv_set_comparator,
	.counter_enable = hpm_synt_drv_counter_enable,
	.counter_reset = hpm_synt_drv_counter_reset,
	.get_counter = hpm_synt_drv_get_counter,
	.counter_debug_stop = hpm_synt_drv_counter_debug_stop,
	.timestamp_enable = hpm_synt_drv_timestamp_enable,
	.timestamp_reset = hpm_synt_drv_timestamp_reset,
	.timestamp_update = hpm_synt_drv_timestamp_update,
	.get_timestamp = hpm_synt_drv_get_timestamp,
	.get_saved_timestamp = hpm_synt_drv_get_saved_timestamp,
	.timestamp_debug_stop = hpm_synt_drv_timestamp_debug_stop,
};

static int hpm_synt_init(const struct device *dev)
{
	const struct hpm_synt_config *config = dev->config;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	return clock_control_on(config->clock_dev, config->clock_subsys);
}

#define HPM_SYNT_INIT(n)                                                                           \
	static const struct hpm_synt_config hpm_synt_config_##n = {                                \
		.base = (SYNT_Type *)DT_INST_REG_ADDR(n),                                          \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
	};                                                                                         \
                                                                                                   \
	static struct hpm_synt_data hpm_synt_data_##n;                                             \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_synt_init, NULL, &hpm_synt_data_##n, &hpm_synt_config_##n,    \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &hpm_synt_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_SYNT_INIT)
