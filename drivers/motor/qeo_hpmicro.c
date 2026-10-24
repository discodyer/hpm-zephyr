/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_qeo

#include <errno.h>
#include <math.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <drivers/motor/hpmicro_qeo.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <hpm_common.h>
#include <hpm_qeo_drv.h>

BUILD_ASSERT((int)HPM_QEO_ABZ_OUTPUT_ABZ == (int)qeo_abz_output_abz);
BUILD_ASSERT((int)HPM_QEO_ABZ_OUTPUT_PULSE_DIRECTION == (int)qeo_abz_output_pulse_revise);
BUILD_ASSERT((int)HPM_QEO_ABZ_OUTPUT_UP_DOWN == (int)qeo_abz_output_up_down);
BUILD_ASSERT((int)HPM_QEO_ABZ_OUTPUT_THREE_PHASE == (int)qeo_abz_output_three_phase);
BUILD_ASSERT((int)HPM_QEO_Z_PULSE_25_PERCENT == (int)qeo_z_pulse_25_percent);
BUILD_ASSERT((int)HPM_QEO_Z_PULSE_75_PERCENT == (int)qeo_z_pulse_75_percent);
BUILD_ASSERT((int)HPM_QEO_Z_PULSE_100_PERCENT == (int)qeo_z_pulse_100_percent);
BUILD_ASSERT((int)HPM_QEO_WAVE_COSINE == (int)qeo_wave_cosine);
BUILD_ASSERT((int)HPM_QEO_WAVE_SADDLE == (int)qeo_wave_saddle);
BUILD_ASSERT((int)HPM_QEO_WAVE_ABS_COSINE == (int)qeo_wave_abs_cosine);
BUILD_ASSERT((int)HPM_QEO_WAVE_SAW == (int)qeo_wave_saw);
BUILD_ASSERT((int)HPM_QEO_WAVE_ABOVE_MAX_LIMIT_MAX == (int)qeo_wave_above_max_limit_max_val);
BUILD_ASSERT((int)HPM_QEO_WAVE_ABOVE_MAX_LIMIT_ZERO == (int)qeo_wave_above_max_limit_zero);
BUILD_ASSERT((int)HPM_QEO_WAVE_ABOVE_MAX_LIMIT_LEVEL0 ==
	     (int)qeo_wave_above_max_limit_max_level0_val);
BUILD_ASSERT((int)HPM_QEO_WAVE_HIGH_AREA_LIMIT_MAX == (int)qeo_wave_high_area_limit_max_val);
BUILD_ASSERT((int)HPM_QEO_WAVE_HIGH_AREA_LIMIT_LEVEL0 ==
	     (int)qeo_wave_high_area_limit_max_level0_val);
BUILD_ASSERT((int)HPM_QEO_WAVE_LOW_AREA_LIMIT_ZERO == (int)qeo_wave_low_area_limit_zero);
BUILD_ASSERT((int)HPM_QEO_WAVE_LOW_AREA_LIMIT_LEVEL1 ==
	     (int)qeo_wave_low_area_limit_min_level1_val);
BUILD_ASSERT((int)HPM_QEO_WAVE_BELOW_MIN_LIMIT_ZERO == (int)qeo_wave_below_min_limit_zero);
BUILD_ASSERT((int)HPM_QEO_WAVE_BELOW_MIN_LIMIT_MAX == (int)qeo_wave_below_min_limit_max_val);
BUILD_ASSERT((int)HPM_QEO_WAVE_BELOW_MIN_LIMIT_LEVEL1 ==
	     (int)qeo_wave_below_min_limit_min_level1_val);
BUILD_ASSERT((int)HPM_QEO_PWM_OUTPUT_NOT_FORCED == (int)qeo_pwm_output_not_force);
BUILD_ASSERT((int)HPM_QEO_PWM_OUTPUT_FORCE_LOW == (int)qeo_pwm_output_force_0);
BUILD_ASSERT((int)HPM_QEO_PWM_OUTPUT_FORCE_HIGH == (int)qeo_pwm_output_force_1);
BUILD_ASSERT((int)HPM_QEO_PWM_SAFETY_OUTPUT_LOW == (int)qeo_pwm_safety_output_0);
BUILD_ASSERT((int)HPM_QEO_PWM_SAFETY_OUTPUT_HIGH == (int)qeo_pwm_safety_output_1);
BUILD_ASSERT((int)HPM_QEO_PWM_SAFETY_OUTPUT_HIGH_Z == (int)qeo_pwm_safety_output_highz);

struct hpm_qeo_config {
	QEO_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
};

struct hpm_qeo_data {
	struct k_spinlock lock;
	uint32_t clock_frequency;
	bool software_position;
};

static int hpm_qeo_drv_configure_abz(const struct device *dev,
				     const struct hpm_qeo_abz_config *config)
{
	const struct hpm_qeo_config *qeo = dev->config;
	struct hpm_qeo_data *data = dev->data;
	qeo_abz_mode_t mode;
	k_spinlock_key_t key;

	if ((config == NULL) || (config->lines == 0U) ||
	    (config->output > HPM_QEO_ABZ_OUTPUT_THREE_PHASE) ||
	    (config->z_pulse_width > HPM_QEO_Z_PULSE_100_PERCENT)) {
		return -EINVAL;
	}

	qeo_abz_get_default_mode_config(qeo->base, &mode);
	mode.a_inv_pol = config->invert_a;
	mode.b_inv_pol = config->invert_b;
	mode.z_inv_pol = config->invert_z;
	mode.output_type = config->output;

	if (config->output == HPM_QEO_ABZ_OUTPUT_ABZ) {
		mode.z_pulse_period = config->z_pulse_width;
	}

	key = k_spin_lock(&data->lock);
	qeo_abz_config_mode(qeo->base, &mode);
	qeo_abz_set_resolution_lines(qeo->base, config->lines);

	if (config->max_frequency != 0U) {
		if (qeo_abz_set_max_frequency(qeo->base, data->clock_frequency,
					      config->max_frequency) != status_success) {
			k_spin_unlock(&data->lock, key);
			return -EIO;
		}
	}

	if (config->watchdog_frequency != 0U) {
		if (qeo_abz_set_wdog_frequency(qeo->base, data->clock_frequency,
					       config->watchdog_frequency) != status_success) {
			k_spin_unlock(&data->lock, key);
			return -EIO;
		}
	} else {
		qeo_abz_disable_wdog(qeo->base);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static bool hpm_qeo_angle_is_valid(double angle)
{
	return isfinite(angle) && (angle >= 0.0) && (angle <= 360.0);
}

static bool hpm_qeo_amplitude_is_valid(double amplitude)
{
	/* The hardware field is unsigned 4.12 fixed point. */
	return isfinite(amplitude) && (amplitude > 0.0) && (amplitude < 16.0);
}

static bool hpm_qeo_mid_point_is_valid(double shift)
{
	/* The hardware field is signed 5.27 fixed point. */
	return isfinite(shift) && (shift >= -16.0) && (shift < 16.0);
}

static bool hpm_qeo_wave_limit_is_valid(const struct hpm_qeo_wave_limit_config *limits)
{
	return (limits->above_max_limit <= HPM_QEO_WAVE_ABOVE_MAX_LIMIT_LEVEL0) &&
	       (limits->high_area0_limit <= HPM_QEO_WAVE_HIGH_AREA_LIMIT_LEVEL0) &&
	       (limits->high_area1_limit <= HPM_QEO_WAVE_HIGH_AREA_LIMIT_LEVEL0) &&
	       (limits->low_area0_limit <= HPM_QEO_WAVE_LOW_AREA_LIMIT_LEVEL1) &&
	       (limits->low_area1_limit <= HPM_QEO_WAVE_LOW_AREA_LIMIT_LEVEL1) &&
	       (limits->below_min_limit <= HPM_QEO_WAVE_BELOW_MIN_LIMIT_LEVEL1);
}

static void hpm_qeo_wave_to_sdk(QEO_Type *base, const struct hpm_qeo_wave_config *config,
				qeo_wave_mode_t *sdk_config)
{
	qeo_wave_get_default_mode_config(base, sdk_config);
	sdk_config->wave_type = (uint8_t)config->type;
	sdk_config->saddle_type = (uint8_t)config->saddle_type;

	/* The SDK structures name the per-channel limit fields explicitly. */
	sdk_config->wave0.above_max_limit = (uint8_t)config->channel[0].limits.above_max_limit;
	sdk_config->wave0.high_area0_limit = (uint8_t)config->channel[0].limits.high_area0_limit;
	sdk_config->wave0.high_area1_limit = (uint8_t)config->channel[0].limits.high_area1_limit;
	sdk_config->wave0.low_area0_limit = (uint8_t)config->channel[0].limits.low_area0_limit;
	sdk_config->wave0.low_area1_limit = (uint8_t)config->channel[0].limits.low_area1_limit;
	sdk_config->wave0.below_min_limit = (uint8_t)config->channel[0].limits.below_min_limit;
	sdk_config->wave1.above_max_limit = (uint8_t)config->channel[1].limits.above_max_limit;
	sdk_config->wave1.high_area0_limit = (uint8_t)config->channel[1].limits.high_area0_limit;
	sdk_config->wave1.high_area1_limit = (uint8_t)config->channel[1].limits.high_area1_limit;
	sdk_config->wave1.low_area0_limit = (uint8_t)config->channel[1].limits.low_area0_limit;
	sdk_config->wave1.low_area1_limit = (uint8_t)config->channel[1].limits.low_area1_limit;
	sdk_config->wave1.below_min_limit = (uint8_t)config->channel[1].limits.below_min_limit;
	sdk_config->wave2.above_max_limit = (uint8_t)config->channel[2].limits.above_max_limit;
	sdk_config->wave2.high_area0_limit = (uint8_t)config->channel[2].limits.high_area0_limit;
	sdk_config->wave2.high_area1_limit = (uint8_t)config->channel[2].limits.high_area1_limit;
	sdk_config->wave2.low_area0_limit = (uint8_t)config->channel[2].limits.low_area0_limit;
	sdk_config->wave2.low_area1_limit = (uint8_t)config->channel[2].limits.low_area1_limit;
	sdk_config->wave2.below_min_limit = (uint8_t)config->channel[2].limits.below_min_limit;
}

static int hpm_qeo_drv_configure_wave(const struct device *dev,
				      const struct hpm_qeo_wave_config *config)
{
	const struct hpm_qeo_config *qeo = dev->config;
	struct hpm_qeo_data *data = dev->data;
	qeo_wave_mode_t sdk_config;
	k_spinlock_key_t key;
	uint8_t channel;

	if ((config == NULL) || (config->lines == 0U) || (config->type > HPM_QEO_WAVE_SAW) ||
	    (config->saddle_type > HPM_QEO_WAVE_SADDLE_TRIANGULAR)) {
		return -EINVAL;
	}

	for (channel = 0U; channel < HPM_QEO_WAVE_CHANNEL_COUNT; channel++) {
		const struct hpm_qeo_wave_channel_config *channel_config =
			&config->channel[channel];

		if (!hpm_qeo_angle_is_valid(channel_config->phase_shift_degrees) ||
		    (channel_config->enable_amplitude &&
		     !hpm_qeo_amplitude_is_valid(channel_config->amplitude)) ||
		    !hpm_qeo_mid_point_is_valid(channel_config->mid_point_shift) ||
		    !hpm_qeo_wave_limit_is_valid(&channel_config->limits)) {
			return -EINVAL;
		}
	}

	hpm_qeo_wave_to_sdk(qeo->base, config, &sdk_config);

	key = k_spin_lock(&data->lock);
	qeo_wave_config_mode(qeo->base, &sdk_config);
	qeo_wave_set_resolution_lines(qeo->base, config->lines);

	for (channel = 0U; channel < HPM_QEO_WAVE_CHANNEL_COUNT; channel++) {
		const struct hpm_qeo_wave_channel_config *channel_config =
			&config->channel[channel];

		qeo_wave_set_phase_shift(qeo->base, channel, channel_config->phase_shift_degrees);
		if (channel_config->enable_amplitude) {
			qeo_wave_enable_amplitude(qeo->base, channel, channel_config->amplitude);
		} else {
			qeo_wave_disable_amplitude(qeo->base, channel);
		}
		qeo_wave_set_mid_point_shift(qeo->base, channel, channel_config->mid_point_shift);
		qeo_wave_set_max_limit(qeo->base, channel, channel_config->limits.max_limit0,
				       channel_config->limits.max_limit1);
		qeo_wave_set_min_limit(qeo->base, channel, channel_config->limits.min_limit0,
				       channel_config->limits.min_limit1);
		qeo_wave_set_deadzone_shift(qeo->base, channel, channel_config->deadzone_shift);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_qeo_drv_get_wave_output(const struct device *dev, uint8_t channel, uint16_t *value)
{
	const struct hpm_qeo_config *qeo = dev->config;

	if ((channel >= HPM_QEO_WAVE_CHANNEL_COUNT) || (value == NULL)) {
		return -EINVAL;
	}

	*value = qeo_get_wave_output_val(qeo->base, channel);

	return 0;
}

static int hpm_qeo_drv_configure_wave_vd_vq(
	const struct device *dev,
	const struct hpm_qeo_wave_vd_vq_config config[HPM_QEO_WAVE_CHANNEL_COUNT])
{
	const struct hpm_qeo_config *qeo = dev->config;
	struct hpm_qeo_data *data = dev->data;
	k_spinlock_key_t key;
	uint8_t channel;

	if (config == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	for (channel = 0U; channel < HPM_QEO_WAVE_CHANNEL_COUNT; channel++) {
		uint32_t enable = BIT(QEO_WAVE_MODE_EN_WAVE0_VD_VQ_INJECT_SHIFT + channel);

		if (config[channel].enable) {
			qeo->base->WAVE.MODE |= enable;
		} else {
			qeo->base->WAVE.MODE &= ~enable;
		}

		qeo->base->WAVE.VD_VQ_INJECT[channel] =
			QEO_WAVE_VD_VQ_INJECT_VD_VAL_SET((uint16_t)config[channel].vd) |
			QEO_WAVE_VD_VQ_INJECT_VQ_VAL_SET((uint16_t)config[channel].vq);
	}

	qeo->base->WAVE.VD_VQ_LOAD = QEO_WAVE_VD_VQ_LOAD_LOAD_MASK;
	k_spin_unlock(&data->lock, key);

	return 0;
}

static bool hpm_qeo_pwm_phase_output_is_valid(const struct hpm_qeo_pwm_phase_output_table *output)
{
	uint8_t channel;

	for (channel = 0U; channel < HPM_QEO_PWM_CHANNEL_COUNT; channel++) {
		if ((output->output[channel] != HPM_QEO_PWM_OUTPUT_NOT_FORCED) &&
		    (output->output[channel] != HPM_QEO_PWM_OUTPUT_FORCE_LOW) &&
		    (output->output[channel] != HPM_QEO_PWM_OUTPUT_FORCE_HIGH)) {
			return false;
		}
	}

	return true;
}

static int hpm_qeo_drv_configure_pwm(const struct device *dev,
				     const struct hpm_qeo_pwm_config *config)
{
	const struct hpm_qeo_config *qeo = dev->config;
	struct hpm_qeo_data *data = dev->data;
	qeo_pwm_mode_t sdk_config;
	k_spinlock_key_t key;
	uint8_t index;

	if ((config == NULL) || (config->lines == 0U) || (config->phase_count == 0U) ||
	    (config->phase_count > HPM_QEO_PWM_PHASE_COUNT) ||
	    (config->phase_table > HPM_QEO_PWM_PHASE_TABLE_NEGATIVE)) {
		return -EINVAL;
	}

	for (index = 0U; index < ARRAY_SIZE(config->phase_shift_degrees); index++) {
		if (!hpm_qeo_angle_is_valid(config->phase_shift_degrees[index])) {
			return -EINVAL;
		}
	}

	qeo_pwm_get_default_mode_config(qeo->base, &sdk_config);
	sdk_config.phase_num = config->phase_count;
	sdk_config.shield_hardware_trig_safety = config->bypass_hardware_trigger_safety;
	sdk_config.revise_pairs_output = config->phase_table == HPM_QEO_PWM_PHASE_TABLE_NEGATIVE;

	key = k_spin_lock(&data->lock);
	qeo_pwm_config_mode(qeo->base, &sdk_config);
	qeo_pwm_set_resolution_lines(qeo->base, config->lines);
	for (index = 0U; index < ARRAY_SIZE(config->phase_shift_degrees); index++) {
		qeo_pwm_set_phase_shift(qeo->base, index, config->phase_shift_degrees[index]);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_qeo_drv_set_pwm_phase_table(const struct device *dev,
					   enum hpm_qeo_pwm_phase_table table, uint8_t index,
					   const struct hpm_qeo_pwm_phase_output_table *output)
{
	const struct hpm_qeo_config *qeo = dev->config;
	struct hpm_qeo_data *data = dev->data;
	qeo_pwm_phase_output_table_t sdk_output;
	k_spinlock_key_t key;
	uint8_t sdk_index;

	if ((table > HPM_QEO_PWM_PHASE_TABLE_NEGATIVE) || (index >= HPM_QEO_PWM_PHASE_COUNT) ||
	    (output == NULL) || !hpm_qeo_pwm_phase_output_is_valid(output)) {
		return -EINVAL;
	}

	sdk_output.pwm0_output = (uint8_t)output->output[0];
	sdk_output.pwm1_output = (uint8_t)output->output[1];
	sdk_output.pwm2_output = (uint8_t)output->output[2];
	sdk_output.pwm3_output = (uint8_t)output->output[3];
	sdk_output.pwm4_output = (uint8_t)output->output[4];
	sdk_output.pwm5_output = (uint8_t)output->output[5];
	sdk_output.pwm6_output = (uint8_t)output->output[6];
	sdk_output.pwm7_output = (uint8_t)output->output[7];
	sdk_index = index +
		    ((table == HPM_QEO_PWM_PHASE_TABLE_NEGATIVE) ? HPM_QEO_PWM_PHASE_COUNT : 0U);

	key = k_spin_lock(&data->lock);
	qeo_pwm_config_phase_table(qeo->base, sdk_index, &sdk_output);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_qeo_drv_select_pwm_phase_table(const struct device *dev,
					      enum hpm_qeo_pwm_phase_table table)
{
	const struct hpm_qeo_config *qeo = dev->config;
	struct hpm_qeo_data *data = dev->data;
	k_spinlock_key_t key;

	if (table > HPM_QEO_PWM_PHASE_TABLE_NEGATIVE) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	qeo_pwm_select_phase_table(qeo->base, table == HPM_QEO_PWM_PHASE_TABLE_POSITIVE);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_qeo_drv_set_pwm_safety_table(const struct device *dev,
					    const struct hpm_qeo_pwm_safety_output_table *output)
{
	const struct hpm_qeo_config *qeo = dev->config;
	struct hpm_qeo_data *data = dev->data;
	qeo_pwm_safety_output_table_t sdk_output;
	k_spinlock_key_t key;
	uint8_t channel;

	if (output == NULL) {
		return -EINVAL;
	}

	for (channel = 0U; channel < HPM_QEO_PWM_CHANNEL_COUNT; channel++) {
		if (output->output[channel] > HPM_QEO_PWM_SAFETY_OUTPUT_HIGH_Z) {
			return -EINVAL;
		}
	}

	sdk_output.pwm0_output = (uint8_t)output->output[0];
	sdk_output.pwm1_output = (uint8_t)output->output[1];
	sdk_output.pwm2_output = (uint8_t)output->output[2];
	sdk_output.pwm3_output = (uint8_t)output->output[3];
	sdk_output.pwm4_output = (uint8_t)output->output[4];
	sdk_output.pwm5_output = (uint8_t)output->output[5];
	sdk_output.pwm6_output = (uint8_t)output->output[6];
	sdk_output.pwm7_output = (uint8_t)output->output[7];

	key = k_spin_lock(&data->lock);
	qeo_pwm_config_safety_table(qeo->base, &sdk_output);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_qeo_drv_set_pwm_safety(const struct device *dev, bool enter)
{
	const struct hpm_qeo_config *qeo = dev->config;
	struct hpm_qeo_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	if (enter) {
		qeo_pwm_software_enter_safety(qeo->base);
	} else {
		qeo_pwm_software_exit_safety(qeo->base);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_qeo_drv_pwm_in_safety(const struct device *dev, bool *in_safety)
{
	const struct hpm_qeo_config *qeo = dev->config;

	if (in_safety == NULL) {
		return -EINVAL;
	}

	*in_safety = ((qeo->base->PWM.MODE & QEO_PWM_MODE_PWM_ENTER_SAFETY_MODE_MASK) != 0U) ||
		     qeo_pwm_check_hardware_trig_safety(qeo->base);

	return 0;
}

static int hpm_qeo_drv_set_position_source(const struct device *dev,
					   enum hpm_qeo_position_source source)
{
	const struct hpm_qeo_config *qeo = dev->config;
	struct hpm_qeo_data *data = dev->data;
	k_spinlock_key_t key;

	if (source > HPM_QEO_POSITION_SOFTWARE) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	if (source == HPM_QEO_POSITION_SOFTWARE) {
		qeo_enable_software_position_inject(qeo->base);
	} else {
		qeo_disable_software_position_inject(qeo->base);
	}
	data->software_position = source == HPM_QEO_POSITION_SOFTWARE;
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_qeo_drv_inject_position(const struct device *dev, uint32_t position)
{
	const struct hpm_qeo_config *qeo = dev->config;
	struct hpm_qeo_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	if (!data->software_position) {
		k_spin_unlock(&data->lock, key);
		return -EACCES;
	}
	qeo_software_position_inject(qeo->base, position);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_qeo_drv_get_position(const struct device *dev, uint32_t *position)
{
	const struct hpm_qeo_config *qeo = dev->config;

	if (position == NULL) {
		return -EINVAL;
	}

	*position = QEO_DEBUG2_ABZ_OWN_POSTION_GET(qeo->base->DEBUG2);

	return 0;
}

static int hpm_qeo_drv_calculation_finished(const struct device *dev, bool *finished)
{
	const struct hpm_qeo_config *qeo = dev->config;

	if (finished == NULL) {
		return -EINVAL;
	}

	*finished = qeo_check_calculate_finish(qeo->base);

	return 0;
}

static int hpm_qeo_init(const struct device *dev)
{
	const struct hpm_qeo_config *config = dev->config;
	struct hpm_qeo_data *data = dev->data;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	return clock_control_get_rate(config->clock_dev, config->clock_subsys,
				      &data->clock_frequency);
}

static DEVICE_API(hpm_qeo, hpm_qeo_api) = {
	.configure_abz = hpm_qeo_drv_configure_abz,
	.configure_wave = hpm_qeo_drv_configure_wave,
	.get_wave_output = hpm_qeo_drv_get_wave_output,
	.configure_wave_vd_vq = hpm_qeo_drv_configure_wave_vd_vq,
	.configure_pwm = hpm_qeo_drv_configure_pwm,
	.set_pwm_phase_table = hpm_qeo_drv_set_pwm_phase_table,
	.select_pwm_phase_table = hpm_qeo_drv_select_pwm_phase_table,
	.set_pwm_safety_table = hpm_qeo_drv_set_pwm_safety_table,
	.set_pwm_safety = hpm_qeo_drv_set_pwm_safety,
	.pwm_in_safety = hpm_qeo_drv_pwm_in_safety,
	.set_position_source = hpm_qeo_drv_set_position_source,
	.inject_position = hpm_qeo_drv_inject_position,
	.get_position = hpm_qeo_drv_get_position,
	.calculation_finished = hpm_qeo_drv_calculation_finished,
};

#define HPM_QEO_INIT(n)                                                                            \
	static const struct hpm_qeo_config hpm_qeo_config_##n = {                                  \
		.base = (QEO_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
	};                                                                                         \
                                                                                                   \
	static struct hpm_qeo_data hpm_qeo_data_##n;                                               \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_qeo_init, NULL, &hpm_qeo_data_##n, &hpm_qeo_config_##n,       \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &hpm_qeo_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_QEO_INIT)
