/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SYNT_H_
#define ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SYNT_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro synchronization timer
 * @defgroup hpm_synt_interface HPMicro SYNT
 * @ingroup io_interfaces
 * @{
 */

/** Timestamp update operation. */
enum hpm_synt_timestamp_update {
	HPM_SYNT_TIMESTAMP_SET = 0,
	HPM_SYNT_TIMESTAMP_INCREMENT,
	HPM_SYNT_TIMESTAMP_DECREMENT,
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_synt_driver_api {
	int (*set_reload)(const struct device *dev, uint32_t reload);
	int (*set_comparator)(const struct device *dev, uint8_t channel, uint32_t value);
	int (*counter_enable)(const struct device *dev, bool enable);
	int (*counter_reset)(const struct device *dev);
	int (*get_counter)(const struct device *dev, uint32_t *value);
	int (*counter_debug_stop)(const struct device *dev, bool enable);
	int (*timestamp_enable)(const struct device *dev, bool enable);
	int (*timestamp_reset)(const struct device *dev);
	int (*timestamp_update)(const struct device *dev, enum hpm_synt_timestamp_update operation,
				uint32_t value);
	int (*get_timestamp)(const struct device *dev, uint32_t *value);
	int (*get_saved_timestamp)(const struct device *dev, uint32_t *value);
	int (*timestamp_debug_stop)(const struct device *dev, bool enable);
};
/** @endcond */

/** Set the synchronization counter reload value. */
static inline int hpm_synt_set_reload(const struct device *dev, uint32_t reload)
{
	return DEVICE_API_GET(hpm_synt, dev)->set_reload(dev, reload);
}

/** Set a synchronization comparator value. */
static inline int hpm_synt_set_comparator(const struct device *dev, uint8_t channel, uint32_t value)
{
	return DEVICE_API_GET(hpm_synt, dev)->set_comparator(dev, channel, value);
}

/** Enable or disable the synchronization counter. */
static inline int hpm_synt_counter_enable(const struct device *dev, bool enable)
{
	return DEVICE_API_GET(hpm_synt, dev)->counter_enable(dev, enable);
}

/** Reset the synchronization counter. */
static inline int hpm_synt_counter_reset(const struct device *dev)
{
	return DEVICE_API_GET(hpm_synt, dev)->counter_reset(dev);
}

/** Read the synchronization counter. */
static inline int hpm_synt_get_counter(const struct device *dev, uint32_t *value)
{
	return DEVICE_API_GET(hpm_synt, dev)->get_counter(dev, value);
}

/** Stop the synchronization counter while the CPU is halted for debug. */
static inline int hpm_synt_counter_debug_stop(const struct device *dev, bool enable)
{
	return DEVICE_API_GET(hpm_synt, dev)->counter_debug_stop(dev, enable);
}

/** Enable or disable the timestamp counter. */
static inline int hpm_synt_timestamp_enable(const struct device *dev, bool enable)
{
	return DEVICE_API_GET(hpm_synt, dev)->timestamp_enable(dev, enable);
}

/** Reset the timestamp counter to zero. */
static inline int hpm_synt_timestamp_reset(const struct device *dev)
{
	return DEVICE_API_GET(hpm_synt, dev)->timestamp_reset(dev);
}

/** Set, increment or decrement the timestamp counter. */
static inline int hpm_synt_timestamp_update(const struct device *dev,
					    enum hpm_synt_timestamp_update operation,
					    uint32_t value)
{
	return DEVICE_API_GET(hpm_synt, dev)->timestamp_update(dev, operation, value);
}

/** Read the current timestamp. */
static inline int hpm_synt_get_timestamp(const struct device *dev, uint32_t *value)
{
	return DEVICE_API_GET(hpm_synt, dev)->get_timestamp(dev, value);
}

/** Read the timestamp captured by an external trigger. */
static inline int hpm_synt_get_saved_timestamp(const struct device *dev, uint32_t *value)
{
	return DEVICE_API_GET(hpm_synt, dev)->get_saved_timestamp(dev, value);
}

/** Stop the timestamp counter while the CPU is halted for debug. */
static inline int hpm_synt_timestamp_debug_stop(const struct device *dev, bool enable)
{
	return DEVICE_API_GET(hpm_synt, dev)->timestamp_debug_stop(dev, enable);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SYNT_H_ */
