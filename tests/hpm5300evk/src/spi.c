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

ZTEST(hpm_spi, test_half_duplex_api)
{
	const struct spi_dt_spec spec =
		SPI_DT_SPEC_GET(DT_NODELABEL(spi_gpio_cs),
				SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_HALF_DUPLEX);
	const uint8_t tx_data[2] = {0x5a, 0xa5};
	const struct spi_buf tx_buf = {.buf = (void *)tx_data, .len = sizeof(tx_data)};
	const struct spi_buf_set tx_bufs = {.buffers = &tx_buf, .count = 1};

	zassert_equal(spi_write_dt(&spec, &tx_bufs), 0);
}

#ifdef CONFIG_SPI_ASYNC
ZTEST(hpm_spi, test_async_submit)
{
	const struct spi_dt_spec spec = SPI_DT_SPEC_GET(DT_NODELABEL(spi_gpio_cs),
							SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8));
	static const uint8_t tx_data[2] = {0x5a, 0xa5};
	const struct spi_buf tx_buf = {.buf = (void *)tx_data, .len = sizeof(tx_data)};
	const struct spi_buf_set tx_bufs = {.buffers = &tx_buf, .count = 1};

	zassert_ok(spi_write_cb_dt(&spec, &tx_bufs, spi_async_callback, NULL));
}
#endif
#endif

ZTEST_SUITE(hpm_spi, NULL, NULL, NULL, NULL, NULL);
