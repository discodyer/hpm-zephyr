/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/ztest.h>

#include <drivers/adc/hpmicro_adc16.h>

#ifdef CONFIG_ADC_HPMICRO_ADC16_SEQUENCE_DMA
BUILD_ASSERT(IS_ENABLED(CONFIG_NOCACHE_MEMORY),
	     "ADC16 sequence DMA requires non-cacheable memory support");
BUILD_ASSERT(CONFIG_ADC_HPMICRO_ADC16_SEQUENCE_TIMEOUT_MS > 0);
#endif

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

#if defined(CONFIG_ADC_HPMICRO_ADC16_EXTERNAL_TRIGGER) &&                                          \
	DT_NODE_HAS_STATUS(DT_NODELABEL(adc0), okay)
ZTEST(hpm_adc, test_external_trigger_selection)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(adc0));
	enum hpm_adc16_trigger_source source;

	zassert_true(device_is_ready(dev));
	zassert_true(DEVICE_API_IS(adc, dev));
	zassert_true(DEVICE_API_IS(hpm_adc16, dev));
	zassert_ok(hpm_adc16_get_trigger_source(dev, &source));
	zassert_equal(source, HPM_ADC16_TRIGGER_SOFTWARE);
	zassert_ok(hpm_adc16_set_trigger_source(dev, HPM_ADC16_TRIGGER_EXTERNAL));
	zassert_ok(hpm_adc16_get_trigger_source(dev, &source));
	zassert_equal(source, HPM_ADC16_TRIGGER_EXTERNAL);
	zassert_ok(hpm_adc16_set_trigger_source(dev, HPM_ADC16_TRIGGER_SOFTWARE));
	zassert_equal(hpm_adc16_set_trigger_source(dev, (enum hpm_adc16_trigger_source)2), -EINVAL);
	zassert_equal(hpm_adc16_get_trigger_source(dev, NULL), -EINVAL);
}
#endif

#if defined(CONFIG_ADC_HPMICRO_ADC16_PREEMPTION) && DT_NODE_HAS_STATUS(DT_NODELABEL(adc0), okay)
ZTEST(hpm_adc, test_preemption_api_contract)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(adc0));
	struct hpm_adc16_preemption_config config = {
		.trigger = 0U,
		.channel_count = 1U,
		.channels = {0U},
		.sample_cycle = 20U,
	};
	struct hpm_adc16_preemption_result result;

	zassert_true(device_is_ready(dev));
	zassert_true(DEVICE_API_IS(hpm_adc16, dev));
	zassert_ok(hpm_adc16_configure_preemption(dev, &config));
	zassert_ok(hpm_adc16_trigger_preemption(dev));
	zassert_ok(hpm_adc16_stop_preemption(dev));
	zassert_equal(hpm_adc16_configure_preemption(dev, &config), 0);
	zassert_ok(hpm_adc16_stop_preemption(dev));
	zassert_equal(hpm_adc16_stop_preemption(dev), -EINVAL);
	zassert_equal(hpm_adc16_read_preemption(dev, &result, 1U), -EINVAL);
	config.channel_count = HPM_ADC16_PREEMPTION_MAX_CHANNELS + 1U;
	zassert_equal(hpm_adc16_configure_preemption(dev, &config), -EINVAL);
	config.channel_count = 1U;
	config.trigger = 12U;
	zassert_equal(hpm_adc16_configure_preemption(dev, &config), -EINVAL);
	config.trigger = 0U;
	config.sample_cycle = 0x200U;
	zassert_equal(hpm_adc16_configure_preemption(dev, &config), -EINVAL);
	zassert_equal(hpm_adc16_configure_preemption(dev, NULL), -EINVAL);
}
#endif

ZTEST_SUITE(hpm_adc, NULL, NULL, NULL, NULL, NULL);
