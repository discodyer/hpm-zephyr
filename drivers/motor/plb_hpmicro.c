/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_plb

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <drivers/motor/hpmicro_plb.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/kernel.h>

#include <hpm_common.h>
#include <hpm_plb_drv.h>

struct hpm_plb_config {
	PLB_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
};

struct hpm_plb_data {
	struct k_spinlock lock;
};

static bool hpm_plb_channel_is_valid(uint8_t channel)
{
	return channel < HPM_PLB_CHANNEL_COUNT;
}

static bool hpm_plb_slice_operation_is_valid(enum hpm_plb_slice_operation operation)
{
	return operation <= HPM_PLB_SLICE_SHIFT_RIGHT_OR_ONE;
}

static bool hpm_plb_comparator_mode_is_valid(enum hpm_plb_comparator_mode mode)
{
	return (mode <= HPM_PLB_COMPARATOR_LESS_OR_EQUAL) ||
	       ((mode >= HPM_PLB_COMPARATOR_AND_MASK) && (mode <= HPM_PLB_COMPARATOR_XNOR_MASK));
}

static plb_type_b_slice_opt_t hpm_plb_slice_operation_to_sdk(enum hpm_plb_slice_operation operation)
{
	switch (operation) {
	case HPM_PLB_SLICE_KEEP:
		return plb_slice_opt_keep;
	case HPM_PLB_SLICE_LOAD_CMP0:
		return plb_slice_opt_get_cmp0_val;
	case HPM_PLB_SLICE_LOAD_CMP1:
		return plb_slice_opt_get_cmp1_val;
	case HPM_PLB_SLICE_LOAD_CMP2:
		return plb_slice_opt_get_cmp2_val;
	case HPM_PLB_SLICE_ADD_ONE:
		return plb_slice_opt_add_one;
	case HPM_PLB_SLICE_ADD_TWO:
		return plb_slice_opt_add_two;
	case HPM_PLB_SLICE_SUBTRACT_ONE:
		return plb_slice_opt_sub_one;
	case HPM_PLB_SLICE_SUBTRACT_TWO:
		return plb_slice_opt_sub_two;
	case HPM_PLB_SLICE_SHIFT_LEFT:
		return plb_slice_opt_shift_left;
	case HPM_PLB_SLICE_SHIFT_LEFT_OR_ONE:
		return plb_slice_opt_shift_left_add_one;
	case HPM_PLB_SLICE_SHIFT_RIGHT:
		return plb_slice_opt_shift_right;
	case HPM_PLB_SLICE_SHIFT_RIGHT_OR_ONE:
		return plb_slice_opt_shift_right_add_one;
	default:
		return plb_slice_opt_keep;
	}
}

static int hpm_plb_drv_configure_type_a(const struct device *dev, uint8_t channel,
					const struct hpm_plb_type_a_config *config)
{
	const struct hpm_plb_config *plb = dev->config;
	struct hpm_plb_data *data = dev->data;
	plb_type_a_truth_t truth;
	k_spinlock_key_t key;
	uint8_t index;

	if (!hpm_plb_channel_is_valid(channel) || (config == NULL)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	for (index = 0U; index < HPM_PLB_TYPE_A_LUT_COUNT; index++) {
		truth.val = config->truth[index];
		plb_type_a_set_lut(plb->base, (plb_chn_t)channel, (plb_type_a_lut_num_t)index,
				   &truth);
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_plb_drv_inject_type_a(const struct device *dev, uint8_t channel, uint8_t value)
{
	const struct hpm_plb_config *plb = dev->config;
	struct hpm_plb_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_plb_channel_is_valid(channel) || (value > 0x0FU)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	plb_type_a_inject_by_sw(plb->base, (plb_chn_t)channel, value);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_plb_drv_set_type_b_slice(const struct device *dev, uint8_t channel, uint8_t slice,
					enum hpm_plb_slice_operation operation)
{
	const struct hpm_plb_config *plb = dev->config;
	struct hpm_plb_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_plb_channel_is_valid(channel) || (slice >= HPM_PLB_TYPE_B_SLICE_COUNT) ||
	    !hpm_plb_slice_operation_is_valid(operation)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	plb_type_b_set_lut(plb->base, (plb_chn_t)channel, (plb_type_b_lut_slice_t)slice,
			   hpm_plb_slice_operation_to_sdk(operation));
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_plb_drv_set_type_b_slices(const struct device *dev, uint8_t channel, uint16_t mask,
					 enum hpm_plb_slice_operation operation)
{
	const struct hpm_plb_config *plb = dev->config;
	struct hpm_plb_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_plb_channel_is_valid(channel) || !hpm_plb_slice_operation_is_valid(operation)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	plb_type_b_set_lut_range_mask(plb->base, (plb_chn_t)channel, mask,
				      hpm_plb_slice_operation_to_sdk(operation));
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_plb_drv_set_type_b_all_slices(const struct device *dev, uint8_t channel,
					     enum hpm_plb_slice_operation operation)
{
	const struct hpm_plb_config *plb = dev->config;
	struct hpm_plb_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_plb_channel_is_valid(channel) || !hpm_plb_slice_operation_is_valid(operation)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	plb_type_b_set_all_slice(plb->base, (plb_chn_t)channel,
				 hpm_plb_slice_operation_to_sdk(operation));
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_plb_drv_set_type_b_comparator(const struct device *dev, uint8_t channel,
					     uint8_t comparator, uint32_t value,
					     enum hpm_plb_comparator_mode mode)
{
	const struct hpm_plb_config *plb = dev->config;
	struct hpm_plb_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_plb_channel_is_valid(channel) || (comparator >= HPM_PLB_TYPE_B_COMPARATOR_COUNT) ||
	    !hpm_plb_comparator_mode_is_valid(mode)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	plb_type_b_set_cmp_val(plb->base, (plb_chn_t)channel, (plb_type_b_cmp_t)comparator, value);
	plb_type_b_set_cmp_mode(plb->base, (plb_chn_t)channel, (plb_type_b_cmp_t)comparator,
				(plb_type_b_cmp_mode_t)mode);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_plb_drv_set_type_b_counter(const struct device *dev, uint8_t channel, uint32_t value)
{
	const struct hpm_plb_config *plb = dev->config;
	struct hpm_plb_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_plb_channel_is_valid(channel)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	plb_type_b_inject_by_sw(plb->base, (plb_chn_t)channel, value);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_plb_drv_get_type_b_counter(const struct device *dev, uint8_t channel,
					  uint32_t *value)
{
	const struct hpm_plb_config *plb = dev->config;
	struct hpm_plb_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_plb_channel_is_valid(channel) || (value == NULL)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	*value = plb_type_b_get_counter(plb->base, (plb_chn_t)channel);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static DEVICE_API(hpm_plb, hpm_plb_api) = {
	.configure_type_a = hpm_plb_drv_configure_type_a,
	.inject_type_a = hpm_plb_drv_inject_type_a,
	.set_type_b_slice = hpm_plb_drv_set_type_b_slice,
	.set_type_b_slices = hpm_plb_drv_set_type_b_slices,
	.set_type_b_all_slices = hpm_plb_drv_set_type_b_all_slices,
	.set_type_b_comparator = hpm_plb_drv_set_type_b_comparator,
	.set_type_b_counter = hpm_plb_drv_set_type_b_counter,
	.get_type_b_counter = hpm_plb_drv_get_type_b_counter,
};

static int hpm_plb_init(const struct device *dev)
{
	const struct hpm_plb_config *config = dev->config;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	return clock_control_on(config->clock_dev, config->clock_subsys);
}

#define HPM_PLB_INIT(n)                                                                            \
	static const struct hpm_plb_config hpm_plb_config_##n = {                                  \
		.base = (PLB_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
	};                                                                                         \
                                                                                                   \
	static struct hpm_plb_data hpm_plb_data_##n;                                               \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_plb_init, NULL, &hpm_plb_data_##n, &hpm_plb_config_##n,       \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &hpm_plb_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_PLB_INIT)
