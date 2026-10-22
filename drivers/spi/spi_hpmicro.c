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

#include <hpm_clock_drv.h>
#include <hpm_spi_drv.h>

#define HPM_SPI_MAX_WORD_SIZE 32U

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
	struct k_mutex lock;
};

static int hpm_spi_status_to_errno(hpm_stat_t status)
{
	switch (status) {
	case status_success:
		return 0;
	case status_invalid_argument:
		return -EINVAL;
	case status_timeout:
		return -ETIMEDOUT;
	case status_spi_master_busy:
		return -EBUSY;
	default:
		return -EIO;
	}
}

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

	if ((spi_cfg->operation & (SPI_HALF_DUPLEX | SPI_FRAME_FORMAT_TI)) != 0U) {
		return -ENOTSUP;
	}

	if ((spi_cfg->operation & SPI_LINES_MASK) != SPI_LINES_SINGLE) {
		return -ENOTSUP;
	}

	if (spi_cfg->frequency == 0U) {
		return -EINVAL;
	}

	if ((spi_cfg->operation & SPI_HOLD_ON_CS) != 0U) {
		return -ENOTSUP;
	}

	if ((spi_cfg->operation & SPI_LOCK_ON) != 0U) {
		return -ENOTSUP;
	}

	if ((spi_cfg->operation & SPI_CS_ACTIVE_HIGH) != 0U) {
		return -ENOTSUP;
	}

	if (spi_cfg->cs.cs_is_gpio) {
		return -ENOTSUP;
	}

	if ((spi_cfg->peripheral > 3U) || (spi_cfg->cs.setup_ns != 0U) ||
	    (spi_cfg->cs.hold_ns != 0U)) {
		return -ENOTSUP;
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
	spi_format_init(config->base, &format_config);

	return 0;
}

static int hpm_spi_transceive(const struct device *dev, const struct spi_config *spi_cfg,
			      const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs)
{
	const struct hpm_spi_config *config = dev->config;
	struct hpm_spi_data *data = dev->data;
	spi_control_config_t control_config = {0};
	const struct spi_buf *tx = NULL;
	const struct spi_buf *rx = NULL;
	hpm_stat_t status;
	uint32_t tx_len = 0U;
	uint32_t rx_len = 0U;
	int ret;

	if ((tx_bufs == NULL || tx_bufs->count == 0U) &&
	    (rx_bufs == NULL || rx_bufs->count == 0U)) {
		return -EINVAL;
	}

	if ((tx_bufs != NULL) && (tx_bufs->count > 1U)) {
		return -ENOTSUP;
	}

	if ((rx_bufs != NULL) && (rx_bufs->count > 1U)) {
		return -ENOTSUP;
	}

	if (tx_bufs != NULL) {
		tx = &tx_bufs->buffers[0];
		tx_len = tx->len;
	}

	if (rx_bufs != NULL) {
		rx = &rx_bufs->buffers[0];
		rx_len = rx->len;
	}

	if ((tx != NULL) && (tx_len > 0U) && (tx->buf == NULL)) {
		return -EINVAL;
	}

	if ((rx != NULL) && (rx_len > 0U) && (rx->buf == NULL)) {
		return -EINVAL;
	}

	if ((tx_len == 0U) && (rx_len == 0U)) {
		return 0;
	}

	if ((tx_len > 0U) && (rx_len > 0U) && (tx_len != rx_len)) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = hpm_spi_configure(dev, spi_cfg);
	if (ret < 0) {
		goto unlock;
	}

	spi_master_get_default_control_config(&control_config);
	control_config.common_config.cs_index = (spi_cs_index_t)BIT(spi_cfg->peripheral);

	if ((tx_len > 0U) && (rx_len > 0U)) {
		control_config.common_config.trans_mode = spi_trans_write_read_together;
	} else if (tx_len > 0U) {
		control_config.common_config.trans_mode = spi_trans_write_only;
	} else {
		control_config.common_config.trans_mode = spi_trans_read_only;
	}

	status =
		spi_transfer(config->base, &control_config, NULL, NULL, tx != NULL ? tx->buf : NULL,
			     tx_len, rx != NULL ? rx->buf : NULL, rx_len);
	ret = hpm_spi_status_to_errno(status);

unlock:
	k_mutex_unlock(&data->lock);

	return ret;
}

static int hpm_spi_release(const struct device *dev, const struct spi_config *spi_cfg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(spi_cfg);

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

	k_mutex_init(&data->lock);

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
	static struct hpm_spi_data hpm_spi_data_##n;                                               \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_spi_init, NULL, &hpm_spi_data_##n, &hpm_spi_config_##n,       \
			      POST_KERNEL, CONFIG_SPI_INIT_PRIORITY, &hpm_spi_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_SPI_INIT)
