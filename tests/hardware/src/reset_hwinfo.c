/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * Copyright (c) 2026 Cody Gu <gujiaqi@iscas.ac.cn>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <dt-bindings/reset/hpmicro-hpm5300-reset.h>
#include <hpm_common.h>
#include <hpm_ppor_regs.h>
#include <hpm_sysctl_regs.h>

#define HPM_RESET_NODE          DT_NODELABEL(sysctl)
#define HPM_RESET_CTLR          DEVICE_DT_GET(HPM_RESET_NODE)
#define HPM_RESET_PPOR          ((PPOR_Type *)DT_REG_ADDR(DT_NODELABEL(hwinfo)))
#define HPM_HWINFO_ID_BYTES     16U
#define HPM_HWINFO_CANARY       0x5aU
#define HPM_HWINFO_RESET_MARKER 0x48504d52U

#define HPM_HWINFO_SUPPORTED_CAUSES                                                                \
	(RESET_BROWNOUT | RESET_TEMPERATURE | RESET_DEBUG | RESET_WATCHDOG | RESET_SOFTWARE)

static volatile uint32_t reset_marker __noinit;
static volatile uint32_t reset_marker_inv __noinit;

static void *reset_hwinfo_setup(void)
{
	zassert_true(device_is_ready(HPM_RESET_CTLR));

	return NULL;
}

ZTEST(hpm_reset_hwinfo, test_reset_status_mapping)
{
	const SYSCTL_Type *sysctl = (const SYSCTL_Type *)DT_REG_ADDR(HPM_RESET_NODE);
	uint8_t status;

	zassert_ok(reset_status(HPM_RESET_CTLR, HPM_RESET_SOC, &status));
	zassert_equal(status, FIELD_GET(SYSCTL_RESET_CONTROL_HOLD_MASK,
					sysctl->RESET[HPM_RESET_SOC].CONTROL));

	zassert_ok(reset_status(HPM_RESET_CTLR, HPM_RESET_CPU0, &status));
	zassert_equal(status, FIELD_GET(SYSCTL_RESET_CONTROL_HOLD_MASK,
					sysctl->RESET[HPM_RESET_CPU0].CONTROL));

	zassert_equal(reset_status(HPM_RESET_CTLR, HPM_RESET_CPU0 + 1U, &status), -EINVAL);
	zassert_equal(reset_status(HPM_RESET_CTLR, HPM_RESET_SOC, NULL), -EINVAL);
}

ZTEST(hpm_reset_hwinfo, test_device_id)
{
	uint8_t buffer[HPM_HWINFO_ID_BYTES + 2U];
	uint8_t repeat[HPM_HWINFO_ID_BYTES];
	uint8_t partial[7];
	bool all_zero = true;
	bool all_ones = true;
	ssize_t length;

	memset(buffer, HPM_HWINFO_CANARY, sizeof(buffer));
	length = hwinfo_get_device_id(buffer, sizeof(buffer));
	zassert_equal(length, HPM_HWINFO_ID_BYTES);
	zassert_equal(buffer[HPM_HWINFO_ID_BYTES], HPM_HWINFO_CANARY,
		      "device-ID implementation wrote past the UUID");
	zassert_equal(buffer[HPM_HWINFO_ID_BYTES + 1U], HPM_HWINFO_CANARY,
		      "device-ID implementation wrote past the UUID");

	zassert_equal(hwinfo_get_device_id(repeat, sizeof(repeat)), HPM_HWINFO_ID_BYTES);
	zassert_mem_equal(buffer, repeat, sizeof(repeat), "device ID changed between reads");

	length = hwinfo_get_device_id(partial, sizeof(partial));
	zassert_equal(length, sizeof(partial));
	zassert_mem_equal(partial, buffer, sizeof(partial),
			  "short device ID does not match prefix");

	zassert_equal(hwinfo_get_device_id(NULL, 0U), 0);
	zassert_equal(hwinfo_get_device_id(NULL, 1U), -EINVAL);

	for (size_t i = 0U; i < HPM_HWINFO_ID_BYTES; i++) {
		all_zero &= buffer[i] == 0U;
		all_ones &= buffer[i] == 0xffU;
	}

	zassert_false(all_zero, "OTP UUID is all zero");
	zassert_false(all_ones, "OTP UUID is all ones");
}

ZTEST(hpm_reset_hwinfo, test_reset_cause)
{
	uint32_t supported;
	uint32_t cause_first;
	uint32_t cause_second;
	uint32_t cause_cleared;

	zassert_ok(hwinfo_get_supported_reset_cause(&supported));
	zassert_equal(supported, HPM_HWINFO_SUPPORTED_CAUSES);

	zassert_ok(hwinfo_get_reset_cause(&cause_first));
	zassert_equal(cause_first & ~supported, 0U, "unsupported reset cause bits reported");

	if (!(cause_first & RESET_SOFTWARE) || (reset_marker != HPM_HWINFO_RESET_MARKER) ||
	    (reset_marker_inv != ~HPM_HWINFO_RESET_MARKER)) {
		printk("Triggering PPOR software reset to validate the reset cause\n");
		reset_marker = HPM_HWINFO_RESET_MARKER;
		reset_marker_inv = ~HPM_HWINFO_RESET_MARKER;
		HPM_RESET_PPOR->SOFTWARE_RESET = 1000U;
		for (;;) {
			k_busy_wait(1000);
		}
	}

	zassert_equal(cause_first, RESET_SOFTWARE, "PPOR software reset was not reported");

	zassert_ok(hwinfo_get_reset_cause(&cause_second));
	zassert_equal(cause_second, cause_first, "reset cause changed before it was cleared");

	zassert_ok(hwinfo_clear_reset_cause());
	zassert_ok(hwinfo_get_reset_cause(&cause_cleared));
	zassert_equal(cause_cleared, 0U, "PPOR reset causes were not cleared");
	zassert_ok(hwinfo_clear_reset_cause());

	reset_marker = 0U;
	reset_marker_inv = 0U;

	zassert_equal(hwinfo_get_reset_cause(NULL), -EINVAL);
	zassert_equal(hwinfo_get_supported_reset_cause(NULL), -EINVAL);
}

ZTEST_SUITE(hpm_reset_hwinfo, NULL, reset_hwinfo_setup, NULL, NULL, NULL);
