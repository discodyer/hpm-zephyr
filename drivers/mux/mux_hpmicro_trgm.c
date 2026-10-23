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

#include <hpm_common.h>
#include <hpm_trgm_drv.h>

LOG_MODULE_REGISTER(mux_hpmicro_trgm, CONFIG_MUX_LOG_LEVEL);

#define HPM_TRGM_OUTPUT_COUNT ARRAY_SIZE(((TRGM_Type *)0)->TRGOCFG)

struct hpm_trgm_config {
	TRGM_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
};

struct hpm_trgm_data {
	struct k_spinlock lock;
};

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

	/* The TRGOCFG source field is seven bits wide. */
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

static int hpm_trgm_init(const struct device *dev)
{
	const struct hpm_trgm_config *config = dev->config;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	return clock_control_on(config->clock_dev, config->clock_subsys);
}

static DEVICE_API(mux_control, hpm_trgm_api) = {
	.set = hpm_trgm_set,
	.get_state = hpm_trgm_get_state,
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
