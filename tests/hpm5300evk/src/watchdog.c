/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/ztest.h>

#if DT_NODE_HAS_STATUS(DT_NODELABEL(ewdg0), okay)
ZTEST(hpm_watchdog, test_window_timeout_validation)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ewdg0));
	struct wdt_timeout_cfg timeout = {
		.window.min = 1000U,
		.window.max = 1000U,
		.flags = WDT_FLAG_RESET_SOC,
	};

	zassert_true(device_is_ready(dev));
	zassert_equal(wdt_install_timeout(dev, &timeout), -EINVAL);

	timeout.window.min = 750U;
	zassert_ok(wdt_install_timeout(dev, &timeout));
}
#endif

ZTEST_SUITE(hpm_watchdog, NULL, NULL, NULL, NULL, NULL);
