/*
 * Copyright (c) 2022-2026 HPMicro
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_gpio

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <hpm_common.h>
#include <hpm_soc.h>
#include <hpm_gpio_drv.h>

struct hpm_gpio_config {
	GPIO_Type *base;
	uint32_t port;
};

static int hpm_gpio_pin_configure(const struct device *dev, gpio_pin_t pin,
				  gpio_flags_t flags)
{
	const struct hpm_gpio_config *config = dev->config;

	if ((flags & GPIO_OUTPUT) != 0U) {
		if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0U) {
			gpio_write_pin(config->base, config->port, pin, 1);
		} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0U) {
			gpio_write_pin(config->base, config->port, pin, 0);
		}
		gpio_set_pin_output(config->base, config->port, pin);
	} else if ((flags & GPIO_INPUT) != 0U) {
		gpio_set_pin_input(config->base, config->port, pin);
	}

	return 0;
}

static int hpm_gpio_port_get_raw(const struct device *dev, gpio_port_value_t *value)
{
	const struct hpm_gpio_config *config = dev->config;

	*value = config->base->DI[config->port].VALUE;
	return 0;
}

static int hpm_gpio_port_set_masked_raw(const struct device *dev,
					gpio_port_pins_t mask,
					gpio_port_value_t value)
{
	const struct hpm_gpio_config *config = dev->config;

	config->base->DO[config->port].VALUE =
		(config->base->DO[config->port].VALUE & ~mask) | (value & mask);
	return 0;
}

static int hpm_gpio_port_set_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	const struct hpm_gpio_config *config = dev->config;

	config->base->DO[config->port].SET = pins;
	return 0;
}

static int hpm_gpio_port_clear_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	const struct hpm_gpio_config *config = dev->config;

	config->base->DO[config->port].CLEAR = pins;
	return 0;
}

static int hpm_gpio_port_toggle_bits(const struct device *dev, gpio_port_pins_t pins)
{
	const struct hpm_gpio_config *config = dev->config;

	config->base->DO[config->port].TOGGLE = pins;
	return 0;
}

static const struct gpio_driver_api hpm_gpio_api = {
	.pin_configure = hpm_gpio_pin_configure,
	.port_get_raw = hpm_gpio_port_get_raw,
	.port_set_masked_raw = hpm_gpio_port_set_masked_raw,
	.port_set_bits_raw = hpm_gpio_port_set_bits_raw,
	.port_clear_bits_raw = hpm_gpio_port_clear_bits_raw,
	.port_toggle_bits = hpm_gpio_port_toggle_bits,
};

#define HPM_GPIO_INIT(n)						\
	static const struct hpm_gpio_config hpm_gpio_config_##n = {	\
		.base = (GPIO_Type *)DT_INST_REG_ADDR(n),		\
		.port = DT_INST_PROP(n, hpmicro_gpio_port),		\
	};								\
									\
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, &hpm_gpio_config_##n, \
			      PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY,	\
			      &hpm_gpio_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_GPIO_INIT)
