/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_DAC_HPMICRO_DAC_H_
#define ZEPHYR_INCLUDE_DRIVERS_DAC_HPMICRO_DAC_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro DAC12 extensions
 * @defgroup hpm_dac_interface HPMicro DAC
 * @ingroup dac_interface_ext
 * @{
 */

#define HPM_DAC_MAX_VALUE      4095U
#define HPM_DAC_STEP_COUNT     4U
#define HPM_DAC_BUFFER_COUNT   2U
#define HPM_DAC_MAX_BUFFER_LEN 65536U
#define HPM_DAC_DMA_NONE       0U
#define HPM_DAC_DMA_BUF0       BIT(0)
#define HPM_DAC_DMA_BUF1       BIT(1)

/** DAC operating mode, matching the fixed HPM SDK dac_mode_t values. */
enum hpm_dac_mode {
	HPM_DAC_MODE_DIRECT = 0,
	HPM_DAC_MODE_STEP,
	HPM_DAC_MODE_BUFFER,
	HPM_DAC_MODE_TRIGGER,
};

/** DAC step direction. */
enum hpm_dac_step_direction {
	HPM_DAC_STEP_UP = 0,
	HPM_DAC_STEP_DOWN,
};

/** DAC step completion behavior. */
enum hpm_dac_step_round_mode {
	HPM_DAC_STEP_ONESHOT = 0,
	HPM_DAC_STEP_LOOP,
};

/** DAC buffer sample layout. */
enum hpm_dac_buffer_data_mode {
	/** Each 32-bit word contains two 12-bit samples. */
	HPM_DAC_BUFFER_TWO_POINT = 0,
	/** Each 32-bit word contains one 12-bit sample. */
	HPM_DAC_BUFFER_ONE_POINT,
};

/** DAC buffer hardware burst length. */
enum hpm_dac_buffer_burst {
	HPM_DAC_BUFFER_BURST_SINGLE = 0,
	HPM_DAC_BUFFER_BURST_INCR4 = 3,
	HPM_DAC_BUFFER_BURST_INCR8 = 5,
};

struct hpm_dac_step_config {
	enum hpm_dac_step_direction direction;
	enum hpm_dac_step_round_mode round_mode;
	uint16_t start;
	uint16_t end;
	uint8_t step;
};

struct hpm_dac_buffer_config {
	enum hpm_dac_buffer_data_mode data_mode;
	enum hpm_dac_buffer_burst burst;
	const uint32_t *buffer0;
	uint16_t buffer0_len;
	const uint32_t *buffer1;
	uint16_t buffer1_len;
	uint8_t stop0;
	uint8_t stop1;
	/** HPM_DAC_DMA_BUF0 and/or HPM_DAC_DMA_BUF1. */
	uint8_t dma_mask;
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_dac_driver_api {
	/** Must remain first so the device also implements the DAC API. */
	struct dac_driver_api dac_api;
	int (*set_mode)(const struct device *dev, uint8_t channel, enum hpm_dac_mode mode);
	int (*configure_step)(const struct device *dev, uint8_t step_index,
			      const struct hpm_dac_step_config *config);
	int (*trigger_step)(const struct device *dev, uint8_t step_index);
	int (*configure_buffer)(const struct device *dev,
				const struct hpm_dac_buffer_config *config);
	int (*trigger_buffer)(const struct device *dev);
	int (*set_hardware_trigger)(const struct device *dev, bool enable);
	int (*get_status)(const struct device *dev, uint32_t *status);
	int (*clear_status)(const struct device *dev, uint32_t status);
};

DEVICE_API_EXTENDS(hpm_dac, dac, dac_api);
/** @endcond */

/**
 * @brief Reinitialize the DAC in direct, step, buffer or trigger mode.
 *
 * @param dev DAC device.
 * @param channel Channel identifier; only channel 0 is implemented by HPM5361.
 * @param mode DAC operating mode.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid channel or mode.
 */
static inline int hpm_dac_set_mode(const struct device *dev, uint8_t channel,
				   enum hpm_dac_mode mode)
{
	return DEVICE_API_GET(hpm_dac, dev)->set_mode(dev, channel, mode);
}

/**
 * @brief Configure one of the four DAC step generators.
 *
 * This API is available in step mode. Set @p step_index to 0 through 3.
 */
static inline int hpm_dac_configure_step(const struct device *dev, uint8_t step_index,
					 const struct hpm_dac_step_config *config)
{
	return DEVICE_API_GET(hpm_dac, dev)->configure_step(dev, step_index, config);
}

/** @brief Issue a software trigger for a configured step generator. */
static inline int hpm_dac_trigger_step(const struct device *dev, uint8_t step_index)
{
	return DEVICE_API_GET(hpm_dac, dev)->trigger_step(dev, step_index);
}

/**
 * @brief Configure the DAC internal buffer engine.
 *
 * This API is available in buffer mode. Both ping-pong buffers must be present,
 * 32-byte aligned, non-overlapping and remain valid while the engine is active.
 * HPM5361 uses a 16-bit sample count, which includes a hardware "minus one"
 * encoding.
 */
static inline int hpm_dac_configure_buffer(const struct device *dev,
					   const struct hpm_dac_buffer_config *config)
{
	return DEVICE_API_GET(hpm_dac, dev)->configure_buffer(dev, config);
}

/** @brief Issue a software trigger for the configured buffer engine. */
static inline int hpm_dac_trigger_buffer(const struct device *dev)
{
	return DEVICE_API_GET(hpm_dac, dev)->trigger_buffer(dev);
}

/** @brief Enable or disable the external hardware trigger input. */
static inline int hpm_dac_set_hardware_trigger(const struct device *dev, bool enable)
{
	return DEVICE_API_GET(hpm_dac, dev)->set_hardware_trigger(dev, enable);
}

/** @brief Read and latch the DAC status flags. */
static inline int hpm_dac_get_status(const struct device *dev, uint32_t *status)
{
	return DEVICE_API_GET(hpm_dac, dev)->get_status(dev, status);
}

/** @brief Clear write-one-to-clear DAC status flags. */
static inline int hpm_dac_clear_status(const struct device *dev, uint32_t status)
{
	return DEVICE_API_GET(hpm_dac, dev)->clear_status(dev, status);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_DAC_HPMICRO_DAC_H_ */
