/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/reboot.h>
#include <zephyr/toolchain.h>
#include <zephyr/types.h>

#if defined(CONFIG_XIP)
/*
 * BootROM reads this NOR configuration block from flash offset 0x400.
 * Values match the HPM5300EVK QSPI NOR configuration.
 */
__section(".nor_cfg_option") static const uint32_t hpm_nor_cfg_option[4] = {
	0xfcf90002U,
	0x00000005U,
	0x00001000U,
	0x00000000U,
};
#endif

void sys_arch_reboot(int type)
{
	ARG_UNUSED(type);
}
