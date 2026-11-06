/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * Copyright (c) 2026 Cody Gu <gujiaqi@iscas.ac.cn>
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_sysctl

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/sys/util.h>

#include <hpm_common.h>
#include <hpm_sysctl_regs.h>

struct hpm_reset_config {
	SYSCTL_Type *base;
};

static bool hpm_reset_id_is_valid(const struct hpm_reset_config *config, uint32_t id)
{
	return id < ARRAY_SIZE(config->base->RESET);
}

static int hpm_reset_status(const struct device *dev, uint32_t id, uint8_t *status)
{
	const struct hpm_reset_config *config = dev->config;

	if ((status == NULL) || !hpm_reset_id_is_valid(config, id)) {
		return -EINVAL;
	}

	*status = FIELD_GET(SYSCTL_RESET_CONTROL_HOLD_MASK, config->base->RESET[id].CONTROL);

	return 0;
}

static int hpm_reset_line_assert(const struct device *dev, uint32_t id)
{
	const struct hpm_reset_config *config = dev->config;

	if (!hpm_reset_id_is_valid(config, id)) {
		return -EINVAL;
	}

	/*
	 * HOLD performs the reset and keeps the domain asserted until
	 * reset_line_deassert(). The current core will reboot before this
	 * function can return when either SYSCTL domain is selected.
	 */
	config->base->RESET[id].CONTROL |= SYSCTL_RESET_CONTROL_HOLD_MASK;

	return 0;
}

static int hpm_reset_line_deassert(const struct device *dev, uint32_t id)
{
	const struct hpm_reset_config *config = dev->config;

	if (!hpm_reset_id_is_valid(config, id)) {
		return -EINVAL;
	}

	config->base->RESET[id].CONTROL &= ~SYSCTL_RESET_CONTROL_HOLD_MASK;

	return 0;
}

static int hpm_reset_line_toggle(const struct device *dev, uint32_t id)
{
	const struct hpm_reset_config *config = dev->config;

	if (!hpm_reset_id_is_valid(config, id)) {
		return -EINVAL;
	}

	/*
	 * RESET performs the reset and releases it automatically. It has the
	 * same immediate reboot consequence as hpm_reset_line_assert().
	 */
	config->base->RESET[id].CONTROL |= SYSCTL_RESET_CONTROL_RESET_MASK;

	return 0;
}

static DEVICE_API(reset, hpm_reset_api) = {
	.status = hpm_reset_status,
	.line_assert = hpm_reset_line_assert,
	.line_deassert = hpm_reset_line_deassert,
	.line_toggle = hpm_reset_line_toggle,
};

#define HPM_RESET_INIT(n)                                                                          \
	static const struct hpm_reset_config hpm_reset_config_##n = {                              \
		.base = (SYSCTL_Type *)DT_INST_REG_ADDR(n),                                        \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, &hpm_reset_config_##n, POST_KERNEL,             \
			      CONFIG_RESET_INIT_PRIORITY, &hpm_reset_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_RESET_INIT)
