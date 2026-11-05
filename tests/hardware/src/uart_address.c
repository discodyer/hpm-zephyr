/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <drivers/serial/hpmicro_uart.h>

#define UART_ADDRESS_RX DT_NODELABEL(uart1)
#define UART_ADDRESS_TX DT_NODELABEL(uart2)
#define ADDRESS_0       0x55U
#define ADDRESS_1       0x77U
#define WRONG_ADDRESS   0x66U
#define FRAME_DATA_LEN  8U

static const struct device *const rx = DEVICE_DT_GET(UART_ADDRESS_RX);
static const struct device *const tx = DEVICE_DT_GET(UART_ADDRESS_TX);
static const struct uart_config uart_cfg = {
	.baudrate = 115200,
	.parity = UART_CFG_PARITY_NONE,
	.stop_bits = UART_CFG_STOP_BITS_1,
	.data_bits = UART_CFG_DATA_BITS_8,
	.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
};

static const uint8_t frame0[FRAME_DATA_LEN] = {
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
};
static const uint8_t frame1[FRAME_DATA_LEN] = {
	0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
};

static void flush_rx(void)
{
	unsigned char value;
	int guard = 64;

	while ((uart_poll_in(rx, &value) == 0) && (guard-- > 0)) {
	}
}

static void send_frame(uint8_t address, const uint8_t *data)
{
	uart_poll_out(tx, address);
	for (size_t i = 0; i < FRAME_DATA_LEN; i++) {
		uart_poll_out(tx, data[i]);
	}
	/* 8N1 with nine-bit address flag takes under 1 ms at 115200 baud. */
	k_sleep(K_MSEC(5));
}

static void assert_no_frame(void)
{
	unsigned char value;

	zassert_not_equal(uart_poll_in(rx, &value), 0,
			  "non-matching address data was not filtered");
}

static void assert_frame(uint8_t address, const uint8_t *data)
{
	unsigned char value;

	zassert_ok(uart_poll_in(rx, &value), "missing address byte");
	zassert_equal(value, address, "address byte mismatch: 0x%02x", value);
	for (size_t i = 0; i < FRAME_DATA_LEN; i++) {
		zassert_ok(uart_poll_in(rx, &value), "missing data byte %u", i);
		zassert_equal(value, data[i], "data mismatch at %u: 0x%02x", i, value);
	}
	assert_no_frame();
}

static void uart_address_before(void *fixture)
{
	const struct hpm_uart_address_match_config match = {
		.enable = true,
		.address0 = ADDRESS_0,
		.address0_enable = true,
		.address1 = ADDRESS_1,
		.address1_enable = true,
		.nine_bit = true,
	};

	ARG_UNUSED(fixture);
	zassert_true(device_is_ready(rx));
	zassert_true(device_is_ready(tx));
	zassert_ok(uart_configure(rx, &uart_cfg));
	zassert_ok(uart_configure(tx, &uart_cfg));
	zassert_ok(hpm_uart_configure_address_match(rx, &match));
	zassert_ok(hpm_uart_configure_address_match(tx, &match));
	flush_rx();
}

static void uart_address_after(void *fixture)
{
	const struct hpm_uart_address_match_config disabled = {
		.enable = false,
	};

	ARG_UNUSED(fixture);
	(void)hpm_uart_configure_address_match(rx, &disabled);
	(void)hpm_uart_configure_address_match(tx, &disabled);
}

ZTEST(hpm_uart_address, test_nine_bit_address_filter)
{
	bool matched;

	zassert_equal(hpm_uart_configure_address_match(rx,
						       &(struct hpm_uart_address_match_config){
							       .enable = false,
							       .nine_bit = true,
						       }),
		      -EINVAL);

	send_frame(WRONG_ADDRESS, frame0);
	zassert_ok(hpm_uart_get_address_match(rx, &matched));
	zassert_false(matched, "wrong address set match flag");
	assert_no_frame();

	send_frame(ADDRESS_0, frame0);
	zassert_ok(hpm_uart_get_address_match(rx, &matched));
	zassert_true(matched, "address0 did not match");
	zassert_ok(hpm_uart_get_address_match_idle(rx, &matched));
	zassert_true(matched, "address0 idle event did not assert");
	assert_frame(ADDRESS_0, frame0);
	zassert_ok(hpm_uart_get_address_match(rx, &matched));
	zassert_false(matched, "address-match flag did not clear");

	send_frame(ADDRESS_1, frame1);
	zassert_ok(hpm_uart_get_address_match(rx, &matched));
	zassert_true(matched, "address1 did not match");
	assert_frame(ADDRESS_1, frame1);
	zassert_equal(uart_err_check(rx), 0);
}

ZTEST_SUITE(hpm_uart_address, NULL, NULL, uart_address_before, uart_address_after, NULL);
