/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_SENSOR_HPMICRO_QEIV2_H_
#define ZEPHYR_INCLUDE_DRIVERS_SENSOR_HPMICRO_QEIV2_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro QEIV2 quadrature encoder extensions
 * @defgroup hpm_qeiv2_interface HPMicro QEIV2
 * @ingroup sensor_interface_ext
 * @{
 */

#define HPM_QEIV2_SPEED_CHANNEL_COUNT   2U
#define HPM_QEIV2_COMPARE_CHANNEL_COUNT 2U

/** QEIV2 input decoding mode. */
enum hpm_qeiv2_mode {
	/** Quadrature A/B with optional Z. */
	HPM_QEIV2_MODE_ABZ = 0,
	/** Pulse and direction: A is the pulse input and B is the direction input. */
	HPM_QEIV2_MODE_PULSE_DIRECTION,
	/** Up/down: A counts up and B counts down. */
	HPM_QEIV2_MODE_UP_DOWN,
	/** Three-phase U/V/W input on A/B/Z. */
	HPM_QEIV2_MODE_UVW,
};

/** Runtime decoder configuration. */
struct hpm_qeiv2_mode_config {
	enum hpm_qeiv2_mode mode;
	/** Number of counts in one mechanical revolution. */
	uint32_t counts_per_revolution;
	/** Phase-counter value loaded on Z calibration. */
	uint32_t phase_index;
	/** Calibrate the phase counter from Z. */
	bool z_calibration;
	/** Ignore A/B when Z calibration is enabled. */
	bool z_calibration_ignore_ab;
};

/** Raw event flags reported by QEIV2. */
#define HPM_QEIV2_EVENT_WATCHDOG          (1U << 31U)
#define HPM_QEIV2_EVENT_HOME              (1U << 30U)
#define HPM_QEIV2_EVENT_POSITION_COMPARE  (1U << 29U)
#define HPM_QEIV2_EVENT_Z_PHASE           (1U << 28U)
#define HPM_QEIV2_EVENT_Z_MISS            (1U << 27U)
#define HPM_QEIV2_EVENT_WIDTH_TIME        (1U << 26U)
#define HPM_QEIV2_EVENT_POSITION2_COMPARE (1U << 25U)
#define HPM_QEIV2_EVENT_DIRECTION_CHANGE  (1U << 24U)
#define HPM_QEIV2_EVENT_CYCLE0            (1U << 23U)
#define HPM_QEIV2_EVENT_CYCLE1            (1U << 22U)
#define HPM_QEIV2_EVENT_PULSE0            (1U << 21U)
#define HPM_QEIV2_EVENT_PULSE1            (1U << 20U)
#define HPM_QEIV2_EVENT_HOME2             (1U << 19U)
#define HPM_QEIV2_EVENT_FAULT             (1U << 18U)
#define HPM_QEIV2_EVENT_ALL                                                                        \
	(HPM_QEIV2_EVENT_WATCHDOG | HPM_QEIV2_EVENT_HOME | HPM_QEIV2_EVENT_POSITION_COMPARE |      \
	 HPM_QEIV2_EVENT_Z_PHASE | HPM_QEIV2_EVENT_Z_MISS | HPM_QEIV2_EVENT_WIDTH_TIME |           \
	 HPM_QEIV2_EVENT_POSITION2_COMPARE | HPM_QEIV2_EVENT_DIRECTION_CHANGE |                    \
	 HPM_QEIV2_EVENT_CYCLE0 | HPM_QEIV2_EVENT_CYCLE1 | HPM_QEIV2_EVENT_PULSE0 |                \
	 HPM_QEIV2_EVENT_PULSE1 | HPM_QEIV2_EVENT_HOME2 | HPM_QEIV2_EVENT_FAULT)

/** Compare source used by one of the two QEIV2 comparators. */
enum hpm_qeiv2_compare_source {
	HPM_QEIV2_COMPARE_PHASE = 0,
	HPM_QEIV2_COMPARE_POSITION,
};

/** Direction matched by a comparator. */
enum hpm_qeiv2_compare_direction {
	HPM_QEIV2_COMPARE_POSITIVE = 0,
	HPM_QEIV2_COMPARE_NEGATIVE,
};

struct hpm_qeiv2_compare_config {
	enum hpm_qeiv2_compare_source source;
	/** Phase-counter or position value to match. */
	uint32_t value;
	/** Ignore the configured direction. */
	bool ignore_direction;
	enum hpm_qeiv2_compare_direction direction;
	/** Ignore the revolution-counter condition. Valid for phase compares only. */
	bool ignore_z;
	/** Revolution-counter value to match when @ref ignore_z is false. */
	uint32_t z_value;
};

/** Raw snapshots produced by the pulse-count and duration-based speed counters. */
struct hpm_qeiv2_speed_snapshot {
	/** Cycles elapsed for the configured pulse count (latest sample). */
	uint32_t pulse_based_cycles;
	/** Cycles elapsed for the configured pulse count (previous sample). */
	uint32_t pulse_based_cycles_previous;
	/** Pulses observed for the configured cycle count (latest sample). */
	uint32_t duration_based_pulses;
	/** Pulses observed for the configured cycle count (previous sample). */
	uint32_t duration_based_pulses_previous;
	/** Cycles observed for the configured cycle count (latest sample). */
	uint32_t duration_based_cycles;
	/** Cycles observed for the configured cycle count (previous sample). */
	uint32_t duration_based_cycles_previous;
};

/**
 * Callback invoked from the QEIV2 interrupt context.
 *
 * @param dev QEIV2 sensor device.
 * @param events Enabled event flags that fired.
 * @param user_data Callback context.
 */
typedef void (*hpm_qeiv2_event_callback_t)(const struct device *dev, uint32_t events,
					   void *user_data);

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_qeiv2_driver_api {
	/** Must remain first so the device also implements the sensor API. */
	struct sensor_driver_api sensor_api;
	int (*configure_mode)(const struct device *dev, const struct hpm_qeiv2_mode_config *config);
	int (*configure_speed)(const struct device *dev, uint8_t channel, uint32_t pulse_count,
			       uint32_t cycle_count);
	int (*get_speed_snapshot)(const struct device *dev, uint8_t channel,
				  struct hpm_qeiv2_speed_snapshot *snapshot);
	int (*configure_compare)(const struct device *dev, uint8_t channel,
				 const struct hpm_qeiv2_compare_config *config);
	int (*get_events)(const struct device *dev, uint32_t *events);
	int (*clear_events)(const struct device *dev, uint32_t events);
	int (*set_event_callback)(const struct device *dev, uint32_t event_mask,
				  hpm_qeiv2_event_callback_t callback, void *user_data);
};

DEVICE_API_EXTENDS(hpm_qeiv2, sensor, sensor_api);
/** @endcond */

/**
 * @brief Reconfigure the QEIV2 decoder mode.
 *
 * The driver resets the position state, clears pending events and invalidates
 * comparator source selection. In UVW mode the six default position codes are
 * installed using the SDK defaults. Sine/cosine ADC modes are not supported.
 *
 * @param dev QEIV2 device.
 * @param config Decoder configuration.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid configuration.
 * @retval -EIO if the SDK rejects the configuration.
 */
static inline int hpm_qeiv2_configure_mode(const struct device *dev,
					   const struct hpm_qeiv2_mode_config *config)
{
	return DEVICE_API_GET(hpm_qeiv2, dev)->configure_mode(dev, config);
}

/**
 * @brief Configure the two hardware speed counters for one measurement channel.
 *
 * Both thresholds must be non-zero. The pulse and duration measurement paths
 * are independent in hardware and run at the same time.
 *
 * @param dev QEIV2 device.
 * @param channel Speed channel, from 0 through 1.
 * @param pulse_count Number of input pulses used for pulse-based measurement.
 * @param cycle_count Number of peripheral clock cycles used for duration-based measurement.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid channel or zero threshold.
 */
static inline int hpm_qeiv2_configure_speed(const struct device *dev, uint8_t channel,
					    uint32_t pulse_count, uint32_t cycle_count)
{
	return DEVICE_API_GET(hpm_qeiv2, dev)
		->configure_speed(dev, channel, pulse_count, cycle_count);
}

/**
 * @brief Read the latest two raw speed-counter snapshots.
 *
 * @param dev QEIV2 device.
 * @param channel Speed channel, from 0 through 1.
 * @param snapshot Output snapshot.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid channel or null output pointer.
 */
static inline int hpm_qeiv2_get_speed_snapshot(const struct device *dev, uint8_t channel,
					       struct hpm_qeiv2_speed_snapshot *snapshot)
{
	return DEVICE_API_GET(hpm_qeiv2, dev)->get_speed_snapshot(dev, channel, snapshot);
}

/**
 * @brief Configure one of the two QEIV2 comparators.
 *
 * Both comparators must use the same source because the hardware has one shared
 * phase/position register selection.
 *
 * @param dev QEIV2 device.
 * @param channel Comparator channel, from 0 through 1.
 * @param config Comparator configuration.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid configuration.
 * @retval -ENOTSUP if the two comparators request different sources.
 */
static inline int hpm_qeiv2_configure_compare(const struct device *dev, uint8_t channel,
					      const struct hpm_qeiv2_compare_config *config)
{
	return DEVICE_API_GET(hpm_qeiv2, dev)->configure_compare(dev, channel, config);
}

/**
 * @brief Read and latch pending event flags.
 *
 * @param dev QEIV2 device.
 * @param events Output combination of HPM_QEIV2_EVENT_* flags.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p events is NULL.
 */
static inline int hpm_qeiv2_get_events(const struct device *dev, uint32_t *events)
{
	return DEVICE_API_GET(hpm_qeiv2, dev)->get_events(dev, events);
}

/**
 * @brief Clear event flags.
 *
 * @param dev QEIV2 device.
 * @param events Combination of HPM_QEIV2_EVENT_* flags to clear.
 *
 * @retval 0 on success.
 * @retval -EINVAL for unknown event bits.
 */
static inline int hpm_qeiv2_clear_events(const struct device *dev, uint32_t events)
{
	return DEVICE_API_GET(hpm_qeiv2, dev)->clear_events(dev, events);
}

/**
 * @brief Enable an event interrupt and install a callback.
 *
 * Passing a NULL callback or a zero mask disables all QEIV2 interrupts.
 * The callback runs in interrupt context.
 *
 * @param dev QEIV2 device.
 * @param event_mask Combination of HPM_QEIV2_EVENT_* flags.
 * @param callback Callback to invoke, or NULL to disable.
 * @param user_data Callback context.
 *
 * @retval 0 on success.
 * @retval -EINVAL for unknown event bits.
 */
static inline int hpm_qeiv2_set_event_callback(const struct device *dev, uint32_t event_mask,
					       hpm_qeiv2_event_callback_t callback, void *user_data)
{
	return DEVICE_API_GET(hpm_qeiv2, dev)
		->set_event_callback(dev, event_mask, callback, user_data);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_SENSOR_HPMICRO_QEIV2_H_ */
