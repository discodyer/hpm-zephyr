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
	/** Single-phase pulse input on A. */
	HPM_QEIV2_MODE_SINGLE,
	/** Single sine input on ADC X; requires hpm_qeiv2_configure_sincos(). */
	HPM_QEIV2_MODE_SINE,
	/** Orthogonal sine/cosine input on ADC X/Y. */
	HPM_QEIV2_MODE_SIN_COS,
};

/** ADC input selection for sine/cosine decoding. */
struct hpm_qeiv2_adc_config {
	/** ADC instance index. */
	uint8_t adc;
	/** ADC channel index in the QEIV2 ADC channel namespace. */
	uint8_t channel;
	/** Q14 correction coefficient for the selected ADC input. */
	int16_t param0;
	/** Q14 correction coefficient for the other ADC input. */
	int16_t param1;
	/** Value subtracted from the unsigned ADC sample. */
	uint32_t offset;
};

/** Sine/cosine decoder configuration. */
struct hpm_qeiv2_sincos_config {
	/** X-axis ADC source. */
	struct hpm_qeiv2_adc_config x;
	/** Y-axis ADC source. */
	struct hpm_qeiv2_adc_config y;
	/** Enable the Y input; must match the configured decoder mode. */
	bool enable_y;
	/** ADC X/Y sampling delay in QEIV2 clock cycles. */
	uint32_t xy_delay;
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

/** Edge and direction conditions that set the HOME flags. */
struct hpm_qeiv2_home_config {
	/** Match H0 rising edge while rotating forward. */
	bool rising_forward;
	/** Match H0 rising edge while rotating reverse. */
	bool rising_reverse;
	/** Match H0 falling edge while rotating forward. */
	bool falling_forward;
	/** Match H0 falling edge while rotating reverse. */
	bool falling_reverse;
	/** Match H1 rising edge while rotating forward. */
	bool rising_forward_home2;
	/** Match H1 rising edge while rotating reverse. */
	bool rising_reverse_home2;
	/** Match H1 falling edge while rotating forward. */
	bool falling_forward_home2;
	/** Match H1 falling edge while rotating reverse. */
	bool falling_reverse_home2;
};

/** Fault input behavior. */
struct hpm_qeiv2_fault_config {
	/** Suppress valid position output while the fault input is asserted. */
	bool pause_position;
};

/** Watchdog configuration. */
struct hpm_qeiv2_watchdog_config {
	/** Enable the stall watchdog. */
	bool enable;
	/** Timeout in QEIV2 clock cycles. */
	uint32_t timeout;
	/** Minimum phase-counter change that clears the watchdog; zero clears on any change. */
	uint8_t clear_threshold;
};

/** QEIV2 event routing targets. */
enum hpm_qeiv2_event_route {
	HPM_QEIV2_EVENT_ROUTE_INTERRUPT,
	HPM_QEIV2_EVENT_ROUTE_DMA,
	HPM_QEIV2_EVENT_ROUTE_TRIGGER,
	HPM_QEIV2_EVENT_ROUTE_READ,
};

/** Speed snapshot source used for RPM conversion. */
enum hpm_qeiv2_speed_source {
	/** Use pulse_based_cycles. */
	HPM_QEIV2_SPEED_SOURCE_PULSE,
	/** Use duration_based_pulses and duration_based_cycles. */
	HPM_QEIV2_SPEED_SOURCE_DURATION,
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
	/** Current encoder direction when the snapshot was read. */
	bool reverse;
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
	int (*speed_to_rpm)(const struct device *dev, uint8_t channel,
			    enum hpm_qeiv2_speed_source source,
			    const struct hpm_qeiv2_speed_snapshot *snapshot,
			    struct sensor_value *rpm);
	int (*configure_sincos)(const struct device *dev,
				const struct hpm_qeiv2_sincos_config *config);
	int (*configure_watchdog)(const struct device *dev,
				  const struct hpm_qeiv2_watchdog_config *config);
	int (*configure_home)(const struct device *dev, const struct hpm_qeiv2_home_config *config);
	int (*configure_fault)(const struct device *dev,
			       const struct hpm_qeiv2_fault_config *config);
	int (*set_event_route)(const struct device *dev, enum hpm_qeiv2_event_route route,
			       uint32_t event_mask, bool enable);
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
 * installed using the SDK defaults. Sine modes additionally require
 * hpm_qeiv2_configure_sincos() before the decoder can produce a position.
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
 * @brief Convert a hardware speed snapshot to revolutions per minute.
 *
 * The result is signed according to the encoder direction observed when the
 * snapshot was read. A zero measurement window means that no complete sample
 * is available yet. The configured pulse count and counts-per-revolution must
 * use the same pulse/count unit.
 *
 * @param dev QEIV2 device.
 * @param channel Speed channel, from 0 through 1.
 * @param source Hardware speed measurement path to use.
 * @param snapshot Snapshot returned by hpm_qeiv2_get_speed_snapshot().
 * @param rpm Output speed in revolutions per minute.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid argument or a channel without speed setup.
 * @retval -EDOM if no complete measurement window has been captured.
 * @retval -ERANGE if the result cannot be represented.
 */
static inline int hpm_qeiv2_speed_to_rpm(const struct device *dev, uint8_t channel,
					 enum hpm_qeiv2_speed_source source,
					 const struct hpm_qeiv2_speed_snapshot *snapshot,
					 struct sensor_value *rpm)
{
	return DEVICE_API_GET(hpm_qeiv2, dev)->speed_to_rpm(dev, channel, source, snapshot, rpm);
}

/**
 * @brief Configure sine/cosine ADC input and correction.
 *
 * This configures only the QEIV2 ADC source selectors, offset, correction and
 * X/Y delay. ADC sampling, TRGM ADC matrix routing and the sample trigger must
 * be configured by their owning drivers.
 *
 * @param dev QEIV2 device.
 * @param config ADC source and correction configuration.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid configuration.
 * @retval -ENOTSUP if the current decoder mode is not a sine mode.
 */
static inline int hpm_qeiv2_configure_sincos(const struct device *dev,
					     const struct hpm_qeiv2_sincos_config *config)
{
	return DEVICE_API_GET(hpm_qeiv2, dev)->configure_sincos(dev, config);
}

/**
 * @brief Configure the stall watchdog.
 *
 * @param dev QEIV2 device.
 * @param config Watchdog configuration.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid timeout, clear threshold or enabled zero timeout.
 */
static inline int hpm_qeiv2_configure_watchdog(const struct device *dev,
					       const struct hpm_qeiv2_watchdog_config *config)
{
	return DEVICE_API_GET(hpm_qeiv2, dev)->configure_watchdog(dev, config);
}

/**
 * @brief Configure HOME0/HOME1 edge and direction matching.
 *
 * @param dev QEIV2 device.
 * @param config HOME configuration.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p config is NULL.
 */
static inline int hpm_qeiv2_configure_home(const struct device *dev,
					   const struct hpm_qeiv2_home_config *config)
{
	return DEVICE_API_GET(hpm_qeiv2, dev)->configure_home(dev, config);
}

/**
 * @brief Configure fault input behavior.
 *
 * @param dev QEIV2 device.
 * @param config Fault configuration.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p config is NULL.
 */
static inline int hpm_qeiv2_configure_fault(const struct device *dev,
					    const struct hpm_qeiv2_fault_config *config)
{
	return DEVICE_API_GET(hpm_qeiv2, dev)->configure_fault(dev, config);
}

/**
 * @brief Route QEIV2 events to interrupt, DMA, trigger or read outputs.
 *
 * Masked events are enabled or disabled atomically. Event flags are not
 * cleared by this operation. Interrupt routing coexists with the callback API.
 *
 * @param dev QEIV2 device.
 * @param route Routing target.
 * @param event_mask Combination of HPM_QEIV2_EVENT_* flags.
 * @param enable True to enable, false to disable.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid route or event mask.
 */
static inline int hpm_qeiv2_set_event_route(const struct device *dev,
					    enum hpm_qeiv2_event_route route, uint32_t event_mask,
					    bool enable)
{
	return DEVICE_API_GET(hpm_qeiv2, dev)->set_event_route(dev, route, event_mask, enable);
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
 * Passing a NULL callback or a zero mask removes the callback. Interrupts
 * requested through hpm_qeiv2_set_event_route() remain enabled.
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
