# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_QEO_HPMICRO qeo_hpmicro.c)
zephyr_library_sources_ifdef(CONFIG_SEI_HPMICRO sei_hpmicro.c)
