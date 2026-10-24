# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_QEO_HPMICRO qeo_hpmicro.c)
zephyr_library_sources_ifdef(CONFIG_SEI_HPMICRO sei_hpmicro.c)
zephyr_library_sources_ifdef(CONFIG_SEI_SSI_HPMICRO sei_ssi.c)
zephyr_library_sources_ifdef(CONFIG_SEI_TAMAGAWA_HPMICRO sei_tamagawa.c)
zephyr_library_sources_ifdef(CONFIG_MMC_HPMICRO mmc_hpmicro.c)
zephyr_library_sources_ifdef(CONFIG_RDC_HPMICRO rdc_hpmicro.c)
zephyr_library_sources_ifdef(CONFIG_SYNT_HPMICRO synt_hpmicro.c)
zephyr_library_sources_ifdef(CONFIG_PLB_HPMICRO plb_hpmicro.c)
