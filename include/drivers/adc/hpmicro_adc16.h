/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_ADC_HPMICRO_ADC16_H_
#define ZEPHYR_INCLUDE_DRIVERS_ADC_HPMICRO_ADC16_H_

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ADC16 sequence trigger source. */
enum hpm_adc16_trigger_source {
	/** Trigger each sequence from adc_read() or adc_read_async(). */
	HPM_ADC16_TRIGGER_SOFTWARE = 0,
	/** Wait for a rising edge routed by TRGM to the ADC STRGI input. */
	HPM_ADC16_TRIGGER_EXTERNAL,
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_adc16_driver_api {
	/** Must remain first so the device also implements the ADC API. */
	struct adc_driver_api adc_api;
	int (*set_trigger_source)(const struct device *dev, enum hpm_adc16_trigger_source source);
	int (*get_trigger_source)(const struct device *dev, enum hpm_adc16_trigger_source *source);
};

DEVICE_API_EXTENDS(hpm_adc16, adc, adc_api);
/** @endcond */

/**
 * @brief Select the ADC16 sequence trigger source.
 *
 * In external mode, adc_read() and adc_read_async() arm the sequence and wait
 * for a rising edge on the ADC STRGI input. The caller remains responsible for
 * routing a source to that input through TRGM.
 *
 * @param dev ADC16 device.
 * @param source Software or external trigger.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid source.
 * @retval -ENOTSUP if the extension is disabled or polling fallback is active.
 * @retval -EBUSY if a conversion is in progress.
 */
static inline int hpm_adc16_set_trigger_source(const struct device *dev,
					       enum hpm_adc16_trigger_source source)
{
	const struct hpm_adc16_driver_api *api = DEVICE_API_GET(hpm_adc16, dev);

	if (api->set_trigger_source == NULL) {
		return -ENOTSUP;
	}

	return api->set_trigger_source(dev, source);
}

/**
 * @brief Read back the ADC16 sequence trigger source.
 *
 * @retval 0 on success.
 * @retval -ENOTSUP if the extension is disabled or polling fallback is active.
 */
static inline int hpm_adc16_get_trigger_source(const struct device *dev,
					       enum hpm_adc16_trigger_source *source)
{
	const struct hpm_adc16_driver_api *api = DEVICE_API_GET(hpm_adc16, dev);

	if (api->get_trigger_source == NULL) {
		return -ENOTSUP;
	}

	return api->get_trigger_source(dev, source);
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_ADC_HPMICRO_ADC16_H_ */
