/* SPDX-License-Identifier: Apache-2.0 */

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

ZTEST_SUITE(hpm_spi_loop, NULL, NULL, NULL, spi_after, NULL);
