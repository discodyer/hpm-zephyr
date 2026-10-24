/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_MMC_H_
#define ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_MMC_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro motion processing unit
 * @defgroup hpm_mmc_interface HPMicro MMC
 * @ingroup io_interfaces
 * @{
 */

#define HPM_MMC_PREDICTOR_COUNT 2U

#define HPM_MMC_TRACK_STATUS_SHADOW_READ_DONE    (1U << 0)
#define HPM_MMC_TRACK_STATUS_COEFFICIENTS_DONE   (1U << 1)
#define HPM_MMC_TRACK_STATUS_POSITION_DONE       (1U << 2)
#define HPM_MMC_TRACK_STATUS_OUT_OF_SYNC         (1U << 4)
#define HPM_MMC_TRACK_STATUS_IDLE                (1U << 5)
#define HPM_MMC_TRACK_STATUS_PRED1_POSITION_DONE (1U << 6)
#define HPM_MMC_TRACK_STATUS_PRED0_POSITION_DONE (1U << 7)
#define HPM_MMC_TRACK_STATUS_DELTA_POSITION_DONE (1U << 8)
#define HPM_MMC_TRACK_STATUS_POSITION_TRIGGER    (1U << 9)
#define HPM_MMC_TRACK_STATUS_SPEED_TRIGGER       (1U << 10)

#define HPM_MMC_PREDICTOR_STATUS_IDLE                (1U << 5)
#define HPM_MMC_PREDICTOR_STATUS_DELTA_POSITION_DONE (1U << 6)
#define HPM_MMC_PREDICTOR_STATUS_POSITION_TRIGGER    (1U << 8)
#define HPM_MMC_PREDICTOR_STATUS_SPEED_TRIGGER       (1U << 9)
#define HPM_MMC_PREDICTOR_STATUS_OPEN_LOOP           (1U << 10)

/** Event source handled by the shared MMC interrupt. */
enum hpm_mmc_event_source {
	HPM_MMC_EVENT_SOURCE_TRACK = 0,
	HPM_MMC_EVENT_SOURCE_PREDICTOR0,
	HPM_MMC_EVENT_SOURCE_PREDICTOR1,
	HPM_MMC_EVENT_SOURCE_COUNT,
};

/** Track events that can generate an interrupt. */
#define HPM_MMC_TRACK_EVENT_ALL                                                                    \
	(HPM_MMC_TRACK_STATUS_SHADOW_READ_DONE | HPM_MMC_TRACK_STATUS_COEFFICIENTS_DONE |          \
	 HPM_MMC_TRACK_STATUS_POSITION_DONE | HPM_MMC_TRACK_STATUS_OUT_OF_SYNC |                   \
	 HPM_MMC_TRACK_STATUS_PRED1_POSITION_DONE | HPM_MMC_TRACK_STATUS_PRED0_POSITION_DONE |     \
	 HPM_MMC_TRACK_STATUS_DELTA_POSITION_DONE | HPM_MMC_TRACK_STATUS_POSITION_TRIGGER |        \
	 HPM_MMC_TRACK_STATUS_SPEED_TRIGGER)

/** Predictor events that can generate an interrupt. */
#define HPM_MMC_PREDICTOR_EVENT_ALL                                                                \
	(HPM_MMC_PREDICTOR_STATUS_DELTA_POSITION_DONE |                                            \
	 HPM_MMC_PREDICTOR_STATUS_POSITION_TRIGGER | HPM_MMC_PREDICTOR_STATUS_SPEED_TRIGGER)

typedef void (*hpm_mmc_event_callback_t)(const struct device *dev, uint32_t events,
					 void *user_data);

/** Trigger used to apply new position or delta-position parameters. */
enum hpm_mmc_position_trigger {
	HPM_MMC_POSITION_TRIGGER_TIMESTAMP = 0,
	HPM_MMC_POSITION_TRIGGER_INPUT0_RISING,
	HPM_MMC_POSITION_TRIGGER_INPUT1_RISING,
	HPM_MMC_POSITION_TRIGGER_OUTPUT0_RISING,
	HPM_MMC_POSITION_TRIGGER_OUTPUT1_RISING,
	HPM_MMC_POSITION_TRIGGER_SELF_POSITION,
	HPM_MMC_POSITION_TRIGGER_SELF_SPEED,
};

/** Position parameter fields selected by an update request. */
enum hpm_mmc_position_update {
	HPM_MMC_POSITION_UPDATE_NONE = 0,
	HPM_MMC_POSITION_UPDATE_POSITION = (1U << 0),
	HPM_MMC_POSITION_UPDATE_REVOLUTION = (1U << 1),
	HPM_MMC_POSITION_UPDATE_SPEED = (1U << 2),
	HPM_MMC_POSITION_UPDATE_ACCELERATION = (1U << 3),
	HPM_MMC_POSITION_UPDATE_ALL = (1U << 4) - 1U,
};

/** Track coefficient fields selected by an update request. */
enum hpm_mmc_coefficient_update {
	HPM_MMC_COEFFICIENT_UPDATE_NONE = 0,
	HPM_MMC_COEFFICIENT_UPDATE_P = (1U << 0),
	HPM_MMC_COEFFICIENT_UPDATE_I = (1U << 1),
	HPM_MMC_COEFFICIENT_UPDATE_A = (1U << 2),
	HPM_MMC_COEFFICIENT_UPDATE_ALL = (1U << 3) - 1U,
};

/** Predictor execution period. */
enum hpm_mmc_predictor_time_mode {
	HPM_MMC_PREDICTOR_BY_PERIOD = 0,
	HPM_MMC_PREDICTOR_CONTINUOUS = 1,
	HPM_MMC_PREDICTOR_ONCE = 2,
};

struct hpm_mmc_track_position_mode {
	bool discrete;
	uint32_t discrete_lines;
	uint32_t continuous_step_threshold;
	uint32_t continuous_circle_threshold;
	uint32_t out_of_sync_theta_threshold;
};

struct hpm_mmc_track_mode {
	bool force_acceleration_zero;
	bool enable_ms_coefficients;
	bool open_loop;
	bool position_16bit;
	bool sync_new_position;
	struct hpm_mmc_track_position_mode position_mode;
};

struct hpm_mmc_position_config {
	uint32_t time;
	uint32_t position;
	int32_t revolution;
	double speed;
	double acceleration;
	uint32_t update_mask;
	enum hpm_mmc_position_trigger trigger;
};

struct hpm_mmc_coefficient_config {
	uint32_t time;
	double p;
	double i;
	double a;
	uint32_t update_mask;
};

struct hpm_mmc_position_trigger_config {
	bool enable;
	bool less_than;
	uint32_t position;
	int32_t revolution;
};

struct hpm_mmc_speed_trigger_config {
	bool enable;
	bool less_than;
	bool absolute;
	double speed;
};

struct hpm_mmc_predictor_mode {
	bool speed_trigger_interrupt;
	bool position_trigger_interrupt;
	bool delta_position_interrupt;
	bool open_loop;
	enum hpm_mmc_predictor_time_mode time_mode;
	uint8_t first_trigger_type;
	uint8_t next_trigger_type;
};

struct hpm_mmc_predictor_period {
	uint32_t offset_time;
	uint32_t period_time;
	uint32_t first_time;
};

struct hpm_mmc_position_result {
	uint32_t time;
	uint32_t position;
	int32_t revolution;
	double speed;
	double acceleration;
};

struct hpm_mmc_coefficient_result {
	double p;
	double i;
	double a;
};

struct hpm_mmc_status {
	uint32_t track;
	uint32_t predictor[HPM_MMC_PREDICTOR_COUNT];
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_mmc_driver_api {
	int (*configure_track_mode)(const struct device *dev,
				    const struct hpm_mmc_track_mode *config);
	int (*configure_track_coefficients)(const struct device *dev,
					    const struct hpm_mmc_coefficient_config *config);
	int (*configure_track_position)(const struct device *dev,
					const struct hpm_mmc_position_config *config);
	int (*configure_track_delta_position)(const struct device *dev,
					      const struct hpm_mmc_position_config *config);
	int (*configure_track_position_trigger)(
		const struct device *dev, const struct hpm_mmc_position_trigger_config *config);
	int (*configure_track_speed_trigger)(const struct device *dev,
					     const struct hpm_mmc_speed_trigger_config *config);
	int (*configure_predictor_mode)(const struct device *dev, uint8_t predictor,
					const struct hpm_mmc_predictor_mode *config);
	int (*configure_predictor_position)(const struct device *dev, uint8_t predictor,
					    const struct hpm_mmc_position_config *config);
	int (*configure_predictor_delta_position)(const struct device *dev, uint8_t predictor,
						  const struct hpm_mmc_position_config *config);
	int (*configure_predictor_period)(const struct device *dev, uint8_t predictor,
					  const struct hpm_mmc_predictor_period *config);
	int (*configure_predictor_position_trigger)(
		const struct device *dev, uint8_t predictor,
		const struct hpm_mmc_position_trigger_config *config);
	int (*configure_predictor_speed_trigger)(const struct device *dev, uint8_t predictor,
						 const struct hpm_mmc_speed_trigger_config *config);
	int (*enable_predictor)(const struct device *dev, uint8_t predictor, bool enable);
	int (*enable)(const struct device *dev, bool enable);
	int (*reset)(const struct device *dev);
	int (*get_track_result)(const struct device *dev, struct hpm_mmc_position_result *position,
				struct hpm_mmc_coefficient_result *coefficients);
	int (*get_predictor_result)(const struct device *dev, uint8_t predictor,
				    struct hpm_mmc_position_result *result);
	int (*get_status)(const struct device *dev, struct hpm_mmc_status *status);
	int (*clear_track_status)(const struct device *dev, uint32_t mask);
	int (*clear_predictor_status)(const struct device *dev, uint8_t predictor, uint32_t mask);
	int (*set_event_callback)(const struct device *dev, enum hpm_mmc_event_source source,
				  uint32_t event_mask, hpm_mmc_event_callback_t callback,
				  void *user_data);
};
/** @endcond */

static inline int hpm_mmc_configure_track_mode(const struct device *dev,
					       const struct hpm_mmc_track_mode *config)
{
	return DEVICE_API_GET(hpm_mmc, dev)->configure_track_mode(dev, config);
}

static inline int
hpm_mmc_configure_track_coefficients(const struct device *dev,
				     const struct hpm_mmc_coefficient_config *config)
{
	return DEVICE_API_GET(hpm_mmc, dev)->configure_track_coefficients(dev, config);
}

static inline int hpm_mmc_configure_track_position(const struct device *dev,
						   const struct hpm_mmc_position_config *config)
{
	return DEVICE_API_GET(hpm_mmc, dev)->configure_track_position(dev, config);
}

static inline int
hpm_mmc_configure_track_delta_position(const struct device *dev,
				       const struct hpm_mmc_position_config *config)
{
	return DEVICE_API_GET(hpm_mmc, dev)->configure_track_delta_position(dev, config);
}

static inline int
hpm_mmc_configure_track_position_trigger(const struct device *dev,
					 const struct hpm_mmc_position_trigger_config *config)
{
	return DEVICE_API_GET(hpm_mmc, dev)->configure_track_position_trigger(dev, config);
}

static inline int
hpm_mmc_configure_track_speed_trigger(const struct device *dev,
				      const struct hpm_mmc_speed_trigger_config *config)
{
	return DEVICE_API_GET(hpm_mmc, dev)->configure_track_speed_trigger(dev, config);
}

static inline int hpm_mmc_configure_predictor_mode(const struct device *dev, uint8_t predictor,
						   const struct hpm_mmc_predictor_mode *config)
{
	return DEVICE_API_GET(hpm_mmc, dev)->configure_predictor_mode(dev, predictor, config);
}

static inline int hpm_mmc_configure_predictor_position(const struct device *dev, uint8_t predictor,
						       const struct hpm_mmc_position_config *config)
{
	return DEVICE_API_GET(hpm_mmc, dev)->configure_predictor_position(dev, predictor, config);
}

static inline int
hpm_mmc_configure_predictor_delta_position(const struct device *dev, uint8_t predictor,
					   const struct hpm_mmc_position_config *config)
{
	return DEVICE_API_GET(hpm_mmc, dev)
		->configure_predictor_delta_position(dev, predictor, config);
}

static inline int hpm_mmc_configure_predictor_period(const struct device *dev, uint8_t predictor,
						     const struct hpm_mmc_predictor_period *config)
{
	return DEVICE_API_GET(hpm_mmc, dev)->configure_predictor_period(dev, predictor, config);
}

static inline int
hpm_mmc_configure_predictor_position_trigger(const struct device *dev, uint8_t predictor,
					     const struct hpm_mmc_position_trigger_config *config)
{
	return DEVICE_API_GET(hpm_mmc, dev)
		->configure_predictor_position_trigger(dev, predictor, config);
}

static inline int
hpm_mmc_configure_predictor_speed_trigger(const struct device *dev, uint8_t predictor,
					  const struct hpm_mmc_speed_trigger_config *config)
{
	return DEVICE_API_GET(hpm_mmc, dev)
		->configure_predictor_speed_trigger(dev, predictor, config);
}

static inline int hpm_mmc_enable_predictor(const struct device *dev, uint8_t predictor, bool enable)
{
	return DEVICE_API_GET(hpm_mmc, dev)->enable_predictor(dev, predictor, enable);
}

static inline int hpm_mmc_enable(const struct device *dev, bool enable)
{
	return DEVICE_API_GET(hpm_mmc, dev)->enable(dev, enable);
}

static inline int hpm_mmc_reset(const struct device *dev)
{
	return DEVICE_API_GET(hpm_mmc, dev)->reset(dev);
}

static inline int hpm_mmc_get_track_result(const struct device *dev,
					   struct hpm_mmc_position_result *position,
					   struct hpm_mmc_coefficient_result *coefficients)
{
	return DEVICE_API_GET(hpm_mmc, dev)->get_track_result(dev, position, coefficients);
}

static inline int hpm_mmc_get_predictor_result(const struct device *dev, uint8_t predictor,
					       struct hpm_mmc_position_result *result)
{
	return DEVICE_API_GET(hpm_mmc, dev)->get_predictor_result(dev, predictor, result);
}

static inline int hpm_mmc_get_status(const struct device *dev, struct hpm_mmc_status *status)
{
	return DEVICE_API_GET(hpm_mmc, dev)->get_status(dev, status);
}

static inline int hpm_mmc_clear_track_status(const struct device *dev, uint32_t mask)
{
	return DEVICE_API_GET(hpm_mmc, dev)->clear_track_status(dev, mask);
}

static inline int hpm_mmc_clear_predictor_status(const struct device *dev, uint8_t predictor,
						 uint32_t mask)
{
	return DEVICE_API_GET(hpm_mmc, dev)->clear_predictor_status(dev, predictor, mask);
}

/**
 * @brief Enable event interrupts and install a callback for one MMC event source.
 *
 * Track and predictor sources share the MMC instance interrupt. The track
 * source accepts HPM_MMC_TRACK_STATUS_* interrupt-capable events; predictor
 * sources accept HPM_MMC_PREDICTOR_STATUS_* interrupt-capable events. Passing
 * a NULL callback or zero mask disables that source's callback.
 *
 * @param dev MMC device.
 * @param source Track or one of the two predictors.
 * @param event_mask Event flags to enable.
 * @param callback Callback invoked from interrupt context.
 * @param user_data Callback context.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid source or event mask.
 */
static inline int hpm_mmc_set_event_callback(const struct device *dev,
					     enum hpm_mmc_event_source source, uint32_t event_mask,
					     hpm_mmc_event_callback_t callback, void *user_data)
{
	return DEVICE_API_GET(hpm_mmc, dev)
		->set_event_callback(dev, source, event_mask, callback, user_data);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_MMC_H_ */
