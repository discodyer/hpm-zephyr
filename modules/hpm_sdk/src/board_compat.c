/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include "board.h"

void board_delay_ms(uint32_t ms)
{
	k_busy_wait(ms * 1000U);
}
