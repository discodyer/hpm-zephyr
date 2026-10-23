/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_dac

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hpm_clock_drv.h>
#include <hpm_dac_drv.h>

LOG_MODULE_REGISTER(dac_hpmicro, CONFIG_DAC_LOG_LEVEL);

struct hpm_dac_config {
	DAC_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	clock_name_t clock_name;
	clk_src_t clock_src;
	clock_name_t ana_clock_name;
	clock_source_t ana_clock_src;
	uint32_t ana_clock_div;
	const struct pinctrl_dev_config *pincfg;
};

struct hpm_dac_data {
	struct k_mutex lock;
	bool configured;
};

static int hpm_dac_channel_setup(const struct device *dev,
				 const struct dac_channel_cfg *channel_cfg)
{
	struct hpm_dac_data *data = dev->data;
	const struct hpm_dac_config *config = dev->config;
	dac_config_t dac_config;
	uint32_t input_freq;
	uint8_t ana_div;
	hpm_stat_t status;

	if (channel_cfg == NULL) {
		return -EINVAL;
	}

	if (channel_cfg->channel_id != 0U) {
		return -ENOTSUP;
	}

	if (channel_cfg->resolution != 12U) {
		return -ENOTSUP;
	}

	if (channel_cfg->internal || channel_cfg->buffered) {
		return -ENOTSUP;
	}

	input_freq = clock_get_frequency(config->clock_name);
	status = dac_calc_ana_divider(input_freq, DAC_SOC_ANA_CLK_FREQ_MAX, &ana_div);
	if (status != status_success) {
		return -EINVAL;
	}

	dac_get_default_config(&dac_config);
	dac_config.dac_mode = dac_mode_direct;
	dac_config.ana_div = ana_div;
	dac_config.sync_mode = clock_get_source(config->clock_name) == clk_dac_src_ahb0;

	k_mutex_lock(&data->lock, K_FOREVER);
	status = dac_init(config->base, &dac_config);
	if (status == status_success) {
		dac_enable_conversion(config->base, true);
		data->configured = true;
	}
	k_mutex_unlock(&data->lock);

	return status == status_success ? 0 : -EINVAL;
}

static int hpm_dac_write_value(const struct device *dev, uint8_t channel, uint32_t value)
{
	const struct hpm_dac_config *config = dev->config;
	struct hpm_dac_data *data = dev->data;
	hpm_stat_t status;

	if (channel != 0U) {
		return -ENOTSUP;
	}

	if (value > DAC_SOC_MAX_DATA) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	if (!data->configured) {
		k_mutex_unlock(&data->lock);
		return -EINVAL;
	}

	status = dac_set_direct_config(config->base, (uint16_t)value);
	k_mutex_unlock(&data->lock);

	return status == status_success ? 0 : -EINVAL;
}

static DEVICE_API(dac, hpm_dac_api) = {
	.channel_setup = hpm_dac_channel_setup,
	.write_value = hpm_dac_write_value,
};

#define HPM_DAC_INIT(n)                                                                            \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
                                                                                                   \
	static struct hpm_dac_data hpm_dac_data_##n;                                               \
                                                                                                   \
	static const struct hpm_dac_config hpm_dac_config_##n = {                                  \
		.base = (DAC_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.clock_name = DT_INST_PROP(n, clock_name),                                         \
		.clock_src = (clk_src_t)DT_INST_PROP(n, clock_src),                                \
		.ana_clock_name = DT_INST_PROP(n, ana_clock_name),                                 \
		.ana_clock_src = DT_INST_PROP(n, ana_clock_src),                                   \
		.ana_clock_div = DT_INST_CLOCKS_CELL_BY_IDX(n, 0, divider),                        \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
	};                                                                                         \
                                                                                                   \
	static int hpm_dac_init_##n(const struct device *dev)                                      \
	{                                                                                          \
		const struct hpm_dac_config *config = dev->config;                                 \
		struct hpm_dac_data *data = dev->data;                                             \
		hpm_stat_t status;                                                                 \
		int ret;                                                                           \
                                                                                                   \
		if (!device_is_ready(config->clock_dev)) {                                         \
			return -ENODEV;                                                            \
		}                                                                                  \
                                                                                                   \
		if (DT_INST_NODE_HAS_PROP(n, pinctrl_0)) {                                         \
			ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);          \
			if (ret < 0) {                                                             \
				return ret;                                                        \
			}                                                                          \
		}                                                                                  \
                                                                                                   \
		if (config->clock_src != (clk_src_t)clk_dac_src_ahb0) {                            \
			status = clock_set_source_divider(config->ana_clock_name,                  \
							  config->ana_clock_src,                   \
							  config->ana_clock_div);                  \
			if (status != status_success) {                                            \
				return -EIO;                                                       \
			}                                                                          \
		}                                                                                  \
                                                                                                   \
		status = clock_set_dac_source(config->clock_name, config->clock_src);              \
		if (status != status_success) {                                                    \
			return -EIO;                                                               \
		}                                                                                  \
                                                                                                   \
		ret = clock_control_on(config->clock_dev, config->clock_subsys);                   \
		if (ret < 0) {                                                                     \
			return ret;                                                                \
		}                                                                                  \
                                                                                                   \
		k_mutex_init(&data->lock);                                                         \
                                                                                                   \
		return 0;                                                                          \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_dac_init_##n, NULL, &hpm_dac_data_##n, &hpm_dac_config_##n,   \
			      POST_KERNEL, CONFIG_DAC_INIT_PRIORITY, &hpm_dac_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_DAC_INIT)
