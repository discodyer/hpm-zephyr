/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_adc16

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/adc/adc.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <hpm_adc16_drv.h>
#include <hpm_clock_drv.h>

LOG_MODULE_REGISTER(adc_hpmicro_adc16, CONFIG_ADC_LOG_LEVEL);

#define HPM_ADC16_MAX_CHANNELS         (ADC16_SOC_MAX_CH_NUM + 1U)
#define HPM_ADC16_SAMPLE_CYCLE_DEFAULT 10U
#define HPM_ADC16_SAMPLE_CYCLE_MAX     0x1FFU
#define HPM_ADC16_CONV_CLK_MAX         ADC16_SOC_CONV_CLK_FREQ_MAX

struct hpm_adc16_config {
	ADC16_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	clock_name_t clock_name;
	clk_src_t clock_src;
	clock_name_t ana_clock_name;
	clock_source_t ana_clock_src;
	uint32_t ana_clock_div;
	const struct pinctrl_dev_config *pincfg;
};

struct hpm_adc16_data {
	struct k_mutex lock;
	struct k_work async_work;
	const struct device *dev;
	struct adc_sequence async_sequence;
	struct k_poll_signal *async_signal;
	atomic_t async_busy;
	uint8_t resolution;
	uint8_t clock_divider;
};

static int hpm_adc16_channel_setup(const struct device *dev,
				   const struct adc_channel_cfg *channel_cfg)
{
	adc16_channel_config_t config;
	hpm_stat_t status;

	if (channel_cfg == NULL) {
		return -EINVAL;
	}

	if (channel_cfg->channel_id >= HPM_ADC16_MAX_CHANNELS) {
		return -EINVAL;
	}

	if (channel_cfg->gain != ADC_GAIN_1) {
		return -ENOTSUP;
	}

	if (channel_cfg->reference != ADC_REF_INTERNAL) {
		return -ENOTSUP;
	}

	if (channel_cfg->differential) {
		return -ENOTSUP;
	}

	if (channel_cfg->acquisition_time != ADC_ACQ_TIME_DEFAULT) {
		if (ADC_ACQ_TIME_UNIT(channel_cfg->acquisition_time) != ADC_ACQ_TIME_TICKS) {
			return -ENOTSUP;
		}

		if ((ADC_ACQ_TIME_VALUE(channel_cfg->acquisition_time) == 0U) ||
		    (ADC_ACQ_TIME_VALUE(channel_cfg->acquisition_time) >
		     HPM_ADC16_SAMPLE_CYCLE_MAX)) {
			return -EINVAL;
		}
	}

	adc16_get_channel_default_config(&config);
	config.ch = channel_cfg->channel_id;
	config.sample_cycle = channel_cfg->acquisition_time == ADC_ACQ_TIME_DEFAULT
				      ? HPM_ADC16_SAMPLE_CYCLE_DEFAULT
				      : ADC_ACQ_TIME_VALUE(channel_cfg->acquisition_time);

	status = adc16_init_channel(((const struct hpm_adc16_config *)dev->config)->base, &config);
	if (status != status_success) {
		return -EINVAL;
	}

	return 0;
}

static uint8_t hpm_adc16_resolution(uint8_t resolution)
{
	switch (resolution) {
	case 8U:
		return adc16_res_8_bits;
	case 10U:
		return adc16_res_10_bits;
	case 12U:
		return adc16_res_12_bits;
	case 16U:
		return adc16_res_16_bits;
	default:
		return 0U;
	}
}

static int hpm_adc16_validate_sequence(const struct adc_sequence *sequence)
{
	size_t samples;

	if ((sequence == NULL) || (sequence->buffer == NULL)) {
		return -EINVAL;
	}

	if ((sequence->channels == 0U) ||
	    ((sequence->channels & ~BIT_MASK(HPM_ADC16_MAX_CHANNELS)) != 0U)) {
		return -EINVAL;
	}

	if ((sequence->options != NULL) &&
	    ((sequence->options->callback != NULL) || (sequence->options->extra_samplings != 0U) ||
	     (sequence->options->interval_us != 0U))) {
		return -ENOTSUP;
	}

	if ((sequence->oversampling != 0U) || sequence->calibrate) {
		return -ENOTSUP;
	}

	if (hpm_adc16_resolution(sequence->resolution) == 0U) {
		return -EINVAL;
	}

	samples = POPCOUNT(sequence->channels);
	if (sequence->buffer_size < samples * sizeof(uint16_t)) {
		return -ENOMEM;
	}

	return 0;
}

static int hpm_adc16_read_sequence(const struct device *dev, const struct adc_sequence *sequence)
{
	const struct hpm_adc16_config *config = dev->config;
	struct hpm_adc16_data *data = dev->data;
	adc16_config_t adc_config;
	uint16_t *buffer;
	uint32_t channels;
	uint32_t channel;
	uint8_t adc_resolution;
	hpm_stat_t status;
	int ret;

	ret = hpm_adc16_validate_sequence(sequence);
	if (ret < 0) {
		return ret;
	}

	buffer = sequence->buffer;
	adc_resolution = hpm_adc16_resolution(sequence->resolution);

	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->resolution != sequence->resolution) {
		adc16_get_default_config(&adc_config);
		adc_config.res = adc_resolution;
		adc_config.adc_clk_div = data->clock_divider;
		adc_config.conv_mode = adc16_conv_mode_oneshot;
		adc_config.wait_dis = false;

		status = adc16_init(config->base, &adc_config);
		if (status != status_success) {
			ret = -EIO;
			goto out;
		}

		adc16_enable_oneshot_mode(config->base);
		data->resolution = sequence->resolution;
	}

	channels = sequence->channels;
	while (channels != 0U) {
		uint16_t result;

		channel = find_lsb_set(channels) - 1U;
		channels &= ~BIT(channel);

		status = adc16_get_oneshot_result(config->base, channel, &result);
		if (status != status_success) {
			ret = -EIO;
			goto out;
		}

		if (sequence->resolution < 16U) {
			result >>= 16U - sequence->resolution;
		}
		*buffer++ = result;
	}

	ret = 0;

out:
	k_mutex_unlock(&data->lock);

	return ret;
}

static int hpm_adc16_read(const struct device *dev, const struct adc_sequence *sequence)
{
	struct hpm_adc16_data *data = dev->data;
	int ret;

	if (!atomic_cas(&data->async_busy, 0, 1)) {
		return -EBUSY;
	}

	ret = hpm_adc16_read_sequence(dev, sequence);
	atomic_clear(&data->async_busy);

	return ret;
}

#if defined(CONFIG_ADC_ASYNC)
static void hpm_adc16_async_work_handler(struct k_work *work)
{
	struct hpm_adc16_data *data = CONTAINER_OF(work, struct hpm_adc16_data, async_work);
	int ret;

	ret = hpm_adc16_read_sequence(data->dev, &data->async_sequence);
	if (data->async_signal != NULL) {
		k_poll_signal_raise(data->async_signal, ret);
	}
	data->async_signal = NULL;
	atomic_clear(&data->async_busy);
}

static int hpm_adc16_read_async(const struct device *dev, const struct adc_sequence *sequence,
				struct k_poll_signal *async)
{
	struct hpm_adc16_data *data = dev->data;
	int ret;

	ret = hpm_adc16_validate_sequence(sequence);
	if (ret < 0) {
		return ret;
	}

	if (!atomic_cas(&data->async_busy, 0, 1)) {
		return -EBUSY;
	}

	data->async_sequence = *sequence;
	data->async_signal = async;
	ret = k_work_submit(&data->async_work);
	if (ret < 0) {
		data->async_signal = NULL;
		atomic_clear(&data->async_busy);
		return ret;
	}

	return 0;
}
#endif

#define HPM_ADC16_API(n)                                                                           \
	static DEVICE_API(adc, hpm_adc16_api_##n) = {                                              \
		.channel_setup = hpm_adc16_channel_setup,                                          \
		.read = hpm_adc16_read,                                                            \
		IF_ENABLED(CONFIG_ADC_ASYNC, (.read_async = hpm_adc16_read_async,)) .ref_internal =   \
						     DT_INST_PROP_OR(n, vref_mv, 3300),            \
	};

#define HPM_ADC16_INIT(n)                                                                            \
	PINCTRL_DT_INST_DEFINE(n);                                                                   \
	HPM_ADC16_API(n)                                                                             \
                                                                                                     \
	static struct hpm_adc16_data hpm_adc16_data_##n;                                             \
                                                                                                     \
	static const struct hpm_adc16_config hpm_adc16_config_##n = {                                \
		.base = (ADC16_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                  \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),                \
		.clock_name = DT_INST_PROP(n, clock_name),                                           \
		.clock_src = (clk_src_t)DT_INST_PROP(n, clock_src),                                  \
		.ana_clock_name = DT_INST_PROP(n, ana_clock_name),                                   \
		.ana_clock_src = DT_INST_PROP(n, ana_clock_src),                                     \
		.ana_clock_div = DT_INST_CLOCKS_CELL_BY_IDX(n, 0, divider),                          \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                         \
	};                                                                                           \
                                                                                                     \
	static int hpm_adc16_init_##n(const struct device *dev)                                      \
	{                                                                                            \
		const struct hpm_adc16_config *config = dev->config;                                 \
		struct hpm_adc16_data *data = dev->data;                                             \
		adc16_config_t adc_config;                                                           \
		uint32_t adc_input_freq;                                                             \
		uint32_t clock_divider;                                                              \
		hpm_stat_t status;                                                                   \
		int ret;                                                                             \
                                                                                                     \
		if (!device_is_ready(config->clock_dev)) {                                           \
			return -ENODEV;                                                              \
		}                                                                                    \
                                                                                                     \
		if (DT_INST_NODE_HAS_PROP(n, pinctrl_0)) {                                           \
			ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);            \
			if (ret < 0) {                                                               \
				return ret;                                                          \
			}                                                                            \
		}                                                                                    \
                                                                                                     \
		if (config->clock_src != (clk_src_t)clk_adc_src_ahb0) {                              \
			status = clock_set_source_divider(config->ana_clock_name,                    \
							  config->ana_clock_src,                     \
							  config->ana_clock_div);                    \
			if (status != status_success) {                                              \
				return -EIO;                                                         \
			}                                                                            \
		}                                                                                    \
                                                                                                     \
		status = clock_set_adc_source(config->clock_name, config->clock_src);                \
		if (status != status_success) {                                                      \
			return -EIO;                                                                 \
		}                                                                                    \
                                                                                                     \
		ret = clock_control_on(config->clock_dev, config->clock_subsys);                     \
		if (ret < 0) {                                                                       \
			return ret;                                                                  \
		}                                                                                    \
                                                                                                     \
		adc_input_freq = clock_get_frequency(config->clock_name);                            \
		status = adc16_calc_clock_divider(adc_input_freq, HPM_ADC16_CONV_CLK_MAX,            \
						  &clock_divider);                                   \
		if (status != status_success) {                                                      \
			return -EINVAL;                                                              \
		}                                                                                    \
                                                                                                     \
		adc16_get_default_config(&adc_config);                                               \
		adc_config.res = adc16_res_16_bits;                                                  \
		adc_config.adc_clk_div = (uint8_t)clock_divider;                                     \
		adc_config.conv_mode = adc16_conv_mode_oneshot;                                      \
		adc_config.wait_dis = false;                                                         \
		status = adc16_init(config->base, &adc_config);                                      \
		if (status != status_success) {                                                      \
			return -EIO;                                                                 \
		}                                                                                    \
		adc16_enable_oneshot_mode(config->base);                                             \
		data->dev = dev;                                                                     \
		data->resolution = 16U;                                                              \
		data->clock_divider = (uint8_t)clock_divider;                                        \
		k_mutex_init(&data->lock);                                                           \
		IF_ENABLED(CONFIG_ADC_ASYNC,                                                       \
			   (k_work_init(&data->async_work, hpm_adc16_async_work_handler);)) \
                                                                                                     \
		return 0;                                                                            \
	}                                                                                            \
                                                                                                     \
	DEVICE_DT_INST_DEFINE(n, hpm_adc16_init_##n, NULL, &hpm_adc16_data_##n,                      \
			      &hpm_adc16_config_##n, POST_KERNEL, CONFIG_ADC_INIT_PRIORITY,          \
			      &hpm_adc16_api_##n);

DT_INST_FOREACH_STATUS_OKAY(HPM_ADC16_INIT)
