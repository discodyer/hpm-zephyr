/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_mmc

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <drivers/motor/hpmicro_mmc.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#include <hpm_common.h>
#include <hpm_mmc_drv.h>

struct hpm_mmc_config {
	MMC_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	void (*irq_config_func)(const struct device *dev);
};

struct hpm_mmc_event_route {
	uint32_t event_mask;
	hpm_mmc_event_callback_t callback;
	void *user_data;
};

struct hpm_mmc_data {
	struct k_spinlock lock;
	struct hpm_mmc_event_route route[HPM_MMC_EVENT_SOURCE_COUNT];
};

BUILD_ASSERT((int)HPM_MMC_POSITION_TRIGGER_TIMESTAMP == (int)mmc_pos_update_by_timestamp);
BUILD_ASSERT((int)HPM_MMC_POSITION_TRIGGER_INPUT0_RISING ==
	     (int)mmc_pos_update_by_intrgr0_rise_edge);
BUILD_ASSERT((int)HPM_MMC_POSITION_TRIGGER_INPUT1_RISING ==
	     (int)mmc_pos_update_by_intrgr1_rise_edge);
BUILD_ASSERT((int)HPM_MMC_POSITION_TRIGGER_OUTPUT0_RISING ==
	     (int)mmc_pos_update_by_outtrgr0_rise_edge);
BUILD_ASSERT((int)HPM_MMC_POSITION_TRIGGER_OUTPUT1_RISING ==
	     (int)mmc_pos_update_by_outtrgr1_rise_edge);
BUILD_ASSERT((int)HPM_MMC_POSITION_TRIGGER_SELF_POSITION == (int)mmc_pos_update_by_self_pos_thr);
BUILD_ASSERT((int)HPM_MMC_POSITION_TRIGGER_SELF_SPEED == (int)mmc_pos_update_by_self_speed_thr);
BUILD_ASSERT((int)HPM_MMC_PREDICTOR_BY_PERIOD == (int)mmc_pred_by_period);
BUILD_ASSERT((int)HPM_MMC_PREDICTOR_CONTINUOUS == (int)mmc_pred_continuously_repeat);
BUILD_ASSERT((int)HPM_MMC_PREDICTOR_ONCE == (int)mmc_pred_only_once);
BUILD_ASSERT(HPM_MMC_PREDICTOR_STATUS_IDLE == MMC_BR_BR_ST_IDLE_MASK);
BUILD_ASSERT(HPM_MMC_PREDICTOR_STATUS_DELTA_POSITION_DONE == MMC_BR_BR_ST_INI_DELTA_POS_DONE_MASK);
BUILD_ASSERT(HPM_MMC_PREDICTOR_STATUS_POSITION_TRIGGER == MMC_BR_BR_ST_POS_TRG_VLD_MASK);
BUILD_ASSERT(HPM_MMC_PREDICTOR_STATUS_SPEED_TRIGGER == MMC_BR_BR_ST_SPEED_TRG_VLD_MASK);
BUILD_ASSERT(HPM_MMC_PREDICTOR_STATUS_OPEN_LOOP == MMC_BR_BR_ST_OPEN_LOOP_ST_MASK);
BUILD_ASSERT(sizeof(((struct hpm_mmc_data *)0)->route) /
		     sizeof(((struct hpm_mmc_data *)0)->route[0]) ==
	     HPM_MMC_EVENT_SOURCE_COUNT);

static bool hpm_mmc_predictor_is_valid(uint8_t predictor)
{
	return predictor < HPM_MMC_PREDICTOR_COUNT;
}

static bool hpm_mmc_position_config_is_valid(const struct hpm_mmc_position_config *config)
{
	return (config != NULL) && (config->update_mask <= (uint32_t)HPM_MMC_POSITION_UPDATE_ALL) &&
	       (config->trigger <= HPM_MMC_POSITION_TRIGGER_SELF_SPEED);
}

static void hpm_mmc_from_position_config(const struct hpm_mmc_position_config *config,
					 mmc_pos_or_delta_pos_input_t *sdk_config)
{
	sdk_config->pos_time = config->time;
	sdk_config->position = config->position;
	sdk_config->revolution = config->revolution;
	sdk_config->speed = config->speed;
	sdk_config->accel = config->acceleration;
	sdk_config->cmd_mask = config->update_mask;
	sdk_config->trigger = config->trigger;
}

static int hpm_mmc_drv_configure_track_mode(const struct device *dev,
					    const struct hpm_mmc_track_mode *config)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	mmc_track_mode_t sdk_config;
	k_spinlock_key_t key;

	if ((config == NULL) ||
	    (config->position_mode.discrete && (config->position_mode.discrete_lines == 0U))) {
		return -EINVAL;
	}

	mmc_track_get_default_mode_config(mmc->base, &sdk_config);
	sdk_config.force_accel_to_zero = config->force_acceleration_zero;
	sdk_config.en_ms_coef = config->enable_ms_coefficients;
	sdk_config.open_loop_mode = config->open_loop;
	sdk_config.pos_16bit_type = config->position_16bit;
	sdk_config.sync_new_pos = config->sync_new_position;
	sdk_config.pos_mode.discrete_pos_mode = config->position_mode.discrete;
	sdk_config.pos_mode.discrete_line = config->position_mode.discrete_lines;
	sdk_config.pos_mode.continuous_step_thr = config->position_mode.continuous_step_threshold;
	sdk_config.pos_mode.continuous_circ_thr = config->position_mode.continuous_circle_threshold;
	sdk_config.pos_mode.oosync_theta_thr = config->position_mode.out_of_sync_theta_threshold;

	key = k_spin_lock(&data->lock);
	mmc_track_config_mode(mmc->base, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_mmc_drv_configure_track_coefficients(const struct device *dev,
						    const struct hpm_mmc_coefficient_config *config)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	mmc_coef_input_t sdk_config;
	k_spinlock_key_t key;

	if ((config == NULL) || (config->update_mask > (uint32_t)HPM_MMC_COEFFICIENT_UPDATE_ALL)) {
		return -EINVAL;
	}

	sdk_config.coef_time = config->time;
	sdk_config.coef_p = config->p;
	sdk_config.coef_i = config->i;
	sdk_config.coef_a = config->a;
	sdk_config.cmd_mask = config->update_mask;

	key = k_spin_lock(&data->lock);
	mmc_track_config_coef_para(mmc->base, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_mmc_drv_configure_track_position(const struct device *dev,
						const struct hpm_mmc_position_config *config)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	mmc_pos_or_delta_pos_input_t sdk_config;
	k_spinlock_key_t key;

	if (!hpm_mmc_position_config_is_valid(config)) {
		return -EINVAL;
	}

	hpm_mmc_from_position_config(config, &sdk_config);
	key = k_spin_lock(&data->lock);
	mmc_track_config_pos_para(mmc->base, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_mmc_drv_configure_track_delta_position(const struct device *dev,
						      const struct hpm_mmc_position_config *config)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	mmc_pos_or_delta_pos_input_t sdk_config;
	k_spinlock_key_t key;

	if (!hpm_mmc_position_config_is_valid(config)) {
		return -EINVAL;
	}

	hpm_mmc_from_position_config(config, &sdk_config);
	key = k_spin_lock(&data->lock);
	mmc_track_config_delta_para(mmc->base, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int
hpm_mmc_drv_configure_track_position_trigger(const struct device *dev,
					     const struct hpm_mmc_position_trigger_config *config)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	mmc_pos_trig_t sdk_config;
	k_spinlock_key_t key;

	if (config == NULL) {
		return -EINVAL;
	}

	sdk_config.enable = config->enable;
	sdk_config.less_than = config->less_than;
	sdk_config.position_thr = config->position;
	sdk_config.revolution_thr = config->revolution;

	key = k_spin_lock(&data->lock);
	mmc_track_config_position_trig(mmc->base, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int
hpm_mmc_drv_configure_track_speed_trigger(const struct device *dev,
					  const struct hpm_mmc_speed_trigger_config *config)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	mmc_speed_trig_t sdk_config;
	k_spinlock_key_t key;

	if (config == NULL) {
		return -EINVAL;
	}

	sdk_config.enable = config->enable;
	sdk_config.less_than = config->less_than;
	sdk_config.absolute_compare = config->absolute;
	sdk_config.speed_thr = config->speed;

	key = k_spin_lock(&data->lock);
	mmc_track_config_speed_trig(mmc->base, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_mmc_drv_configure_predictor_mode(const struct device *dev, uint8_t predictor,
						const struct hpm_mmc_predictor_mode *config)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	mmc_pred_mode_t sdk_config;
	k_spinlock_key_t key;

	if (!hpm_mmc_predictor_is_valid(predictor) || (config == NULL) ||
	    (config->time_mode > HPM_MMC_PREDICTOR_ONCE)) {
		return -EINVAL;
	}

	mmc_pred_get_default_mode_config(mmc->base, &sdk_config);
	sdk_config.speed_trig_int = config->speed_trigger_interrupt;
	sdk_config.position_trig_int = config->position_trigger_interrupt;
	sdk_config.delta_pos_done_trig_int = config->delta_position_interrupt;
	sdk_config.open_loop_mode = config->open_loop;
	sdk_config.pred_mode = config->time_mode;
	sdk_config.first_pred_trig_type = config->first_trigger_type;
	sdk_config.not_first_pred_trig_type = config->next_trigger_type;

	key = k_spin_lock(&data->lock);
	uint32_t routed = data->route[HPM_MMC_EVENT_SOURCE_PREDICTOR0 + predictor].event_mask;

	if ((routed & HPM_MMC_PREDICTOR_STATUS_DELTA_POSITION_DONE) != 0U) {
		sdk_config.delta_pos_done_trig_int = true;
	}
	if ((routed & HPM_MMC_PREDICTOR_STATUS_POSITION_TRIGGER) != 0U) {
		sdk_config.position_trig_int = true;
	}
	if ((routed & HPM_MMC_PREDICTOR_STATUS_SPEED_TRIGGER) != 0U) {
		sdk_config.speed_trig_int = true;
	}
	mmc_pred_config_mode(mmc->base, predictor, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_mmc_drv_configure_predictor_position(const struct device *dev, uint8_t predictor,
						    const struct hpm_mmc_position_config *config)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	mmc_pos_or_delta_pos_input_t sdk_config;
	k_spinlock_key_t key;

	if (!hpm_mmc_predictor_is_valid(predictor) || !hpm_mmc_position_config_is_valid(config)) {
		return -EINVAL;
	}

	hpm_mmc_from_position_config(config, &sdk_config);
	key = k_spin_lock(&data->lock);
	mmc_pred_config_pos_para(mmc->base, predictor, &sdk_config, true);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int
hpm_mmc_drv_configure_predictor_delta_position(const struct device *dev, uint8_t predictor,
					       const struct hpm_mmc_position_config *config)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	mmc_pos_or_delta_pos_input_t sdk_config;
	k_spinlock_key_t key;

	if (!hpm_mmc_predictor_is_valid(predictor) || !hpm_mmc_position_config_is_valid(config)) {
		return -EINVAL;
	}

	hpm_mmc_from_position_config(config, &sdk_config);
	key = k_spin_lock(&data->lock);
	mmc_pred_config_delta_para(mmc->base, predictor, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_mmc_drv_configure_predictor_period(const struct device *dev, uint8_t predictor,
						  const struct hpm_mmc_predictor_period *config)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	mmc_pred_period_time_t sdk_config;
	k_spinlock_key_t key;

	if (!hpm_mmc_predictor_is_valid(predictor) || (config == NULL)) {
		return -EINVAL;
	}

	sdk_config.offset_time = config->offset_time;
	sdk_config.period_time = config->period_time;
	sdk_config.first_time = config->first_time;

	key = k_spin_lock(&data->lock);
	mmc_pred_config_period_time(mmc->base, predictor, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_mmc_drv_configure_predictor_position_trigger(
	const struct device *dev, uint8_t predictor,
	const struct hpm_mmc_position_trigger_config *config)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	mmc_pos_trig_t sdk_config;
	k_spinlock_key_t key;

	if (!hpm_mmc_predictor_is_valid(predictor) || (config == NULL)) {
		return -EINVAL;
	}

	sdk_config.enable = config->enable;
	sdk_config.less_than = config->less_than;
	sdk_config.position_thr = config->position;
	sdk_config.revolution_thr = config->revolution;

	key = k_spin_lock(&data->lock);
	mmc_pred_config_position_trig(mmc->base, predictor, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int
hpm_mmc_drv_configure_predictor_speed_trigger(const struct device *dev, uint8_t predictor,
					      const struct hpm_mmc_speed_trigger_config *config)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	mmc_speed_trig_t sdk_config;
	k_spinlock_key_t key;

	if (!hpm_mmc_predictor_is_valid(predictor) || (config == NULL)) {
		return -EINVAL;
	}

	sdk_config.enable = config->enable;
	sdk_config.less_than = config->less_than;
	sdk_config.absolute_compare = config->absolute;
	sdk_config.speed_thr = config->speed;

	key = k_spin_lock(&data->lock);
	mmc_pred_config_speed_trig(mmc->base, predictor, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_mmc_drv_enable_predictor(const struct device *dev, uint8_t predictor, bool enable)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_mmc_predictor_is_valid(predictor)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	if (enable) {
		mmc_enable_pred(mmc->base, predictor);
	} else {
		mmc_disable_pred(mmc->base, predictor);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_mmc_drv_enable(const struct device *dev, bool enable)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	if (enable) {
		mmc_enable_module(mmc->base);
	} else {
		mmc_disable_module(mmc->base);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_mmc_drv_reset(const struct device *dev)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	mmc_software_reset(mmc->base);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static void hpm_mmc_from_position_result(const mmc_pos_out_t *sdk_result,
					 struct hpm_mmc_position_result *result)
{
	result->time = sdk_result->time;
	result->position = sdk_result->position;
	result->revolution = sdk_result->revolution;
	result->speed = sdk_result->speed;
	result->acceleration = sdk_result->accel;
}

static int hpm_mmc_drv_get_track_result(const struct device *dev,
					struct hpm_mmc_position_result *position,
					struct hpm_mmc_coefficient_result *coefficients)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	mmc_pos_out_t sdk_position;
	mmc_coef_out_t sdk_coefficients;
	k_spinlock_key_t key;

	if ((position == NULL) && (coefficients == NULL)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	mmc_track_get_result(mmc->base, &sdk_position, &sdk_coefficients);
	k_spin_unlock(&data->lock, key);

	if (position != NULL) {
		hpm_mmc_from_position_result(&sdk_position, position);
	}

	if (coefficients != NULL) {
		coefficients->p = sdk_coefficients.coef_p;
		coefficients->i = sdk_coefficients.coef_i;
		coefficients->a = sdk_coefficients.coef_a;
	}

	return 0;
}

static int hpm_mmc_drv_get_predictor_result(const struct device *dev, uint8_t predictor,
					    struct hpm_mmc_position_result *result)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	mmc_pos_out_t sdk_result;
	k_spinlock_key_t key;

	if (!hpm_mmc_predictor_is_valid(predictor) || (result == NULL)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	mmc_pred_get_result(mmc->base, predictor, &sdk_result);
	k_spin_unlock(&data->lock, key);

	hpm_mmc_from_position_result(&sdk_result, result);

	return 0;
}

static int hpm_mmc_drv_get_status(const struct device *dev, struct hpm_mmc_status *status)
{
	const struct hpm_mmc_config *mmc = dev->config;
	uint8_t predictor;

	if (status == NULL) {
		return -EINVAL;
	}

	status->track = mmc_track_get_status(mmc->base);
	for (predictor = 0U; predictor < HPM_MMC_PREDICTOR_COUNT; predictor++) {
		status->predictor[predictor] = mmc_pred_get_status(mmc->base, predictor);
	}

	return 0;
}

static int hpm_mmc_drv_clear_track_status(const struct device *dev, uint32_t mask)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	mmc_track_clear_status(mmc->base, mask & HPM_MMC_TRACK_EVENT_ALL);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_mmc_drv_clear_predictor_status(const struct device *dev, uint8_t predictor,
					      uint32_t mask)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_mmc_predictor_is_valid(predictor)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	mmc_pred_clear_status(mmc->base, predictor, mask & HPM_MMC_PREDICTOR_EVENT_ALL);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static uint32_t hpm_mmc_event_mask(enum hpm_mmc_event_source source)
{
	switch (source) {
	case HPM_MMC_EVENT_SOURCE_TRACK:
		return HPM_MMC_TRACK_EVENT_ALL;
	case HPM_MMC_EVENT_SOURCE_PREDICTOR0:
	case HPM_MMC_EVENT_SOURCE_PREDICTOR1:
		return HPM_MMC_PREDICTOR_EVENT_ALL;
	default:
		return 0U;
	}
}

static int hpm_mmc_drv_set_event_callback(const struct device *dev,
					  enum hpm_mmc_event_source source, uint32_t event_mask,
					  hpm_mmc_event_callback_t callback, void *user_data)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	uint32_t allowed = hpm_mmc_event_mask(source);
	k_spinlock_key_t key;

	if ((allowed == 0U) || ((event_mask & ~allowed) != 0U)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	if ((callback == NULL) || (event_mask == 0U)) {
		data->route[source].event_mask = 0U;
		data->route[source].callback = NULL;
		data->route[source].user_data = NULL;
	} else {
		data->route[source].event_mask = event_mask;
		data->route[source].callback = callback;
		data->route[source].user_data = user_data;
	}

	if (source == HPM_MMC_EVENT_SOURCE_TRACK) {
		mmc_track_enable_interrupt(mmc->base, data->route[source].event_mask);
	} else {
		uint8_t predictor = (source == HPM_MMC_EVENT_SOURCE_PREDICTOR0) ? 0U : 1U;
		uint32_t sdk_mask = 0U;

		if ((event_mask & HPM_MMC_PREDICTOR_STATUS_DELTA_POSITION_DONE) != 0U) {
			sdk_mask |= mmc_pred_init_delta_pos_done_int;
		}
		if ((event_mask & HPM_MMC_PREDICTOR_STATUS_POSITION_TRIGGER) != 0U) {
			sdk_mask |= mmc_pred_pos_trig_valid_int;
		}
		if ((event_mask & HPM_MMC_PREDICTOR_STATUS_SPEED_TRIGGER) != 0U) {
			sdk_mask |= mmc_pred_speed_trig_valid_int;
		}

		mmc_pred_disable_interrupt(mmc->base, predictor,
					   mmc_pred_init_delta_pos_done_int |
						   mmc_pred_pos_trig_valid_int |
						   mmc_pred_speed_trig_valid_int);
		mmc_pred_enable_interrupt(mmc->base, predictor, sdk_mask);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static uint32_t hpm_mmc_predictor_int_events(uint32_t ctrl)
{
	uint32_t events = 0U;

	if ((ctrl & MMC_BR_BR_CTRL_INI_DELTA_POS_DONE_IE_MASK) != 0U) {
		events |= HPM_MMC_PREDICTOR_STATUS_DELTA_POSITION_DONE;
	}
	if ((ctrl & MMC_BR_BR_CTRL_POS_TRG_VALID_IE_MASK) != 0U) {
		events |= HPM_MMC_PREDICTOR_STATUS_POSITION_TRIGGER;
	}
	if ((ctrl & MMC_BR_BR_CTRL_SPEED_TRG_VALID_IE_MASK) != 0U) {
		events |= HPM_MMC_PREDICTOR_STATUS_SPEED_TRIGGER;
	}

	return events;
}

static void hpm_mmc_isr(const struct device *dev)
{
	const struct hpm_mmc_config *mmc = dev->config;
	struct hpm_mmc_data *data = dev->data;
	uint32_t track_events = 0U;
	uint32_t predictor_events[HPM_MMC_PREDICTOR_COUNT] = {0U};
	hpm_mmc_event_callback_t callbacks[HPM_MMC_EVENT_SOURCE_COUNT] = {NULL};
	void *user_data[HPM_MMC_EVENT_SOURCE_COUNT] = {NULL};
	k_spinlock_key_t key;
	uint8_t i;

	key = k_spin_lock(&data->lock);

	track_events =
		mmc_track_get_status(mmc->base) & mmc->base->INT_EN & HPM_MMC_TRACK_EVENT_ALL;
	if (track_events != 0U) {
		mmc_track_clear_status(mmc->base, track_events);
	}

	for (i = 0U; i < HPM_MMC_PREDICTOR_COUNT; i++) {
		enum hpm_mmc_event_source source = HPM_MMC_EVENT_SOURCE_PREDICTOR0 + i;
		uint32_t enabled_events = hpm_mmc_predictor_int_events(mmc->base->BR[i].BR_CTRL);
		uint32_t pending = mmc_pred_get_status(mmc->base, i) & enabled_events;

		if (pending != 0U) {
			mmc_pred_clear_status(mmc->base, i, pending);
			predictor_events[i] = pending & data->route[source].event_mask;
		}
	}

	track_events &= data->route[HPM_MMC_EVENT_SOURCE_TRACK].event_mask;
	for (i = 0U; i < HPM_MMC_EVENT_SOURCE_COUNT; i++) {
		callbacks[i] = data->route[i].callback;
		user_data[i] = data->route[i].user_data;
	}
	k_spin_unlock(&data->lock, key);

	if ((callbacks[HPM_MMC_EVENT_SOURCE_TRACK] != NULL) && (track_events != 0U)) {
		callbacks[HPM_MMC_EVENT_SOURCE_TRACK](dev, track_events,
						      user_data[HPM_MMC_EVENT_SOURCE_TRACK]);
	}
	for (i = 0U; i < HPM_MMC_PREDICTOR_COUNT; i++) {
		enum hpm_mmc_event_source source = HPM_MMC_EVENT_SOURCE_PREDICTOR0 + i;

		if ((callbacks[source] != NULL) && (predictor_events[i] != 0U)) {
			callbacks[source](dev, predictor_events[i], user_data[source]);
		}
	}
}

static DEVICE_API(hpm_mmc, hpm_mmc_api) = {
	.configure_track_mode = hpm_mmc_drv_configure_track_mode,
	.configure_track_coefficients = hpm_mmc_drv_configure_track_coefficients,
	.configure_track_position = hpm_mmc_drv_configure_track_position,
	.configure_track_delta_position = hpm_mmc_drv_configure_track_delta_position,
	.configure_track_position_trigger = hpm_mmc_drv_configure_track_position_trigger,
	.configure_track_speed_trigger = hpm_mmc_drv_configure_track_speed_trigger,
	.configure_predictor_mode = hpm_mmc_drv_configure_predictor_mode,
	.configure_predictor_position = hpm_mmc_drv_configure_predictor_position,
	.configure_predictor_delta_position = hpm_mmc_drv_configure_predictor_delta_position,
	.configure_predictor_period = hpm_mmc_drv_configure_predictor_period,
	.configure_predictor_position_trigger = hpm_mmc_drv_configure_predictor_position_trigger,
	.configure_predictor_speed_trigger = hpm_mmc_drv_configure_predictor_speed_trigger,
	.enable_predictor = hpm_mmc_drv_enable_predictor,
	.enable = hpm_mmc_drv_enable,
	.reset = hpm_mmc_drv_reset,
	.get_track_result = hpm_mmc_drv_get_track_result,
	.get_predictor_result = hpm_mmc_drv_get_predictor_result,
	.get_status = hpm_mmc_drv_get_status,
	.clear_track_status = hpm_mmc_drv_clear_track_status,
	.clear_predictor_status = hpm_mmc_drv_clear_predictor_status,
	.set_event_callback = hpm_mmc_drv_set_event_callback,
};

static int hpm_mmc_init(const struct device *dev)
{
	const struct hpm_mmc_config *config = dev->config;
	uint32_t clock_frequency;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		return ret;
	}

	ret = clock_control_get_rate(config->clock_dev, config->clock_subsys, &clock_frequency);
	if (ret < 0) {
		return ret;
	}

	if (clock_frequency == 0U) {
		return -EINVAL;
	}

	mmc_set_sysclk_freq(config->base, clock_frequency);
	mmc_software_reset(config->base);
	config->irq_config_func(dev);

	return 0;
}

#define HPM_MMC_IRQ_CONFIG(n)                                                                      \
	static void hpm_mmc_irq_config_##n(const struct device *dev)                               \
	{                                                                                          \
		ARG_UNUSED(dev);                                                                   \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_mmc_isr,                \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}

#define HPM_MMC_INIT(n)                                                                            \
	HPM_MMC_IRQ_CONFIG(n)                                                                      \
	static const struct hpm_mmc_config hpm_mmc_config_##n = {                                  \
		.base = (MMC_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.irq_config_func = hpm_mmc_irq_config_##n,                                         \
	};                                                                                         \
                                                                                                   \
	static struct hpm_mmc_data hpm_mmc_data_##n;                                               \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_mmc_init, NULL, &hpm_mmc_data_##n, &hpm_mmc_config_##n,       \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &hpm_mmc_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_MMC_INIT)
