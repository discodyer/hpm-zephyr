/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

int main(void)
{
	const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	int64_t next_heartbeat = 0;
	uint32_t heartbeat = 0;
	int ret;

	if (!device_is_ready(console) || (led.port != NULL && !gpio_is_ready_dt(&led))) {
		printk("Bring-up: console or LED device is not ready\n");
		return -ENODEV;
	}

	if (led.port != NULL) {
		ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			printk("Bring-up: LED configuration failed: %d\n", ret);
			return ret;
		}
	}

	printk("HPM Zephyr bring-up: %s; UART echo and 1 s heartbeat\n", CONFIG_BOARD);

	while (true) {
		unsigned char c;
		int64_t now = k_uptime_get();

		while (uart_poll_in(console, &c) == 0) {
			uart_poll_out(console, c);
		}

		if (now >= next_heartbeat) {
			if (led.port != NULL) {
				ret = gpio_pin_toggle_dt(&led);
				if (ret < 0) {
					printk("Bring-up: LED toggle failed: %d\n", ret);
					return ret;
				}
			}
			printk("heartbeat=%u uptime=%lld ms cycles=%u\n", heartbeat++,
			       (long long)now, k_cycle_get_32());
			next_heartbeat = now + MSEC_PER_SEC;
		}

		k_sleep(K_MSEC(10));
	}
}
