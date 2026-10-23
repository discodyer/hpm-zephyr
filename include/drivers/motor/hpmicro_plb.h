/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_PLB_H_
#define ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_PLB_H_

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro programmable logic block
 * @defgroup hpm_plb_interface HPMicro PLB
 * @ingroup io_interfaces
 * @{
 */

#define HPM_PLB_CHANNEL_COUNT           4U
#define HPM_PLB_TYPE_A_LUT_COUNT        4U
#define HPM_PLB_TYPE_B_SLICE_COUNT      16U
#define HPM_PLB_TYPE_B_COMPARATOR_COUNT 4U

/** TYPE_B data-unit operation selected by the 4-bit slice value. */
enum hpm_plb_slice_operation {
	HPM_PLB_SLICE_KEEP = 0,
	HPM_PLB_SLICE_LOAD_CMP0,
	HPM_PLB_SLICE_LOAD_CMP1,
	HPM_PLB_SLICE_LOAD_CMP2,
	HPM_PLB_SLICE_ADD_ONE,
	HPM_PLB_SLICE_ADD_TWO,
	HPM_PLB_SLICE_SUBTRACT_ONE,
	HPM_PLB_SLICE_SUBTRACT_TWO,
	HPM_PLB_SLICE_SHIFT_LEFT,
	HPM_PLB_SLICE_SHIFT_LEFT_OR_ONE,
	HPM_PLB_SLICE_SHIFT_RIGHT,
	HPM_PLB_SLICE_SHIFT_RIGHT_OR_ONE,
};

/** TYPE_B comparator output mode. */
enum hpm_plb_comparator_mode {
	HPM_PLB_COMPARATOR_ZERO = 0,
	HPM_PLB_COMPARATOR_ONE,
	HPM_PLB_COMPARATOR_GREATER,
	HPM_PLB_COMPARATOR_LESS,
	HPM_PLB_COMPARATOR_EQUAL,
	HPM_PLB_COMPARATOR_NOT_EQUAL,
	HPM_PLB_COMPARATOR_GREATER_OR_EQUAL,
	HPM_PLB_COMPARATOR_LESS_OR_EQUAL,
	HPM_PLB_COMPARATOR_AND_MASK = 10,
	HPM_PLB_COMPARATOR_OR_MASK,
	HPM_PLB_COMPARATOR_XOR_MASK,
	HPM_PLB_COMPARATOR_NAND_MASK,
	HPM_PLB_COMPARATOR_NOR_MASK,
	HPM_PLB_COMPARATOR_XNOR_MASK,
};

struct hpm_plb_type_a_config {
	/** Truth table for each of the four LUT outputs. */
	uint16_t truth[HPM_PLB_TYPE_A_LUT_COUNT];
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_plb_driver_api {
	int (*configure_type_a)(const struct device *dev, uint8_t channel,
				const struct hpm_plb_type_a_config *config);
	int (*inject_type_a)(const struct device *dev, uint8_t channel, uint8_t value);
	int (*set_type_b_slice)(const struct device *dev, uint8_t channel, uint8_t slice,
				enum hpm_plb_slice_operation operation);
	int (*set_type_b_slices)(const struct device *dev, uint8_t channel, uint16_t mask,
				 enum hpm_plb_slice_operation operation);
	int (*set_type_b_all_slices)(const struct device *dev, uint8_t channel,
				     enum hpm_plb_slice_operation operation);
	int (*set_type_b_comparator)(const struct device *dev, uint8_t channel, uint8_t comparator,
				     uint32_t value, enum hpm_plb_comparator_mode mode);
	int (*set_type_b_counter)(const struct device *dev, uint8_t channel, uint32_t value);
	int (*get_type_b_counter)(const struct device *dev, uint8_t channel, uint32_t *value);
};
/** @endcond */

static inline int hpm_plb_configure_type_a(const struct device *dev, uint8_t channel,
					   const struct hpm_plb_type_a_config *config)
{
	return DEVICE_API_GET(hpm_plb, dev)->configure_type_a(dev, channel, config);
}

static inline int hpm_plb_inject_type_a(const struct device *dev, uint8_t channel, uint8_t value)
{
	return DEVICE_API_GET(hpm_plb, dev)->inject_type_a(dev, channel, value);
}

static inline int hpm_plb_set_type_b_slice(const struct device *dev, uint8_t channel, uint8_t slice,
					   enum hpm_plb_slice_operation operation)
{
	return DEVICE_API_GET(hpm_plb, dev)->set_type_b_slice(dev, channel, slice, operation);
}

static inline int hpm_plb_set_type_b_slices(const struct device *dev, uint8_t channel,
					    uint16_t mask, enum hpm_plb_slice_operation operation)
{
	return DEVICE_API_GET(hpm_plb, dev)->set_type_b_slices(dev, channel, mask, operation);
}

static inline int hpm_plb_set_type_b_all_slices(const struct device *dev, uint8_t channel,
						enum hpm_plb_slice_operation operation)
{
	return DEVICE_API_GET(hpm_plb, dev)->set_type_b_all_slices(dev, channel, operation);
}

static inline int hpm_plb_set_type_b_comparator(const struct device *dev, uint8_t channel,
						uint8_t comparator, uint32_t value,
						enum hpm_plb_comparator_mode mode)
{
	return DEVICE_API_GET(hpm_plb, dev)
		->set_type_b_comparator(dev, channel, comparator, value, mode);
}

static inline int hpm_plb_set_type_b_counter(const struct device *dev, uint8_t channel,
					     uint32_t value)
{
	return DEVICE_API_GET(hpm_plb, dev)->set_type_b_counter(dev, channel, value);
}

static inline int hpm_plb_get_type_b_counter(const struct device *dev, uint8_t channel,
					     uint32_t *value)
{
	return DEVICE_API_GET(hpm_plb, dev)->get_type_b_counter(dev, channel, value);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_PLB_H_ */
