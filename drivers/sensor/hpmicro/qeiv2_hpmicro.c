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
#include <zephyr/logging/log.h>

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
	bool z_calibration;
};

struct hpm_qeiv2_data {
	int32_t position;
	int32_t revolutions;
	uint32_t angle_raw;
};

static int hpm_qeiv2_configure(const struct hpm_qeiv2_config *config)
{
	qeiv2_mode_config_t mode_config = {0};

	mode_config.work_mode = qeiv2_work_mode_abz;
	mode_config.spd_tmr_content_sel = qeiv2_spd_tmr_as_pos_angle;
	mode_config.z_count_inc_mode = config->z_calibration ? qeiv2_z_count_inc_on_z_input_assert
							     : qeiv2_z_count_inc_on_phase_count_max;
	mode_config.phcnt_max = config->counts_per_revolution;
	mode_config.z_cali_enable = config->z_calibration;
	mode_config.z_cali_ignore_ab = false;
	mode_config.phcnt_idx = config->phase_index;

	qeiv2_config_mode(config->base, &mode_config);
	qeiv2_config_abz_uvw_signal_edge(config->base, true, true, config->z_calibration, true,
					 true);
	qeiv2_set_phase_cnt(config->base, 0U);
	qeiv2_set_position(config->base, 0U);

	if (config->filter_length != 0U) {
		qeiv2_config_filter(config->base, qeiv2_filter_phase_a, false,
				    qeiv2_filter_mode_delay, false, config->filter_length);
		qeiv2_config_filter(config->base, qeiv2_filter_phase_b, false,
				    qeiv2_filter_mode_delay, false, config->filter_length);
	}

	return 0;
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

	return hpm_qeiv2_configure(config);
}

static DEVICE_API(sensor, hpm_qeiv2_api) = {
	.sample_fetch = hpm_qeiv2_sample_fetch,
	.channel_get = hpm_qeiv2_channel_get,
};

#define HPM_QEIV2_INIT(n)                                                                          \
	BUILD_ASSERT(DT_INST_PROP(n, counts_per_revolution) > 1U,                                  \
		     "counts-per-revolution must be at least 2");                                  \
	BUILD_ASSERT(DT_INST_PROP_OR(n, phase_index, 0) < DT_INST_PROP(n, counts_per_revolution),  \
		     "phase-index must be below counts-per-revolution");                           \
	BUILD_ASSERT(DT_INST_PROP_OR(n, filter_length, 0) <= 511U,                                 \
		     "filter-length must not exceed 511");                                         \
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
		.z_calibration = DT_INST_PROP(n, z_calibration),                                   \
	};                                                                                         \
                                                                                                   \
	SENSOR_DEVICE_DT_INST_DEFINE(n, hpm_qeiv2_init, NULL, &hpm_qeiv2_data_##n,                 \
				     &hpm_qeiv2_config_##n, POST_KERNEL,                           \
				     CONFIG_SENSOR_INIT_PRIORITY, &hpm_qeiv2_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_QEIV2_INIT)
