/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_trgm

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/mux.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/mux/hpmicro_trgm.h>
#include <hpm_common.h>
#include <hpm_trgm_drv.h>
#include <hpm_trgm_soc_drv.h>

LOG_MODULE_REGISTER(mux_hpmicro_trgm, CONFIG_MUX_LOG_LEVEL);

BUILD_ASSERT(ARRAY_SIZE(((TRGM_Type *)0)->FILTCFG) == HPM_TRGM_FILTER_COUNT);
BUILD_ASSERT(ARRAY_SIZE(((TRGM_Type *)0)->TRGOCFG) == HPM_TRGM_OUTPUT_COUNT);
BUILD_ASSERT(ARRAY_SIZE(((TRGM_Type *)0)->DMACFG) == HPM_TRGM_DMA_REQUEST_COUNT);
BUILD_ASSERT(HPM_TRGM_FILTER_COUNT == 28U);
BUILD_ASSERT(HPM_TRGM_OUTPUT_COUNT == 137U);
BUILD_ASSERT(HPM_TRGM_DMA_REQUEST_COUNT == 8U);
BUILD_ASSERT((int)HPM_TRGM_FILTER_BYPASS == (int)trgm_filter_mode_bypass);
BUILD_ASSERT((int)HPM_TRGM_FILTER_RAPID_CHANGE == (int)trgm_filter_mode_rapid_change);
BUILD_ASSERT((int)HPM_TRGM_FILTER_DELAY == (int)trgm_filter_mode_delay);
BUILD_ASSERT((int)HPM_TRGM_FILTER_STABLE_LOW == (int)trgm_filter_mode_stable_low);
BUILD_ASSERT((int)HPM_TRGM_FILTER_STABLE_HIGH == (int)trgm_filter_mode_stable_high);
BUILD_ASSERT((int)HPM_TRGM_ADC_CONSUMER_QEI0_ADC0 == (int)trgm_adc_matrix_output_to_qei0_adc0);
BUILD_ASSERT((int)HPM_TRGM_ADC_CONSUMER_QEI1_ADC1 == (int)trgm_adc_matrix_output_to_qei1_adc1);
BUILD_ASSERT((int)HPM_TRGM_ADC_SOURCE_ADC0 == (int)trgm_adc_matrix_in_from_adc0);
BUILD_ASSERT((int)HPM_TRGM_ADC_SOURCE_RDC_ADC1 == (int)trgm_adc_matrix_in_from_rdc_adc1);
BUILD_ASSERT((int)HPM_TRGM_DAC_CONSUMER_ACMP0 == (int)trgm_dac_matrix_output_to_acmp0);
BUILD_ASSERT((int)HPM_TRGM_DAC_CONSUMER_DAC1 == (int)trgm_dac_matrix_output_to_dac1);
BUILD_ASSERT((int)HPM_TRGM_DAC_SOURCE_QEO0_DAC0 == (int)trgm_dac_matrix_in_from_qeo0_dac0);
BUILD_ASSERT((int)HPM_TRGM_DAC_SOURCE_RDC_DAC1 == (int)trgm_dac_matrix_in_from_rdc_dac1);
BUILD_ASSERT((int)HPM_TRGM_POS_CONSUMER_SEI_POS0 == (int)trgm_pos_matrix_output_to_sei_pos0);
BUILD_ASSERT((int)HPM_TRGM_POS_CONSUMER_QEO1 == (int)trgm_pos_matrix_output_to_qeo1);
BUILD_ASSERT((int)HPM_TRGM_POS_SOURCE_SEI_POS0 == (int)trgm_pos_matrix_in_from_sei_pos0);
BUILD_ASSERT((int)HPM_TRGM_POS_SOURCE_MMC1_POS1 == (int)trgm_pos_matrix_in_from_mmc1_pos1);

struct hpm_trgm_config {
	TRGM_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
};

struct hpm_trgm_data {
	struct k_spinlock lock;
};

static bool hpm_trgm_filter_mode_is_valid(enum hpm_trgm_filter_mode mode)
{
	return (mode == HPM_TRGM_FILTER_BYPASS) || (mode == HPM_TRGM_FILTER_RAPID_CHANGE) ||
	       (mode == HPM_TRGM_FILTER_DELAY) || (mode == HPM_TRGM_FILTER_STABLE_LOW) ||
	       (mode == HPM_TRGM_FILTER_STABLE_HIGH);
}

static bool hpm_trgm_output_type_is_valid(enum hpm_trgm_output_type type)
{
	return type <= HPM_TRGM_OUTPUT_BOTH_EDGE_PULSE;
}

static trgm_output_type_t hpm_trgm_output_type_to_sdk(enum hpm_trgm_output_type type)
{
	switch (type) {
	case HPM_TRGM_OUTPUT_SAME_AS_INPUT:
		return trgm_output_same_as_input;
	case HPM_TRGM_OUTPUT_FALLING_EDGE_PULSE:
		return trgm_output_pulse_at_input_falling_edge;
	case HPM_TRGM_OUTPUT_RISING_EDGE_PULSE:
		return trgm_output_pulse_at_input_rising_edge;
	case HPM_TRGM_OUTPUT_BOTH_EDGE_PULSE:
		return trgm_output_pulse_at_input_both_edge;
	default:
		return trgm_output_same_as_input;
	}
}

static enum hpm_trgm_output_type hpm_trgm_output_type_from_register(uint32_t value)
{
	uint32_t edge = value & (TRGM_TRGOCFG_FEDG2PEN_MASK | TRGM_TRGOCFG_REDG2PEN_MASK);

	if (edge == TRGM_TRGOCFG_FEDG2PEN_MASK) {
		return HPM_TRGM_OUTPUT_FALLING_EDGE_PULSE;
	}
	if (edge == TRGM_TRGOCFG_REDG2PEN_MASK) {
		return HPM_TRGM_OUTPUT_RISING_EDGE_PULSE;
	}
	if (edge == (TRGM_TRGOCFG_FEDG2PEN_MASK | TRGM_TRGOCFG_REDG2PEN_MASK)) {
		return HPM_TRGM_OUTPUT_BOTH_EDGE_PULSE;
	}

	return HPM_TRGM_OUTPUT_SAME_AS_INPUT;
}

static int hpm_trgm_set(const struct device *dev, const struct mux_control *control, uint32_t state)
{
	const struct hpm_trgm_config *config = dev->config;
	struct hpm_trgm_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t output;

	if ((control == NULL) || (control->len != 1U)) {
		return -EINVAL;
	}

	output = control->cells[0];
	if (output >= HPM_TRGM_OUTPUT_COUNT) {
		return -EINVAL;
	}

	if (state > TRGM_TRGOCFG_TRIGOSEL_MASK) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	trgm_output_update_source(config->base, (uint8_t)output, (uint8_t)state);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_trgm_get_state(const struct device *dev, const struct mux_control *control,
			      uint32_t *state)
{
	const struct hpm_trgm_config *config = dev->config;
	uint32_t output;

	if ((control == NULL) || (control->len != 1U) || (state == NULL)) {
		return -EINVAL;
	}

	output = control->cells[0];
	if (output >= HPM_TRGM_OUTPUT_COUNT) {
		return -EINVAL;
	}

	*state = config->base->TRGOCFG[output] & TRGM_TRGOCFG_TRIGOSEL_MASK;

	return 0;
}

static int hpm_trgm_drv_configure_filter(const struct device *dev, uint8_t filter,
					 const struct hpm_trgm_filter_config *config)
{
	const struct hpm_trgm_config *trgm = dev->config;
	struct hpm_trgm_data *data = dev->data;
	trgm_filter_t sdk_config;
	k_spinlock_key_t key;

	if ((filter >= HPM_TRGM_FILTER_COUNT) || (config == NULL) ||
	    !hpm_trgm_filter_mode_is_valid(config->mode) ||
	    (config->filter_length > HPM_TRGM_FILTER_LENGTH_MAX)) {
		return -EINVAL;
	}

	sdk_config.invert = config->invert;
	sdk_config.sync = config->sync;
	sdk_config.filter_length = config->filter_length;
	sdk_config.mode = (trgm_filter_mode_t)config->mode;

	key = k_spin_lock(&data->lock);
	trgm_filter_config(trgm->base, filter, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_trgm_drv_get_filter(const struct device *dev, uint8_t filter,
				   struct hpm_trgm_filter_config *config)
{
	const struct hpm_trgm_config *trgm = dev->config;
	uint32_t value;

	if ((filter >= HPM_TRGM_FILTER_COUNT) || (config == NULL)) {
		return -EINVAL;
	}

	value = trgm->base->FILTCFG[filter];
	config->invert = TRGM_FILTCFG_OUTINV_GET(value) != 0U;
	config->mode = (enum hpm_trgm_filter_mode)TRGM_FILTCFG_MODE_GET(value);
	config->sync = TRGM_FILTCFG_SYNCEN_GET(value) != 0U;
#if defined(TRGM_SOC_HAS_FILTER_SHIFT) && TRGM_SOC_HAS_FILTER_SHIFT
	config->filter_length = TRGM_FILTCFG_FILTLEN_BASE_GET(value)
				<< TRGM_FILTCFG_FILTLEN_SHIFT_GET(value);
#else
	config->filter_length = TRGM_FILTCFG_FILTLEN_GET(value);
#endif

	return 0;
}

static int hpm_trgm_drv_configure_output(const struct device *dev, uint8_t output,
					 const struct hpm_trgm_output_config *config)
{
	const struct hpm_trgm_config *trgm = dev->config;
	struct hpm_trgm_data *data = dev->data;
	trgm_output_t sdk_config;
	k_spinlock_key_t key;

	if ((output >= HPM_TRGM_OUTPUT_COUNT) || (config == NULL) ||
	    (config->source > TRGM_TRGOCFG_TRIGOSEL_MASK) ||
	    !hpm_trgm_output_type_is_valid(config->type)) {
		return -EINVAL;
	}

	sdk_config.input = config->source;
	sdk_config.invert = config->invert;
	sdk_config.type = hpm_trgm_output_type_to_sdk(config->type);

	key = k_spin_lock(&data->lock);
	trgm_output_config(trgm->base, output, &sdk_config);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_trgm_drv_get_output(const struct device *dev, uint8_t output,
				   struct hpm_trgm_output_config *config)
{
	const struct hpm_trgm_config *trgm = dev->config;
	uint32_t value;

	if ((output >= HPM_TRGM_OUTPUT_COUNT) || (config == NULL)) {
		return -EINVAL;
	}

	value = trgm->base->TRGOCFG[output];
	config->source = TRGM_TRGOCFG_TRIGOSEL_GET(value);
	config->invert = TRGM_TRGOCFG_OUTINV_GET(value) != 0U;
	config->type = hpm_trgm_output_type_from_register(value);

	return 0;
}

static int hpm_trgm_drv_configure_dma_request(const struct device *dev, uint8_t request,
					      uint8_t source)
{
	const struct hpm_trgm_config *trgm = dev->config;
	struct hpm_trgm_data *data = dev->data;
	k_spinlock_key_t key;

	if ((request >= HPM_TRGM_DMA_REQUEST_COUNT) || (source > HPM_TRGM0_DMA_SRC_TRGM1)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	trgm_dma_request_config(trgm->base, request, source);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_trgm_drv_get_dma_request(const struct device *dev, uint8_t request, uint8_t *source)
{
	const struct hpm_trgm_config *trgm = dev->config;

	if ((request >= HPM_TRGM_DMA_REQUEST_COUNT) || (source == NULL)) {
		return -EINVAL;
	}

	*source = TRGM_DMACFG_DMASRCSEL_GET(trgm->base->DMACFG[request]);

	return 0;
}

static int hpm_trgm_drv_configure_adc_matrix(const struct device *dev,
					     enum hpm_trgm_adc_consumer consumer,
					     const struct hpm_trgm_matrix_config *config)
{
	const struct hpm_trgm_config *trgm = dev->config;
	struct hpm_trgm_data *data = dev->data;
	k_spinlock_key_t key;

	if (((unsigned int)consumer >= HPM_TRGM_ADC_CONSUMER_COUNT) || (config == NULL) ||
	    (config->source >= HPM_TRGM_ADC_SOURCE_COUNT)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	trgm_adc_matrix_config(trgm->base, (trgm_adc_matrix_out_t)consumer,
			       (trgm_adc_matrix_in_t)config->source, config->invert);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_trgm_drv_get_adc_matrix(const struct device *dev,
				       enum hpm_trgm_adc_consumer consumer,
				       struct hpm_trgm_matrix_config *config)
{
	const struct hpm_trgm_config *trgm = dev->config;
	uint32_t shift;
	uint32_t value;

	if (((unsigned int)consumer >= HPM_TRGM_ADC_CONSUMER_COUNT) || (config == NULL)) {
		return -EINVAL;
	}

	shift = (uint32_t)consumer * 8U;
	value = (trgm->base->ADC_MATRIX_SEL >> shift) & 0xFFU;
	config->source = value & 0x7FU;
	config->invert = (value & 0x80U) != 0U;

	return 0;
}

static int hpm_trgm_drv_configure_dac_matrix(const struct device *dev,
					     enum hpm_trgm_dac_consumer consumer,
					     const struct hpm_trgm_matrix_config *config)
{
	const struct hpm_trgm_config *trgm = dev->config;
	struct hpm_trgm_data *data = dev->data;
	k_spinlock_key_t key;

	if (((unsigned int)consumer >= HPM_TRGM_DAC_CONSUMER_COUNT) || (config == NULL) ||
	    (config->source >= HPM_TRGM_DAC_SOURCE_COUNT)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	trgm_dac_matrix_config(trgm->base, (trgm_dac_matrix_out_t)consumer,
			       (trgm_dac_matrix_in_t)config->source, config->invert);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_trgm_drv_get_dac_matrix(const struct device *dev,
				       enum hpm_trgm_dac_consumer consumer,
				       struct hpm_trgm_matrix_config *config)
{
	const struct hpm_trgm_config *trgm = dev->config;
	uint32_t shift;
	uint32_t value;

	if (((unsigned int)consumer >= HPM_TRGM_DAC_CONSUMER_COUNT) || (config == NULL)) {
		return -EINVAL;
	}

	shift = (uint32_t)consumer * 8U;
	value = (trgm->base->DAC_MATRIX_SEL >> shift) & 0xFFU;
	config->source = value & 0x7FU;
	config->invert = (value & 0x80U) != 0U;

	return 0;
}

static int hpm_trgm_drv_configure_pos_matrix(const struct device *dev,
					     enum hpm_trgm_pos_consumer consumer,
					     const struct hpm_trgm_matrix_config *config)
{
	const struct hpm_trgm_config *trgm = dev->config;
	struct hpm_trgm_data *data = dev->data;
	k_spinlock_key_t key;

	if (((unsigned int)consumer >= HPM_TRGM_POS_CONSUMER_COUNT) || (config == NULL) ||
	    (config->source >= HPM_TRGM_POS_SOURCE_COUNT)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	trgm_pos_matrix_config(trgm->base, (trgm_pos_matrix_out_t)consumer,
			       (trgm_pos_matrix_in_t)config->source, config->invert);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_trgm_drv_get_pos_matrix(const struct device *dev,
				       enum hpm_trgm_pos_consumer consumer,
				       struct hpm_trgm_matrix_config *config)
{
	const struct hpm_trgm_config *trgm = dev->config;
	uint32_t shift;
	uint32_t value;

	if (((unsigned int)consumer >= HPM_TRGM_POS_CONSUMER_COUNT) || (config == NULL)) {
		return -EINVAL;
	}

	if ((uint32_t)consumer < 4U) {
		shift = (uint32_t)consumer * 8U;
		value = (trgm->base->POS_MATRIX_SEL0 >> shift) & 0xFFU;
	} else {
		shift = ((uint32_t)consumer - 4U) * 8U;
		value = (trgm->base->POS_MATRIX_SEL1 >> shift) & 0xFFU;
	}
	config->source = value & 0x7FU;
	config->invert = (value & 0x80U) != 0U;

	return 0;
}

static int hpm_trgm_init(const struct device *dev)
{
	const struct hpm_trgm_config *config = dev->config;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	return clock_control_on(config->clock_dev, config->clock_subsys);
}

static DEVICE_API(hpm_trgm, hpm_trgm_api) = {
	.mux_api =
		{
			.set = hpm_trgm_set,
			.get_state = hpm_trgm_get_state,
		},
	.configure_filter = hpm_trgm_drv_configure_filter,
	.get_filter = hpm_trgm_drv_get_filter,
	.configure_output = hpm_trgm_drv_configure_output,
	.get_output = hpm_trgm_drv_get_output,
	.configure_dma_request = hpm_trgm_drv_configure_dma_request,
	.get_dma_request = hpm_trgm_drv_get_dma_request,
	.configure_adc_matrix = hpm_trgm_drv_configure_adc_matrix,
	.get_adc_matrix = hpm_trgm_drv_get_adc_matrix,
	.configure_dac_matrix = hpm_trgm_drv_configure_dac_matrix,
	.get_dac_matrix = hpm_trgm_drv_get_dac_matrix,
	.configure_pos_matrix = hpm_trgm_drv_configure_pos_matrix,
	.get_pos_matrix = hpm_trgm_drv_get_pos_matrix,
};

#define HPM_TRGM_INIT(n)                                                                           \
	static struct hpm_trgm_data hpm_trgm_data_##n;                                             \
                                                                                                   \
	static const struct hpm_trgm_config hpm_trgm_config_##n = {                                \
		.base = (TRGM_Type *)DT_INST_REG_ADDR(n),                                          \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_trgm_init, NULL, &hpm_trgm_data_##n, &hpm_trgm_config_##n,    \
			      POST_KERNEL, CONFIG_MUX_INIT_PRIORITY, &hpm_trgm_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_TRGM_INIT)
