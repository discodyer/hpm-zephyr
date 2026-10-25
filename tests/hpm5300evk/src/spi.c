/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/spi.h>
#include <zephyr/ztest.h>

#if DT_HAS_HPMICRO_SPI_GPIO_CS_TEST_ENABLED
static void spi_async_callback(const struct device *dev, int result, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(result);
	ARG_UNUSED(user_data);
}

ZTEST(hpm_spi, test_gpio_cs_configuration)
{
	const struct spi_dt_spec spec = SPI_DT_SPEC_GET(DT_NODELABEL(spi_gpio_cs),
							SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8));

	zassert_true(spi_cs_is_gpio_dt(&spec));
	zassert_true(spi_is_ready_dt(&spec));
}

ZTEST(hpm_spi, test_rejects_unsupported_word_size)
{
	const struct spi_dt_spec spec =
		SPI_DT_SPEC_GET(DT_NODELABEL(spi_gpio_cs),
				SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(4));
	const uint8_t tx_data[2] = {0x5a, 0xa5};
	const struct spi_buf tx_buf = {
		.buf = (void *)tx_data,
		.len = sizeof(tx_data),
	};
	const struct spi_buf_set tx_bufs = {.buffers = &tx_buf, .count = 1};

	zassert_equal(spi_write_dt(&spec, &tx_bufs), -ENOTSUP);
}

ZTEST(hpm_spi, test_half_duplex_api)
{
	const struct spi_dt_spec spec =
		SPI_DT_SPEC_GET(DT_NODELABEL(spi_gpio_cs),
				SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_HALF_DUPLEX);
	const uint8_t tx_data[2] = {0x5a, 0xa5};
	const struct spi_buf tx_buf = {
		.buf = (void *)tx_data,
		.len = sizeof(tx_data),
	};
	const struct spi_buf_set tx_bufs = {.buffers = &tx_buf, .count = 1};

	zassert_equal(spi_write_dt(&spec, &tx_bufs), 0);
}

#ifdef CONFIG_SPI_ASYNC
ZTEST(hpm_spi, test_async_submit)
{
	const struct spi_dt_spec spec = SPI_DT_SPEC_GET(DT_NODELABEL(spi_gpio_cs),
							SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8));
	static const uint8_t tx_data[2] = {0x5a, 0xa5};
	const struct spi_buf tx_buf = {
		.buf = (void *)tx_data,
		.len = sizeof(tx_data),
	};
	const struct spi_buf_set tx_bufs = {.buffers = &tx_buf, .count = 1};

	zassert_ok(spi_write_cb_dt(&spec, &tx_bufs, spi_async_callback, NULL));
}

#ifdef CONFIG_SPI_HPMICRO_DMA
ZTEST(hpm_spi, test_async_dma_rejects_mismatched_buffers)
{
	const struct spi_dt_spec spec = SPI_DT_SPEC_GET(DT_NODELABEL(spi_gpio_cs),
							SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8));
	static const uint8_t tx_data[4] = {0x5a, 0xa5, 0x3c, 0xc3};
	static uint8_t rx_data[3];
	const struct spi_buf tx_buf = {
		.buf = (void *)tx_data,
		.len = sizeof(tx_data),
	};
	const struct spi_buf rx_buf = {.buf = rx_data, .len = sizeof(rx_data)};
	const struct spi_buf_set tx_bufs = {.buffers = &tx_buf, .count = 1};
	const struct spi_buf_set rx_bufs = {.buffers = &rx_buf, .count = 1};

	zassert_equal(spi_transceive_cb_dt(&spec, &tx_bufs, &rx_bufs, spi_async_callback, NULL),
		      -EINVAL);
}

ZTEST(hpm_spi, test_async_dma_multiblock_submit)
{
	const struct spi_dt_spec spec = SPI_DT_SPEC_GET(DT_NODELABEL(spi_gpio_cs),
							SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8));
	static const uint8_t tx_first[3] = {0x5a, 0xa5, 0x3c};
	static const uint8_t tx_second[2] = {0xc3, 0x99};
	const struct spi_buf tx_bufs_data[] = {
		{.buf = (void *)tx_first, .len = sizeof(tx_first)},
		{.buf = (void *)tx_second, .len = sizeof(tx_second)},
	};
	const struct spi_buf_set tx_bufs = {.buffers = tx_bufs_data,
					    .count = ARRAY_SIZE(tx_bufs_data)};

	zassert_ok(spi_write_cb_dt(&spec, &tx_bufs, spi_async_callback, NULL));
}
#endif
#endif
#endif

#ifdef CONFIG_SPI_EXTENDED_MODES
ZTEST(hpm_spi, test_quad_write_api)
{
	const struct spi_dt_spec spec = SPI_DT_SPEC_GET(DT_NODELABEL(spi_gpio_cs),
							SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) |
								SPI_LINES_QUAD | SPI_HALF_DUPLEX);
	static const uint8_t tx_data[4] = {0x5a, 0xa5, 0x3c, 0xc3};
	const struct spi_buf tx_buf = {
		.buf = (void *)tx_data,
		.len = sizeof(tx_data),
	};
	const struct spi_buf_set tx_bufs = {.buffers = &tx_buf, .count = 1};

	zassert_equal(spi_write_dt(&spec, &tx_bufs), 0);
}

ZTEST(hpm_spi, test_dual_read_api)
{
	const struct spi_dt_spec spec = SPI_DT_SPEC_GET(DT_NODELABEL(spi_gpio_cs),
							SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) |
								SPI_LINES_DUAL | SPI_HALF_DUPLEX);
	uint8_t rx_data[4];
	const struct spi_buf rx_buf = {.buf = rx_data, .len = sizeof(rx_data)};
	const struct spi_buf_set rx_bufs = {.buffers = &rx_buf, .count = 1};

	zassert_equal(spi_read_dt(&spec, &rx_bufs), 0);
}
#endif

ZTEST_SUITE(hpm_spi, NULL, NULL, NULL, NULL, NULL);
