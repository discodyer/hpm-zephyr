# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_SPI_HPMICRO spi_hpmicro.c)
zephyr_library_include_directories_ifdef(CONFIG_SPI_HPMICRO ${ZEPHYR_BASE}/drivers/spi)
