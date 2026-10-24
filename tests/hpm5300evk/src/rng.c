/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/ztest.h>

#if DT_NODE_HAS_STATUS(DT_NODELABEL(rng), okay)
ZTEST(hpm_rng, test_isr_api)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(rng));
	uint8_t entropy[4];

	zassert_true(DEVICE_API_IS(entropy, dev));
	zassert_true(entropy_get_entropy_isr(dev, entropy, sizeof(entropy), 0U) >= 0);
}
#endif

ZTEST_SUITE(hpm_rng, NULL, NULL, NULL, NULL, NULL);
