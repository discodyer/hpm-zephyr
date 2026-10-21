# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_CLOCK_CONTROL_HPMICRO clock_control_hpmicro.c)
zephyr_library_sources_ifdef(CONFIG_CLOCK_CONTROL_HPMICRO_PLLV2 clock_control_hpmicro_pllv2.c)
