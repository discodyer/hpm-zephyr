/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_qeiv2

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/sensor/hpmicro_qeiv2.h>
#include <hpm_clock_drv.h>
#include <hpm_qeiv2_drv.h>

LOG_MODULE_REGISTER(qeiv2_hpmicro, CONFIG_SENSOR_LOG_LEVEL);

struct hpm_qeiv2_config {
	QEIV2_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	clock_name_t clock_name;
	clock_source_t clock_src;
	uint32_t clock_div;
	const struct pinctrl_dev_config *pincfg;
	uint32_t counts_per_revolution;
	uint32_t phase_index;
	uint16_t filter_length;
	enum hpm_qeiv2_mode mode;
	bool z_calibration;
	void (*irq_config_func)(const struct device *dev);
};

struct hpm_qeiv2_data {
	struct k_spinlock lock;
	int32_t position;
	int32_t revolutions;
	uint32_t angle_raw;
	uint32_t latched_events;
	uint32_t event_mask;
	hpm_qeiv2_event_callback_t callback;
	void *callback_user_data;
	bool compare_source_valid;
	enum hpm_qeiv2_compare_source compare_source;
};

BUILD_ASSERT((uint32_t)HPM_QEIV2_EVENT_WATCHDOG == QEIV2_EVENT_WDOG_FLAG_MASK);
BUILD_ASSERT((uint32_t)HPM_QEIV2_EVENT_HOME == QEIV2_EVENT_HOME_FLAG_MASK);
BUILD_ASSERT((uint32_t)HPM_QEIV2_EVENT_POSITION_COMPARE == QEIV2_EVENT_POSITION_COMPARE_FLAG_MASK);
BUILD_ASSERT((uint32_t)HPM_QEIV2_EVENT_Z_PHASE == QEIV2_EVENT_Z_PHASE_FLAG_MASK);
BUILD_ASSERT((uint32_t)HPM_QEIV2_EVENT_Z_MISS == QEIV2_EVENT_Z_MISS_FLAG_MASK);
BUILD_ASSERT((uint32_t)HPM_QEIV2_EVENT_WIDTH_TIME == QEIV2_EVENT_WIDTH_TIME_FLAG_MASK);
BUILD_ASSERT((uint32_t)HPM_QEIV2_EVENT_POSITION2_COMPARE ==
	     QEIV2_EVENT_POSITION2_COMPARE_FLAG_MASK);
BUILD_ASSERT((uint32_t)HPM_QEIV2_EVENT_DIRECTION_CHANGE == QEIV2_EVENT_DIR_CHG_FLAG_MASK);
BUILD_ASSERT((uint32_t)HPM_QEIV2_EVENT_CYCLE0 == QEIV2_EVENT_CYCLE0_FLAG_MASK);
BUILD_ASSERT((uint32_t)HPM_QEIV2_EVENT_CYCLE1 == QEIV2_EVENT_CYCLE1_FLAG_MASK);
BUILD_ASSERT((uint32_t)HPM_QEIV2_EVENT_PULSE0 == QEIV2_EVENT_PULSE0_FLAG_MASK);
BUILD_ASSERT((uint32_t)HPM_QEIV2_EVENT_PULSE1 == QEIV2_EVENT_PULSE1_FLAG_MASK);
BUILD_ASSERT((uint32_t)HPM_QEIV2_EVENT_HOME2 == QEIV2_EVENT_HOME2_FLAG_MASK);
BUILD_ASSERT((uint32_t)HPM_QEIV2_EVENT_FAULT == QEIV2_EVENT_FAULT_FLAG_MASK);
BUILD_ASSERT((int)HPM_QEIV2_MODE_ABZ == (int)qeiv2_work_mode_abz);
BUILD_ASSERT((int)HPM_QEIV2_MODE_PULSE_DIRECTION == (int)qeiv2_work_mode_pd);
BUILD_ASSERT((int)HPM_QEIV2_MODE_UP_DOWN == (int)qeiv2_work_mode_ud);
BUILD_ASSERT((int)HPM_QEIV2_MODE_UVW == (int)qeiv2_work_mode_uvw);

static int hpm_qeiv2_apply_mode(const struct device *dev, const struct hpm_qeiv2_mode_config *mode)
{
	const struct hpm_qeiv2_config *config = dev->config;
	struct hpm_qeiv2_data *data = dev->data;
	qeiv2_mode_config_t sdk_config = {0};
	qeiv2_uvw_config_t uvw_config;
	k_spinlock_key_t key;
	hpm_stat_t status = status_success;

	if ((mode == NULL) || ((unsigned int)mode->mode > HPM_QEIV2_MODE_UVW) ||
	    (mode->counts_per_revolution <= 1U) ||
	    (mode->phase_index >= mode->counts_per_revolution) ||
	    (mode->z_calibration && (mode->mode != HPM_QEIV2_MODE_ABZ)) ||
	    (mode->z_calibration_ignore_ab && !mode->z_calibration)) {
		return -EINVAL;
	}

	sdk_config.work_mode = (qeiv2_work_mode_t)mode->mode;
	sdk_config.spd_tmr_content_sel = qeiv2_spd_tmr_as_spd_tm;
	sdk_config.z_count_inc_mode = mode->z_calibration ? qeiv2_z_count_inc_on_z_input_assert
							  : qeiv2_z_count_inc_on_phase_count_max;
	sdk_config.phcnt_max = mode->counts_per_revolution;
	sdk_config.z_cali_enable = mode->z_calibration;
	sdk_config.z_cali_ignore_ab = mode->z_calibration_ignore_ab;
	sdk_config.phcnt_idx = mode->phase_index;

	key = k_spin_lock(&data->lock);
	qeiv2_disable_irq(config->base, HPM_QEIV2_EVENT_ALL);
	qeiv2_clear_status(config->base, HPM_QEIV2_EVENT_ALL);
	qeiv2_config_mode(config->base, &sdk_config);

	switch (mode->mode) {
	case HPM_QEIV2_MODE_ABZ:
		qeiv2_config_abz_uvw_signal_edge(config->base, true, true, mode->z_calibration,
						 true, true);
		break;
	case HPM_QEIV2_MODE_PULSE_DIRECTION:
		qeiv2_config_abz_uvw_signal_edge(config->base, true, false, false, true, true);
		break;
	case HPM_QEIV2_MODE_UP_DOWN:
		qeiv2_config_abz_uvw_signal_edge(config->base, true, true, false, true, true);
		break;
	case HPM_QEIV2_MODE_UVW:
		qeiv2_get_uvw_position_defconfig(&uvw_config);
		status = qeiv2_config_uvw_position(config->base, &uvw_config);
		break;
	default:
		status = status_invalid_argument;
		break;
	}

	if (status != status_success) {
		data->compare_source_valid = false;
		if (data->callback != NULL) {
			qeiv2_enable_irq(config->base, data->event_mask);
		}
		k_spin_unlock(&data->lock, key);
		return -EIO;
	}

	qeiv2_set_phase_cnt(config->base, 0U);
	qeiv2_set_position(config->base, 0U);
	if (config->filter_length != 0U) {
		qeiv2_config_filter(config->base, qeiv2_filter_phase_a, false,
				    qeiv2_filter_mode_delay, false, config->filter_length);
		qeiv2_config_filter(config->base, qeiv2_filter_phase_b, false,
				    qeiv2_filter_mode_delay, false, config->filter_length);
	}

	data->position = 0;
	data->revolutions = 0;
	data->angle_raw = 0U;
	data->latched_events = 0U;
	data->compare_source_valid = false;
	if (data->callback != NULL) {
		qeiv2_enable_irq(config->base, data->event_mask);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_qeiv2_drv_configure_mode(const struct device *dev,
					const struct hpm_qeiv2_mode_config *mode)
{
	return hpm_qeiv2_apply_mode(dev, mode);
}

static int hpm_qeiv2_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct hpm_qeiv2_config *config = dev->config;
	struct hpm_qeiv2_data *data = dev->data;

	if ((chan != SENSOR_CHAN_ALL) && (chan != SENSOR_CHAN_ROTATION) &&
	    (chan != SENSOR_CHAN_ENCODER_COUNT) && (chan != SENSOR_CHAN_ENCODER_REVOLUTIONS)) {
		return -ENOTSUP;
	}

	qeiv2_load_counter_to_read_registers(config->base);
	data->position = (int32_t)qeiv2_get_postion(config->base);
	data->revolutions =
		(int32_t)qeiv2_get_count_on_read_event(config->base, qeiv2_counter_type_z);
	data->angle_raw = qeiv2_get_angle(config->base);

	return 0;
}

static int hpm_qeiv2_channel_get(const struct device *dev, enum sensor_channel chan,
				 struct sensor_value *value)
{
	struct hpm_qeiv2_data *data = dev->data;
	uint32_t micro_degrees;

	if (value == NULL) {
		return -EINVAL;
	}

	switch (chan) {
	case SENSOR_CHAN_ROTATION:
		micro_degrees = (uint32_t)(((uint64_t)data->angle_raw * 360000000ULL) >> 32);
		value->val1 = (int32_t)(micro_degrees / 1000000U);
		value->val2 = (int32_t)(micro_degrees % 1000000U);
		return 0;
	case SENSOR_CHAN_ENCODER_COUNT:
		value->val1 = data->position;
		value->val2 = 0;
		return 0;
	case SENSOR_CHAN_ENCODER_REVOLUTIONS:
		value->val1 = data->revolutions;
		value->val2 = 0;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int hpm_qeiv2_init(const struct device *dev)
{
	const struct hpm_qeiv2_config *config = dev->config;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	if (clock_set_source_divider(config->clock_name, config->clock_src, config->clock_div) !=
	    status_success) {
		return -EINVAL;
	}

	ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
	if ((ret < 0) && (ret != -ENOENT)) {
		return ret;
	}

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		return ret;
	}

	ret = hpm_qeiv2_apply_mode(dev,
				   &(const struct hpm_qeiv2_mode_config){
					   .mode = config->mode,
					   .counts_per_revolution = config->counts_per_revolution,
					   .phase_index = config->phase_index,
					   .z_calibration = config->z_calibration,
				   });
	if (ret < 0) {
		return ret;
	}

	config->irq_config_func(dev);

	return 0;
}

static int hpm_qeiv2_drv_configure_speed(const struct device *dev, uint8_t channel,
					 uint32_t pulse_count, uint32_t cycle_count)
{
	const struct hpm_qeiv2_config *config = dev->config;

	if ((channel >= HPM_QEIV2_SPEED_CHANNEL_COUNT) || (pulse_count == 0U) ||
	    (cycle_count == 0U)) {
		return -EINVAL;
	}

	switch (channel) {
	case 0U:
		qeiv2_set_pulse0_num(config->base, pulse_count);
		qeiv2_set_cycle0_num(config->base, cycle_count);
		break;
	case 1U:
		qeiv2_set_pulse1_num(config->base, pulse_count);
		qeiv2_set_cycle1_num(config->base, cycle_count);
		break;
	}

	return 0;
}

static int hpm_qeiv2_drv_get_speed_snapshot(const struct device *dev, uint8_t channel,
					    struct hpm_qeiv2_speed_snapshot *snapshot)
{
	const struct hpm_qeiv2_config *config = dev->config;

	if ((channel >= HPM_QEIV2_SPEED_CHANNEL_COUNT) || (snapshot == NULL)) {
		return -EINVAL;
	}

	switch (channel) {
	case 0U:
		snapshot->pulse_based_cycles = qeiv2_get_pulse0_cycle_snap0(config->base);
		snapshot->pulse_based_cycles_previous = qeiv2_get_pulse0_cycle_snap1(config->base);
		snapshot->duration_based_pulses = qeiv2_get_cycle0_pulse_snap0(config->base);
		snapshot->duration_based_pulses_previous =
			qeiv2_get_cycle0_pulse_snap1(config->base);
		snapshot->duration_based_cycles = qeiv2_get_cycle0_pulse0cycle_snap0(config->base);
		snapshot->duration_based_cycles_previous =
			qeiv2_get_cycle0_pulse0cycle_snap1(config->base);
		break;
	case 1U:
		snapshot->pulse_based_cycles = qeiv2_get_pulse1_cycle_snap0(config->base);
		snapshot->pulse_based_cycles_previous = qeiv2_get_pulse1_cycle_snap1(config->base);
		snapshot->duration_based_pulses = qeiv2_get_cycle1_pulse_snap0(config->base);
		snapshot->duration_based_pulses_previous =
			qeiv2_get_cycle1_pulse_snap1(config->base);
		snapshot->duration_based_cycles = qeiv2_get_cycle1_pulse1cycle_snap0(config->base);
		snapshot->duration_based_cycles_previous =
			qeiv2_get_cycle1_pulse1cycle_snap1(config->base);
		break;
	}

	return 0;
}

static int hpm_qeiv2_drv_configure_compare(const struct device *dev, uint8_t channel,
					   const struct hpm_qeiv2_compare_config *compare)
{
	const struct hpm_qeiv2_config *config = dev->config;
	struct hpm_qeiv2_data *data = dev->data;
	k_spinlock_key_t key;
	hpm_stat_t status;

	if ((channel >= HPM_QEIV2_COMPARE_CHANNEL_COUNT) || (compare == NULL) ||
	    ((unsigned int)compare->source > HPM_QEIV2_COMPARE_POSITION) ||
	    ((unsigned int)compare->direction > HPM_QEIV2_COMPARE_NEGATIVE)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	if (data->compare_source_valid && (data->compare_source != compare->source)) {
		k_spin_unlock(&data->lock, key);
		return -ENOTSUP;
	}

	qeiv2_select_spd_tmr_register_content(config->base,
					      compare->source == HPM_QEIV2_COMPARE_PHASE
						      ? qeiv2_spd_tmr_as_spd_tm
						      : qeiv2_spd_tmr_as_pos_angle);

	if (compare->source == HPM_QEIV2_COMPARE_PHASE) {
		qeiv2_phcnt_cmp_match_config_t sdk_compare = {
			.phcnt_cmp_value = compare->value,
			.ignore_rotate_dir = compare->ignore_direction,
			.rotate_dir = compare->direction == HPM_QEIV2_COMPARE_POSITIVE
					      ? qeiv2_rotate_dir_forward
					      : qeiv2_rotate_dir_reverse,
			.ignore_zcmp = compare->ignore_z,
			.zcmp_value = compare->z_value,
		};

		if (channel == 0U) {
			status = qeiv2_config_phcnt_cmp_match_condition(config->base, &sdk_compare);
		} else {
			status =
				qeiv2_config_phcnt_cmp2_match_condition(config->base, &sdk_compare);
		}
	} else {
		qeiv2_pos_cmp_match_config_t sdk_compare = {
			.pos_cmp_value = compare->value,
			.ignore_pos_dir = compare->ignore_direction,
			.pos_dir = compare->direction == HPM_QEIV2_COMPARE_POSITIVE
					   ? qeiv2_pos_dir_increase
					   : qeiv2_pos_dir_decrease,
		};

		if (channel == 0U) {
			status = qeiv2_config_position_cmp_match_condition(config->base,
									   &sdk_compare);
		} else {
			status = qeiv2_config_position_cmp2_match_condition(config->base,
									    &sdk_compare);
		}
	}
	if (status == status_success) {
		data->compare_source = compare->source;
		data->compare_source_valid = true;
	}
	k_spin_unlock(&data->lock, key);

	return status == status_success ? 0 : -EIO;
}

static int hpm_qeiv2_drv_get_events(const struct device *dev, uint32_t *events)
{
	const struct hpm_qeiv2_config *config = dev->config;
	struct hpm_qeiv2_data *data = dev->data;
	k_spinlock_key_t key;

	if (events == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	*events = data->latched_events | qeiv2_get_status(config->base);
	data->latched_events = 0U;
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_qeiv2_drv_clear_events(const struct device *dev, uint32_t events)
{
	const struct hpm_qeiv2_config *config = dev->config;
	struct hpm_qeiv2_data *data = dev->data;
	k_spinlock_key_t key;

	if ((events & ~HPM_QEIV2_EVENT_ALL) != 0U) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	data->latched_events &= ~events;
	qeiv2_clear_status(config->base, events);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_qeiv2_drv_set_event_callback(const struct device *dev, uint32_t event_mask,
					    hpm_qeiv2_event_callback_t callback, void *user_data)
{
	const struct hpm_qeiv2_config *config = dev->config;
	struct hpm_qeiv2_data *data = dev->data;
	k_spinlock_key_t key;

	if ((event_mask & ~HPM_QEIV2_EVENT_ALL) != 0U) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	qeiv2_disable_irq(config->base, HPM_QEIV2_EVENT_ALL);
	qeiv2_clear_status(config->base, HPM_QEIV2_EVENT_ALL);
	data->latched_events = 0U;
	if ((callback == NULL) || (event_mask == 0U)) {
		data->callback = NULL;
		data->callback_user_data = NULL;
		data->event_mask = 0U;
	} else {
		data->callback = callback;
		data->callback_user_data = user_data;
		data->event_mask = event_mask;
		qeiv2_enable_irq(config->base, event_mask);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static void hpm_qeiv2_isr(const struct device *dev)
{
	const struct hpm_qeiv2_config *config = dev->config;
	struct hpm_qeiv2_data *data = dev->data;
	hpm_qeiv2_event_callback_t callback;
	void *user_data;
	k_spinlock_key_t key;
	uint32_t events;

	key = k_spin_lock(&data->lock);
	events = qeiv2_get_status(config->base) & data->event_mask;
	if (events == 0U) {
		k_spin_unlock(&data->lock, key);
		return;
	}

	qeiv2_clear_status(config->base, events);
	data->latched_events |= events;
	callback = data->callback;
	user_data = data->callback_user_data;
	k_spin_unlock(&data->lock, key);

	if (callback != NULL) {
		callback(dev, events, user_data);
	}
}

static DEVICE_API(hpm_qeiv2, hpm_qeiv2_api) = {
	.sensor_api =
		{
			.sample_fetch = hpm_qeiv2_sample_fetch,
			.channel_get = hpm_qeiv2_channel_get,
		},
	.configure_mode = hpm_qeiv2_drv_configure_mode,
	.configure_speed = hpm_qeiv2_drv_configure_speed,
	.get_speed_snapshot = hpm_qeiv2_drv_get_speed_snapshot,
	.configure_compare = hpm_qeiv2_drv_configure_compare,
	.get_events = hpm_qeiv2_drv_get_events,
	.clear_events = hpm_qeiv2_drv_clear_events,
	.set_event_callback = hpm_qeiv2_drv_set_event_callback,
};

#define HPM_QEIV2_IRQ_CONFIG(n)                                                                    \
	static void hpm_qeiv2_irq_config_##n(const struct device *dev)                             \
	{                                                                                          \
		ARG_UNUSED(dev);                                                                   \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_qeiv2_isr,              \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}

#define HPM_QEIV2_INIT(n)                                                                          \
	HPM_QEIV2_IRQ_CONFIG(n)                                                                    \
	BUILD_ASSERT(DT_INST_PROP(n, counts_per_revolution) > 1U,                                  \
		     "counts-per-revolution must be at least 2");                                  \
	BUILD_ASSERT(DT_INST_PROP_OR(n, phase_index, 0) < DT_INST_PROP(n, counts_per_revolution),  \
		     "phase-index must be below counts-per-revolution");                           \
	BUILD_ASSERT(DT_INST_PROP_OR(n, filter_length, 0) <= 511U,                                 \
		     "filter-length must not exceed 511");                                         \
	BUILD_ASSERT(!DT_INST_PROP(n, z_calibration) ||                                            \
			     (DT_INST_ENUM_IDX(n, mode) == HPM_QEIV2_MODE_ABZ),                    \
		     "z-calibration requires ABZ mode");                                           \
                                                                                                   \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
                                                                                                   \
	static struct hpm_qeiv2_data hpm_qeiv2_data_##n;                                           \
                                                                                                   \
	static const struct hpm_qeiv2_config hpm_qeiv2_config_##n = {                              \
		.base = (QEIV2_Type *)DT_INST_REG_ADDR(n),                                         \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.clock_name = DT_INST_PROP(n, clock_name),                                         \
		.clock_src = DT_INST_PROP(n, clock_src),                                           \
		.clock_div = DT_INST_CLOCKS_CELL_BY_IDX(n, 0, divider),                            \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
		.counts_per_revolution = DT_INST_PROP(n, counts_per_revolution),                   \
		.phase_index = DT_INST_PROP_OR(n, phase_index, 0),                                 \
		.filter_length = DT_INST_PROP_OR(n, filter_length, 0),                             \
		.mode = (enum hpm_qeiv2_mode)DT_INST_ENUM_IDX(n, mode),                            \
		.z_calibration = DT_INST_PROP(n, z_calibration),                                   \
		.irq_config_func = hpm_qeiv2_irq_config_##n,                                       \
	};                                                                                         \
                                                                                                   \
	SENSOR_DEVICE_DT_INST_DEFINE(n, hpm_qeiv2_init, NULL, &hpm_qeiv2_data_##n,                 \
				     &hpm_qeiv2_config_##n, POST_KERNEL,                           \
				     CONFIG_SENSOR_INIT_PRIORITY, &hpm_qeiv2_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_QEIV2_INIT)
