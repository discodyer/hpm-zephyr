/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/ztest.h>

#include <drivers/serial/hpmicro_uart.h>

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

ZTEST(hpm_uart, test_address_match_extension_validation)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(uart0));
	const struct hpm_uart_address_match_config invalid_8bit = {
		.enable = true,
		.address0 = 0x80U,
		.address0_enable = true,
	};
	const struct hpm_uart_address_match_config invalid_9bit = {
		.address0 = 0x40U,
		.address0_enable = true,
		.nine_bit = true,
	};
	bool matched;

	zassert_true(DEVICE_API_IS(hpm_uart, dev));
	zassert_equal(hpm_uart_configure_address_match(dev, NULL), -EINVAL);
	zassert_equal(hpm_uart_configure_address_match(dev, &invalid_8bit), -EINVAL);
	zassert_equal(hpm_uart_configure_address_match(dev, &invalid_9bit), -EINVAL);
	zassert_equal(hpm_uart_get_address_match(dev, NULL), -EINVAL);
	zassert_equal(hpm_uart_get_address_match_idle(dev, NULL), -EINVAL);
	zassert_ok(hpm_uart_get_address_match(dev, &matched));
	zassert_ok(hpm_uart_get_address_match_idle(dev, &matched));
}
#endif

#if defined(CONFIG_UART_ASYNC_API) && DT_NODE_HAS_STATUS(DT_NODELABEL(uart0), okay)
ZTEST(hpm_uart, test_async_operations_reject_invalid_arguments)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(uart0));
	static uint8_t buffer[8];

	zassert_ok(uart_callback_set(dev, NULL, NULL));
	zassert_equal(uart_tx(dev, NULL, sizeof(buffer), SYS_FOREVER_US), -EINVAL);
	zassert_equal(uart_rx_enable(dev, NULL, sizeof(buffer), SYS_FOREVER_US), -EINVAL);
	zassert_equal(uart_tx_abort(dev), -EFAULT);
	zassert_equal(uart_rx_disable(dev), -EFAULT);
}
#endif

ZTEST_SUITE(hpm_uart, NULL, NULL, NULL, NULL, NULL);
