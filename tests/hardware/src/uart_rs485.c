/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#define UART_RS485_DATA_LEN 64U
#define EDGE_TIMEOUT_MS     100
#define MAX_DE_SLACK_US     500U

PINCTRL_DT_DEFINE(DT_NODELABEL(hw_fixture));

static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(uart1));
static const struct gpio_dt_spec de_mon = GPIO_DT_SPEC_GET(DT_NODELABEL(hw_fixture), de_gpios);
static const struct gpio_dt_spec tx_mon = GPIO_DT_SPEC_GET(DT_NODELABEL(hw_fixture), tx_gpios);
static struct gpio_callback edge_cb;
static K_SEM_DEFINE(de_fell, 0, 1);
static atomic_t de_rise_cycle;
static atomic_t de_fall_cycle;
static atomic_t tx_first_low_cycle;
static atomic_t tx_last_high_cycle;
static atomic_t de_rise_count;
static atomic_t de_fall_count;
static atomic_t tx_low_count;
static uint8_t tx_data[UART_RS485_DATA_LEN];

static void rs485_edge_cb(const struct device *port, struct gpio_callback *cb,
			  gpio_port_pins_t pins)
{
	uint32_t now = k_cycle_get_32();

	ARG_UNUSED(port);
	ARG_UNUSED(cb);

	if ((pins & BIT(de_mon.pin)) != 0U) {
		if (gpio_pin_get_dt(&de_mon) != 0) {
			if (atomic_get(&de_rise_count) == 0) {
				atomic_set(&de_rise_cycle, now);
			}
			atomic_inc(&de_rise_count);
		} else {
			atomic_set(&de_fall_cycle, now);
			atomic_inc(&de_fall_count);
			k_sem_give(&de_fell);
		}
	}

	if ((pins & BIT(tx_mon.pin)) != 0U) {
		if (gpio_pin_get_dt(&tx_mon) == 0) {
			if (atomic_get(&tx_first_low_cycle) == 0) {
				atomic_set(&tx_first_low_cycle, now);
			}
			atomic_inc(&tx_low_count);
		} else {
			atomic_set(&tx_last_high_cycle, now);
		}
	}
}

static uint32_t cycles_to_us_ceil(uint32_t cycles)
{
	return k_cyc_to_us_ceil32(cycles);
}

static void uart_rs485_before(void *fixture)
{
	struct uart_config config;
	struct uart_config readback;

	ARG_UNUSED(fixture);
	zassert_true(device_is_ready(uart));
	zassert_ok(uart_config_get(uart, &config));
	config.flow_ctrl = UART_CFG_FLOW_CTRL_RS485;
	zassert_ok(uart_configure(uart, &config));
	zassert_ok(uart_config_get(uart, &readback));
	zassert_equal(readback.flow_ctrl, UART_CFG_FLOW_CTRL_RS485);

	zassert_ok(pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(hw_fixture)),
				       PINCTRL_STATE_DEFAULT));
	zassert_ok(gpio_pin_configure_dt(&de_mon, GPIO_INPUT));
	zassert_ok(gpio_pin_configure_dt(&tx_mon, GPIO_INPUT));
	zassert_ok(gpio_pin_interrupt_configure_dt(&de_mon, GPIO_INT_DISABLE));
	zassert_ok(gpio_pin_interrupt_configure_dt(&tx_mon, GPIO_INT_DISABLE));
	zassert_equal(gpio_pin_get_dt(&de_mon), 0, "DE must idle low");
	zassert_equal(gpio_pin_get_dt(&tx_mon), 1, "TX must idle high");

	atomic_clear(&de_rise_cycle);
	atomic_clear(&de_fall_cycle);
	atomic_clear(&tx_first_low_cycle);
	atomic_clear(&tx_last_high_cycle);
	atomic_clear(&de_rise_count);
	atomic_clear(&de_fall_count);
	atomic_clear(&tx_low_count);
	k_sem_reset(&de_fell);
	gpio_init_callback(&edge_cb, rs485_edge_cb, BIT(de_mon.pin) | BIT(tx_mon.pin));
	zassert_ok(gpio_add_callback(de_mon.port, &edge_cb));
	zassert_ok(gpio_pin_interrupt_configure_dt(&de_mon, GPIO_INT_EDGE_BOTH));
	zassert_ok(gpio_pin_interrupt_configure_dt(&tx_mon, GPIO_INT_EDGE_BOTH));

	for (size_t i = 0; i < sizeof(tx_data); i++) {
		tx_data[i] = (uint8_t)(i ^ 0x5aU);
	}
}

static void uart_rs485_after(void *fixture)
{
	struct uart_config config;

	ARG_UNUSED(fixture);
	(void)gpio_pin_interrupt_configure_dt(&de_mon, GPIO_INT_DISABLE);
	(void)gpio_pin_interrupt_configure_dt(&tx_mon, GPIO_INT_DISABLE);
	(void)gpio_remove_callback(de_mon.port, &edge_cb);
	if (uart_config_get(uart, &config) == 0) {
		config.flow_ctrl = UART_CFG_FLOW_CTRL_NONE;
		(void)uart_configure(uart, &config);
	}
}

ZTEST(hpm_uart_rs485, test_de_wraps_tx_burst)
{
	const uint32_t slack_cycles = k_us_to_cyc_ceil32(MAX_DE_SLACK_US);
	const uint32_t expected_us =
		DIV_ROUND_UP(UART_RS485_DATA_LEN * 10U * USEC_PER_SEC, 115200U);
	uint32_t de_us;
	uint32_t tx_us;

	for (size_t i = 0; i < sizeof(tx_data); i++) {
		uart_poll_out(uart, tx_data[i]);
	}

	zassert_ok(k_sem_take(&de_fell, K_MSEC(EDGE_TIMEOUT_MS)), "DE falling edge timeout");
	k_busy_wait(50);

	zassert_equal(atomic_get(&de_rise_count), 1, "DE assertion count");
	zassert_equal(atomic_get(&de_fall_count), 1, "DE release count");
	zassert_true(atomic_get(&tx_low_count) > 1U, "TX activity missing");

	de_us = cycles_to_us_ceil(atomic_get(&de_fall_cycle) - atomic_get(&de_rise_cycle));
	tx_us = cycles_to_us_ceil(atomic_get(&tx_last_high_cycle) -
				  atomic_get(&tx_first_low_cycle));
	zassert_within((int32_t)tx_us, (int32_t)expected_us, 1000, "Unexpected TX burst duration");
	zassert_true((int32_t)(atomic_get(&tx_first_low_cycle) - atomic_get(&de_rise_cycle)) >=
			     -(int32_t)slack_cycles,
		     "DE asserted after TX start");
	zassert_true((int32_t)(atomic_get(&de_fall_cycle) - atomic_get(&tx_last_high_cycle)) >=
			     -(int32_t)slack_cycles,
		     "DE released before TX end");
	zassert_true(de_us >= tx_us, "DE pulse shorter than TX burst");
	zassert_true(de_us - tx_us <= MAX_DE_SLACK_US, "DE release is too late");
}

ZTEST_SUITE(hpm_uart_rs485, NULL, NULL, uart_rs485_before, uart_rs485_after, NULL);
