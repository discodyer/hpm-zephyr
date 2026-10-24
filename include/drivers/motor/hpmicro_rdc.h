/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_RDC_H_
#define ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_RDC_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro resolver-to-digital converter
 * @defgroup hpm_rdc_interface HPMicro RDC
 * @ingroup io_interfaces
 * @{
 */

enum hpm_rdc_output_mode {
	HPM_RDC_OUTPUT_DAC = 0,
	HPM_RDC_OUTPUT_PWM = 1,
};

enum hpm_rdc_excitation_points {
	HPM_RDC_EXCITATION_4_POINTS = 0,
	HPM_RDC_EXCITATION_8_POINTS,
	HPM_RDC_EXCITATION_16_POINTS,
	HPM_RDC_EXCITATION_32_POINTS,
	HPM_RDC_EXCITATION_64_POINTS,
	HPM_RDC_EXCITATION_128_POINTS,
	HPM_RDC_EXCITATION_256_POINTS,
	HPM_RDC_EXCITATION_512_POINTS,
	HPM_RDC_EXCITATION_1024_POINTS,
};

enum hpm_rdc_rectify_signal {
	HPM_RDC_RECTIFY_EXCITATION_0 = 0,
	HPM_RDC_RECTIFY_EXCITATION_90,
	HPM_RDC_RECTIFY_EXCITATION_180,
	HPM_RDC_RECTIFY_EXCITATION_270,
	HPM_RDC_RECTIFY_EXTERNAL,
	HPM_RDC_RECTIFY_EXTERNAL_INVERTED,
};

enum hpm_rdc_accumulation_stamp {
	HPM_RDC_STAMP_END = 0,
	HPM_RDC_STAMP_START = 1,
	HPM_RDC_STAMP_CENTER = 2,
};

enum hpm_rdc_channel {
	HPM_RDC_CHANNEL_I = 0,
	HPM_RDC_CHANNEL_Q = 1,
};

enum hpm_rdc_trigger {
	HPM_RDC_TRIGGER_0 = 0,
	HPM_RDC_TRIGGER_1 = 1,
};

struct hpm_rdc_output_config {
	enum hpm_rdc_output_mode mode;
	uint32_t excitation_period_cycles;
	enum hpm_rdc_excitation_points excitation_points;
	uint8_t pwm_period_samples;
	bool output_swap;
	int32_t amplitude_offset;
	uint16_t amplitude_mantissa;
	uint8_t amplitude_exponent;
	bool pwm_dither;
	bool pwm_p_active_low;
	bool pwm_n_active_low;
	bool hardware_trigger;
	uint32_t hardware_trigger_delay;
	uint8_t dac_channel_i;
	uint8_t dac_channel_q;
	uint8_t pwm_deadzone_p;
	uint8_t pwm_deadzone_n;
};

struct hpm_rdc_input_config {
	enum hpm_rdc_rectify_signal rectify_signal;
	uint8_t accumulation_cycles;
	enum hpm_rdc_accumulation_stamp stamp;
	uint8_t adc_channel_i;
	uint8_t adc_port_i;
	uint8_t adc_channel_q;
	uint8_t adc_port_q;
};

struct hpm_rdc_accumulation_config {
	uint8_t filter_edges;
	uint8_t edge_distance;
	uint8_t right_shift;
	bool discard_invalid;
	uint32_t excitation_carrier_period;
	uint32_t sync_delay_i;
	uint32_t sync_delay_q;
	uint32_t amplitude_max;
	uint32_t amplitude_min;
};

/** RDC interrupt status flags. */
#define HPM_RDC_EVENT_ACC_VALID_I         (1U << 15U)
#define HPM_RDC_EVENT_ACC_VALID_Q         (1U << 14U)
#define HPM_RDC_EVENT_RISING_DELAY_I      (1U << 13U)
#define HPM_RDC_EVENT_FALLING_DELAY_I     (1U << 12U)
#define HPM_RDC_EVENT_RISING_DELAY_Q      (1U << 11U)
#define HPM_RDC_EVENT_FALLING_DELAY_Q     (1U << 10U)
#define HPM_RDC_EVENT_SAMPLE_RISING_I     (1U << 9U)
#define HPM_RDC_EVENT_SAMPLE_FALLING_I    (1U << 8U)
#define HPM_RDC_EVENT_SAMPLE_RISING_Q     (1U << 7U)
#define HPM_RDC_EVENT_SAMPLE_FALLING_Q    (1U << 6U)
#define HPM_RDC_EVENT_ACC_OVERFLOW_I      (1U << 5U)
#define HPM_RDC_EVENT_ACC_OVERFLOW_Q      (1U << 4U)
#define HPM_RDC_EVENT_ACC_UNDERFLOW_I     (1U << 3U)
#define HPM_RDC_EVENT_ACC_UNDERFLOW_Q     (1U << 2U)
#define HPM_RDC_EVENT_AMPLITUDE_OVERFLOW  (1U << 1U)
#define HPM_RDC_EVENT_AMPLITUDE_UNDERFLOW (1U << 0U)
#define HPM_RDC_EVENT_ALL                                                                          \
	(HPM_RDC_EVENT_ACC_VALID_I | HPM_RDC_EVENT_ACC_VALID_Q | HPM_RDC_EVENT_RISING_DELAY_I |    \
	 HPM_RDC_EVENT_FALLING_DELAY_I | HPM_RDC_EVENT_RISING_DELAY_Q |                            \
	 HPM_RDC_EVENT_FALLING_DELAY_Q | HPM_RDC_EVENT_SAMPLE_RISING_I |                           \
	 HPM_RDC_EVENT_SAMPLE_FALLING_I | HPM_RDC_EVENT_SAMPLE_RISING_Q |                          \
	 HPM_RDC_EVENT_SAMPLE_FALLING_Q | HPM_RDC_EVENT_ACC_OVERFLOW_I |                           \
	 HPM_RDC_EVENT_ACC_OVERFLOW_Q | HPM_RDC_EVENT_ACC_UNDERFLOW_I |                            \
	 HPM_RDC_EVENT_ACC_UNDERFLOW_Q | HPM_RDC_EVENT_AMPLITUDE_OVERFLOW |                        \
	 HPM_RDC_EVENT_AMPLITUDE_UNDERFLOW)

typedef void (*hpm_rdc_event_callback_t)(const struct device *dev, uint32_t events,
					 void *user_data);

struct hpm_rdc_result {
	uint32_t i;
	uint32_t q;
	int32_t min_i;
	int32_t max_i;
	int32_t min_q;
	int32_t max_q;
	uint32_t rise_delay_i;
	uint32_t fall_delay_i;
	uint32_t rise_delay_q;
	uint32_t fall_delay_q;
	uint32_t interrupt_status;
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_rdc_driver_api {
	int (*configure_output)(const struct device *dev,
				const struct hpm_rdc_output_config *config);
	int (*configure_input)(const struct device *dev, const struct hpm_rdc_input_config *config);
	int (*configure_accumulation)(const struct device *dev,
				      const struct hpm_rdc_accumulation_config *config);
	int (*set_channel_threshold)(const struct device *dev, enum hpm_rdc_channel channel,
				     int32_t threshold);
	int (*set_channel_sync_delay)(const struct device *dev, enum hpm_rdc_channel channel,
				      uint32_t delay);
	int (*configure_trigger)(const struct device *dev, enum hpm_rdc_trigger trigger,
				 int32_t offset, bool enable);
	int (*excitation_enable)(const struct device *dev, bool enable);
	int (*software_trigger)(const struct device *dev);
	int (*accumulation_enable)(const struct device *dev, bool enable);
	int (*get_result)(const struct device *dev, struct hpm_rdc_result *result);
	int (*clear_extrema)(const struct device *dev);
	int (*get_events)(const struct device *dev, uint32_t *events);
	int (*clear_events)(const struct device *dev, uint32_t events);
	int (*set_event_callback)(const struct device *dev, uint32_t event_mask,
				  hpm_rdc_event_callback_t callback, void *user_data);
};
/** @endcond */

static inline int hpm_rdc_configure_output(const struct device *dev,
					   const struct hpm_rdc_output_config *config)
{
	return DEVICE_API_GET(hpm_rdc, dev)->configure_output(dev, config);
}

static inline int hpm_rdc_configure_input(const struct device *dev,
					  const struct hpm_rdc_input_config *config)
{
	return DEVICE_API_GET(hpm_rdc, dev)->configure_input(dev, config);
}

static inline int hpm_rdc_configure_accumulation(const struct device *dev,
						 const struct hpm_rdc_accumulation_config *config)
{
	return DEVICE_API_GET(hpm_rdc, dev)->configure_accumulation(dev, config);
}

static inline int hpm_rdc_set_channel_threshold(const struct device *dev,
						enum hpm_rdc_channel channel, int32_t threshold)
{
	return DEVICE_API_GET(hpm_rdc, dev)->set_channel_threshold(dev, channel, threshold);
}

static inline int hpm_rdc_set_channel_sync_delay(const struct device *dev,
						 enum hpm_rdc_channel channel, uint32_t delay)
{
	return DEVICE_API_GET(hpm_rdc, dev)->set_channel_sync_delay(dev, channel, delay);
}

static inline int hpm_rdc_configure_trigger(const struct device *dev, enum hpm_rdc_trigger trigger,
					    int32_t offset, bool enable)
{
	return DEVICE_API_GET(hpm_rdc, dev)->configure_trigger(dev, trigger, offset, enable);
}

static inline int hpm_rdc_excitation_enable(const struct device *dev, bool enable)
{
	return DEVICE_API_GET(hpm_rdc, dev)->excitation_enable(dev, enable);
}

static inline int hpm_rdc_software_trigger(const struct device *dev)
{
	return DEVICE_API_GET(hpm_rdc, dev)->software_trigger(dev);
}

static inline int hpm_rdc_accumulation_enable(const struct device *dev, bool enable)
{
	return DEVICE_API_GET(hpm_rdc, dev)->accumulation_enable(dev, enable);
}

static inline int hpm_rdc_get_result(const struct device *dev, struct hpm_rdc_result *result)
{
	return DEVICE_API_GET(hpm_rdc, dev)->get_result(dev, result);
}

static inline int hpm_rdc_clear_extrema(const struct device *dev)
{
	return DEVICE_API_GET(hpm_rdc, dev)->clear_extrema(dev);
}

/**
 * @brief Read and clear pending RDC event flags.
 *
 * @param dev RDC device.
 * @param events Output combination of HPM_RDC_EVENT_* flags.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p events is NULL.
 */
static inline int hpm_rdc_get_events(const struct device *dev, uint32_t *events)
{
	return DEVICE_API_GET(hpm_rdc, dev)->get_events(dev, events);
}

/**
 * @brief Clear selected RDC event flags.
 *
 * @param dev RDC device.
 * @param events Combination of HPM_RDC_EVENT_* flags.
 *
 * @retval 0 on success.
 * @retval -EINVAL for unknown event bits.
 */
static inline int hpm_rdc_clear_events(const struct device *dev, uint32_t events)
{
	return DEVICE_API_GET(hpm_rdc, dev)->clear_events(dev, events);
}

/**
 * @brief Enable RDC event interrupts and install a callback.
 *
 * Passing a NULL callback or zero mask disables all RDC interrupts.
 *
 * @param dev RDC device.
 * @param event_mask Combination of HPM_RDC_EVENT_* flags.
 * @param callback Callback invoked from interrupt context.
 * @param user_data Callback context.
 *
 * @retval 0 on success.
 * @retval -EINVAL for unknown event bits.
 */
static inline int hpm_rdc_set_event_callback(const struct device *dev, uint32_t event_mask,
					     hpm_rdc_event_callback_t callback, void *user_data)
{
	return DEVICE_API_GET(hpm_rdc, dev)
		->set_event_callback(dev, event_mask, callback, user_data);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_RDC_H_ */
