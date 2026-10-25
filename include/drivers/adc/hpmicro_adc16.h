/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_ADC_HPMICRO_ADC16_H_
#define ZEPHYR_INCLUDE_DRIVERS_ADC_HPMICRO_ADC16_H_

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>

#define HPM_ADC16_PREEMPTION_MAX_CHANNELS 4U

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

/** Configuration for one ADC16 preemption queue. */
struct hpm_adc16_preemption_config {
	/** Hardware trigger queue, from 0 (PTRGI0A) through 11 (PTRGI3C). */
	uint8_t trigger;
	/** Number of channels converted by each trigger. */
	uint8_t channel_count;
	/** Channels converted in order after the trigger edge. */
	uint8_t channels[HPM_ADC16_PREEMPTION_MAX_CHANNELS];
	/** Sample-cycle count; zero selects the driver default. */
	uint16_t sample_cycle;
	/** Forward this ADC's samples to the connected motor sensor unit. */
	bool motor_enable;
};

/** One result copied from the ADC16 preemption DMA buffer. */
struct hpm_adc16_preemption_result {
	/** Unsigned conversion result. */
	uint16_t value;
	/** ADC input channel. */
	uint8_t channel;
	/** Position within the triggered conversion sequence. */
	uint8_t sequence;
	/** DMA cycle bit used to detect a buffer wrap. */
	bool cycle;
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_adc16_driver_api {
	/** Must remain first so the device also implements the ADC API. */
	struct adc_driver_api adc_api;
	int (*set_trigger_source)(const struct device *dev, enum hpm_adc16_trigger_source source);
	int (*get_trigger_source)(const struct device *dev, enum hpm_adc16_trigger_source *source);
#if defined(CONFIG_ADC_HPMICRO_ADC16_PREEMPTION)
	int (*configure_preemption)(const struct device *dev,
				    const struct hpm_adc16_preemption_config *config);
	int (*trigger_preemption)(const struct device *dev);
	int (*read_preemption)(const struct device *dev,
			       struct hpm_adc16_preemption_result *results, uint8_t result_count);
	int (*stop_preemption)(const struct device *dev);
#endif
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

#if defined(CONFIG_ADC_HPMICRO_ADC16_PREEMPTION) || defined(__DOXYGEN__)
/**
 * @brief Configure one ADC16 preemption queue.
 *
 * The queue is armed for hardware triggers routed through TRGM. The caller
 * remains responsible for selecting the matching ADCX_PTRGI output in TRGM.
 * Preemption and the generic sequence API are mutually exclusive while a
 * preemption queue is active. The hardware uses a fixed 192-byte DMA buffer,
 * with one 16-byte slot for each of the 12 trigger inputs. Each slot contains
 * four 32-bit result records, regardless of the configured channel count.
 * Completion events are coalesced: if several triggers complete before the
 * results are read, the subsequent read returns the latest sequence while the
 * corresponding slot is overwritten by the ADC hardware.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid trigger or channel list.
 * @retval -EBUSY when a sequence conversion is in progress.
 * @retval -ENOTSUP for motor output on silicon without the required feature.
 */
static inline int hpm_adc16_configure_preemption(const struct device *dev,
						 const struct hpm_adc16_preemption_config *config)
{
	return DEVICE_API_GET(hpm_adc16, dev)->configure_preemption(dev, config);
}

/** @brief Issue a software trigger for the configured preemption queue. */
static inline int hpm_adc16_trigger_preemption(const struct device *dev)
{
	return DEVICE_API_GET(hpm_adc16, dev)->trigger_preemption(dev);
}

/**
 * @brief Copy one completed preemption sequence from the internal DMA buffer.
 *
 * @param results Destination array.
 * @param result_count Must equal the configured channel count.
 *
 * @retval 0 on success.
 * @retval -EAGAIN when no new trigger-complete event is pending.
 * @retval -EIO when the ADC reported a trigger or DMA error.
 */
static inline int hpm_adc16_read_preemption(const struct device *dev,
					    struct hpm_adc16_preemption_result *results,
					    uint8_t result_count)
{
	return DEVICE_API_GET(hpm_adc16, dev)->read_preemption(dev, results, result_count);
}

/** @brief Disable the configured preemption queue and motor sample output. */
static inline int hpm_adc16_stop_preemption(const struct device *dev)
{
	return DEVICE_API_GET(hpm_adc16, dev)->stop_preemption(dev);
}
#endif /* CONFIG_ADC_HPMICRO_ADC16_PREEMPTION */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_ADC_HPMICRO_ADC16_H_ */
