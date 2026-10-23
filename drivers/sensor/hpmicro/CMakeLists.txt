# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_TSNS_HPMICRO tsns_hpmicro.c)
zephyr_library_sources_ifdef(CONFIG_QEIV2_HPMICRO qeiv2_hpmicro.c)
