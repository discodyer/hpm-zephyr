/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_crc

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/crc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hpm_crc_drv.h>

LOG_MODULE_REGISTER(crc_hpmicro, CONFIG_CRC_LOG_LEVEL);

#define HPM_CRC_CHANNELS 8U

struct hpm_crc_config {
	CRC_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	uint8_t channel;
};

struct hpm_crc_data {
	struct k_sem lock;
};

static int hpm_crc_width(enum crc_type type, uint8_t *width)
{
	switch (type) {
	case CRC8:
	case CRC8_CCITT:
	case CRC8_ROHC:
		*width = CRC_POLY_WIDTH_8;
		return 0;
	case CRC16:
	case CRC16_ANSI:
	case CRC16_CCITT:
	case CRC16_ITU_T:
		*width = CRC_POLY_WIDTH_16;
		return 0;
	case CRC32_C:
	case CRC32_IEEE:
	case CRC32_MPEG2:
		*width = CRC_POLY_WIDTH_32;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static uint32_t hpm_crc_xorout(enum crc_type type)
{
	switch (type) {
	case CRC32_IEEE:
		/*
		 * Zephyr's crc32_c() wrapper applies the CRC32C final XOR after
		 * crc_finish(), while crc32_ieee() expects the driver to return
		 * the final standard IEEE value.
		 */
		return UINT32_MAX;
	default:
		return 0U;
	}
}

static int hpm_crc_begin(const struct device *dev, struct crc_ctx *ctx)
{
	const struct hpm_crc_config *config = dev->config;
	struct hpm_crc_data *data = dev->data;
	crc_channel_config_t channel_cfg;
	uint8_t width;
	int ret;

	if ((ctx == NULL) || (ctx->state != CRC_STATE_IDLE)) {
		return -EINVAL;
	}

	ret = hpm_crc_width(ctx->type, &width);
	if (ret < 0) {
		return ret;
	}

	channel_cfg.preset = crc_preset_none;
	channel_cfg.poly = ctx->polynomial;
	channel_cfg.init = ctx->seed;
	channel_cfg.poly_width = width;
	channel_cfg.in_byte_order = crc_in_byte_order_lsb;
	channel_cfg.refin =
		(ctx->reversed & CRC_FLAG_REVERSE_INPUT) != 0U ? crc_refin_true : crc_refin_false;
	channel_cfg.refout = (ctx->reversed & CRC_FLAG_REVERSE_OUTPUT) != 0U ? crc_refout_true
									     : crc_refout_false;
	channel_cfg.xorout = hpm_crc_xorout(ctx->type);

	k_sem_take(&data->lock, K_FOREVER);

	config->base->CHN[config->channel].CLR = CRC_CHN_CLR_CLR_MASK;
	(void)crc_setup_channel_config(config->base, config->channel, &channel_cfg);

	ctx->state = CRC_STATE_IN_PROGRESS;

	return 0;
}

static int hpm_crc_update(const struct device *dev, struct crc_ctx *ctx, const void *buffer,
			  size_t bufsize)
{
	const struct hpm_crc_config *config = dev->config;

	if (ctx == NULL) {
		return -EINVAL;
	}

	if (ctx->state != CRC_STATE_IN_PROGRESS) {
		return -EINVAL;
	}

	if ((buffer == NULL) && (bufsize != 0U)) {
		return -EINVAL;
	}

	if (bufsize != 0U) {
		crc_calc_block_bytes(config->base, config->channel, (uint8_t *)buffer, bufsize);
	}

	ctx->result = crc_get_result(config->base, config->channel);

	return 0;
}

static int hpm_crc_finish(const struct device *dev, struct crc_ctx *ctx)
{
	const struct hpm_crc_config *config = dev->config;
	struct hpm_crc_data *data = dev->data;

	if ((ctx == NULL) || (ctx->state != CRC_STATE_IN_PROGRESS)) {
		return -EINVAL;
	}

	ctx->result = crc_get_result(config->base, config->channel);
	ctx->state = CRC_STATE_IDLE;
	k_sem_give(&data->lock);

	return 0;
}

static DEVICE_API(crc, hpm_crc_api) = {
	.begin = hpm_crc_begin,
	.update = hpm_crc_update,
	.finish = hpm_crc_finish,
};

static int hpm_crc_init(const struct device *dev)
{
	const struct hpm_crc_config *config = dev->config;
	struct hpm_crc_data *data = dev->data;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		return ret;
	}

	k_sem_init(&data->lock, 1, 1);

	return 0;
}

#define HPM_CRC_INIT(n)                                                                            \
	BUILD_ASSERT(DT_INST_PROP_OR(n, channel, 0) < HPM_CRC_CHANNELS, "invalid CRC channel");    \
                                                                                                   \
	static struct hpm_crc_data hpm_crc_data_##n;                                               \
                                                                                                   \
	static const struct hpm_crc_config hpm_crc_config_##n = {                                  \
		.base = (CRC_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.channel = DT_INST_PROP_OR(n, channel, 0),                                         \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_crc_init, NULL, &hpm_crc_data_##n, &hpm_crc_config_##n,       \
			      POST_KERNEL, CONFIG_CRC_DRIVER_INIT_PRIORITY, &hpm_crc_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_CRC_INIT)
