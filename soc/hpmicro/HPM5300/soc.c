/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hpm5300_soc, CONFIG_SOC_LOG_LEVEL);

static int hpm5300_soc_init(void)
{
	/*
	 * Clock and peripheral initialization is intentionally staged after the
	 * minimal CPU/timer/UART bring-up is validated on hardware.
	 */
	return 0;
}

SYS_INIT(hpm5300_soc_init, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
