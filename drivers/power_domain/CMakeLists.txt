# Copyright (c) 2026 The HPM Zephyr Module Contributors
# Copyright (c) 2026 Cody Gu <gujiaqi@iscas.ac.cn>
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_POWER_DOMAIN_HPMICRO power_domain_hpmicro.c)
