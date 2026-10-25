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
#include <zephyr/irq.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <hpm_adc16_drv.h>
#include <hpm_clock_drv.h>

#if defined(CONFIG_ADC_HPMICRO_ADC16_EXTERNAL_TRIGGER)
#include <drivers/adc/hpmicro_adc16.h>
#endif

LOG_MODULE_REGISTER(adc_hpmicro_adc16, CONFIG_ADC_LOG_LEVEL);

#define HPM_ADC16_MAX_CHANNELS         (ADC16_SOC_MAX_CH_NUM + 1U)
#define HPM_ADC16_SAMPLE_CYCLE_DEFAULT 10U
#define HPM_ADC16_SAMPLE_CYCLE_MAX     0x1FFU
#define HPM_ADC16_CONV_CLK_MAX         ADC16_SOC_CONV_CLK_FREQ_MAX
#define HPM_ADC16_SEQ_MAX_SAMPLES      ADC_SOC_SEQ_MAX_LEN
#define HPM_ADC16_DMA_ALIGNMENT        ADC_SOC_DMA_ADDR_ALIGNMENT

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
#ifdef CONFIG_ADC_HPMICRO_ADC16_SEQUENCE_DMA
	void (*irq_config_func)(const struct device *dev);
#endif
};

struct hpm_adc16_data {
	struct k_mutex lock;
	struct k_work async_work;
	const struct device *dev;
	struct adc_sequence async_sequence;
	struct k_poll_signal *async_signal;
	atomic_t async_busy;
#ifdef CONFIG_ADC_HPMICRO_ADC16_SEQUENCE_DMA
	struct k_sem seq_done;
	struct adc_sequence seq_sequence;
	uint8_t seq_channels[HPM_ADC16_SEQ_MAX_SAMPLES];
	uint8_t seq_samples;
	bool seq_error;
	bool sequence_irq_ready;
	uint32_t *seq_buffer;
#if defined(CONFIG_ADC_HPMICRO_ADC16_EXTERNAL_TRIGGER)
	enum hpm_adc16_trigger_source trigger_source;
#endif
#endif
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
#ifdef CONFIG_ADC_HPMICRO_ADC16_SEQUENCE_DMA
	if (samples > HPM_ADC16_SEQ_MAX_SAMPLES) {
		return -ENOTSUP;
	}
#endif
	if (sequence->buffer_size < samples * sizeof(uint16_t)) {
		return -ENOMEM;
	}

	return 0;
}

#ifdef CONFIG_ADC_HPMICRO_ADC16_SEQUENCE_DMA
static uint32_t hpm_adc16_sequence_status_mask(void)
{
	return adc16_event_seq_single_complete | adc16_event_seq_dma_abort |
	       adc16_event_seq_sw_conflict | adc16_event_seq_hw_conflict |
	       adc16_event_read_conflict | adc16_event_dma_fifo_full;
}

static void hpm_adc16_sequence_isr(const struct device *dev)
{
	const struct hpm_adc16_config *config = dev->config;
	struct hpm_adc16_data *data = dev->data;
	struct k_poll_signal *async_signal;
	uint32_t status = adc16_get_status_flags(config->base);
	uint32_t event_mask = hpm_adc16_sequence_status_mask();
	uint16_t *output;
	uint32_t error_mask = adc16_event_seq_dma_abort | adc16_event_seq_sw_conflict |
			      adc16_event_seq_hw_conflict | adc16_event_read_conflict |
			      adc16_event_dma_fifo_full;

	if ((status & event_mask) == 0U) {
		return;
	}

	adc16_disable_interrupts(config->base, event_mask);
	adc16_clear_status_flags(config->base, status);

	data->seq_error = (status & error_mask) != 0U;
	if (!data->seq_error) {
		adc16_seq_dma_data_t *samples = (adc16_seq_dma_data_t *)data->seq_buffer;

		output = data->seq_sequence.buffer;
		for (uint8_t i = 0U; i < data->seq_samples; i++) {
			uint16_t value = samples[i].result;

			if (samples[i].adc_ch != data->seq_channels[i]) {
				data->seq_error = true;
				break;
			}
			if (data->seq_sequence.resolution < 16U) {
				value >>= 16U - data->seq_sequence.resolution;
			}
			output[i] = value;
		}
	}

	async_signal = data->async_signal;
	data->async_signal = NULL;
	atomic_clear(&data->async_busy);
	k_sem_give(&data->seq_done);

	if (async_signal != NULL) {
		k_poll_signal_raise(async_signal, data->seq_error ? -EIO : 0);
	}
}

static int hpm_adc16_start_sequence(const struct device *dev, const struct adc_sequence *sequence)
{
	const struct hpm_adc16_config *config = dev->config;
	struct hpm_adc16_data *data = dev->data;
	adc16_config_t adc_config;
	adc16_seq_config_t seq_config = {0};
	adc16_dma_config_t dma_config = {0};
	uint32_t channels = sequence->channels;
	uint8_t index = 0U;
	hpm_stat_t status;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);

	while (channels != 0U) {
		uint8_t channel = find_lsb_set(channels) - 1U;

		data->seq_channels[index++] = channel;
		channels &= ~BIT(channel);
	}
	data->seq_samples = index;
	data->seq_sequence = *sequence;
	data->seq_error = false;

	adc16_disable_interrupts(config->base, hpm_adc16_sequence_status_mask());
	adc16_clear_status_flags(config->base, UINT32_MAX);

	adc16_get_default_config(&adc_config);
	adc_config.res = hpm_adc16_resolution(sequence->resolution);
	adc_config.adc_clk_div = data->clock_divider;
	adc_config.conv_mode = adc16_conv_mode_sequence;
	adc_config.wait_dis = false;

	status = adc16_init(config->base, &adc_config);
	if (status != status_success) {
		ret = -EIO;
		goto out;
	}
	adc16_disable_oneshot_mode(config->base);

	seq_config.seq_len = data->seq_samples;
	seq_config.restart_en = false;
	seq_config.cont_en = false;
#if defined(CONFIG_ADC_HPMICRO_ADC16_EXTERNAL_TRIGGER)
	seq_config.sw_trig_en = data->trigger_source == HPM_ADC16_TRIGGER_SOFTWARE;
	seq_config.hw_trig_en = data->trigger_source == HPM_ADC16_TRIGGER_EXTERNAL;
#else
	seq_config.sw_trig_en = true;
	seq_config.hw_trig_en = false;
#endif
	for (uint8_t i = 0U; i < data->seq_samples; i++) {
		seq_config.queue[i].ch = data->seq_channels[i];
		seq_config.queue[i].seq_int_en = i == (uint8_t)(data->seq_samples - 1U);
	}

	status = adc16_set_seq_config(config->base, &seq_config);
	if (status != status_success) {
		ret = -EIO;
		goto out;
	}

	dma_config.start_addr = data->seq_buffer;
	dma_config.buff_len_in_4bytes = data->seq_samples;
	dma_config.stop_en = false;
	status = adc16_init_seq_dma(config->base, &dma_config);
	if (status != status_success) {
		ret = -EIO;
		goto out;
	}

	if (!data->sequence_irq_ready) {
		config->irq_config_func(dev);
		data->sequence_irq_ready = true;
	}

	k_sem_reset(&data->seq_done);
	adc16_enable_interrupts(config->base, hpm_adc16_sequence_status_mask());
#if defined(CONFIG_ADC_HPMICRO_ADC16_EXTERNAL_TRIGGER)
	status = seq_config.sw_trig_en ? adc16_trigger_seq_by_sw(config->base)
					 : status_success;
#else
	status = adc16_trigger_seq_by_sw(config->base);
#endif
	if (status != status_success) {
		adc16_disable_interrupts(config->base, hpm_adc16_sequence_status_mask());
		(void)adc16_deinit(config->base);
		ret = -EBUSY;
	}

out:
	data->resolution = sequence->resolution;
	k_mutex_unlock(&data->lock);
	return ret;
}

static int hpm_adc16_read(const struct device *dev, const struct adc_sequence *sequence)
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

	ret = hpm_adc16_start_sequence(dev, sequence);
	if (ret == 0) {
		ret = k_sem_take(&data->seq_done,
				 K_MSEC(CONFIG_ADC_HPMICRO_ADC16_SEQUENCE_TIMEOUT_MS));
		if (ret < 0) {
			const struct hpm_adc16_config *config = dev->config;

			adc16_disable_interrupts(config->base, hpm_adc16_sequence_status_mask());
			ret = -ETIMEDOUT;
			(void)adc16_deinit(config->base);
		} else {
			ret = data->seq_error ? -EIO : 0;
		}
	}

	atomic_clear(&data->async_busy);
	return ret;
}

#if defined(CONFIG_ADC_ASYNC)
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

	data->async_signal = async;
	ret = hpm_adc16_start_sequence(dev, sequence);
	if (ret < 0) {
		data->async_signal = NULL;
		atomic_clear(&data->async_busy);
	}

	return ret;
}
#endif /* CONFIG_ADC_ASYNC */

#else /* CONFIG_ADC_HPMICRO_ADC16_SEQUENCE_DMA */

static int hpm_adc16_read_sequence(const struct device *dev, const struct adc_sequence *sequence)
{
	const struct hpm_adc16_config *config = dev->config;
	struct hpm_adc16_data *data = dev->data;
	adc16_config_t adc_config;
	uint16_t *buffer;
	uint32_t channels;
	uint8_t adc_resolution;
	hpm_stat_t status;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);

	adc_resolution = hpm_adc16_resolution(sequence->resolution);
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

	buffer = sequence->buffer;
	channels = sequence->channels;
	while (channels != 0U) {
		uint16_t result;
		uint8_t channel = find_lsb_set(channels) - 1U;

		channels &= ~BIT(channel);
		status = adc16_get_oneshot_result(config->base, channel, &result);
		if (status != status_success) {
			ret = -EIO;
			break;
		}
		if (sequence->resolution < 16U) {
			result >>= 16U - sequence->resolution;
		}
		*buffer++ = result;
	}

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int hpm_adc16_read(const struct device *dev, const struct adc_sequence *sequence)
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
#endif /* CONFIG_ADC_ASYNC */
#endif /* CONFIG_ADC_HPMICRO_ADC16_SEQUENCE_DMA */

#if defined(CONFIG_ADC_HPMICRO_ADC16_EXTERNAL_TRIGGER)
static int hpm_adc16_set_trigger_source_impl(const struct device *dev,
					     enum hpm_adc16_trigger_source source)
{
	struct hpm_adc16_data *data = dev->data;

	if ((source != HPM_ADC16_TRIGGER_SOFTWARE) && (source != HPM_ADC16_TRIGGER_EXTERNAL)) {
		return -EINVAL;
	}

	if (atomic_get(&data->async_busy) != 0) {
		return -EBUSY;
	}

	data->trigger_source = source;
	return 0;
}

static int hpm_adc16_get_trigger_source_impl(const struct device *dev,
					     enum hpm_adc16_trigger_source *source)
{
	const struct hpm_adc16_data *data = dev->data;

	if (source == NULL) {
		return -EINVAL;
	}

	*source = data->trigger_source;
	return 0;
}
#endif

#ifdef CONFIG_ADC_HPMICRO_ADC16_SEQUENCE_DMA
#define HPM_ADC16_IRQ_CONFIG(n)                                                                    \
	static void hpm_adc16_irq_config_##n(const struct device *dev)                             \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_adc16_sequence_isr,     \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}
#define HPM_ADC16_IRQ_CONFIG_FIELD(n) .irq_config_func = hpm_adc16_irq_config_##n,
#define HPM_ADC16_RUNTIME_INIT(n)                                                                  \
	do {                                                                                       \
		k_sem_init(&data->seq_done, 0, 1);                                                 \
		data->seq_buffer = hpm_adc16_seq_buffer_##n;                                       \
	} while (false)
#else
#define HPM_ADC16_IRQ_CONFIG(n)
#define HPM_ADC16_IRQ_CONFIG_FIELD(n)
#if defined(CONFIG_ADC_ASYNC)
#define HPM_ADC16_RUNTIME_INIT(n)                                                                  \
	do {                                                                                       \
		k_work_init(&data->async_work, hpm_adc16_async_work_handler);                      \
	} while (false)
#else
#define HPM_ADC16_RUNTIME_INIT(n)
#endif
#endif

#if defined(CONFIG_ADC_HPMICRO_ADC16_EXTERNAL_TRIGGER)
#define HPM_ADC16_API(n)                                                                           \
	static DEVICE_API(hpm_adc16, hpm_adc16_api_##n) = {                                        \
		.adc_api =                                                                         \
			{                                                                          \
				.channel_setup = hpm_adc16_channel_setup,                          \
				.read = hpm_adc16_read,                                            \
				IF_ENABLED(CONFIG_ADC_ASYNC,                                       \
					   (.read_async = hpm_adc16_read_async,))                   \
					.ref_internal = DT_INST_PROP_OR(n, vref_mv, 3300),         \
			},                                                                         \
		.set_trigger_source = hpm_adc16_set_trigger_source_impl,                           \
		.get_trigger_source = hpm_adc16_get_trigger_source_impl,                           \
	};
#else
#define HPM_ADC16_API(n)                                                                           \
	static DEVICE_API(adc, hpm_adc16_api_##n) = {                                              \
		.channel_setup = hpm_adc16_channel_setup,                                          \
		.read = hpm_adc16_read,                                                            \
		IF_ENABLED(CONFIG_ADC_ASYNC, (.read_async = hpm_adc16_read_async,)) .ref_internal =   \
						     DT_INST_PROP_OR(n, vref_mv, 3300),            \
	};
#endif

#define HPM_ADC16_INIT(n)                                                                          \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	HPM_ADC16_IRQ_CONFIG(n)                                                                    \
	HPM_ADC16_API(n)                                                                           \
	static uint32_t hpm_adc16_seq_buffer_##n[HPM_ADC16_SEQ_MAX_SAMPLES] __nocache              \
		__aligned(HPM_ADC16_DMA_ALIGNMENT);                                                \
	static struct hpm_adc16_data hpm_adc16_data_##n;                                           \
                                                                                                   \
	static const struct hpm_adc16_config hpm_adc16_config_##n = {                              \
		.base = (ADC16_Type *)DT_INST_REG_ADDR(n),                                         \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.clock_name = DT_INST_PROP(n, clock_name),                                         \
		.clock_src = (clk_src_t)DT_INST_PROP(n, clock_src),                                \
		.ana_clock_name = DT_INST_PROP(n, ana_clock_name),                                 \
		.ana_clock_src = DT_INST_PROP(n, ana_clock_src),                                   \
		.ana_clock_div = DT_INST_CLOCKS_CELL_BY_IDX(n, 0, divider),                        \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
		HPM_ADC16_IRQ_CONFIG_FIELD(n)};                                                    \
                                                                                                   \
	static int hpm_adc16_init_##n(const struct device *dev)                                    \
	{                                                                                          \
		const struct hpm_adc16_config *config = dev->config;                               \
		struct hpm_adc16_data *data = dev->data;                                           \
		adc16_config_t adc_config;                                                         \
		uint32_t adc_input_freq;                                                           \
		uint32_t clock_divider;                                                            \
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
		if (config->clock_src != (clk_src_t)clk_adc_src_ahb0) {                            \
			status = clock_set_source_divider(config->ana_clock_name,                  \
							  config->ana_clock_src,                   \
							  config->ana_clock_div);                  \
			if (status != status_success) {                                            \
				return -EIO;                                                       \
			}                                                                          \
		}                                                                                  \
                                                                                                   \
		status = clock_set_adc_source(config->clock_name, config->clock_src);              \
		if (status != status_success) {                                                    \
			return -EIO;                                                               \
		}                                                                                  \
                                                                                                   \
		ret = clock_control_on(config->clock_dev, config->clock_subsys);                   \
		if (ret < 0) {                                                                     \
			return ret;                                                                \
		}                                                                                  \
                                                                                                   \
		adc_input_freq = clock_get_frequency(config->clock_name);                          \
		status = adc16_calc_clock_divider(adc_input_freq, HPM_ADC16_CONV_CLK_MAX,          \
						  &clock_divider);                                 \
		if (status != status_success) {                                                    \
			return -EINVAL;                                                            \
		}                                                                                  \
                                                                                                   \
		adc16_get_default_config(&adc_config);                                             \
		adc_config.res = adc16_res_16_bits;                                                \
		adc_config.adc_clk_div = (uint8_t)clock_divider;                                   \
		adc_config.conv_mode = adc16_conv_mode_oneshot;                                    \
		adc_config.wait_dis = false;                                                       \
		status = adc16_init(config->base, &adc_config);                                    \
		if (status != status_success) {                                                    \
			return -EIO;                                                               \
		}                                                                                  \
		adc16_enable_oneshot_mode(config->base);                                           \
		data->dev = dev;                                                                   \
		data->resolution = 16U;                                                            \
		data->clock_divider = (uint8_t)clock_divider;                                      \
		k_mutex_init(&data->lock);                                                         \
		HPM_ADC16_RUNTIME_INIT(n);                                                         \
		return 0;                                                                          \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_adc16_init_##n, NULL, &hpm_adc16_data_##n,                    \
			      &hpm_adc16_config_##n, POST_KERNEL, CONFIG_ADC_INIT_PRIORITY,        \
			      &hpm_adc16_api_##n);

DT_INST_FOREACH_STATUS_OKAY(HPM_ADC16_INIT)
