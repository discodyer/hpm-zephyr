/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_QEO_H_
#define ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_QEO_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro quadrature encoder output interface
 * @defgroup hpm_qeo_interface HPMicro QEO
 * @ingroup io_interfaces
 * @{
 */

/** QEO ABZ output format. */
enum hpm_qeo_abz_output {
	/** A/B are quadrature signals and Z is the index pulse. */
	HPM_QEO_ABZ_OUTPUT_ABZ = 0,
	/** A is a speed pulse, B is a direction pulse and Z is unused. */
	HPM_QEO_ABZ_OUTPUT_PULSE_DIRECTION = 1,
	/** A is a forward pulse, B is a reverse pulse and Z is unused. */
	HPM_QEO_ABZ_OUTPUT_UP_DOWN = 2,
	/** A/B/Z are three-phase quadrature pulses. */
	HPM_QEO_ABZ_OUTPUT_THREE_PHASE = 3,
};

/** Z pulse width for quadrature ABZ output. */
enum hpm_qeo_z_pulse_width {
	HPM_QEO_Z_PULSE_25_PERCENT = 0,
	HPM_QEO_Z_PULSE_75_PERCENT = 1,
	HPM_QEO_Z_PULSE_100_PERCENT = 2,
};

/** QEO position source. */
enum hpm_qeo_position_source {
	HPM_QEO_POSITION_HARDWARE = 0,
	HPM_QEO_POSITION_SOFTWARE = 1,
};

/** Static QEO ABZ configuration. */
struct hpm_qeo_abz_config {
	/** Number of output lines per mechanical revolution. */
	uint32_t lines;
	/** Maximum A pulse frequency in Hz. Zero leaves the hardware default. */
	uint32_t max_frequency;
	/** Optional watchdog frequency in Hz. Zero disables the watchdog. */
	uint32_t watchdog_frequency;
	enum hpm_qeo_abz_output output;
	enum hpm_qeo_z_pulse_width z_pulse_width;
	bool invert_a;
	bool invert_b;
	bool invert_z;
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_qeo_driver_api {
	int (*configure_abz)(const struct device *dev, const struct hpm_qeo_abz_config *config);
	int (*set_position_source)(const struct device *dev, enum hpm_qeo_position_source source);
	int (*inject_position)(const struct device *dev, uint32_t position);
	int (*get_position)(const struct device *dev, uint32_t *position);
	int (*calculation_finished)(const struct device *dev, bool *finished);
};
/** @endcond */

/**
 * @brief Configure QEO quadrature output.
 *
 * @param dev QEO device.
 * @param config ABZ configuration.
 *
 * @retval 0 on success.
 * @retval -EINVAL for invalid configuration.
 * @retval -EIO if the SDK rejects the maximum or watchdog frequency.
 */
static inline int hpm_qeo_configure_abz(const struct device *dev,
					const struct hpm_qeo_abz_config *config)
{
	return DEVICE_API_GET(hpm_qeo, dev)->configure_abz(dev, config);
}

/**
 * @brief Select the QEO position source.
 *
 * @param dev QEO device.
 * @param source Hardware position manager or software injection.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid source.
 */
static inline int hpm_qeo_set_position_source(const struct device *dev,
					      enum hpm_qeo_position_source source)
{
	return DEVICE_API_GET(hpm_qeo, dev)->set_position_source(dev, source);
}

/**
 * @brief Inject a position value.
 *
 * The position source must be HPM_QEO_POSITION_SOFTWARE.
 *
 * @param dev QEO device.
 * @param position 32-bit position where full scale is one revolution.
 *
 * @retval 0 on success.
 * @retval -EACCES if software position injection is not selected.
 */
static inline int hpm_qeo_inject_position(const struct device *dev, uint32_t position)
{
	return DEVICE_API_GET(hpm_qeo, dev)->inject_position(dev, position);
}

/**
 * @brief Read the QEO internal ABZ position.
 *
 * @param dev QEO device.
 * @param position Output position where full scale is one revolution.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p position is NULL.
 */
static inline int hpm_qeo_get_position(const struct device *dev, uint32_t *position)
{
	return DEVICE_API_GET(hpm_qeo, dev)->get_position(dev, position);
}

/**
 * @brief Query whether QEO finished the latest position calculation.
 *
 * @param dev QEO device.
 * @param finished Output completion state.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p finished is NULL.
 */
static inline int hpm_qeo_calculation_finished(const struct device *dev, bool *finished)
{
	return DEVICE_API_GET(hpm_qeo, dev)->calculation_finished(dev, finished);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_QEO_H_ */
