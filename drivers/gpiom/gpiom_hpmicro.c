/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_gpiom

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <drivers/gpiom/hpmicro_gpiom.h>
#include <zephyr/kernel.h>

#include <hpm_common.h>
#include <hpm_gpiom_drv.h>

#define HPM_GPIOM_MAX_PIN 31U

struct hpm_gpiom_config {
	GPIOM_Type *base;
};

struct hpm_gpiom_data {
	struct k_spinlock lock;
};

static bool hpm_gpiom_port_is_valid(enum hpm_gpiom_port port)
{
	switch (port) {
	case HPM_GPIOM_PORT_GPIOA:
	case HPM_GPIOM_PORT_GPIOB:
	case HPM_GPIOM_PORT_GPIOX:
	case HPM_GPIOM_PORT_GPIOY:
		return true;
	default:
		return false;
	}
}

static bool hpm_gpiom_controller_is_valid(enum hpm_gpiom_controller controller)
{
	return (controller == HPM_GPIOM_CONTROLLER_GPIO0) ||
	       (controller == HPM_GPIOM_CONTROLLER_FGPIO0);
}

static int hpm_gpiom_validate(enum hpm_gpiom_port port, uint8_t pin)
{
	if (!hpm_gpiom_port_is_valid(port) || (pin > HPM_GPIOM_MAX_PIN)) {
		return -EINVAL;
	}

	return 0;
}

static int hpm_gpiom_drv_set_controller(const struct device *dev, enum hpm_gpiom_port port,
					uint8_t pin, enum hpm_gpiom_controller controller)
{
	const struct hpm_gpiom_config *config = dev->config;
	struct hpm_gpiom_data *data = dev->data;
	k_spinlock_key_t key;
	int ret;

	ret = hpm_gpiom_validate(port, pin);
	if (ret < 0) {
		return ret;
	}

	if (!hpm_gpiom_controller_is_valid(controller)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	if (gpiom_pin_is_locked(config->base, port, pin)) {
		k_spin_unlock(&data->lock, key);
		return -EACCES;
	}

	gpiom_set_pin_controller(config->base, port, pin, (gpiom_gpio_t)controller);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_gpiom_drv_get_controller(const struct device *dev, enum hpm_gpiom_port port,
					uint8_t pin, enum hpm_gpiom_controller *controller)
{
	const struct hpm_gpiom_config *config = dev->config;
	int ret;

	if (controller == NULL) {
		return -EINVAL;
	}

	ret = hpm_gpiom_validate(port, pin);
	if (ret < 0) {
		return ret;
	}

	*controller = (enum hpm_gpiom_controller)gpiom_get_pin_controller(config->base, port, pin);

	return 0;
}

static int hpm_gpiom_drv_set_visibility(const struct device *dev, enum hpm_gpiom_port port,
					uint8_t pin, enum hpm_gpiom_controller controller,
					bool visible)
{
	const struct hpm_gpiom_config *config = dev->config;
	struct hpm_gpiom_data *data = dev->data;
	k_spinlock_key_t key;
	int ret;

	ret = hpm_gpiom_validate(port, pin);
	if (ret < 0) {
		return ret;
	}

	if (!hpm_gpiom_controller_is_valid(controller)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	if (gpiom_pin_is_locked(config->base, port, pin)) {
		k_spin_unlock(&data->lock, key);
		return -EACCES;
	}

	if (visible) {
		gpiom_enable_pin_visibility(config->base, port, pin, (gpiom_gpio_t)controller);
	} else {
		gpiom_disable_pin_visibility(config->base, port, pin, (gpiom_gpio_t)controller);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_gpiom_drv_get_visibility(const struct device *dev, enum hpm_gpiom_port port,
					uint8_t pin, enum hpm_gpiom_controller controller,
					bool *visible)
{
	const struct hpm_gpiom_config *config = dev->config;
	int ret;

	if (visible == NULL) {
		return -EINVAL;
	}

	ret = hpm_gpiom_validate(port, pin);
	if (ret < 0) {
		return ret;
	}

	if (!hpm_gpiom_controller_is_valid(controller)) {
		return -EINVAL;
	}

	*visible = gpiom_check_pin_visibility(config->base, port, pin, (gpiom_gpio_t)controller);

	return 0;
}

static int hpm_gpiom_drv_lock(const struct device *dev, enum hpm_gpiom_port port, uint8_t pin)
{
	const struct hpm_gpiom_config *config = dev->config;
	struct hpm_gpiom_data *data = dev->data;
	k_spinlock_key_t key;
	int ret;

	ret = hpm_gpiom_validate(port, pin);
	if (ret < 0) {
		return ret;
	}

	key = k_spin_lock(&data->lock);
	if (gpiom_pin_is_locked(config->base, port, pin)) {
		k_spin_unlock(&data->lock, key);
		return -EACCES;
	}

	gpiom_lock_pin(config->base, port, pin);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_gpiom_drv_is_locked(const struct device *dev, enum hpm_gpiom_port port, uint8_t pin,
				   bool *locked)
{
	const struct hpm_gpiom_config *config = dev->config;
	int ret;

	if (locked == NULL) {
		return -EINVAL;
	}

	ret = hpm_gpiom_validate(port, pin);
	if (ret < 0) {
		return ret;
	}

	*locked = gpiom_pin_is_locked(config->base, port, pin);

	return 0;
}

static DEVICE_API(hpm_gpiom, hpm_gpiom_api) = {
	.set_controller = hpm_gpiom_drv_set_controller,
	.get_controller = hpm_gpiom_drv_get_controller,
	.set_visibility = hpm_gpiom_drv_set_visibility,
	.get_visibility = hpm_gpiom_drv_get_visibility,
	.lock = hpm_gpiom_drv_lock,
	.is_locked = hpm_gpiom_drv_is_locked,
};

#define HPM_GPIOM_INIT(n)                                                                          \
	static const struct hpm_gpiom_config hpm_gpiom_config_##n = {                              \
		.base = (GPIOM_Type *)DT_INST_REG_ADDR(n),                                         \
	};                                                                                         \
                                                                                                   \
	static struct hpm_gpiom_data hpm_gpiom_data_##n;                                           \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, &hpm_gpiom_data_##n, &hpm_gpiom_config_##n,           \
			      POST_KERNEL, CONFIG_GPIOM_HPMICRO_INIT_PRIORITY, &hpm_gpiom_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_GPIOM_INIT)
