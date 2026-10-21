/*
 * Copyright (c) 2022-2026 HPMicro
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_uart

#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <errno.h>
#include <hpm_common.h>
#include <hpm_soc.h>
#include <hpm_uart_drv.h>
#include <hpm_clock_drv.h>

struct hpm_zephyr_uart_config {
	UART_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	clock_name_t clock_name;
	clock_source_t clock_src;
	uint32_t clock_div;
	uint32_t current_speed;
	const struct pinctrl_dev_config *pincfg;
};

static int hpm_uart_init(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	uart_config_t uart_config;
	uint32_t clock_freq;
	hpm_stat_t status;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	status = clock_set_source_divider(config->clock_name, config->clock_src, config->clock_div);
	if (status != status_success) {
		return -EINVAL;
	}

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		return ret;
	}

	ret = clock_control_get_rate(config->clock_dev, config->clock_subsys, &clock_freq);
	if (ret < 0) {
		return ret;
	}

	uart_default_config(config->base, &uart_config);
	uart_config.src_freq_in_hz = clock_freq;
	uart_config.baudrate = config->current_speed;

	status = uart_init(config->base, &uart_config);
	if (status != status_success) {
		return -EINVAL;
	}

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
	PINCTRL_DT_INST_DEFINE(n);					\
									\
	static const struct hpm_zephyr_uart_config hpm_uart_config_##n = { \
		.base = (UART_Type *)DT_INST_REG_ADDR(n),		\
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),	\
		.clock_subsys =						\
			(clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name), \
		.clock_name = DT_INST_PROP(n, clock_name),		\
		.clock_src = DT_INST_PROP(n, clock_src),		\
		.clock_div = DT_INST_CLOCKS_CELL_BY_IDX(n, 0, divider),	\
		.current_speed = DT_INST_PROP(n, current_speed),	\
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),		\
	};								\
									\
	DEVICE_DT_INST_DEFINE(n, hpm_uart_init, NULL, NULL,		\
			      &hpm_uart_config_##n, PRE_KERNEL_1,	\
			      CONFIG_SERIAL_INIT_PRIORITY, &hpm_uart_api)

DT_INST_FOREACH_STATUS_OKAY(HPM_UART_INIT);
