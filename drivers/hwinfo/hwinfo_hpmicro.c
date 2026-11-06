/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * Copyright (c) 2026 Cody Gu <gujiaqi@iscas.ac.cn>
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_hwinfo

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <hpm_common.h>
#include <hpm_otp_table.h>
#include <hpm_ppor_regs.h>

#define HPM_HWINFO_NODE     DT_DRV_INST(0)
#define HPM_HWINFO_PPOR     ((PPOR_Type *)DT_REG_ADDR(HPM_HWINFO_NODE))
#define HPM_HWINFO_OTP_NODE DT_PHANDLE(HPM_HWINFO_NODE, hpmicro_otp)

#define HPM_HWINFO_UUID_WORDS (OTP_TABLE_UUID_BIT_LENGTH / 32U)
#define HPM_HWINFO_UUID_BYTES (HPM_HWINFO_UUID_WORDS * sizeof(uint32_t))
#define HPM_HWINFO_UUID_ADDR                                                                       \
	(DT_REG_ADDR(HPM_HWINFO_OTP_NODE) + OTP_TABLE_UUID_OFFSET * sizeof(uint32_t))

#define HPM_HWINFO_SUPPORTED_CAUSES                                                                \
	(RESET_BROWNOUT | RESET_TEMPERATURE | RESET_DEBUG | RESET_WATCHDOG | RESET_SOFTWARE)

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
	     "Exactly one hpmicro,hpm-hwinfo node must be enabled");
BUILD_ASSERT(OTP_TABLE_UUID_BIT_LENGTH == 128U, "Unexpected HPM OTP UUID size");

static uint32_t hpm_hwinfo_translate_reset_causes(uint32_t flags)
{
	uint32_t causes = 0U;

	if ((flags & BIT(0)) != 0U) {
		causes |= RESET_BROWNOUT;
	}
	if ((flags & BIT(1)) != 0U) {
		causes |= RESET_TEMPERATURE;
	}
	if ((flags & (BIT(4) | BIT(5) | BIT(30))) != 0U) {
		causes |= RESET_DEBUG;
	}
	if ((flags & (BIT(16) | BIT(17) | BIT(24))) != 0U) {
		causes |= RESET_WATCHDOG;
	}
	if ((flags & BIT(31)) != 0U) {
		causes |= RESET_SOFTWARE;
	}

	return causes;
}

ssize_t z_impl_hwinfo_get_device_id(uint8_t *buffer, size_t length)
{
	size_t copied = 0U;
	size_t count = MIN(length, (size_t)HPM_HWINFO_UUID_BYTES);

	if ((buffer == NULL) && (length != 0U)) {
		return -EINVAL;
	}

	while (copied < count) {
		uint32_t word = sys_read32(HPM_HWINFO_UUID_ADDR + copied);
		size_t chunk = MIN(count - copied, sizeof(word));

		memcpy(&buffer[copied], &word, chunk);
		copied += chunk;
	}

	return (ssize_t)copied;
}

int z_impl_hwinfo_get_reset_cause(uint32_t *cause)
{
	if (cause == NULL) {
		return -EINVAL;
	}

	*cause = hpm_hwinfo_translate_reset_causes(HPM_HWINFO_PPOR->RESET_FLAG);

	return 0;
}

int z_impl_hwinfo_clear_reset_cause(void)
{
	uint32_t flags = HPM_HWINFO_PPOR->RESET_FLAG;

	HPM_HWINFO_PPOR->RESET_FLAG = flags;

	return 0;
}

int z_impl_hwinfo_get_supported_reset_cause(uint32_t *supported)
{
	if (supported == NULL) {
		return -EINVAL;
	}

	*supported = HPM_HWINFO_SUPPORTED_CAUSES;

	return 0;
}
