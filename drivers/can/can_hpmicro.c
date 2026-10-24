/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_mcan

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

#include <drivers/can/hpmicro_mcan.h>

#include <hpm_clock_drv.h>
#include <hpm_mcan_soc.h>

#include "can_mcan.h"

LOG_MODULE_REGISTER(can_hpmicro, CONFIG_CAN_LOG_LEVEL);

#define HPM_MCAN_MRAM_WINDOW_SIZE 0x2000U

struct hpm_mcan_config {
	mm_reg_t base;
	mem_addr_t mrba;
	mem_addr_t mram;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	clock_name_t clock_name;
	clock_source_t clock_src;
	uint32_t clock_div;
	const struct pinctrl_dev_config *pincfg;
	void (*irq_config_func)(const struct device *dev);
};

static int hpm_mcan_read_reg(const struct device *dev, uint16_t reg, uint32_t *val)
{
	const struct can_mcan_config *mcan_config = dev->config;
	const struct hpm_mcan_config *config = mcan_config->custom;

	return can_mcan_sys_read_reg(config->base, reg, val);
}

static int hpm_mcan_write_reg(const struct device *dev, uint16_t reg, uint32_t val)
{
	const struct can_mcan_config *mcan_config = dev->config;
	const struct hpm_mcan_config *config = mcan_config->custom;

	return can_mcan_sys_write_reg(config->base, reg, val);
}

static int hpm_mcan_read_mram(const struct device *dev, uint16_t offset, void *dst, size_t len)
{
	const struct can_mcan_config *mcan_config = dev->config;
	const struct hpm_mcan_config *config = mcan_config->custom;

	return can_mcan_sys_read_mram(config->mram, offset, dst, len);
}

static int hpm_mcan_write_mram(const struct device *dev, uint16_t offset, const void *src,
			       size_t len)
{
	const struct can_mcan_config *mcan_config = dev->config;
	const struct hpm_mcan_config *config = mcan_config->custom;

	return can_mcan_sys_write_mram(config->mram, offset, src, len);
}

static int hpm_mcan_clear_mram(const struct device *dev, uint16_t offset, size_t len)
{
	const struct can_mcan_config *mcan_config = dev->config;
	const struct hpm_mcan_config *config = mcan_config->custom;

	return can_mcan_sys_clear_mram(config->mram, offset, len);
}

static int hpm_mcan_get_core_clock(const struct device *dev, uint32_t *rate)
{
	const struct can_mcan_config *mcan_config = dev->config;
	const struct hpm_mcan_config *config = mcan_config->custom;

	return clock_control_get_rate(config->clock_dev, config->clock_subsys, rate);
}

static void hpm_mcan_isr(const struct device *dev)
{
	/*
	 * HPM5361 exposes one PLIC line per MCAN instance. The Bosch IP has
	 * separate interrupt lines 0 and 1 internally, so service both.
	 */
	can_mcan_line_0_isr(dev);
	can_mcan_line_1_isr(dev);
}

static int hpm_mcan_drv_configure_tsu(const struct device *dev,
				      const struct hpm_mcan_tsu_config *config)
{
	struct can_mcan_data *data = dev->data;
	uint32_t cccr;
	uint32_t tscfg = 0U;
	uint32_t glb_ctl;
	int ret;

	if ((config == NULL) ||
	    ((config->timebase != HPM_MCAN_TSU_TIMEBASE_INTERNAL) &&
	     (config->timebase != HPM_MCAN_TSU_TIMEBASE_PTPC)) ||
	    ((unsigned int)config->timebase_input >= HPM_MCAN_TSU_INPUT_COUNT)) {
		return -EINVAL;
	}

	if (data->common.started) {
		return -EBUSY;
	}

	ret = can_mcan_read_reg(dev, CAN_MCAN_CCCR, &cccr);
	if (ret != 0) {
		return ret;
	}
	cccr |= CAN_MCAN_CCCR_CCE | CAN_MCAN_CCCR_UTSU;
	ret = can_mcan_write_reg(dev, CAN_MCAN_CCCR, cccr);
	if (ret != 0) {
		return ret;
	}

	tscfg = MCAN_TSCFG_TSUE_MASK | MCAN_TSCFG_TBPRE_SET(0U);
	if (config->timebase == HPM_MCAN_TSU_TIMEBASE_PTPC) {
		tscfg |= MCAN_TSCFG_TBCS_MASK;
	}
	if (config->capture_on_sof) {
		tscfg |= MCAN_TSCFG_SCP_MASK;
	}
	if (config->use_64bit) {
		tscfg |= MCAN_TSCFG_EN64_MASK;
	}

	if (config->timebase == HPM_MCAN_TSU_TIMEBASE_PTPC) {
		ret = can_mcan_read_reg(dev, offsetof(MCAN_Type, GLB_CTL), &glb_ctl);
		if (ret != 0) {
			return ret;
		}
		glb_ctl = (glb_ctl & ~MCAN_GLB_CTL_TSU_TBIN_SEL_MASK) |
			  MCAN_GLB_CTL_TSU_TBIN_SEL_SET(config->timebase_input);
		ret = can_mcan_write_reg(dev, offsetof(MCAN_Type, GLB_CTL), glb_ctl);
		if (ret != 0) {
			return ret;
		}
	}

	ret = can_mcan_write_reg(dev, offsetof(MCAN_Type, TSCFG), tscfg);
	if (ret != 0) {
		return ret;
	}

	return can_mcan_write_reg(dev, CAN_MCAN_TSCC, FIELD_PREP(CAN_MCAN_TSCC_TSS, 2U));
}

static int hpm_mcan_drv_get_tsu_snapshot(const struct device *dev, uint8_t slot,
					 struct hpm_mcan_tsu_snapshot *snapshot)
{
	uint32_t tss1;
	uint32_t value_lo;
	uint32_t value_hi;
	uint32_t tscfg;
	bool is_64bit;
	int ret;

	if ((slot >= HPM_MCAN_TSU_SLOT_COUNT) || (snapshot == NULL)) {
		return -EINVAL;
	}

	ret = can_mcan_read_reg(dev, offsetof(MCAN_Type, TSCFG), &tscfg);
	if (ret != 0) {
		return ret;
	}
	ret = can_mcan_read_reg(dev, offsetof(MCAN_Type, TSS1), &tss1);
	if (ret != 0) {
		return ret;
	}

	is_64bit = MCAN_TSCFG_EN64_GET(tscfg) != 0U;
	if (is_64bit && (slot >= HPM_MCAN_TSU_SLOT_COUNT / 2U)) {
		return -EINVAL;
	}

	snapshot->slot = slot;
	/* TSS1 tracks the logical slot; 64-bit mode consumes two TS_SEL words. */
	snapshot->timestamp_valid = (tss1 & BIT(slot)) != 0U;
	snapshot->timestamp_lost = (tss1 & BIT(slot + 16U)) != 0U;
	snapshot->is_64bit = is_64bit;

	slot = is_64bit ? slot * 2U : slot;
	ret = can_mcan_read_reg(dev, offsetof(MCAN_Type, TS_SEL) + slot * sizeof(uint32_t),
				&value_lo);
	if (ret != 0) {
		return ret;
	}
	snapshot->value32 = value_lo;
	if (is_64bit) {
		ret = can_mcan_read_reg(
			dev, offsetof(MCAN_Type, TS_SEL) + (slot + 1U) * sizeof(uint32_t),
			&value_hi);
		if (ret != 0) {
			return ret;
		}
		snapshot->value64 = ((uint64_t)value_hi << 32U) | value_lo;
	}

	return 0;
}

static int hpm_mcan_drv_decode_rx_timestamp(const struct device *dev, uint16_t encoded,
					    struct hpm_mcan_tsu_snapshot *snapshot)
{
	if (snapshot == NULL) {
		return -EINVAL;
	}
	if ((encoded & BIT(4)) == 0U) {
		return -ENODATA;
	}

	return hpm_mcan_drv_get_tsu_snapshot(dev, encoded & 0x0FU, snapshot);
}

static int hpm_mcan_rx_filter_timestamp(const struct device *dev, int filter_id, bool update,
					bool enable, bool *enabled)
{
	const struct can_mcan_config *mcan_config = dev->config;
	const struct can_mcan_callbacks *cbs = mcan_config->callbacks;
	struct can_mcan_data *data = dev->data;
	bool is_extended;
	uint32_t element;
	uint16_t offset;
	int ret;

	if ((filter_id < 0) || (filter_id >= (cbs->num_std + cbs->num_ext))) {
		return -EINVAL;
	}

	is_extended = filter_id >= cbs->num_std;
	if (is_extended) {
		struct can_mcan_ext_filter ext_element;

		filter_id -= cbs->num_std;
		offset = mcan_config->mram_offsets[CAN_MCAN_MRAM_CFG_EXT_FILTER] +
			 filter_id * sizeof(ext_element);
	} else {
		offset = mcan_config->mram_offsets[CAN_MCAN_MRAM_CFG_STD_FILTER] +
			 filter_id * sizeof(struct can_mcan_std_filter);
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	if (is_extended ? (cbs->ext[filter_id].function == NULL)
			: (cbs->std[filter_id].function == NULL)) {
		ret = -EINVAL;
		goto unlock;
	}

	if (is_extended) {
		struct can_mcan_ext_filter ext_element;

		ret = can_mcan_read_mram(dev, offset, &ext_element, sizeof(ext_element));
		if (ret != 0) {
			goto unlock;
		}
		if (enabled != NULL) {
			*enabled = ext_element.esync != 0U;
		}
		if (update && (enable != (ext_element.esync != 0U))) {
			ext_element.esync = enable ? 1U : 0U;
			ret = can_mcan_write_mram(dev, offset, &ext_element, sizeof(ext_element));
		}
	} else {
		ret = can_mcan_read_mram(dev, offset, &element, sizeof(element));
		if (ret != 0) {
			goto unlock;
		}
		if (enabled != NULL) {
			*enabled = (element & BIT(15)) != 0U;
		}
		if (update && (enable != ((element & BIT(15)) != 0U))) {
			element = enable ? (element | BIT(15)) : (element & ~BIT(15));
			ret = can_mcan_write_mram(dev, offset, &element, sizeof(element));
		}
	}

unlock:
	k_mutex_unlock(&data->lock);

	return ret;
}

static int hpm_mcan_drv_set_rx_filter_timestamp(const struct device *dev, int filter_id,
						bool enable)
{
	return hpm_mcan_rx_filter_timestamp(dev, filter_id, true, enable, NULL);
}

static int hpm_mcan_drv_get_rx_filter_timestamp(const struct device *dev, int filter_id,
						bool *enabled)
{
	if (enabled == NULL) {
		return -EINVAL;
	}

	return hpm_mcan_rx_filter_timestamp(dev, filter_id, false, false, enabled);
}

static int hpm_mcan_init(const struct device *dev)
{
	const struct can_mcan_config *mcan_config = dev->config;
	const struct hpm_mcan_config *config = mcan_config->custom;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		return ret;
	}

	if (clock_set_source_divider(config->clock_name, config->clock_src, config->clock_div) !=
	    status_success) {
		return -EIO;
	}

	ret = can_mcan_configure_mram(dev, config->mrba, config->mram);
	if (ret != 0) {
		return ret;
	}

	ret = can_mcan_init(dev);
	if (ret != 0) {
		return ret;
	}

	config->irq_config_func(dev);

	return 0;
}

static DEVICE_API(hpm_mcan, hpm_mcan_driver_api) = {
	.can_api =
		{
			.get_capabilities = can_mcan_get_capabilities,
			.start = can_mcan_start,
			.stop = can_mcan_stop,
			.set_mode = can_mcan_set_mode,
			.set_timing = can_mcan_set_timing,
			.send = can_mcan_send,
			.add_rx_filter = can_mcan_add_rx_filter,
			.remove_rx_filter = can_mcan_remove_rx_filter,
			.get_state = can_mcan_get_state,
#ifdef CONFIG_CAN_MANUAL_RECOVERY_MODE
			.recover = can_mcan_recover,
#endif
			.get_core_clock = hpm_mcan_get_core_clock,
			.get_max_filters = can_mcan_get_max_filters,
			.timing_min = CAN_MCAN_TIMING_MIN_INITIALIZER,
			.timing_max = CAN_MCAN_TIMING_MAX_INITIALIZER,
#ifdef CONFIG_CAN_FD_MODE
			.set_timing_data = can_mcan_set_timing_data,
			.timing_data_min = CAN_MCAN_TIMING_DATA_MIN_INITIALIZER,
			.timing_data_max = CAN_MCAN_TIMING_DATA_MAX_INITIALIZER,
#endif
		},
	.configure_tsu = hpm_mcan_drv_configure_tsu,
	.get_tsu_snapshot = hpm_mcan_drv_get_tsu_snapshot,
	.decode_rx_timestamp = hpm_mcan_drv_decode_rx_timestamp,
	.set_rx_filter_timestamp = hpm_mcan_drv_set_rx_filter_timestamp,
	.get_rx_filter_timestamp = hpm_mcan_drv_get_rx_filter_timestamp,
};

static const struct can_mcan_ops hpm_mcan_ops = {
	.read_reg = hpm_mcan_read_reg,
	.write_reg = hpm_mcan_write_reg,
	.read_mram = hpm_mcan_read_mram,
	.write_mram = hpm_mcan_write_mram,
	.clear_mram = hpm_mcan_clear_mram,
};

#define HPM_MCAN_IRQ_CONFIG(n)                                                                     \
	static void hpm_mcan_irq_config_##n(const struct device *dev)                              \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_mcan_isr,               \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}

#define HPM_MCAN_INIT(n)                                                                           \
	CAN_MCAN_DT_INST_BUILD_ASSERT_MRAM_CFG(n);                                                 \
	BUILD_ASSERT(CAN_MCAN_DT_INST_MRAM_SIZE(n) == HPM_MCAN_MRAM_WINDOW_SIZE,                   \
		     "HPM MCAN message RAM window must be 8 KiB");                                 \
	BUILD_ASSERT(CAN_MCAN_DT_INST_MRAM_ELEMENTS_SIZE(n) <= CAN_MCAN_DT_INST_MRAM_SIZE(n),      \
		     "Insufficient MCAN message RAM");                                             \
                                                                                                   \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	CAN_MCAN_DT_INST_CALLBACKS_DEFINE(n, hpm_mcan_callbacks_##n);                              \
	HPM_MCAN_IRQ_CONFIG(n)                                                                     \
                                                                                                   \
	static const struct hpm_mcan_config hpm_mcan_config_##n = {                                \
		.base = (mm_reg_t)DT_INST_REG_ADDR_BY_NAME(n, m_can),                              \
		.mrba = (mem_addr_t)DT_INST_REG_ADDR_BY_NAME(n, message_ram),                      \
		.mram = (mem_addr_t)CAN_MCAN_DT_INST_MRAM_ADDR(n),                                 \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.clock_name = DT_INST_PROP(n, clock_name),                                         \
		.clock_src = DT_INST_PROP(n, clock_src),                                           \
		.clock_div = DT_INST_CLOCKS_CELL_BY_IDX(n, 0, divider),                            \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
		.irq_config_func = hpm_mcan_irq_config_##n,                                        \
	};                                                                                         \
                                                                                                   \
	static const struct can_mcan_config can_mcan_cfg_##n = CAN_MCAN_DT_CONFIG_INST_GET(        \
		n, &hpm_mcan_config_##n, &hpm_mcan_ops, &hpm_mcan_callbacks_##n);                  \
                                                                                                   \
	CAN_MCAN_DATA_DEFINE(can_mcan_data_##n, NULL);                                             \
                                                                                                   \
	CAN_DEVICE_DT_INST_DEFINE(n, hpm_mcan_init, NULL, &can_mcan_data_##n, &can_mcan_cfg_##n,   \
				  POST_KERNEL, CONFIG_CAN_INIT_PRIORITY, &hpm_mcan_driver_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_MCAN_INIT)
