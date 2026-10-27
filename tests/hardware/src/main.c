/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>

BUILD_ASSERT(IS_ENABLED(CONFIG_XIP), "Acceptance images must retain the XIP layout");
BUILD_ASSERT(IS_ENABLED(CONFIG_DCACHE), "Keep data cache enabled for DMA/cache acceptance");
BUILD_ASSERT(IS_ENABLED(CONFIG_PMP_DATA_EXECUTION_PREVENTION), "Retain PMP protection");
#if !defined(CONFIG_HPM_TEST_FLASH)
BUILD_ASSERT(!IS_ENABLED(CONFIG_FLASH), "Keep BootROM flash calls in the flash profile");
#endif

#if defined(CONFIG_HPM_TEST_UART)
BUILD_ASSERT(IS_ENABLED(CONFIG_UART_ASYNC_API), "UART profile requires async API");
#endif
#if defined(CONFIG_HPM_TEST_SPI)
BUILD_ASSERT(IS_ENABLED(CONFIG_SPI_HPMICRO_DMA), "SPI profile must exercise DMA");
#endif
#if defined(CONFIG_HPM_TEST_I2C)
BUILD_ASSERT(IS_ENABLED(CONFIG_I2C_HPMICRO_DMA), "I2C profile must exercise DMA");
#endif
#if defined(CONFIG_HPM_TEST_ANALOG)
BUILD_ASSERT(IS_ENABLED(CONFIG_ADC_HPMICRO_ADC16_SEQUENCE_DMA), "Require ADC sequence DMA");
#endif

#if defined(CONFIG_HPM_TEST_CRYPTO)
/* Reuse the SDK-independent known-answer vectors from the build-only suite. */
ZTEST_SUITE(hpm5300evk, NULL, NULL, NULL, NULL, NULL);
#endif
