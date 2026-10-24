/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_spi

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hpm_clock_drv.h>
#include <hpm_spi_drv.h>

LOG_MODULE_REGISTER(spi_hpmicro, CONFIG_SPI_LOG_LEVEL);

#include "spi_context.h"

#define HPM_SPI_MAX_WORD_SIZE 32U
#define HPM_SPI_FIFO_DEPTH    8U

struct hpm_spi_config {
	SPI_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	clock_name_t clock_name;
	clock_source_t clock_src;
	uint32_t clock_div;
	const struct pinctrl_dev_config *pincfg;
};

struct hpm_spi_data {
	struct spi_context ctx;
};

static int hpm_spi_configure(const struct device *dev, const struct spi_config *spi_cfg)
{
	const struct hpm_spi_config *config = dev->config;
	spi_timing_config_t timing_config = {0};
	spi_format_config_t format_config = {0};
	uint32_t clock_freq;
	uint32_t word_size;
	int ret;

	if (SPI_OP_MODE_GET(spi_cfg->operation) != SPI_OP_MODE_CONTROLLER) {
		return -ENOTSUP;
	}

	if ((SPI_MODE_GET(spi_cfg->operation) & SPI_MODE_LOOP) != 0U) {
		return -ENOTSUP;
	}

	if ((spi_cfg->operation & SPI_FRAME_FORMAT_TI) != 0U) {
		return -ENOTSUP;
	}

	if ((spi_cfg->operation & SPI_LINES_MASK) != SPI_LINES_SINGLE) {
		return -ENOTSUP;
	}

	if (spi_cfg->frequency == 0U) {
		return -EINVAL;
	}

	if (spi_cs_is_gpio(spi_cfg)) {
		if (!gpio_is_ready_dt(&spi_cfg->cs.gpio)) {
			return -ENODEV;
		}
	} else {
		if ((spi_cfg->operation & (SPI_CS_ACTIVE_HIGH | SPI_HOLD_ON_CS)) != 0U) {
			return -ENOTSUP;
		}

		if ((spi_cfg->peripheral > 3U) || (spi_cfg->cs.hold_ns != 0U)) {
			return -ENOTSUP;
		}
	}

	word_size = SPI_WORD_SIZE_GET(spi_cfg->operation);
	if ((word_size == 0U) || (word_size > HPM_SPI_MAX_WORD_SIZE)) {
		return -EINVAL;
	}

	ret = clock_control_get_rate(config->clock_dev, config->clock_subsys, &clock_freq);
	if (ret < 0) {
		return ret;
	}

	spi_master_get_default_timing_config(&timing_config);
	timing_config.master_config.clk_src_freq_in_hz = clock_freq;
	timing_config.master_config.sclk_freq_in_hz = spi_cfg->frequency;

	if (!spi_cs_is_gpio(spi_cfg) && (spi_cfg->cs.setup_ns != 0U)) {
		uint64_t setup_ns = spi_cfg->cs.setup_ns;
		uint64_t period_ns = DIV_ROUND_UP(NSEC_PER_SEC, spi_cfg->frequency);
		uint8_t cs2sclk;

		for (cs2sclk = 0U; cs2sclk < 4U; cs2sclk++) {
			if ((period_ns * (cs2sclk + 1U)) / 2U >= setup_ns) {
				break;
			}
		}

		if (cs2sclk >= 4U) {
			return -ENOTSUP;
		}
		timing_config.master_config.cs2sclk = cs2sclk;
	}

	if (spi_master_timing_init(config->base, &timing_config) != status_success) {
		return -EINVAL;
	}

	spi_master_get_default_format_config(&format_config);
	format_config.common_config.data_len_in_bits = word_size;
	format_config.common_config.mode = spi_master_mode;
	format_config.common_config.cpol = (SPI_MODE_GET(spi_cfg->operation) & SPI_MODE_CPOL) != 0U
						   ? spi_sclk_high_idle
						   : spi_sclk_low_idle;
	format_config.common_config.cpha = (SPI_MODE_GET(spi_cfg->operation) & SPI_MODE_CPHA) != 0U
						   ? spi_sclk_sampling_even_clk_edges
						   : spi_sclk_sampling_odd_clk_edges;
	format_config.common_config.lsb = (spi_cfg->operation & SPI_TRANSFER_LSB) != 0U;
	format_config.common_config.mosi_bidir = (spi_cfg->operation & SPI_HALF_DUPLEX) != 0U;
	spi_format_init(config->base, &format_config);

	((struct hpm_spi_data *)dev->data)->ctx.config = spi_cfg;

	return 0;
}

static int hpm_spi_buffer_total(const struct spi_buf_set *bufs, size_t *total)
{
	if (bufs == NULL) {
		*total = 0U;
		return 0;
	}

	if ((bufs->count != 0U) && (bufs->buffers == NULL)) {
		return -EINVAL;
	}

	*total = 0U;
	for (size_t i = 0U; i < bufs->count; i++) {
		*total += bufs->buffers[i].len;
	}

	return 0;
}

static void hpm_spi_fill_next(const struct spi_buf_set *bufs, size_t *index, size_t *offset,
			      uint32_t frame_size, uint32_t *word)
{
	uint8_t *bytes = (uint8_t *)&word;

	*word = 0U;
	if ((bufs == NULL) || (*index >= bufs->count)) {
		return;
	}

	for (uint32_t i = 0U; i < frame_size; i++) {
		while ((*index < bufs->count) && (*offset >= bufs->buffers[*index].len)) {
			(*index)++;
			*offset = 0U;
		}

		if (*index >= bufs->count) {
			return;
		}

		if (bufs->buffers[*index].buf != NULL) {
			bytes[i] = ((const uint8_t *)bufs->buffers[*index].buf)[*offset];
		}
		(*offset)++;
	}
}

static void hpm_spi_store_next(const struct spi_buf_set *bufs, size_t *index, size_t *offset,
			       uint32_t frame_size, uint32_t word)
{
	const uint8_t *bytes = (const uint8_t *)&word;

	if ((bufs == NULL) || (*index >= bufs->count)) {
		return;
	}

	for (uint32_t i = 0U; i < frame_size; i++) {
		while ((*index < bufs->count) && (*offset >= bufs->buffers[*index].len)) {
			(*index)++;
			*offset = 0U;
		}

		if (*index >= bufs->count) {
			return;
		}

		if (bufs->buffers[*index].buf != NULL) {
			((uint8_t *)bufs->buffers[*index].buf)[*offset] = bytes[i];
		}
		(*offset)++;
	}
}

static int hpm_spi_transfer_stream(const struct hpm_spi_config *config,
				   spi_control_config_t *control_config, uint32_t frame_size,
				   uint32_t frames, uint32_t tx_frames,
				   const struct spi_buf_set *tx_bufs,
				   const struct spi_buf_set *rx_bufs)
{
	size_t tx_index = 0U;
	size_t rx_index = 0U;
	size_t tx_offset = 0U;
	size_t rx_offset = 0U;
	uint32_t tx_word = 0U;
	uint32_t rx_word = 0U;
	uint32_t written = 0U;
	uint32_t read = 0U;
	uint32_t retry = 0U;

	if ((tx_bufs == NULL) && (rx_bufs == NULL)) {
		return -EINVAL;
	}

	if (spi_control_init(config->base, control_config, frames, frames) != status_success) {
		return -EIO;
	}

	if (spi_write_address(config->base, spi_master_mode, control_config, NULL) !=
	    status_success) {
		return -EIO;
	}

	if (spi_write_command(config->base, spi_master_mode, control_config, NULL) !=
	    status_success) {
		return -EIO;
	}

	while ((read < frames) || (written < frames)) {
		uint32_t status = config->base->STATUS;

		if ((written < frames) && !(status & SPI_STATUS_TXFULL_MASK)) {
			if (written < tx_frames) {
				hpm_spi_fill_next(tx_bufs, &tx_index, &tx_offset, frame_size,
						  &tx_word);
			} else {
				tx_word = 0U;
			}
			config->base->DATA = tx_word;
			written++;
		}

		if ((read < frames) && !(status & SPI_STATUS_RXEMPTY_MASK)) {
			rx_word = config->base->DATA;
			hpm_spi_store_next(rx_bufs, &rx_index, &rx_offset, frame_size, rx_word);
			read++;
			retry = 0U;
		} else if ((written < frames) || (read < frames)) {
			if (retry++ > HPM_SPI_FIFO_DEPTH * 1024U) {
				return -ETIMEDOUT;
			}
		}
	}

	if (spi_wait_for_idle_status(config->base) != status_success) {
		return -ETIMEDOUT;
	}

	if (read != frames) {
		return -EIO;
	}

	return 0;
}

static int hpm_spi_transceive(const struct device *dev, const struct spi_config *spi_cfg,
			      const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs)
{
	const struct hpm_spi_config *config = dev->config;
	struct hpm_spi_data *data = dev->data;
	spi_control_config_t control_config = {0};
	size_t tx_len;
	size_t rx_len;
	uint32_t word_size;
	uint32_t frame_size;
	uint32_t frames;
	uint32_t tx_frames;
	uint32_t rx_frames;
	int ret;

	spi_context_lock(&data->ctx, false, NULL, NULL, spi_cfg);

	ret = hpm_spi_configure(dev, spi_cfg);
	if (ret < 0) {
		goto out;
	}

	ret = hpm_spi_buffer_total(tx_bufs, &tx_len);
	if (ret < 0) {
		goto out;
	}

	ret = hpm_spi_buffer_total(rx_bufs, &rx_len);
	if (ret < 0) {
		goto out;
	}

	if ((tx_len == 0U) && (rx_len == 0U)) {
		goto out;
	}

	if (((spi_cfg->operation & SPI_HALF_DUPLEX) != 0U) && (tx_len != 0U) && (rx_len != 0U)) {
		ret = -ENOTSUP;
		goto out;
	}

	word_size = SPI_WORD_SIZE_GET(spi_cfg->operation);
	if ((word_size != 8U) && (word_size != 16U) && (word_size != 24U) && (word_size != 32U)) {
		ret = -ENOTSUP;
		goto out;
	}
	frame_size = word_size / 8U;

	if (((tx_len != 0U) && !IS_ALIGNED(tx_len, frame_size)) ||
	    ((rx_len != 0U) && !IS_ALIGNED(rx_len, frame_size))) {
		ret = -EINVAL;
		goto out;
	}

	tx_frames = (uint32_t)(tx_len / frame_size);
	rx_frames = (uint32_t)(rx_len / frame_size);
	frames = MAX(tx_frames, rx_frames);

	spi_master_get_default_control_config(&control_config);
	control_config.common_config.cs_index =
		spi_cs_is_gpio(spi_cfg) ? 0U : (spi_cs_index_t)BIT(spi_cfg->peripheral);

	if ((tx_frames > 0U) && (rx_frames > 0U)) {
		control_config.common_config.trans_mode = spi_trans_write_read_together;
	} else if (tx_frames > 0U) {
		control_config.common_config.trans_mode = spi_trans_write_only;
	} else {
		control_config.common_config.trans_mode = spi_trans_read_only;
	}

	spi_context_cs_control(&data->ctx, true);
	ret = hpm_spi_transfer_stream(config, &control_config, frame_size, frames, tx_frames,
				      tx_bufs, rx_bufs);
	spi_context_cs_control(&data->ctx, false);

out:
	spi_context_release(&data->ctx, ret);
	if (ret < 0) {
		spi_context_unlock_unconditionally(&data->ctx);
	}

	return ret;
}

static int hpm_spi_release(const struct device *dev, const struct spi_config *spi_cfg)
{
	struct hpm_spi_data *data = dev->data;

	ARG_UNUSED(spi_cfg);

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static int hpm_spi_init(const struct device *dev)
{
	const struct hpm_spi_config *config = dev->config;
	struct hpm_spi_data *data = dev->data;
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

	ret = spi_context_cs_configure_all(&data->ctx);
	if (ret < 0) {
		return ret;
	}

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static const struct spi_driver_api hpm_spi_api = {
	.transceive = hpm_spi_transceive,
	.release = hpm_spi_release,
};

#define HPM_SPI_INIT(n)                                                                            \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
                                                                                                   \
	static const struct hpm_spi_config hpm_spi_config_##n = {                                  \
		.base = (SPI_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.clock_name = DT_INST_PROP(n, clock_name),                                         \
		.clock_src = DT_INST_PROP(n, clock_src),                                           \
		.clock_div = DT_INST_CLOCKS_CELL_BY_IDX(n, 0, divider),                            \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
	};                                                                                         \
                                                                                                   \
	static struct hpm_spi_data hpm_spi_data_##n = {                                            \
		SPI_CONTEXT_INIT_LOCK(hpm_spi_data_##n, ctx),                                      \
		SPI_CONTEXT_INIT_SYNC(hpm_spi_data_##n, ctx),                                      \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(n), ctx)};                             \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_spi_init, NULL, &hpm_spi_data_##n, &hpm_spi_config_##n,       \
			      POST_KERNEL, CONFIG_SPI_INIT_PRIORITY, &hpm_spi_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_SPI_INIT)
