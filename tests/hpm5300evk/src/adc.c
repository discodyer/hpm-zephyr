/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/ztest.h>

#if DT_NODE_HAS_STATUS(DT_NODELABEL(adc0), okay) && defined(CONFIG_ADC_ASYNC)
ZTEST(hpm_adc, test_async_api_contract)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(adc0));
	static uint16_t sample;
	struct k_poll_signal signal;
	struct k_poll_event event;
	struct adc_sequence sequence = {
		.channels = BIT(0),
		.buffer = &sample,
		.buffer_size = sizeof(sample),
		.resolution = 12U,
	};

	k_poll_signal_init(&signal);
	k_poll_event_init(&event, K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY, &signal);
	zassert_true(device_is_ready(dev));
	zassert_ok(adc_read_async(dev, &sequence, &signal));
	zassert_ok(k_poll(&event, 1U, K_SECONDS(1)));
}
#endif

ZTEST_SUITE(hpm_adc, NULL, NULL, NULL, NULL, NULL);
