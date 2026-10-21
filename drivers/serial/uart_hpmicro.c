/*
 * Copyright (c) 2022-2026 HPMicro
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_uart

#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <hpm_common.h>
#include <hpm_soc.h>
#include <hpm_uart_drv.h>
#include <hpm_clock_drv.h>

struct hpm_zephyr_uart_config {
	UART_Type *base;
	clock_name_t clock_name;
	uint32_t current_speed;
};

static int hpm_uart_init(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;

	clock_add_to_group(config->clock_name, 0);
	return 0;
}

static int hpm_uart_poll_in(const struct device *dev, unsigned char *c)
{
	const struct hpm_zephyr_uart_config *config = dev->config;

	if (uart_check_status(config->base, uart_stat_data_ready)) {
		*c = uart_read_byte(config->base);
		return 0;
	}

	return -1;
}

static void hpm_uart_poll_out(const struct device *dev, unsigned char c)
{
	const struct hpm_zephyr_uart_config *config = dev->config;

	while (!uart_check_status(config->base, uart_stat_tx_slot_avail)) {
	}

	uart_write_byte(config->base, c);
}

static const struct uart_driver_api hpm_uart_api = {
	.poll_in = hpm_uart_poll_in,
	.poll_out = hpm_uart_poll_out,
};

#define HPM_UART_INIT(n)						\
	static const struct hpm_zephyr_uart_config hpm_uart_config_##n = { \
		.base = (UART_Type *)DT_INST_REG_ADDR(n),		\
		.clock_name = DT_INST_PROP(n, clock_name),		\
		.current_speed = DT_INST_PROP(n, current_speed),	\
	};								\
									\
	DEVICE_DT_INST_DEFINE(n, hpm_uart_init, NULL, NULL,		\
			      &hpm_uart_config_##n, PRE_KERNEL_1,	\
			      CONFIG_SERIAL_INIT_PRIORITY, &hpm_uart_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_UART_INIT)
