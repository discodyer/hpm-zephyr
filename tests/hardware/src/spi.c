/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * Copyright (c) 2026 Cody Gu <gujiaqi@iscas.ac.cn>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ztest.h>

static const struct device *const spi = DEVICE_DT_GET(DT_NODELABEL(spi1));
static uint8_t tx[256] __aligned(32);
static uint8_t rx[256] __aligned(32);
static struct spi_config config = {
	.frequency = 1000000,
	.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8),
	.slave = 0,
	.cs.gpio = GPIO_DT_SPEC_GET_BY_IDX(DT_NODELABEL(spi1), cs_gpios, 0),
	.cs.delay = 1,
	.cs.cs_is_gpio = true,
};
static K_SEM_DEFINE(done, 0, 1);
static int async_result;
static bool async_in_isr;

static void spi_fill_pattern(uint8_t *buffer, size_t length, uint8_t seed)
{
	for (size_t i = 0; i < length; ++i) {
		buffer[i] = (uint8_t)((i * 37U) ^ seed);
	}
}

static void spi_done(const struct device *dev, int result, void *data)
{
	ARG_UNUSED(dev);
	async_result = result;
	async_in_isr = k_is_in_isr();
	k_sem_give(data);
}

static void spi_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)spi_release(spi, &config);
	(void)gpio_pin_set_dt(&config.cs.gpio, 0);
	k_sem_reset(&done);
}

static void spi_check_sync_transfer(uint32_t word_size, size_t length)
{
	struct spi_buf tx_bufs[] = {
		{.buf = tx, .len = length},
	};
	struct spi_buf rx_bufs[] = {
		{.buf = rx, .len = length},
	};
	struct spi_buf_set tx_set = {
		.buffers = tx_bufs,
		.count = ARRAY_SIZE(tx_bufs),
	};
	struct spi_buf_set rx_set = {
		.buffers = rx_bufs,
		.count = ARRAY_SIZE(rx_bufs),
	};
	int ret;

	zassert_equal(length % (word_size / BITS_PER_BYTE), 0U,
		      "%u-bit transfer length is not aligned", word_size);

	config.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(word_size);
	spi_fill_pattern(tx, length, (uint8_t)word_size);
	memset(rx, 0, length);

	ret = spi_transceive(spi, &config, &tx_set, &rx_set);
	zassert_ok(ret, "SPI %u-bit synchronous transfer failed: %d", word_size, ret);
	zassert_equal(gpio_pin_get_dt(&config.cs.gpio), 0,
		      "SPI %u-bit transfer left GPIO CS asserted", word_size);
	zassert_mem_equal(tx, rx, length, "SPI %u-bit synchronous loopback", word_size);
}

static void spi_check_async_transfer(uint32_t word_size, size_t block_length)
{
	struct spi_buf tx_bufs[] = {
		{.buf = tx, .len = block_length},
		{.buf = tx + block_length, .len = block_length},
	};
	struct spi_buf rx_bufs[] = {
		{.buf = rx, .len = block_length},
		{.buf = rx + block_length, .len = block_length},
	};
	struct spi_buf_set tx_set = {
		.buffers = tx_bufs,
		.count = ARRAY_SIZE(tx_bufs),
	};
	struct spi_buf_set rx_set = {
		.buffers = rx_bufs,
		.count = ARRAY_SIZE(rx_bufs),
	};
	int ret;

	config.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(word_size);
	spi_fill_pattern(tx, block_length * ARRAY_SIZE(tx_bufs), (uint8_t)word_size);
	memset(rx, 0, block_length * ARRAY_SIZE(rx_bufs));
	k_sem_reset(&done);
	async_in_isr = false;
	async_result = -EIO;

	ret = spi_transceive_cb(spi, &config, &tx_set, &rx_set, spi_done, &done);
	zassert_ok(ret, "SPI %u-bit asynchronous submit failed: %d", word_size, ret);
	zassert_ok(k_sem_take(&done, K_SECONDS(2)), "SPI async transfer timeout");
	zassert_ok(async_result, "SPI %u-bit asynchronous transfer failed: %d", word_size,
		   async_result);
	zassert_true(async_in_isr, "SPI %u-bit completion did not run in an ISR", word_size);
	zassert_mem_equal(tx, rx, block_length * ARRAY_SIZE(tx_bufs),
			  "SPI %u-bit linked-block DMA loopback", word_size);
	zassert_equal(gpio_pin_get_dt(&config.cs.gpio), 0,
		      "SPI %u-bit DMA transfer left GPIO CS asserted", word_size);
}

ZTEST(hpm_spi_loop, test_all_modes_sync_and_linked_dma)
{
	struct spi_buf tx_bufs[2] = {{.buf = tx, .len = 96}, {.buf = tx + 96, .len = 160}};
	struct spi_buf rx_bufs[2] = {{.buf = rx, .len = 128}, {.buf = rx + 128, .len = 128}};
	struct spi_buf_set tx_set = {.buffers = tx_bufs, .count = 2};
	struct spi_buf_set rx_set = {.buffers = rx_bufs, .count = 2};
	int ret;

	zassert_true(device_is_ready(spi));
	for (size_t i = 0; i < sizeof(tx); ++i) {
		tx[i] = (i * 37) ^ 0x5a;
	}
	for (int mode = 0; mode < 4; ++mode) {
		config.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) |
				   ((mode & 1) ? SPI_MODE_CPHA : 0) |
				   ((mode & 2) ? SPI_MODE_CPOL : 0);
		memset(rx, 0, sizeof(rx));
		ret = spi_transceive(spi, &config, &tx_set, &rx_set);
		zassert_equal(ret, 0, "SPI synchronous mode %d failed: %d", mode, ret);
		zassert_equal(gpio_pin_get_dt(&config.cs.gpio), 0, "SPI sync CS remained asserted");
		for (size_t i = 0; i < sizeof(tx); ++i) {
			if (tx[i] != rx[i]) {
				printk("SPI sync mode %d mismatch at %u: tx=%02x rx=%02x\n", mode,
				       (uint32_t)i, tx[i], rx[i]);
				break;
			}
		}
		zassert_mem_equal(tx, rx, sizeof(tx), "SPI synchronous loopback mode %d", mode);
		memset(rx, 0, sizeof(rx));
		async_in_isr = false;
		zassert_ok(spi_transceive_cb(spi, &config, &tx_set, &rx_set, spi_done, &done));
		zassert_ok(k_sem_take(&done, K_SECONDS(2)), "SPI async/DMA completion timeout");
		zassert_ok(async_result);
		zassert_true(async_in_isr, "SPI async completion did not run in an ISR");
		zassert_mem_equal(tx, rx, sizeof(tx), "SPI async linked DMA mode %d", mode);
		zassert_equal(gpio_pin_get_dt(&config.cs.gpio), 0, "CS remained asserted");
	}
}

ZTEST(hpm_spi_loop, test_sync_word_sizes)
{
	zassert_true(device_is_ready(spi));

	spi_check_sync_transfer(16, 128);
	spi_check_sync_transfer(24, 96);
	spi_check_sync_transfer(32, 128);
}

ZTEST(hpm_spi_loop, test_async_linked_dma_word_sizes)
{
	zassert_true(device_is_ready(spi));

	spi_check_async_transfer(16, 64);
	spi_check_async_transfer(32, 64);
}

ZTEST(hpm_spi_loop, test_gpio_cs_hold_on_cs)
{
	struct spi_buf tx_bufs[] = {{.buf = tx, .len = 32}};
	struct spi_buf rx_bufs[] = {{.buf = rx, .len = 32}};
	struct spi_buf_set tx_set = {
		.buffers = tx_bufs,
		.count = ARRAY_SIZE(tx_bufs),
	};
	struct spi_buf_set rx_set = {
		.buffers = rx_bufs,
		.count = ARRAY_SIZE(rx_bufs),
	};
	int ret;

	zassert_true(device_is_ready(spi));
	spi_fill_pattern(tx, sizeof(tx), 0xa5);
	memset(rx, 0, sizeof(rx));

	config.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_HOLD_ON_CS;
	ret = spi_transceive(spi, &config, &tx_set, &rx_set);
	zassert_ok(ret, "SPI hold-on-CS transfer failed: %d", ret);
	zassert_mem_equal(tx, rx, 32, "SPI hold-on-CS loopback");
	zassert_equal(gpio_pin_get_dt(&config.cs.gpio), 1,
		      "SPI_HOLD_ON_CS did not leave GPIO CS asserted");

	config.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8);
	memset(rx, 0, sizeof(rx));
	ret = spi_transceive(spi, &config, &tx_set, &rx_set);
	zassert_ok(ret, "SPI CS release transfer failed: %d", ret);
	zassert_mem_equal(tx, rx, 32, "SPI CS release loopback");
	zassert_equal(gpio_pin_get_dt(&config.cs.gpio), 0,
		      "SPI_HOLD_ON_CS did not release GPIO CS on the next transfer");
}

ZTEST(hpm_spi_loop, test_native_cs_transfer)
{
	struct spi_config native_config = {
		.frequency = 1000000,
		.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8),
		.peripheral = 0,
		.cs.setup_ns = 500,
		.cs.hold_ns = 500,
	};
	struct spi_buf tx_bufs[] = {{.buf = tx, .len = 32}};
	struct spi_buf rx_bufs[] = {{.buf = rx, .len = 32}};
	struct spi_buf_set tx_set = {
		.buffers = tx_bufs,
		.count = ARRAY_SIZE(tx_bufs),
	};
	struct spi_buf_set rx_set = {
		.buffers = rx_bufs,
		.count = ARRAY_SIZE(rx_bufs),
	};
	int ret;

	zassert_true(device_is_ready(spi));
	spi_fill_pattern(tx, 32, 0x3c);
	memset(rx, 0, 32);

	ret = spi_transceive(spi, &native_config, &tx_set, &rx_set);
	zassert_ok(ret, "SPI native-CS transfer failed: %d", ret);
	zassert_mem_equal(tx, rx, 32, "SPI native-CS loopback");
}

ZTEST(hpm_spi_loop, test_rejects_unsupported_word_sizes)
{
	struct spi_buf tx_bufs[] = {{.buf = tx, .len = 8}};
	struct spi_buf rx_bufs[] = {{.buf = rx, .len = 8}};
	struct spi_buf_set tx_set = {
		.buffers = tx_bufs,
		.count = ARRAY_SIZE(tx_bufs),
	};
	struct spi_buf_set rx_set = {
		.buffers = rx_bufs,
		.count = ARRAY_SIZE(rx_bufs),
	};
	const uint32_t unsupported[] = {4, 12, 40};

	zassert_true(device_is_ready(spi));
	for (size_t i = 0; i < ARRAY_SIZE(unsupported); ++i) {
		int ret;

		config.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(unsupported[i]);
		ret = spi_transceive(spi, &config, &tx_set, &rx_set);
		zassert_equal(ret, -ENOTSUP, "SPI unsupported %u-bit accepted: %d", unsupported[i],
			      ret);
	}

	spi_check_sync_transfer(8, 32);
}

ZTEST_SUITE(hpm_spi_loop, NULL, NULL, NULL, spi_after, NULL);
