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

#define HPM_QEO_WAVE_CHANNEL_COUNT 3U
#define HPM_QEO_PWM_CHANNEL_COUNT  8U
#define HPM_QEO_PWM_PHASE_COUNT    12U

/** QEO wave output type. */
enum hpm_qeo_wave_type {
	/** Cosine wave. */
	HPM_QEO_WAVE_COSINE = 0,
	/** Saddle wave. */
	HPM_QEO_WAVE_SADDLE = 1,
	/** Full-wave rectified cosine. */
	HPM_QEO_WAVE_ABS_COSINE = 2,
	/** Sawtooth wave. */
	HPM_QEO_WAVE_SAW = 3,
};

/** Saddle wave generation mode. */
enum hpm_qeo_wave_saddle_type {
	/** Standard saddle wave; channels should be 120 degrees apart. */
	HPM_QEO_WAVE_SADDLE_STANDARD = 0,
	/** Saddle wave synthesized by triangular-wave superposition. */
	HPM_QEO_WAVE_SADDLE_TRIANGULAR = 1,
};

/** Action when a wave output is above its maximum limit. */
enum hpm_qeo_wave_above_max_limit {
	HPM_QEO_WAVE_ABOVE_MAX_LIMIT_MAX = 0,
	HPM_QEO_WAVE_ABOVE_MAX_LIMIT_ZERO = 1,
	HPM_QEO_WAVE_ABOVE_MAX_LIMIT_LEVEL0 = 2,
};

/** Action in the high-area limit regions. */
enum hpm_qeo_wave_high_area_limit {
	HPM_QEO_WAVE_HIGH_AREA_LIMIT_MAX = 0,
	HPM_QEO_WAVE_HIGH_AREA_LIMIT_LEVEL0 = 1,
};

/** Action in the low-area limit regions. */
enum hpm_qeo_wave_low_area_limit {
	HPM_QEO_WAVE_LOW_AREA_LIMIT_ZERO = 0,
	HPM_QEO_WAVE_LOW_AREA_LIMIT_LEVEL1 = 1,
};

/** Action when a wave output is below its minimum limit. */
enum hpm_qeo_wave_below_min_limit {
	HPM_QEO_WAVE_BELOW_MIN_LIMIT_ZERO = 0,
	HPM_QEO_WAVE_BELOW_MIN_LIMIT_MAX = 1,
	HPM_QEO_WAVE_BELOW_MIN_LIMIT_LEVEL1 = 2,
};

struct hpm_qeo_wave_limit_config {
	enum hpm_qeo_wave_above_max_limit above_max_limit;
	enum hpm_qeo_wave_high_area_limit high_area0_limit;
	enum hpm_qeo_wave_high_area_limit high_area1_limit;
	enum hpm_qeo_wave_low_area_limit low_area0_limit;
	enum hpm_qeo_wave_low_area_limit low_area1_limit;
	enum hpm_qeo_wave_below_min_limit below_min_limit;
	uint16_t min_limit0;
	uint16_t min_limit1;
	uint16_t max_limit0;
	uint16_t max_limit1;
};

struct hpm_qeo_wave_channel_config {
	/** Phase advance in degrees, from 0 through 360. */
	double phase_shift_degrees;
	/** Enable the programmed amplitude scale. */
	bool enable_amplitude;
	/** Amplitude scale; 1.0 is full scale, maximum is below 16.0. */
	double amplitude;
	/** Signed mid-point offset; 1.0 is one range, range is [-16.0, 16.0). */
	double mid_point_shift;
	/** Signed dead-zone compensation in output counts. */
	int16_t deadzone_shift;
	struct hpm_qeo_wave_limit_config limits;
};

struct hpm_qeo_wave_config {
	/** Number of wave periods per input-position revolution. */
	uint32_t lines;
	enum hpm_qeo_wave_type type;
	enum hpm_qeo_wave_saddle_type saddle_type;
	struct hpm_qeo_wave_channel_config channel[HPM_QEO_WAVE_CHANNEL_COUNT];
};

/** QEO PWM FORCE output value. */
enum hpm_qeo_pwm_force_output {
	/** Leave the output controlled by the PWM peripheral. */
	HPM_QEO_PWM_OUTPUT_NOT_FORCED = 0,
	/** Force the output low. */
	HPM_QEO_PWM_OUTPUT_FORCE_LOW = 2,
	/** Force the output high. */
	HPM_QEO_PWM_OUTPUT_FORCE_HIGH = 3,
};

/** QEO PWM safety-mode output value. */
enum hpm_qeo_pwm_safety_output {
	HPM_QEO_PWM_SAFETY_OUTPUT_LOW = 0,
	HPM_QEO_PWM_SAFETY_OUTPUT_HIGH = 1,
	HPM_QEO_PWM_SAFETY_OUTPUT_HIGH_Z = 2,
};

/** PWM phase table selected by the position direction. */
enum hpm_qeo_pwm_phase_table {
	HPM_QEO_PWM_PHASE_TABLE_POSITIVE = 0,
	HPM_QEO_PWM_PHASE_TABLE_NEGATIVE = 1,
};

struct hpm_qeo_pwm_phase_output_table {
	enum hpm_qeo_pwm_force_output output[HPM_QEO_PWM_CHANNEL_COUNT];
};

struct hpm_qeo_pwm_safety_output_table {
	enum hpm_qeo_pwm_safety_output output[HPM_QEO_PWM_CHANNEL_COUNT];
};

struct hpm_qeo_pwm_config {
	/** Number of PWM commutation periods per input-position revolution. */
	uint32_t lines;
	/** Number of commutation steps, from 1 through 12. */
	uint8_t phase_count;
	/** Phase advance in degrees for each of the four PWM phase groups. */
	double phase_shift_degrees[4];
	/** Ignore hardware safety-mode trigger input. */
	bool bypass_hardware_trigger_safety;
	/** Phase table used for the selected position direction. */
	enum hpm_qeo_pwm_phase_table phase_table;
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
	int (*configure_wave)(const struct device *dev, const struct hpm_qeo_wave_config *config);
	int (*get_wave_output)(const struct device *dev, uint8_t channel, uint16_t *value);
	int (*configure_pwm)(const struct device *dev, const struct hpm_qeo_pwm_config *config);
	int (*set_pwm_phase_table)(const struct device *dev, enum hpm_qeo_pwm_phase_table table,
				   uint8_t index,
				   const struct hpm_qeo_pwm_phase_output_table *output);
	int (*select_pwm_phase_table)(const struct device *dev, enum hpm_qeo_pwm_phase_table table);
	int (*set_pwm_safety_table)(const struct device *dev,
				    const struct hpm_qeo_pwm_safety_output_table *output);
	int (*set_pwm_safety)(const struct device *dev, bool enter);
	int (*pwm_in_safety)(const struct device *dev, bool *in_safety);
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
 * @brief Configure QEO wave output.
 *
 * This configures all three wave channels. VD/VQ injection is intentionally not
 * exposed because the pinned SDK helper writes the VQ value into both fields.
 *
 * @param dev QEO device.
 * @param config Wave configuration.
 *
 * @retval 0 on success.
 * @retval -EINVAL for invalid configuration.
 */
static inline int hpm_qeo_configure_wave(const struct device *dev,
					 const struct hpm_qeo_wave_config *config)
{
	return DEVICE_API_GET(hpm_qeo, dev)->configure_wave(dev, config);
}

/**
 * @brief Read a calculated wave output.
 *
 * @param dev QEO device.
 * @param channel Wave channel, from 0 through 2.
 * @param value Output 16-bit wave value.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid channel or null output pointer.
 */
static inline int hpm_qeo_get_wave_output(const struct device *dev, uint8_t channel,
					  uint16_t *value)
{
	return DEVICE_API_GET(hpm_qeo, dev)->get_wave_output(dev, channel, value);
}

/**
 * @brief Configure QEO PWM FORCE output.
 *
 * @param dev QEO device.
 * @param config PWM configuration.
 *
 * @retval 0 on success.
 * @retval -EINVAL for invalid configuration.
 */
static inline int hpm_qeo_configure_pwm(const struct device *dev,
					const struct hpm_qeo_pwm_config *config)
{
	return DEVICE_API_GET(hpm_qeo, dev)->configure_pwm(dev, config);
}

/**
 * @brief Write one positive or negative PWM phase table.
 *
 * @param dev QEO device.
 * @param table Positive or negative table group.
 * @param index Commutation-step index, from 0 through 11.
 * @param output Forced output value for each PWM channel.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid table, index or output value.
 */
static inline int hpm_qeo_set_pwm_phase_table(const struct device *dev,
					      enum hpm_qeo_pwm_phase_table table, uint8_t index,
					      const struct hpm_qeo_pwm_phase_output_table *output)
{
	return DEVICE_API_GET(hpm_qeo, dev)->set_pwm_phase_table(dev, table, index, output);
}

/**
 * @brief Select the positive or negative PWM phase table group.
 *
 * @param dev QEO device.
 * @param table Table group to use.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid table group.
 */
static inline int hpm_qeo_select_pwm_phase_table(const struct device *dev,
						 enum hpm_qeo_pwm_phase_table table)
{
	return DEVICE_API_GET(hpm_qeo, dev)->select_pwm_phase_table(dev, table);
}

/**
 * @brief Configure the PWM safety output table.
 *
 * @param dev QEO device.
 * @param output Safety output value for each PWM channel.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid output value.
 */
static inline int hpm_qeo_set_pwm_safety_table(const struct device *dev,
					       const struct hpm_qeo_pwm_safety_output_table *output)
{
	return DEVICE_API_GET(hpm_qeo, dev)->set_pwm_safety_table(dev, output);
}

/**
 * @brief Enter or leave software-forced PWM safety mode.
 *
 * @param dev QEO device.
 * @param enter True to enter safety mode, false to leave it.
 *
 * @retval 0 on success.
 */
static inline int hpm_qeo_set_pwm_safety(const struct device *dev, bool enter)
{
	return DEVICE_API_GET(hpm_qeo, dev)->set_pwm_safety(dev, enter);
}

/**
 * @brief Query whether QEO PWM is in safety mode.
 *
 * @param dev QEO device.
 * @param in_safety Output safety-mode state.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p in_safety is NULL.
 */
static inline int hpm_qeo_pwm_in_safety(const struct device *dev, bool *in_safety)
{
	return DEVICE_API_GET(hpm_qeo, dev)->pwm_in_safety(dev, in_safety);
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
