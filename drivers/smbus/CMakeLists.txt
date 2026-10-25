# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(${ZEPHYR_BASE}/drivers/smbus)

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_SMBUS_HPMICRO smbus_hpmicro.c)
