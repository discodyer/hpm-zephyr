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

#include <hpm_clock_drv.h>

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

static DEVICE_API(can, hpm_mcan_driver_api) = {
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
