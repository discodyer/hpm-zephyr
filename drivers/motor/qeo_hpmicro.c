/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_qeo

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <drivers/motor/hpmicro_qeo.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/kernel.h>

#include <hpm_common.h>
#include <hpm_qeo_drv.h>

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
