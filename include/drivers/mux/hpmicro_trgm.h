/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MUX_HPMICRO_TRGM_H_
#define ZEPHYR_INCLUDE_DRIVERS_MUX_HPMICRO_TRGM_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/mux.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro Trigger Manager extensions
 * @defgroup hpm_trgm_interface HPMicro TRGM
 * @ingroup mux_interface
 * @{
 */

#define HPM_TRGM_FILTER_COUNT       28U
#define HPM_TRGM_OUTPUT_COUNT       137U
#define HPM_TRGM_DMA_REQUEST_COUNT  8U
#define HPM_TRGM_ADC_CONSUMER_COUNT 4U
#define HPM_TRGM_ADC_SOURCE_COUNT   4U
#define HPM_TRGM_DAC_CONSUMER_COUNT 4U
#define HPM_TRGM_DAC_SOURCE_COUNT   8U
#define HPM_TRGM_POS_CONSUMER_COUNT 6U
#define HPM_TRGM_POS_SOURCE_COUNT   8U

/** TRGM output indices for motor-control inter-IP routes. */
#define HPM_TRGM_OUTPUT_QEO0_TRIG_IN0 48U
#define HPM_TRGM_OUTPUT_QEO0_TRIG_IN1 49U
#define HPM_TRGM_OUTPUT_QEO1_TRIG_IN0 50U
#define HPM_TRGM_OUTPUT_QEO1_TRIG_IN1 51U
#define HPM_TRGM_OUTPUT_SEI_TRIG_IN0  52U
#define HPM_TRGM_OUTPUT_SEI_TRIG_IN1  53U
#define HPM_TRGM_OUTPUT_SEI_TRIG_IN2  54U
#define HPM_TRGM_OUTPUT_SEI_TRIG_IN3  55U
#define HPM_TRGM_OUTPUT_SEI_TRIG_IN4  56U
#define HPM_TRGM_OUTPUT_SEI_TRIG_IN5  57U
#define HPM_TRGM_OUTPUT_SEI_TRIG_IN6  58U
#define HPM_TRGM_OUTPUT_SEI_TRIG_IN7  59U
#define HPM_TRGM_OUTPUT_MMC0_TRIG_IN0 60U
#define HPM_TRGM_OUTPUT_MMC0_TRIG_IN1 61U
#define HPM_TRGM_OUTPUT_MMC1_TRIG_IN0 62U
#define HPM_TRGM_OUTPUT_MMC1_TRIG_IN1 63U
#define HPM_TRGM_OUTPUT_PLB_IN_00     64U
#define HPM_TRGM_OUTPUT_PLB_IN_31     95U
#define HPM_TRGM_OUTPUT_RDC_TRIG_IN0  124U
#define HPM_TRGM_OUTPUT_RDC_TRIG_IN1  125U
#define HPM_TRGM_OUTPUT_SYNT_TRIG     126U

/** TRGM input indices for PWM reference and ADC trigger outputs. */
#define HPM_TRGM_INPUT_PWM0_CH8REF   48U
#define HPM_TRGM_INPUT_PWM0_CH15REF  55U
#define HPM_TRGM_INPUT_PWM1_CH8REF   56U
#define HPM_TRGM_INPUT_PWM1_CH15REF  63U

/** TRGM output indices for ADC sequence and preemption triggers. */
#define HPM_TRGM_OUTPUT_ADC0_STRGI    32U
#define HPM_TRGM_OUTPUT_ADC1_STRGI    33U
#define HPM_TRGM_OUTPUT_ADCX_PTRGI0A  34U
#define HPM_TRGM_OUTPUT_ADCX_PTRGI0B  35U
#define HPM_TRGM_OUTPUT_ADCX_PTRGI0C  36U
#define HPM_TRGM_OUTPUT_ADCX_PTRGI1A  37U
#define HPM_TRGM_OUTPUT_ADCX_PTRGI1B  38U
#define HPM_TRGM_OUTPUT_ADCX_PTRGI1C  39U
#define HPM_TRGM_OUTPUT_ADCX_PTRGI2A  40U
#define HPM_TRGM_OUTPUT_ADCX_PTRGI2B  41U
#define HPM_TRGM_OUTPUT_ADCX_PTRGI2C  42U
#define HPM_TRGM_OUTPUT_ADCX_PTRGI3A  43U
#define HPM_TRGM_OUTPUT_ADCX_PTRGI3B  44U
#define HPM_TRGM_OUTPUT_ADCX_PTRGI3C  45U

/** TRGM input indices for motor-control source signals. */
#define HPM_TRGM_INPUT_ACMP0_OUT     6U
#define HPM_TRGM_INPUT_ACMP1_OUT     7U
#define HPM_TRGM_INPUT_SYNT0_CH0     24U
#define HPM_TRGM_INPUT_SYNT0_CH1     25U
#define HPM_TRGM_INPUT_SYNT0_CH2     26U
#define HPM_TRGM_INPUT_SYNT0_CH3     27U
#define HPM_TRGM_INPUT_MMC0_TRGO_0   28U
#define HPM_TRGM_INPUT_MMC0_TRGO_1   29U
#define HPM_TRGM_INPUT_MMC1_TRGO_0   30U
#define HPM_TRGM_INPUT_MMC1_TRGO_1   31U
#define HPM_TRGM_INPUT_QEO0_TRGO_0   32U
#define HPM_TRGM_INPUT_QEO0_TRGO_1   33U
#define HPM_TRGM_INPUT_QEO0_TRGO_2   34U
#define HPM_TRGM_INPUT_QEO0_TRGO_3   35U
#define HPM_TRGM_INPUT_QEO0_TRGO_4   36U
#define HPM_TRGM_INPUT_QEO0_TRGO_5   37U
#define HPM_TRGM_INPUT_QEO0_TRGO_6   38U
#define HPM_TRGM_INPUT_QEO0_TRGO_7   39U
#define HPM_TRGM_INPUT_QEO1_TRGO_0   40U
#define HPM_TRGM_INPUT_QEO1_TRGO_1   41U
#define HPM_TRGM_INPUT_QEO1_TRGO_2   42U
#define HPM_TRGM_INPUT_QEO1_TRGO_3   43U
#define HPM_TRGM_INPUT_QEO1_TRGO_4   44U
#define HPM_TRGM_INPUT_QEO1_TRGO_5   45U
#define HPM_TRGM_INPUT_QEO1_TRGO_6   46U
#define HPM_TRGM_INPUT_QEO1_TRGO_7   47U
#define HPM_TRGM_INPUT_PLB_OUT00     64U
#define HPM_TRGM_INPUT_PLB_OUT31     95U
#define HPM_TRGM_INPUT_RDC_TRGO_0    96U
#define HPM_TRGM_INPUT_RDC_TRGO_1    97U
#define HPM_TRGM_INPUT_SEI_TRGO_0    100U
#define HPM_TRGM_INPUT_SEI_TRGO_1    101U
#define HPM_TRGM_INPUT_SEI_TRGO_2    102U
#define HPM_TRGM_INPUT_SEI_TRGO_3    103U
#define HPM_TRGM_INPUT_SEI_TRGO_4    104U
#define HPM_TRGM_INPUT_SEI_TRGO_5    105U
#define HPM_TRGM_INPUT_SEI_TRGO_6    106U
#define HPM_TRGM_INPUT_SEI_TRGO_7    107U

/** Largest filter length representable by FILTLEN_BASE/FILTLEN_SHIFT. */
#define HPM_TRGM_FILTER_LENGTH_MAX 65408U

/** Output indices reserved for the two TRGM interrupt request signals. */
#define HPM_TRGM_OUTPUT_IRQ0 133U
#define HPM_TRGM_OUTPUT_IRQ1 134U
/** Output indices reserved for the two TRGM DMA request signals. */
#define HPM_TRGM_OUTPUT_DMA0 135U
#define HPM_TRGM_OUTPUT_DMA1 136U

/** TRGM input filter mode. Values match the HPM5361 FILTCFG.MODE field. */
enum hpm_trgm_filter_mode {
	HPM_TRGM_FILTER_BYPASS = 0,
	HPM_TRGM_FILTER_RAPID_CHANGE = 4,
	HPM_TRGM_FILTER_DELAY = 5,
	HPM_TRGM_FILTER_STABLE_LOW = 6,
	HPM_TRGM_FILTER_STABLE_HIGH = 7,
};

/** Output edge conversion. */
enum hpm_trgm_output_type {
	HPM_TRGM_OUTPUT_SAME_AS_INPUT = 0,
	HPM_TRGM_OUTPUT_FALLING_EDGE_PULSE,
	HPM_TRGM_OUTPUT_RISING_EDGE_PULSE,
	HPM_TRGM_OUTPUT_BOTH_EDGE_PULSE,
};

/** Common motor-control trigger route. */
enum hpm_trgm_motor_route {
	HPM_TRGM_MOTOR_ROUTE_SYNT_TO_QEO0_SAFETY,
	HPM_TRGM_MOTOR_ROUTE_MMC0_TO_QEO0,
	HPM_TRGM_MOTOR_ROUTE_MMC1_TO_QEO1,
	HPM_TRGM_MOTOR_ROUTE_SEI_TO_MMC0,
	HPM_TRGM_MOTOR_ROUTE_SEI_TO_MMC1,
	HPM_TRGM_MOTOR_ROUTE_MMC0_TO_RDC,
	HPM_TRGM_MOTOR_ROUTE_PLB_TO_QEO0,
	HPM_TRGM_MOTOR_ROUTE_PLB_TO_QEO1,
	HPM_TRGM_MOTOR_ROUTE_COUNT,
};

struct hpm_trgm_motor_route_config {
	/** Source selection, from the corresponding source enum or SDK macro. */
	uint8_t source;
	/** Invert the selected trigger. */
	bool invert;
	/** Convert the selected signal to a one-cycle pulse on both edges. */
	bool edge_pulse;
};

/** ADC matrix consumers. Values match the HPM5361 ADC_MATRIX_SEL fields. */
enum hpm_trgm_adc_consumer {
	HPM_TRGM_ADC_CONSUMER_QEI0_ADC0 = 0,
	HPM_TRGM_ADC_CONSUMER_QEI0_ADC1,
	HPM_TRGM_ADC_CONSUMER_QEI1_ADC0,
	HPM_TRGM_ADC_CONSUMER_QEI1_ADC1,
};

/** ADC matrix sources. Values match the HPM5361 ADC_MATRIX_SEL fields. */
enum hpm_trgm_adc_source {
	HPM_TRGM_ADC_SOURCE_ADC0 = 0,
	HPM_TRGM_ADC_SOURCE_ADC1,
	HPM_TRGM_ADC_SOURCE_RDC_ADC0,
	HPM_TRGM_ADC_SOURCE_RDC_ADC1,
};

/** DAC matrix consumers. Values match the HPM5361 DAC_MATRIX_SEL fields. */
enum hpm_trgm_dac_consumer {
	HPM_TRGM_DAC_CONSUMER_ACMP0 = 0,
	HPM_TRGM_DAC_CONSUMER_ACMP1,
	HPM_TRGM_DAC_CONSUMER_DAC0,
	HPM_TRGM_DAC_CONSUMER_DAC1,
};

/** DAC matrix sources. Values match the HPM5361 DAC_MATRIX_SEL fields. */
enum hpm_trgm_dac_source {
	HPM_TRGM_DAC_SOURCE_QEO0_DAC0 = 0,
	HPM_TRGM_DAC_SOURCE_QEO0_DAC1,
	HPM_TRGM_DAC_SOURCE_QEO0_DAC2,
	HPM_TRGM_DAC_SOURCE_QEO1_DAC0,
	HPM_TRGM_DAC_SOURCE_QEO1_DAC1,
	HPM_TRGM_DAC_SOURCE_QEO1_DAC2,
	HPM_TRGM_DAC_SOURCE_RDC_DAC0,
	HPM_TRGM_DAC_SOURCE_RDC_DAC1,
};

/** Position matrix consumers. Values match the HPM5361 POS_MATRIX_SEL fields. */
enum hpm_trgm_pos_consumer {
	HPM_TRGM_POS_CONSUMER_SEI_POS0 = 0,
	HPM_TRGM_POS_CONSUMER_SEI_POS1,
	HPM_TRGM_POS_CONSUMER_MMC0,
	HPM_TRGM_POS_CONSUMER_MMC1,
	HPM_TRGM_POS_CONSUMER_QEO0,
	HPM_TRGM_POS_CONSUMER_QEO1,
};

/** Position matrix sources. Values match the HPM5361 POS_MATRIX_SEL fields. */
enum hpm_trgm_pos_source {
	HPM_TRGM_POS_SOURCE_SEI_POS0 = 0,
	HPM_TRGM_POS_SOURCE_SEI_POS1,
	HPM_TRGM_POS_SOURCE_QEI0,
	HPM_TRGM_POS_SOURCE_QEI1,
	HPM_TRGM_POS_SOURCE_MMC0_POS0,
	HPM_TRGM_POS_SOURCE_MMC0_POS1,
	HPM_TRGM_POS_SOURCE_MMC1_POS0,
	HPM_TRGM_POS_SOURCE_MMC1_POS1,
};

struct hpm_trgm_filter_config {
	enum hpm_trgm_filter_mode mode;
	/** Invert the filtered input before it enters the trigger matrix. */
	bool invert;
	/** Synchronize the input with the TRGM clock. */
	bool sync;
	/** Requested filter width in TRGM clock cycles. */
	uint32_t filter_length;
};

struct hpm_trgm_output_config {
	/** TRGM input source, using an HPM_TRGM0_INPUT_SRC_* value. */
	uint8_t source;
	enum hpm_trgm_output_type type;
	/** Invert the output after edge conversion. */
	bool invert;
};

struct hpm_trgm_matrix_config {
	uint8_t source;
	bool invert;
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_trgm_driver_api {
	/** Must remain first so the device also implements the mux_control API. */
	struct mux_control_driver_api mux_api;
	int (*configure_filter)(const struct device *dev, uint8_t filter,
				const struct hpm_trgm_filter_config *config);
	int (*get_filter)(const struct device *dev, uint8_t filter,
			  struct hpm_trgm_filter_config *config);
	int (*configure_output)(const struct device *dev, uint8_t output,
				const struct hpm_trgm_output_config *config);
	int (*get_output)(const struct device *dev, uint8_t output,
			  struct hpm_trgm_output_config *config);
	int (*configure_dma_request)(const struct device *dev, uint8_t request, uint8_t source);
	int (*get_dma_request)(const struct device *dev, uint8_t request, uint8_t *source);
	int (*configure_adc_matrix)(const struct device *dev, enum hpm_trgm_adc_consumer consumer,
				    const struct hpm_trgm_matrix_config *config);
	int (*get_adc_matrix)(const struct device *dev, enum hpm_trgm_adc_consumer consumer,
			      struct hpm_trgm_matrix_config *config);
	int (*configure_dac_matrix)(const struct device *dev, enum hpm_trgm_dac_consumer consumer,
				    const struct hpm_trgm_matrix_config *config);
	int (*get_dac_matrix)(const struct device *dev, enum hpm_trgm_dac_consumer consumer,
			      struct hpm_trgm_matrix_config *config);
	int (*configure_pos_matrix)(const struct device *dev, enum hpm_trgm_pos_consumer consumer,
				    const struct hpm_trgm_matrix_config *config);
	int (*get_pos_matrix)(const struct device *dev, enum hpm_trgm_pos_consumer consumer,
			      struct hpm_trgm_matrix_config *config);
	int (*configure_motor_route)(const struct device *dev, enum hpm_trgm_motor_route route,
				     const struct hpm_trgm_motor_route_config *config);
};

DEVICE_API_EXTENDS(hpm_trgm, mux_control, mux_api);
/** @endcond */

/**
 * @brief Configure one TRGM input digital filter.
 *
 * The SDK converts @ref hpm_trgm_filter_config.filter_length into the closest
 * representable FILTLEN_BASE and FILTLEN_SHIFT value, rounding down.
 *
 * @param dev TRGM device.
 * @param filter Filter index, from 0 through HPM_TRGM_FILTER_COUNT - 1.
 * @param config Filter configuration.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid filter, mode or length.
 */
static inline int hpm_trgm_configure_filter(const struct device *dev, uint8_t filter,
					    const struct hpm_trgm_filter_config *config)
{
	return DEVICE_API_GET(hpm_trgm, dev)->configure_filter(dev, filter, config);
}

/**
 * @brief Read back one TRGM input digital filter.
 *
 * @param dev TRGM device.
 * @param filter Filter index.
 * @param config Output filter configuration.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid filter or null output pointer.
 */
static inline int hpm_trgm_get_filter(const struct device *dev, uint8_t filter,
				      struct hpm_trgm_filter_config *config)
{
	return DEVICE_API_GET(hpm_trgm, dev)->get_filter(dev, filter, config);
}

/**
 * @brief Configure a TRGM output source, polarity and edge conversion.
 *
 * Outputs HPM_TRGM_OUTPUT_IRQ0 and HPM_TRGM_OUTPUT_IRQ1 route a selected input
 * to the two hardware interrupt signals. Outputs HPM_TRGM_OUTPUT_DMA0 and
 * HPM_TRGM_OUTPUT_DMA1 route a selected input to the two TRGM DMA signals.
 *
 * @param dev TRGM device.
 * @param output Output index, from 0 through HPM_TRGM_OUTPUT_COUNT - 1.
 * @param config Output configuration.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid output, source or output type.
 */
static inline int hpm_trgm_configure_output(const struct device *dev, uint8_t output,
					    const struct hpm_trgm_output_config *config)
{
	return DEVICE_API_GET(hpm_trgm, dev)->configure_output(dev, output, config);
}

/**
 * @brief Read back a TRGM output configuration.
 *
 * @param dev TRGM device.
 * @param output Output index.
 * @param config Output configuration.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid output or null output pointer.
 */
static inline int hpm_trgm_get_output(const struct device *dev, uint8_t output,
				      struct hpm_trgm_output_config *config)
{
	return DEVICE_API_GET(hpm_trgm, dev)->get_output(dev, output, config);
}

/**
 * @brief Route one TRGM DMA request through DMAMUX.
 *
 * @param dev TRGM device.
 * @param request DMA request output, from 0 through HPM_TRGM_DMA_REQUEST_COUNT - 1.
 * @param source DMA source, using an HPM_TRGM0_DMA_SRC_* value.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid request or source.
 */
static inline int hpm_trgm_configure_dma_request(const struct device *dev, uint8_t request,
						 uint8_t source)
{
	return DEVICE_API_GET(hpm_trgm, dev)->configure_dma_request(dev, request, source);
}

/**
 * @brief Read back a TRGM DMA request source.
 *
 * @param dev TRGM device.
 * @param request DMA request output.
 * @param source Output DMA source.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid request or null output pointer.
 */
static inline int hpm_trgm_get_dma_request(const struct device *dev, uint8_t request,
					   uint8_t *source)
{
	return DEVICE_API_GET(hpm_trgm, dev)->get_dma_request(dev, request, source);
}

/** @brief Configure one ADC matrix consumer. */
static inline int hpm_trgm_configure_adc_matrix(const struct device *dev,
						enum hpm_trgm_adc_consumer consumer,
						const struct hpm_trgm_matrix_config *config)
{
	return DEVICE_API_GET(hpm_trgm, dev)->configure_adc_matrix(dev, consumer, config);
}

/** @brief Read back one ADC matrix consumer. */
static inline int hpm_trgm_get_adc_matrix(const struct device *dev,
					  enum hpm_trgm_adc_consumer consumer,
					  struct hpm_trgm_matrix_config *config)
{
	return DEVICE_API_GET(hpm_trgm, dev)->get_adc_matrix(dev, consumer, config);
}

/** @brief Configure one DAC matrix consumer. */
static inline int hpm_trgm_configure_dac_matrix(const struct device *dev,
						enum hpm_trgm_dac_consumer consumer,
						const struct hpm_trgm_matrix_config *config)
{
	return DEVICE_API_GET(hpm_trgm, dev)->configure_dac_matrix(dev, consumer, config);
}

/** @brief Read back one DAC matrix consumer. */
static inline int hpm_trgm_get_dac_matrix(const struct device *dev,
					  enum hpm_trgm_dac_consumer consumer,
					  struct hpm_trgm_matrix_config *config)
{
	return DEVICE_API_GET(hpm_trgm, dev)->get_dac_matrix(dev, consumer, config);
}

/** @brief Configure one position matrix consumer. */
static inline int hpm_trgm_configure_pos_matrix(const struct device *dev,
						enum hpm_trgm_pos_consumer consumer,
						const struct hpm_trgm_matrix_config *config)
{
	return DEVICE_API_GET(hpm_trgm, dev)->configure_pos_matrix(dev, consumer, config);
}

/** @brief Read back one position matrix consumer. */
static inline int hpm_trgm_get_pos_matrix(const struct device *dev,
					  enum hpm_trgm_pos_consumer consumer,
					  struct hpm_trgm_matrix_config *config)
{
	return DEVICE_API_GET(hpm_trgm, dev)->get_pos_matrix(dev, consumer, config);
}

/**
 * @brief Configure a common motor-control trigger route.
 *
 * The route is expressed in source/destination terms so consumers do not need
 * to duplicate TRGM output indices. The source numbering is the same as the
 * TRGM input list in the SoC manual. Position routes use the position matrix;
 * all other routes use TRGM outputs.
 *
 * @param dev TRGM device.
 * @param route Motor-control route.
 * @param config Source and signal processing.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid route or configuration.
 */
static inline int
hpm_trgm_configure_motor_route(const struct device *dev, enum hpm_trgm_motor_route route,
			       const struct hpm_trgm_motor_route_config *config)
{
	return DEVICE_API_GET(hpm_trgm, dev)->configure_motor_route(dev, route, config);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MUX_HPMICRO_TRGM_H_ */
