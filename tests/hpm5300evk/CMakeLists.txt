# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hpm5300evk_smoke)

target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_CRYPTO_HPMICRO app PRIVATE src/crypto.c)
target_sources_ifdef(CONFIG_SPI_HPMICRO app PRIVATE src/spi.c)
target_sources_ifdef(CONFIG_DMA_HPMICRO app PRIVATE src/dma.c)
target_sources_ifdef(CONFIG_UHC_HPMICRO app PRIVATE src/usb_host.c)
target_sources_ifdef(CONFIG_ENTROPY_HPMICRO_RNG app PRIVATE src/rng.c)
target_sources_ifdef(CONFIG_TSNS_HPMICRO app PRIVATE src/tsns.c)
target_sources_ifdef(CONFIG_WDT_HPMICRO app PRIVATE src/watchdog.c)
target_sources_ifdef(CONFIG_ADC_HPMICRO_ADC16 app PRIVATE src/adc.c)
target_sources_ifdef(CONFIG_UART_HPMICRO app PRIVATE src/uart.c)
