/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_spi

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <hpm_clock_drv.h>
#include <hpm_spi_drv.h>

LOG_MODULE_REGISTER(spi_hpmicro, CONFIG_SPI_LOG_LEVEL);

#include "spi_context.h"

#define HPM_SPI_MAX_WORD_SIZE  32U
#define HPM_SPI_FIFO_DEPTH     8U
#define HPM_SPI_DMA_NO_CHANNEL UINT32_MAX

struct hpm_spi_config {
	SPI_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	clock_name_t clock_name;
	clock_source_t clock_src;
	uint32_t clock_div;
	const struct pinctrl_dev_config *pincfg;
#ifdef CONFIG_SPI_HPMICRO_DMA
	const struct device *dma_dev;
	uint32_t tx_dma_channel;
	uint32_t rx_dma_channel;
	uint32_t tx_dma_request;
	uint32_t rx_dma_request;
	void (*irq_config_func)(const struct device *dev);
#endif
};

struct hpm_spi_data {
	struct spi_context ctx;
#ifdef CONFIG_SPI_ASYNC
	const struct device *dev;
	struct k_work async_work;
	const struct spi_config *async_config;
	const struct spi_buf_set *async_tx_bufs;
	const struct spi_buf_set *async_rx_bufs;
	spi_callback_t async_callback;
	void *async_userdata;
	atomic_t async_busy;
	int async_result;
#ifdef CONFIG_SPI_HPMICRO_DMA
	struct k_spinlock dma_lock;
	struct dma_block_config tx_dma_block;
	struct dma_block_config rx_dma_block;
	const uint8_t *dma_tx_buf;
	uint8_t *dma_rx_buf;
	size_t dma_rx_len;
	uint32_t dma_pending;
	struct k_work_delayable dma_timeout_work;
	bool dma_active;
	bool dma_error;
	bool dma_timeout;
#endif
#endif
};

#ifdef CONFIG_SPI_HPMICRO_DMA
#define HPM_SPI_DMA_TX_DONE  BIT(0)
#define HPM_SPI_DMA_RX_DONE  BIT(1)
#define HPM_SPI_DMA_SPI_DONE BIT(2)
#endif

#ifdef CONFIG_SPI_HPMICRO_DMA
static void hpm_spi_dma_finish(const struct device *dev, int status);
static void hpm_spi_dma_try_finish(const struct device *dev, uint32_t event);

static void hpm_spi_dma_timeout_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct hpm_spi_data *data = CONTAINER_OF(dwork, struct hpm_spi_data, dma_timeout_work);

	data->dma_timeout = true;
	hpm_spi_dma_try_finish(data->dev, 0U);
}

static void hpm_spi_dma_try_finish(const struct device *dev, uint32_t event)
{
	struct hpm_spi_data *data = dev->data;
	k_spinlock_key_t key;
	bool finish = false;
	int status = 0;

	key = k_spin_lock(&data->dma_lock);
	if (data->dma_active) {
		data->dma_pending &= ~event;
		if (data->dma_timeout) {
			status = -ETIMEDOUT;
		} else if (data->dma_error) {
			status = -EIO;
		}
		if ((data->dma_pending == 0U) || data->dma_error || data->dma_timeout) {
			data->dma_active = false;
			finish = true;
		}
	}
	k_spin_unlock(&data->dma_lock, key);

	if (finish) {
		hpm_spi_dma_finish(dev, status);
	}
}

static void hpm_spi_dma_tx_callback(const struct device *dma_dev, void *user_data, uint32_t channel,
				    int status)
{
	const struct device *dev = user_data;
	struct hpm_spi_data *data = dev->data;

	ARG_UNUSED(dma_dev);
	ARG_UNUSED(channel);

	if (status < 0) {
		data->dma_error = true;
	}
	hpm_spi_dma_try_finish(dev, HPM_SPI_DMA_TX_DONE);
}

static void hpm_spi_dma_rx_callback(const struct device *dma_dev, void *user_data, uint32_t channel,
				    int status)
{
	const struct device *dev = user_data;
	struct hpm_spi_data *data = dev->data;

	ARG_UNUSED(dma_dev);
	ARG_UNUSED(channel);

	if (status < 0) {
		data->dma_error = true;
	}
	hpm_spi_dma_try_finish(dev, HPM_SPI_DMA_RX_DONE);
}

static void hpm_spi_isr(const struct device *dev)
{
	const struct hpm_spi_config *config = dev->config;
	struct hpm_spi_data *data = dev->data;
	uint32_t status = spi_get_interrupt_status(config->base);

	spi_clear_interrupt_status(config->base, status);
	if ((status & (spi_rx_fifo_overflow_int | spi_tx_fifo_underflow_int)) != 0U) {
		data->dma_error = true;
		hpm_spi_dma_try_finish(dev, 0U);
		return;
	}
	if ((status & spi_end_int) != 0U) {
		hpm_spi_dma_try_finish(dev, HPM_SPI_DMA_SPI_DONE);
	}
}

static void hpm_spi_dma_finish(const struct device *dev, int status)
{
	const struct hpm_spi_config *config = dev->config;
	struct hpm_spi_data *data = dev->data;
	uint8_t *rx_buf = data->dma_rx_buf;
	size_t rx_len = data->dma_rx_len;

	(void)k_work_cancel_delayable(&data->dma_timeout_work);
	spi_disable_interrupt(config->base,
			      spi_end_int | spi_rx_fifo_overflow_int | spi_tx_fifo_underflow_int);
	spi_disable_tx_dma(config->base);
	spi_disable_rx_dma(config->base);
	(void)dma_stop(config->dma_dev, config->tx_dma_channel);
	(void)dma_stop(config->dma_dev, config->rx_dma_channel);
	spi_context_cs_control(&data->ctx, false);

	if ((status == 0) && (rx_buf != NULL) && (rx_len != 0U)) {
		(void)sys_cache_data_invd_range(rx_buf, rx_len);
	}
	data->dma_tx_buf = NULL;
	data->dma_rx_buf = NULL;
	data->dma_rx_len = 0U;
	atomic_clear(&data->async_busy);
	spi_context_complete(&data->ctx, dev, status);
}
#endif /* CONFIG_SPI_HPMICRO_DMA */

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

#ifdef CONFIG_SPI_ASYNC
#ifdef CONFIG_SPI_HPMICRO_DMA
static bool hpm_spi_async_dma_supported(const struct device *dev, const struct spi_config *spi_cfg,
					const struct spi_buf_set *tx_bufs,
					const struct spi_buf_set *rx_bufs)
{
	const struct hpm_spi_config *config = dev->config;
	uint32_t word_size;

	if (!IS_ENABLED(CONFIG_SPI_HPMICRO_DMA) || (config->dma_dev == NULL) ||
	    (SPI_OP_MODE_GET(spi_cfg->operation) != SPI_OP_MODE_CONTROLLER) ||
	    ((spi_cfg->operation & SPI_LINES_MASK) != SPI_LINES_SINGLE)) {
		return false;
	}

	word_size = SPI_WORD_SIZE_GET(spi_cfg->operation);
	if ((word_size != 8U) && (word_size != 16U) && (word_size != 32U)) {
		return false;
	}

	if ((tx_bufs != NULL) && ((tx_bufs->count > 1U) || (tx_bufs->buffers == NULL))) {
		return false;
	}
	if ((rx_bufs != NULL) && ((rx_bufs->count > 1U) || (rx_bufs->buffers == NULL))) {
		return false;
	}
	if ((tx_bufs != NULL) && (tx_bufs->count != 0U) &&
	    (config->tx_dma_channel == HPM_SPI_DMA_NO_CHANNEL)) {
		return false;
	}
	if ((rx_bufs != NULL) && (rx_bufs->count != 0U) &&
	    (config->rx_dma_channel == HPM_SPI_DMA_NO_CHANNEL)) {
		return false;
	}

	return true;
}
#endif /* CONFIG_SPI_HPMICRO_DMA */

#ifdef CONFIG_SPI_HPMICRO_DMA
static int hpm_spi_transceive_dma_async(const struct device *dev, const struct spi_config *spi_cfg,
					const struct spi_buf_set *tx_bufs,
					const struct spi_buf_set *rx_bufs, spi_callback_t callback,
					void *userdata)
{
	const struct hpm_spi_config *config = dev->config;
	struct hpm_spi_data *data = dev->data;
	spi_control_config_t control_config = {0};
	struct dma_config dma_cfg = {0};
	const uint8_t *tx_buf = NULL;
	uint8_t *rx_buf = NULL;
	size_t tx_len = 0U;
	size_t rx_len = 0U;
	uint32_t word_size;
	uint32_t data_size;
	uint32_t frames;
	int ret;

	if ((tx_bufs != NULL) && (tx_bufs->count != 0U)) {
		tx_buf = tx_bufs->buffers[0].buf;
		tx_len = tx_bufs->buffers[0].len;
	}
	if ((rx_bufs != NULL) && (rx_bufs->count != 0U)) {
		rx_buf = rx_bufs->buffers[0].buf;
		rx_len = rx_bufs->buffers[0].len;
	}
	if ((tx_len == 0U) && (rx_len == 0U)) {
		return -EINVAL;
	}
	if ((tx_len != 0U) && (tx_buf == NULL)) {
		return -EINVAL;
	}
	if ((rx_len != 0U) && (rx_buf == NULL)) {
		return -EINVAL;
	}

	word_size = SPI_WORD_SIZE_GET(spi_cfg->operation);
	data_size = word_size / BITS_PER_BYTE;
	if (!IS_ALIGNED(tx_len, data_size) || !IS_ALIGNED(rx_len, data_size) ||
	    ((tx_len != 0U) && (rx_len != 0U) && (tx_len != rx_len))) {
		return -EINVAL;
	}
	frames = MAX(tx_len, rx_len) / data_size;

	spi_context_lock(&data->ctx, true, callback, userdata, spi_cfg);
	ret = hpm_spi_configure(dev, spi_cfg);
	if (ret < 0) {
		spi_context_unlock_unconditionally(&data->ctx);
		return ret;
	}

	spi_master_get_default_control_config(&control_config);
	control_config.common_config.cs_index =
		spi_cs_is_gpio(spi_cfg) ? 0U : (spi_cs_index_t)BIT(spi_cfg->peripheral);
	control_config.common_config.data_phase_fmt = spi_single_io_mode;
	control_config.common_config.tx_dma_enable = tx_len != 0U;
	control_config.common_config.rx_dma_enable = rx_len != 0U;
	if ((tx_len != 0U) && (rx_len != 0U)) {
		control_config.common_config.trans_mode = spi_trans_write_read_together;
	} else if (tx_len != 0U) {
		control_config.common_config.trans_mode = spi_trans_write_only;
	} else {
		control_config.common_config.trans_mode = spi_trans_read_only;
	}

	data->dma_tx_buf = tx_buf;
	data->dma_rx_buf = rx_buf;
	data->dma_rx_len = rx_len;
	data->dma_error = false;
	data->dma_timeout = false;
	data->dma_pending = HPM_SPI_DMA_SPI_DONE;
	(void)k_work_reschedule(&data->dma_timeout_work, K_MSEC(CONFIG_SPI_HPMICRO_DMA_TIMEOUT_MS));

	if (tx_len != 0U) {
		data->tx_dma_block = (struct dma_block_config){
			.source_address = (uint32_t)tx_buf,
			.dest_address = (uint32_t)&config->base->DATA,
			.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
			.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
			.block_size = tx_len,
		};
		dma_cfg.channel_direction = MEMORY_TO_PERIPHERAL;
		dma_cfg.dma_slot = config->tx_dma_request;
		dma_cfg.source_data_size = data_size;
		dma_cfg.dest_data_size = data_size;
		dma_cfg.source_burst_length = data_size;
		dma_cfg.dest_burst_length = data_size;
		dma_cfg.block_count = 1U;
		dma_cfg.head_block = &data->tx_dma_block;
		dma_cfg.user_data = (void *)dev;
		dma_cfg.dma_callback = hpm_spi_dma_tx_callback;
		(void)sys_cache_data_flush_range((void *)tx_buf, tx_len);
		ret = dma_config(config->dma_dev, config->tx_dma_channel, &dma_cfg);
		if (ret < 0) {
			goto error;
		}
		data->dma_pending |= HPM_SPI_DMA_TX_DONE;
	}

	if (rx_len != 0U) {
		data->rx_dma_block = (struct dma_block_config){
			.source_address = (uint32_t)&config->base->DATA,
			.dest_address = (uint32_t)rx_buf,
			.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
			.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT,
			.block_size = rx_len,
		};
		dma_cfg = (struct dma_config){0};
		dma_cfg.channel_direction = PERIPHERAL_TO_MEMORY;
		dma_cfg.dma_slot = config->rx_dma_request;
		dma_cfg.source_data_size = data_size;
		dma_cfg.dest_data_size = data_size;
		dma_cfg.source_burst_length = data_size;
		dma_cfg.dest_burst_length = data_size;
		dma_cfg.block_count = 1U;
		dma_cfg.head_block = &data->rx_dma_block;
		dma_cfg.user_data = (void *)dev;
		dma_cfg.dma_callback = hpm_spi_dma_rx_callback;
		(void)sys_cache_data_invd_range(rx_buf, rx_len);
		ret = dma_config(config->dma_dev, config->rx_dma_channel, &dma_cfg);
		if (ret < 0) {
			goto error;
		}
		data->dma_pending |= HPM_SPI_DMA_RX_DONE;
	}

	spi_set_tx_fifo_threshold(config->base, 0U);
	spi_set_rx_fifo_threshold(config->base, 0U);
	spi_clear_interrupt_status(config->base, UINT32_MAX);
	spi_enable_interrupt(config->base,
			     spi_end_int | spi_rx_fifo_overflow_int | spi_tx_fifo_underflow_int);

	data->dma_active = true;
	spi_context_cs_control(&data->ctx, true);
	if (tx_len != 0U) {
		ret = dma_start(config->dma_dev, config->tx_dma_channel);
		if (ret < 0) {
			goto error_active;
		}
	}
	if (rx_len != 0U) {
		ret = dma_start(config->dma_dev, config->rx_dma_channel);
		if (ret < 0) {
			goto error_active;
		}
	}

	ret = spi_setup_dma_transfer(config->base, &control_config, NULL, NULL, frames, frames);
	if (ret != status_success) {
		ret = -EIO;
		goto error_active;
	}
	return 0;

error_active:
	(void)k_work_cancel_delayable(&data->dma_timeout_work);
	spi_context_cs_control(&data->ctx, false);
	data->dma_active = false;
	spi_disable_interrupt(config->base,
			      spi_end_int | spi_rx_fifo_overflow_int | spi_tx_fifo_underflow_int);
	(void)dma_stop(config->dma_dev, config->tx_dma_channel);
	(void)dma_stop(config->dma_dev, config->rx_dma_channel);
	spi_disable_tx_dma(config->base);
	spi_disable_rx_dma(config->base);
error:
	(void)k_work_cancel_delayable(&data->dma_timeout_work);
	data->dma_tx_buf = NULL;
	data->dma_rx_buf = NULL;
	data->dma_rx_len = 0U;
	spi_context_unlock_unconditionally(&data->ctx);
	return ret;
}
#endif /* CONFIG_SPI_HPMICRO_DMA */

static void hpm_spi_async_work_handler(struct k_work *work)
{
	struct hpm_spi_data *data = CONTAINER_OF(work, struct hpm_spi_data, async_work);
	const struct device *dev = data->dev;
	spi_callback_t callback = data->async_callback;
	void *userdata = data->async_userdata;
	int ret;

	ret = hpm_spi_transceive(dev, data->async_config, data->async_tx_bufs, data->async_rx_bufs);

	data->async_config = NULL;
	data->async_tx_bufs = NULL;
	data->async_rx_bufs = NULL;
	data->async_callback = NULL;
	data->async_userdata = NULL;
	data->async_result = ret;
	atomic_clear(&data->async_busy);

	if (callback != NULL) {
		callback(dev, ret, userdata);
	}
}

static int hpm_spi_transceive_async(const struct device *dev, const struct spi_config *spi_cfg,
				    const struct spi_buf_set *tx_bufs,
				    const struct spi_buf_set *rx_bufs, spi_callback_t callback,
				    void *userdata)
{
	struct hpm_spi_data *data = dev->data;
	int ret;

#ifdef CONFIG_SPI_HPMICRO_DMA
	if (hpm_spi_async_dma_supported(dev, spi_cfg, tx_bufs, rx_bufs)) {
		if (!atomic_cas(&data->async_busy, 0, 1)) {
			return -EBUSY;
		}
		ret = hpm_spi_transceive_dma_async(dev, spi_cfg, tx_bufs, rx_bufs, callback,
						   userdata);
		if (ret < 0) {
			atomic_clear(&data->async_busy);
		}
		return ret;
	}
#endif

	if (!atomic_cas(&data->async_busy, 0, 1)) {
		return -EBUSY;
	}

	data->async_config = spi_cfg;
	data->async_tx_bufs = tx_bufs;
	data->async_rx_bufs = rx_bufs;
	data->async_callback = callback;
	data->async_userdata = userdata;

	ret = k_work_submit(&data->async_work);
	if (ret < 0) {
		data->async_config = NULL;
		data->async_tx_bufs = NULL;
		data->async_rx_bufs = NULL;
		data->async_callback = NULL;
		data->async_userdata = NULL;
		data->async_result = -EIO;
		atomic_clear(&data->async_busy);
	}

	return ret < 0 ? ret : 0;
}
#endif /* CONFIG_SPI_ASYNC */

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

#ifdef CONFIG_SPI_ASYNC
	data->dev = dev;
	k_work_init(&data->async_work, hpm_spi_async_work_handler);
#endif
#ifdef CONFIG_SPI_HPMICRO_DMA
	if (config->dma_dev != NULL) {
		uint32_t tx_filter = BIT(config->tx_dma_channel);
		uint32_t rx_filter = BIT(config->rx_dma_channel);
		int channel;

		if (!device_is_ready(config->dma_dev)) {
			return -ENODEV;
		}
		config->irq_config_func(dev);
		k_work_init_delayable(&data->dma_timeout_work, hpm_spi_dma_timeout_handler);
		channel = dma_request_channel(config->dma_dev, &tx_filter);
		if (channel < 0) {
			return channel;
		}
		channel = dma_request_channel(config->dma_dev, &rx_filter);
		if (channel < 0) {
			dma_release_channel(config->dma_dev, config->tx_dma_channel);
			return channel;
		}
	}
#endif

	ret = spi_context_cs_configure_all(&data->ctx);
	if (ret < 0) {
		return ret;
	}

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static DEVICE_API(spi, hpm_spi_api) = {
	.transceive = hpm_spi_transceive,
#ifdef CONFIG_SPI_ASYNC
	.transceive_async = hpm_spi_transceive_async,
#endif
	.release = hpm_spi_release,
};

#ifdef CONFIG_SPI_HPMICRO_DMA
#define HPM_SPI_DMA_CONFIG(n)                                                                      \
	.dma_dev = COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, tx),                                       \
			       (DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(n, tx))), (NULL)),                          \
		 .tx_dma_channel = COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, tx),                                 \
				      (DT_INST_DMAS_CELL_BY_NAME(n, tx, channel)), (0)),                           \
			  .rx_dma_channel = COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, rx),                                 \
				      (DT_INST_DMAS_CELL_BY_NAME(n, rx, channel)), (0)),                  \
				   .tx_dma_request = COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, tx),                                 \
				      (DT_INST_DMAS_CELL_BY_NAME(n, tx, request)), (0)),         \
					    .rx_dma_request = COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, rx),                                 \
				      (DT_INST_DMAS_CELL_BY_NAME(n, rx, request)), (0)),                                         \
						     .irq_config_func = hpm_spi_irq_config_##n,

#define HPM_SPI_IRQ_CONFIG(n)                                                                      \
	static void hpm_spi_irq_config_##n(const struct device *dev)                               \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_spi_isr,                \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}
#else
#define HPM_SPI_DMA_CONFIG(n)
#define HPM_SPI_IRQ_CONFIG(n)
#endif

#define HPM_SPI_INIT(n)                                                                            \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	HPM_SPI_IRQ_CONFIG(n)                                                                      \
                                                                                                   \
	static const struct hpm_spi_config hpm_spi_config_##n = {                                  \
		.base = (SPI_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.clock_name = DT_INST_PROP(n, clock_name),                                         \
		.clock_src = DT_INST_PROP(n, clock_src),                                           \
		.clock_div = DT_INST_CLOCKS_CELL_BY_IDX(n, 0, divider),                            \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
		HPM_SPI_DMA_CONFIG(n)};                                                            \
                                                                                                   \
	static struct hpm_spi_data hpm_spi_data_##n = {                                            \
		SPI_CONTEXT_INIT_LOCK(hpm_spi_data_##n, ctx),                                      \
		SPI_CONTEXT_INIT_SYNC(hpm_spi_data_##n, ctx),                                      \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(n), ctx)};                             \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_spi_init, NULL, &hpm_spi_data_##n, &hpm_spi_config_##n,       \
			      POST_KERNEL, CONFIG_SPI_INIT_PRIORITY, &hpm_spi_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_SPI_INIT)
