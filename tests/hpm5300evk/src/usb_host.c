/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/usb/usbh.h>
#include <zephyr/ztest.h>

USBH_CONTROLLER_DEFINE(hpm_usbh_ctx, DEVICE_DT_GET(DT_NODELABEL(usb0)));

ZTEST(hpm_usb_host, test_controller_init)
{
	zassert_true(device_is_ready(hpm_usbh_ctx.dev));
	zassert_ok(usbh_init(&hpm_usbh_ctx));
	zassert_ok(usbh_enable(&hpm_usbh_ctx));
	zassert_true(uhc_is_enabled(hpm_usbh_ctx.dev));
	zassert_ok(usbh_disable(&hpm_usbh_ctx));
	zassert_ok(usbh_shutdown(&hpm_usbh_ctx));
}

ZTEST_SUITE(hpm_usb_host, NULL, NULL, NULL, NULL, NULL);
