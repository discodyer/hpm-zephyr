/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_opamp

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/opamp.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hpm_opamp_drv.h>

LOG_MODULE_REGISTER(opamp_hpmicro, CONFIG_OPAMP_LOG_LEVEL);

enum hpm_opamp_reference_source {
	HPM_OPAMP_REFERENCE_INTERNAL = 0,
	HPM_OPAMP_REFERENCE_EXTERNAL,
	HPM_OPAMP_REFERENCE_DAC,
};

struct hpm_opamp_config {
	OPAMP_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	const struct pinctrl_dev_config *pincfg;
	uint8_t functional_mode;
	uint8_t reference_source;
	uint8_t positive_input;
	uint8_t negative_input;
	uint8_t gain;
	uint8_t miller_capacitor;
	bool external_filter_capacitor;
	bool phase_margin_capacitor;
};

struct hpm_opamp_data {
	struct k_mutex lock;
};

static opamp_gain_t hpm_opamp_dt_gain(uint8_t gain)
{
	switch (gain) {
	case 0U:
		return gain_x_2;
	case 1U:
		return gain_x_4;
	case 2U:
		return gain_x_8;
	case 3U:
		return gain_x_16;
	case 4U:
		return gain_x_32;
	case 5U:
	default:
		return gain_x_64;
	}
}

static opamp_miller_cap_t hpm_opamp_miller_capacitor(uint8_t capacitor)
{
	switch (capacitor) {
	case 0U:
		return miller_cap_x_5;
	case 1U:
		return miller_cap_x_6;
	case 2U:
		return miller_cap_x_7;
	case 3U:
		return miller_cap_x_8;
	case 4U:
		return miller_cap_x_10;
	case 5U:
		return miller_cap_x_13;
	case 6U:
		return miller_cap_x_15;
	case 7U:
	default:
		return miller_cap_x_18;
	}
}

static opamp_mode_t hpm_opamp_mode(uint8_t functional_mode, uint8_t reference_source)
{
	switch (functional_mode) {
	case OPAMP_FUNCTIONAL_MODE_FOLLOWER:
		return mode_follow;
	case OPAMP_FUNCTIONAL_MODE_STANDALONE:
		return mode_user;
	case OPAMP_FUNCTIONAL_MODE_INVERTING:
		switch (reference_source) {
		case HPM_OPAMP_REFERENCE_EXTERNAL:
			return mode_invert_extern_vol;
		case HPM_OPAMP_REFERENCE_DAC:
			return mode_invert_dac_vol;
		case HPM_OPAMP_REFERENCE_INTERNAL:
		default:
			return mode_invert_intern_vol;
		}
	case OPAMP_FUNCTIONAL_MODE_NON_INVERTING:
	default:
		switch (reference_source) {
		case HPM_OPAMP_REFERENCE_EXTERNAL:
			return mode_non_invert_extern_vol;
		case HPM_OPAMP_REFERENCE_DAC:
			return mode_non_invert_dac_vol;
		case HPM_OPAMP_REFERENCE_INTERNAL:
		default:
			return mode_non_invert_gnd_vol;
		}
	}
}

static int hpm_opamp_validate_inputs(const struct hpm_opamp_config *config)
{
	switch (config->functional_mode) {
	case OPAMP_FUNCTIONAL_MODE_FOLLOWER:
		return config->positive_input < inp_pad_vsupply_x_0_25 ? 0 : -EINVAL;
	case OPAMP_FUNCTIONAL_MODE_STANDALONE:
		return 0;
	case OPAMP_FUNCTIONAL_MODE_INVERTING:
		if (config->reference_source == HPM_OPAMP_REFERENCE_INTERNAL) {
			if (config->external_filter_capacitor) {
				return -EINVAL;
			}
			return config->positive_input >= inp_pad_vsupply_x_0_25 &&
					       config->negative_input < inm_pad_floating
				       ? 0
				       : -EINVAL;
		}
		if ((config->positive_input >= inp_pad_vsupply_x_0_25) ||
		    (config->negative_input >= inm_pad_floating) ||
		    (config->external_filter_capacitor &&
		     config->reference_source != HPM_OPAMP_REFERENCE_EXTERNAL)) {
			return -EINVAL;
		}
		return 0;
	case OPAMP_FUNCTIONAL_MODE_NON_INVERTING:
		if (config->positive_input >= inp_pad_vsupply_x_0_25) {
			return -EINVAL;
		}
		if ((config->reference_source == HPM_OPAMP_REFERENCE_EXTERNAL) ||
		    ((config->reference_source == HPM_OPAMP_REFERENCE_INTERNAL) &&
		     config->external_filter_capacitor)) {
			return config->negative_input < inm_pad_floating ? 0 : -EINVAL;
		}
		if (config->external_filter_capacitor) {
			return -EINVAL;
		}
		return 0;
	default:
		return -EINVAL;
	}
}

static int hpm_opamp_init(const struct device *dev)
{
	const struct hpm_opamp_config *config = dev->config;
	struct hpm_opamp_data *data = dev->data;
	opamp_cfg_t opamp_cfg = {
		.mode = hpm_opamp_mode(config->functional_mode, config->reference_source),
		.negative_input_pin = (opamp_inm_pad_t)config->negative_input,
		.positive_input_pin = (opamp_inp_pad_t)config->positive_input,
		.gain = hpm_opamp_dt_gain(config->gain),
		.miller_cap = hpm_opamp_miller_capacitor(config->miller_capacitor),
		.enable_extern_filter_cap = config->external_filter_capacitor,
		.enable_phase_margin_cap = config->phase_margin_capacitor,
	};
	hpm_stat_t status;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	if (hpm_opamp_validate_inputs(config) < 0) {
		LOG_ERR("%s: input selection is invalid for the functional mode", dev->name);
		return -EINVAL;
	}

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		return ret;
	}

	/*
	 * The SDK selects these terminals internally for the fixed follower,
	 * internal-reference and DAC-reference topologies.
	 */
	if (config->functional_mode == OPAMP_FUNCTIONAL_MODE_FOLLOWER) {
		opamp_cfg.negative_input_pin = inm_pad_floating;
	} else if (config->functional_mode == OPAMP_FUNCTIONAL_MODE_NON_INVERTING) {
		if (config->reference_source == HPM_OPAMP_REFERENCE_DAC) {
			opamp_cfg.negative_input_pin = (opamp_inm_pad_t)0x02;
		} else if (config->reference_source == HPM_OPAMP_REFERENCE_INTERNAL &&
			   !config->external_filter_capacitor) {
			opamp_cfg.negative_input_pin = inm_pad_floating;
		}
	} else if (config->functional_mode == OPAMP_FUNCTIONAL_MODE_INVERTING &&
		   config->reference_source == HPM_OPAMP_REFERENCE_DAC) {
		opamp_cfg.positive_input_pin = (opamp_inp_pad_t)0x02;
	}

	status = opamp_init(config->base, &opamp_cfg);
	if (status != status_success) {
		return -EINVAL;
	}

	opamp_enable(config->base);
	k_mutex_init(&data->lock);

	return 0;
}

static int hpm_opamp_gain_to_sdk(enum opamp_gain gain, opamp_gain_t *sdk_gain)
{
	switch (gain) {
	case OPAMP_GAIN_2:
		*sdk_gain = gain_x_2;
		return 0;
	case OPAMP_GAIN_4:
		*sdk_gain = gain_x_4;
		return 0;
	case OPAMP_GAIN_8:
		*sdk_gain = gain_x_8;
		return 0;
	case OPAMP_GAIN_16:
		*sdk_gain = gain_x_16;
		return 0;
	case OPAMP_GAIN_32:
		*sdk_gain = gain_x_32;
		return 0;
	case OPAMP_GAIN_64:
		*sdk_gain = gain_x_64;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int hpm_opamp_set_gain(const struct device *dev, enum opamp_gain gain)
{
	const struct hpm_opamp_config *config = dev->config;
	struct hpm_opamp_data *data = dev->data;
	opamp_gain_t sdk_gain;
	int ret;

	if (!IS_ENABLED(CONFIG_OPAMP_HPMICRO_PROGRAMMABLE_GAIN)) {
		return -ENOTSUP;
	}

	if ((config->functional_mode == OPAMP_FUNCTIONAL_MODE_FOLLOWER) ||
	    (config->functional_mode == OPAMP_FUNCTIONAL_MODE_STANDALONE)) {
		return 0;
	}

	ret = hpm_opamp_gain_to_sdk(gain, &sdk_gain);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	opamp_gain_select(config->base, sdk_gain);
	k_mutex_unlock(&data->lock);

	return 0;
}

static DEVICE_API(opamp, hpm_opamp_api) = {
	.set_gain = hpm_opamp_set_gain,
};

#define HPM_OPAMP_INIT(n)                                                                          \
	BUILD_ASSERT(DT_INST_ENUM_IDX(n, functional_mode) != OPAMP_FUNCTIONAL_MODE_DIFFERENTIAL,   \
		     "HPM5300 OPAMP does not support differential mode");                          \
	BUILD_ASSERT(DT_INST_ENUM_IDX(n, positive_input) <= inp_pad_floating,                      \
		     "invalid positive input");                                                    \
	BUILD_ASSERT(DT_INST_ENUM_IDX(n, negative_input) <= inm_pad_floating,                      \
		     "invalid negative input");                                                    \
	BUILD_ASSERT(DT_INST_ENUM_IDX(n, gain) <= 5U, "invalid gain");                             \
	BUILD_ASSERT(DT_INST_ENUM_IDX(n, miller_capacitor) <= 7U, "invalid miller capacitor");     \
                                                                                                   \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
                                                                                                   \
	static struct hpm_opamp_data hpm_opamp_data_##n;                                           \
                                                                                                   \
	static const struct hpm_opamp_config hpm_opamp_config_##n = {                              \
		.base = (OPAMP_Type *)DT_INST_REG_ADDR(n),                                         \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
		.functional_mode = DT_INST_ENUM_IDX(n, functional_mode),                           \
		.reference_source = DT_INST_ENUM_IDX(n, reference_source),                         \
		.positive_input = DT_INST_ENUM_IDX(n, positive_input),                             \
		.negative_input = DT_INST_ENUM_IDX(n, negative_input),                             \
		.gain = DT_INST_ENUM_IDX(n, gain),                                                 \
		.miller_capacitor = DT_INST_ENUM_IDX(n, miller_capacitor),                         \
		.external_filter_capacitor = DT_INST_PROP(n, external_filter_capacitor),           \
		.phase_margin_capacitor = DT_INST_PROP(n, phase_margin_capacitor),                 \
	};                                                                                         \
                                                                                                   \
	static int hpm_opamp_init_##n(const struct device *dev)                                    \
	{                                                                                          \
		const struct hpm_opamp_config *config = dev->config;                               \
		int ret;                                                                           \
                                                                                                   \
		if (DT_INST_NODE_HAS_PROP(n, pinctrl_0)) {                                         \
			ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);          \
			if (ret < 0) {                                                             \
				return ret;                                                        \
			}                                                                          \
		}                                                                                  \
                                                                                                   \
		return hpm_opamp_init(dev);                                                        \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_opamp_init_##n, NULL, &hpm_opamp_data_##n,                    \
			      &hpm_opamp_config_##n, POST_KERNEL, CONFIG_OPAMP_INIT_PRIORITY,      \
			      &hpm_opamp_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_OPAMP_INIT)
