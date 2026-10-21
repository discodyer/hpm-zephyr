/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/ztest.h>
#include <dt-bindings/clock/hpm5361-clocks.h>

#define UART_NODE  DT_NODELABEL(uart0)
#define CLOCK_NODE DT_NODELABEL(clk)
#define GPIO_NODE  DT_NODELABEL(gpioa)

BUILD_ASSERT(DT_NODE_HAS_STATUS(UART_NODE, okay));
BUILD_ASSERT(DT_NODE_HAS_STATUS(CLOCK_NODE, okay));
BUILD_ASSERT(DT_NODE_HAS_STATUS(GPIO_NODE, okay));
BUILD_ASSERT(DT_CLOCKS_CELL_BY_IDX(UART_NODE, 0, name) == CLOCK_UART0);
BUILD_ASSERT(DT_PROP(GPIO_NODE, ngpios) == 32);

ZTEST(hpm5300evk, test_devices_ready)
{
	zassert_true(device_is_ready(DEVICE_DT_GET(DT_NODELABEL(clk))));
	zassert_true(device_is_ready(DEVICE_DT_GET(DT_NODELABEL(uart0))));
	zassert_true(device_is_ready(DEVICE_DT_GET(DT_NODELABEL(gpioa))));
}

ZTEST(hpm5300evk, test_clock_rate)
{
	const struct device *clk = DEVICE_DT_GET(DT_NODELABEL(clk));
	uint32_t rate = 0U;

	zassert_ok(clock_control_get_rate(
		clk, (clock_control_subsys_t)DT_PROP(UART_NODE, clock_name), &rate));
	zassert_equal(rate, 24000000U);
}

ZTEST(hpm5300evk, test_gpio_interrupt_config)
{
	const struct device *gpio = DEVICE_DT_GET(DT_NODELABEL(gpioa));

	zassert_ok(gpio_pin_configure(gpio, 0, GPIO_INPUT));
	zassert_ok(gpio_pin_interrupt_configure(gpio, 0, GPIO_INT_EDGE_RISING));
	zassert_ok(gpio_pin_interrupt_configure(gpio, 0, GPIO_INT_EDGE_BOTH));
	zassert_ok(gpio_pin_interrupt_configure(gpio, 0, GPIO_INT_DISABLE));
}

ZTEST_SUITE(hpm5300evk, NULL, NULL, NULL, NULL, NULL);
