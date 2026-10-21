/*
 * Copyright (c) 2022-2026 HPMicro
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_gpio

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/irq.h>
#include <hpm_common.h>
#include <hpm_gpio_drv.h>
#include <hpm_soc.h>

struct hpm_gpio_config {
	struct gpio_driver_config common;
	GPIO_Type *base;
	uint32_t port;
	void (*irq_config_func)(const struct device *dev);
};

struct hpm_gpio_data {
	struct gpio_driver_data common;
	sys_slist_t callbacks;
};

static int hpm_gpio_pin_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
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

static int hpm_gpio_port_set_masked_raw(const struct device *dev, gpio_port_pins_t mask,
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

static int hpm_gpio_pin_interrupt_configure(const struct device *dev, gpio_pin_t pin,
					    enum gpio_int_mode mode, enum gpio_int_trig trig)
{
	const struct hpm_gpio_config *config = dev->config;
	gpio_interrupt_trigger_t trigger;

	if (mode == GPIO_INT_MODE_DISABLED) {
		gpio_disable_pin_interrupt(config->base, config->port, pin);
		return 0;
	}

	if (mode == GPIO_INT_MODE_LEVEL) {
		switch (trig) {
		case GPIO_INT_TRIG_LOW:
			trigger = gpio_interrupt_trigger_level_low;
			break;
		case GPIO_INT_TRIG_HIGH:
			trigger = gpio_interrupt_trigger_level_high;
			break;
		default:
			return -ENOTSUP;
		}
	} else if (mode == GPIO_INT_MODE_EDGE) {
		switch (trig) {
		case GPIO_INT_TRIG_LOW:
			trigger = gpio_interrupt_trigger_edge_falling;
			break;
		case GPIO_INT_TRIG_HIGH:
			trigger = gpio_interrupt_trigger_edge_rising;
			break;
		case GPIO_INT_TRIG_BOTH:
			trigger = gpio_interrupt_trigger_edge_both;
			break;
		default:
			return -ENOTSUP;
		}
	} else {
		return -ENOTSUP;
	}

	gpio_clear_pin_interrupt_flag(config->base, config->port, pin);
	gpio_config_pin_interrupt(config->base, config->port, pin, trigger);
	gpio_enable_pin_interrupt(config->base, config->port, pin);

	return 0;
}

static int hpm_gpio_manage_callback(const struct device *dev, struct gpio_callback *callback,
				    bool set)
{
	struct hpm_gpio_data *data = dev->data;

	return gpio_manage_callback(&data->callbacks, callback, set);
}

static void hpm_gpio_isr(const struct device *dev)
{
	const struct hpm_gpio_config *config = dev->config;
	struct hpm_gpio_data *data = dev->data;
	uint32_t status;

	status = config->base->IF[config->port].VALUE;
	status &= config->common.port_pin_mask;
	config->base->IF[config->port].VALUE = status;

	gpio_fire_callbacks(&data->callbacks, dev, status);
}

static const struct gpio_driver_api hpm_gpio_api = {
	.pin_configure = hpm_gpio_pin_configure,
	.port_get_raw = hpm_gpio_port_get_raw,
	.port_set_masked_raw = hpm_gpio_port_set_masked_raw,
	.port_set_bits_raw = hpm_gpio_port_set_bits_raw,
	.port_clear_bits_raw = hpm_gpio_port_clear_bits_raw,
	.port_toggle_bits = hpm_gpio_port_toggle_bits,
	.pin_interrupt_configure = hpm_gpio_pin_interrupt_configure,
	.manage_callback = hpm_gpio_manage_callback,
};

#define HPM_GPIO_IRQ_CONFIG(n)                                                                     \
	static void hpm_gpio_irq_config_##n(const struct device *dev)                              \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_gpio_isr,               \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}

#define HPM_GPIO_INIT(n)                                                                           \
	HPM_GPIO_IRQ_CONFIG(n)                                                                     \
                                                                                                   \
	static struct hpm_gpio_data hpm_gpio_data_##n;                                             \
                                                                                                   \
	static const struct hpm_gpio_config hpm_gpio_config_##n = {                                \
		.common =                                                                          \
			{                                                                          \
				.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_DT_INST(n),               \
			},                                                                         \
		.base = (GPIO_Type *)DT_INST_REG_ADDR(n),                                          \
		.port = DT_INST_PROP(n, hpmicro_gpio_port),                                        \
		.irq_config_func = hpm_gpio_irq_config_##n,                                        \
	};                                                                                         \
                                                                                                   \
	static int hpm_gpio_init_##n(const struct device *dev)                                     \
	{                                                                                          \
		const struct hpm_gpio_config *config = dev->config;                                \
                                                                                                   \
		config->irq_config_func(dev);                                                      \
                                                                                                   \
		return 0;                                                                          \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_gpio_init_##n, NULL, &hpm_gpio_data_##n,                      \
			      &hpm_gpio_config_##n, PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY,       \
			      &hpm_gpio_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_GPIO_INIT)
