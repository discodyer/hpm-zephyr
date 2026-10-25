/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/ztest.h>

#if DT_NODE_HAS_STATUS(DT_NODELABEL(uart0), okay)
ZTEST(hpm_uart, test_runtime_configuration_api)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(uart0));
	struct uart_config current;
	struct uart_config requested = {
		.baudrate = 115200U,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};

	zassert_ok(uart_configure(dev, &requested));
	zassert_ok(uart_config_get(dev, &current));
	zassert_equal(current.baudrate, requested.baudrate);
	zassert_equal(current.parity, requested.parity);
	zassert_equal(current.stop_bits, requested.stop_bits);
	zassert_equal(current.data_bits, requested.data_bits);

	requested.flow_ctrl = UART_CFG_FLOW_CTRL_RTS_CTS;
	zassert_ok(uart_configure(dev, &requested));
	zassert_ok(uart_config_get(dev, &current));
	zassert_equal(current.flow_ctrl, UART_CFG_FLOW_CTRL_RTS_CTS);

	requested.flow_ctrl = UART_CFG_FLOW_CTRL_RS485;
	zassert_ok(uart_configure(dev, &requested));
	zassert_ok(uart_config_get(dev, &current));
	zassert_equal(current.flow_ctrl, UART_CFG_FLOW_CTRL_RS485);

	requested.flow_ctrl = UART_CFG_FLOW_CTRL_DTR_DSR;
	zassert_equal(uart_configure(dev, &requested), -ENOTSUP);
}
#endif

ZTEST_SUITE(hpm_uart, NULL, NULL, NULL, NULL, NULL);
