/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/spi.h>
#include <zephyr/ztest.h>

#if DT_HAS_HPMICRO_SPI_GPIO_CS_TEST_ENABLED
ZTEST(hpm_spi, test_gpio_cs_configuration)
{
	const struct spi_dt_spec spec = SPI_DT_SPEC_GET(DT_NODELABEL(spi_gpio_cs),
							SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8));

	zassert_true(spi_cs_is_gpio_dt(&spec));
	zassert_true(spi_is_ready_dt(&spec));
}
#endif

ZTEST_SUITE(hpm_spi, NULL, NULL, NULL, NULL, NULL);
