/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_i2c

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#include <drivers/i2c/hpmicro_i2c.h>

#include <hpm_clock_drv.h>
#include <hpm_i2c_drv.h>

struct hpm_i2c_config {
	I2C_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	clock_name_t clock_name;
	clock_source_t clock_src;
	uint32_t clock_div;
	uint32_t bitrate;
	const struct pinctrl_dev_config *pincfg;
#if defined(CONFIG_I2C_TARGET) || defined(CONFIG_I2C_HPMICRO_DMA)
	void (*irq_config_func)(const struct device *dev);
	uint32_t irq;
#endif
#ifdef CONFIG_I2C_HPMICRO_DMA
	const struct device *dma_dev;
	uint32_t tx_dma_channel;
	uint32_t rx_dma_channel;
	uint32_t dma_request;
#endif
};

struct hpm_i2c_data {
	struct k_mutex lock;
	uint32_t dev_config;
#ifdef CONFIG_I2C_TARGET
	struct i2c_target_config *target;
	bool target_selected;
	bool target_write_active;
	bool target_read_active;
#endif
#ifdef CONFIG_I2C_HPMICRO_DMA
	struct k_sem dma_done;
	struct k_sem i2c_done;
	struct dma_block_config dma_block;
	volatile bool dma_active;
	volatile bool dma_error;
	volatile bool dma_transfer_active;
#endif
};

static int hpm_i2c_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t num_msgs,
			    uint16_t addr);

static int hpm_i2c_status_to_errno(hpm_stat_t status)
{
	switch (status) {
	case status_success:
		return 0;
	case status_invalid_argument:
		return -EINVAL;
	case status_timeout:
		return -ETIMEDOUT;
	case status_i2c_no_addr_hit:
		return -ENXIO;
	case status_i2c_no_ack:
		return -EIO;
	case status_i2c_bus_busy:
		return -EBUSY;
	case status_i2c_not_supported:
		return -ENOTSUP;
	default:
		return -EIO;
	}
}

static int hpm_i2c_speed_to_mode(uint32_t bitrate, uint8_t *mode)
{
	switch (bitrate) {
	case 100000U:
		*mode = i2c_mode_normal;
		return 0;
	case 400000U:
		*mode = i2c_mode_fast;
		return 0;
	case 1000000U:
		*mode = i2c_mode_fast_plus;
		return 0;
	default:
		return -EINVAL;
	}
}

static int hpm_i2c_speed_to_bitrate(uint32_t speed, uint32_t *bitrate)
{
	switch (speed) {
	case I2C_SPEED_STANDARD:
		*bitrate = 100000U;
		return 0;
	case I2C_SPEED_FAST:
		*bitrate = 400000U;
		return 0;
	case I2C_SPEED_FAST_PLUS:
		*bitrate = 1000000U;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static uint32_t hpm_i2c_mode_to_speed(uint8_t mode)
{
	switch (mode) {
	case i2c_mode_normal:
		return I2C_SPEED_STANDARD;
	case i2c_mode_fast:
		return I2C_SPEED_FAST;
	case i2c_mode_fast_plus:
		return I2C_SPEED_FAST_PLUS;
	default:
		return 0U;
	}
}

static int hpm_i2c_master_reinit(const struct device *dev, uint32_t dev_config)
{
	const struct hpm_i2c_config *config = dev->config;
	i2c_config_t hal_config = {0};
	uint32_t bitrate;
	uint32_t clock_freq;
	int ret;

	ret = clock_control_get_rate(config->clock_dev, config->clock_subsys, &clock_freq);
	if (ret < 0) {
		return ret;
	}

	ret = hpm_i2c_speed_to_bitrate(I2C_SPEED_GET(dev_config), &bitrate);
	if (ret < 0) {
		return ret;
	}

	ret = hpm_i2c_speed_to_mode(bitrate, &hal_config.i2c_mode);
	if (ret < 0) {
		return ret;
	}

	hal_config.is_10bit_addressing = (dev_config & I2C_ADDR_10_BITS) != 0U;
	return i2c_init_master(config->base, clock_freq, &hal_config) == status_success ? 0 : -EIO;
}

static int hpm_i2c_configure(const struct device *dev, uint32_t dev_config)
{
	struct hpm_i2c_data *data = dev->data;
	int ret;

	if ((dev_config & I2C_MODE_CONTROLLER) == 0U) {
		return -ENOTSUP;
	}

#ifdef CONFIG_I2C_TARGET
	if (data->target != NULL) {
		return -EBUSY;
	}
#endif

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = hpm_i2c_master_reinit(dev, dev_config);
	if (ret < 0) {
		k_mutex_unlock(&data->lock);
		return ret;
	}

	data->dev_config = dev_config;
	k_mutex_unlock(&data->lock);

	return 0;
}

static int hpm_i2c_get_config(const struct device *dev, uint32_t *dev_config)
{
	const struct hpm_i2c_data *data = dev->data;

	if (dev_config == NULL) {
		return -EINVAL;
	}

	*dev_config = data->dev_config;

	return 0;
}

static bool hpm_i2c_msg_is_10bit(const struct hpm_i2c_data *data, const struct i2c_msg *msg)
{
	return ((data->dev_config & I2C_ADDR_10_BITS) != 0U) ||
	       ((msg->flags & I2C_MSG_ADDR_10_BITS) != 0U);
}

#ifdef CONFIG_I2C_HPMICRO_DMA
static void hpm_i2c_dma_callback(const struct device *dma_dev, void *user_data, uint32_t channel,
				 int status)
{
	struct hpm_i2c_data *data = user_data;

	ARG_UNUSED(dma_dev);
	ARG_UNUSED(channel);

	data->dma_error = status < 0;
	data->dma_active = false;
	k_sem_give(&data->dma_done);
}

static int hpm_i2c_dma_start(const struct device *dev, const struct i2c_msg *msg)
{
	const struct hpm_i2c_config *config = dev->config;
	struct hpm_i2c_data *data = dev->data;
	struct dma_config dma_cfg = {0};
	uint32_t channel;
	int ret;

	channel = i2c_is_read_op(msg) ? config->rx_dma_channel : config->tx_dma_channel;
	data->dma_block = (struct dma_block_config){
		.source_address =
			i2c_is_read_op(msg) ? (uint32_t)&config->base->DATA : (uint32_t)msg->buf,
		.dest_address =
			i2c_is_read_op(msg) ? (uint32_t)msg->buf : (uint32_t)&config->base->DATA,
		.source_addr_adj =
			i2c_is_read_op(msg) ? DMA_ADDR_ADJ_NO_CHANGE : DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj =
			i2c_is_read_op(msg) ? DMA_ADDR_ADJ_INCREMENT : DMA_ADDR_ADJ_NO_CHANGE,
		.block_size = msg->len,
	};
	dma_cfg.channel_direction =
		i2c_is_read_op(msg) ? PERIPHERAL_TO_MEMORY : MEMORY_TO_PERIPHERAL;
	dma_cfg.dma_slot = config->dma_request;
	dma_cfg.source_data_size = 1U;
	dma_cfg.dest_data_size = 1U;
	dma_cfg.block_count = 1U;
	dma_cfg.head_block = &data->dma_block;
	dma_cfg.user_data = data;
	dma_cfg.dma_callback = hpm_i2c_dma_callback;

	if (i2c_is_read_op(msg)) {
		(void)sys_cache_data_invd_range(msg->buf, msg->len);
	} else {
		(void)sys_cache_data_flush_range(msg->buf, msg->len);
	}

	k_sem_reset(&data->dma_done);
	data->dma_error = false;
	data->dma_active = true;
	ret = dma_config(config->dma_dev, channel, &dma_cfg);
	if (ret < 0) {
		data->dma_active = false;
		return ret;
	}
	ret = dma_start(config->dma_dev, channel);
	if (ret < 0) {
		data->dma_active = false;
		return ret;
	}
	return 0;
}

static void hpm_i2c_dma_stop(const struct device *dev, bool read)
{
	const struct hpm_i2c_config *config = dev->config;
	struct hpm_i2c_data *data = dev->data;

	if (data->dma_active) {
		(void)dma_stop(config->dma_dev,
			       read ? config->rx_dma_channel : config->tx_dma_channel);
		data->dma_active = false;
	}
}

static void hpm_i2c_dma_abort(const struct device *dev, bool read)
{
	const struct hpm_i2c_config *config = dev->config;
	struct hpm_i2c_data *data = dev->data;

	i2c_disable_irq(config->base, I2C_EVENT_TRANSACTION_COMPLETE | I2C_EVENT_LOSS_ARBITRATION);
	hpm_i2c_dma_stop(dev, read);
	i2c_dma_disable(config->base);
	i2c_reset(config->base);
	(void)hpm_i2c_master_reinit(dev, data->dev_config);
}

static int hpm_i2c_transfer_frame_dma(const struct device *dev, uint16_t addr,
				      const struct i2c_msg *msg)
{
	const struct hpm_i2c_config *config = dev->config;
	struct hpm_i2c_data *data = dev->data;
	bool read = i2c_is_read_op(msg);
	hpm_stat_t status;
	int ret;

	if ((msg->buf == NULL) || (msg->len == 0U) || (msg->len > I2C_SOC_TRANSFER_COUNT_MAX)) {
		return -EINVAL;
	}

	ret = hpm_i2c_dma_start(dev, msg);
	if (ret < 0) {
		return ret;
	}

	/* DMA must be armed before the controller is told to consume its request. */
	k_sem_reset(&data->i2c_done);
	data->dma_transfer_active = true;
	i2c_clear_status(config->base, I2C_STATUS_CMPL_MASK);
	i2c_enable_irq(config->base, I2C_EVENT_TRANSACTION_COMPLETE | I2C_EVENT_LOSS_ARBITRATION);
	status = read ? i2c_master_start_dma_read(config->base, addr, msg->len)
		      : i2c_master_start_dma_write(config->base, addr, msg->len);
	if (status != status_success) {
		data->dma_transfer_active = false;
		hpm_i2c_dma_abort(dev, read);
		return hpm_i2c_status_to_errno(status);
	}

	if (k_sem_take(&data->dma_done, K_MSEC(CONFIG_I2C_HPMICRO_DMA_TIMEOUT_MS)) < 0) {
		data->dma_transfer_active = false;
		hpm_i2c_dma_abort(dev, read);
		return -ETIMEDOUT;
	}
	if (k_sem_take(&data->i2c_done, K_MSEC(CONFIG_I2C_HPMICRO_DMA_TIMEOUT_MS)) < 0) {
		data->dma_transfer_active = false;
		hpm_i2c_dma_abort(dev, read);
		return -ETIMEDOUT;
	}
	data->dma_transfer_active = false;
	i2c_disable_irq(config->base, I2C_EVENT_TRANSACTION_COMPLETE | I2C_EVENT_LOSS_ARBITRATION);
	if (data->dma_error) {
		hpm_i2c_dma_abort(dev, read);
		return -EIO;
	}
	if (!read && !I2C_STATUS_ACK_GET(i2c_get_status(config->base))) {
		hpm_i2c_dma_abort(dev, read);
		return -EIO;
	}

	i2c_clear_status(config->base, I2C_STATUS_CMPL_MASK);
	i2c_dma_disable(config->base);

	if (read) {
		(void)sys_cache_data_invd_range(msg->buf, msg->len);
	}
	return 0;
}
#endif /* CONFIG_I2C_HPMICRO_DMA */

static int hpm_i2c_transfer_frame(const struct hpm_i2c_config *config,
				  const struct hpm_i2c_data *data, uint16_t addr,
				  const struct i2c_msg *msg)
{
	uint16_t flags = I2C_WRITE_CHECK_ACK;

	if ((msg->len == 0U) || (msg->len > I2C_SOC_TRANSFER_COUNT_MAX)) {
		return -EINVAL;
	}

	if (i2c_is_read_op(msg)) {
		flags = I2C_RD;
	}
	if (!i2c_is_stop_op(msg)) {
		flags |= I2C_NO_STOP;
	}
	if (hpm_i2c_msg_is_10bit(data, msg)) {
		flags |= I2C_ADDR_10BIT;
	}

	return hpm_i2c_status_to_errno(
		i2c_master_transfer(config->base, addr, msg->buf, msg->len, flags));
}

static int hpm_i2c_smbus_quick_impl(const struct device *dev, uint16_t addr, bool read)
{
	const struct hpm_i2c_config *config = dev->config;
	struct hpm_i2c_data *data = dev->data;
	uint32_t retry = 0U;
	uint32_t ctrl;
	int ret = 0;

	if (addr > BIT_MASK(7)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
#ifdef CONFIG_I2C_TARGET
	if (data->target != NULL) {
		ret = -EBUSY;
		goto unlock;
	}
#endif

	while ((config->base->STATUS & I2C_STATUS_BUSBUSY_MASK) != 0U) {
		if (retry++ > 5000U) {
			ret = -EBUSY;
			goto unlock;
		}
	}

	i2c_clear_status(config->base, I2C_STATUS_CMPL_MASK);
	i2c_clear_fifo(config->base);
	i2c_master_set_slave_address(config->base, addr);
	ctrl = I2C_CTRL_PHASE_START_SET(true) | I2C_CTRL_PHASE_ADDR_SET(true) |
	       I2C_CTRL_PHASE_DATA_SET(false) | I2C_CTRL_PHASE_STOP_SET(true) |
	       I2C_CTRL_DIR_SET(read ? I2C_DIR_MASTER_READ : I2C_DIR_MASTER_WRITE);
	config->base->CTRL = ctrl;
	i2c_master_issue_data_transmission(config->base);

	retry = 0U;
	while (!i2c_is_addrhit(config->base)) {
		if (retry++ > 5000U) {
			ret = -ENXIO;
			goto unlock;
		}
	}
	i2c_clear_status(config->base, I2C_STATUS_ADDRHIT_MASK);

	retry = 0U;
	while ((config->base->STATUS & I2C_STATUS_CMPL_MASK) == 0U) {
		if (retry++ > 5000U) {
			ret = -ETIMEDOUT;
			goto unlock;
		}
	}
	i2c_clear_status(config->base, I2C_STATUS_CMPL_MASK);

unlock:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int hpm_i2c_smbus_block_transfer_impl(const struct device *dev, uint16_t addr,
					     const uint8_t *tx_buf, uint8_t tx_len, uint8_t *count,
					     uint8_t *rx_buf, uint8_t *pec)
{
	const struct hpm_i2c_config *config = dev->config;
	struct hpm_i2c_data *data = dev->data;
	uint8_t block[HPM_I2C_SMBUS_BLOCK_BYTES_MAX + 1U];
	uint8_t discard;
	hpm_stat_t status;
#if defined(CONFIG_I2C_TARGET) || defined(CONFIG_I2C_HPMICRO_DMA)
	bool irq_was_enabled;
#endif

	if ((tx_buf == NULL) || (tx_len == 0U) || (count == NULL) || (rx_buf == NULL) ||
	    (addr > BIT_MASK(7))) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
#ifdef CONFIG_I2C_TARGET
	if (data->target != NULL) {
		k_mutex_unlock(&data->lock);
		return -EBUSY;
	}
#endif

#if defined(CONFIG_I2C_TARGET) || defined(CONFIG_I2C_HPMICRO_DMA)
	/* Sequence reads poll the FIFO and manually ACK each byte. */
	irq_was_enabled = irq_is_enabled(config->irq);
	irq_disable(config->irq);
#endif

	status = i2c_master_seq_transmit_check_ack(config->base, addr, (uint8_t *)tx_buf, tx_len,
						   i2c_frist_frame, true);
	if (status != status_success) {
		goto unlock;
	}

	status = i2c_master_seq_receive(config->base, addr, count, 1U, i2c_frist_frame);
	if (status != status_success) {
		goto unlock;
	}

	if (*count > HPM_I2C_SMBUS_BLOCK_BYTES_MAX) {
		/* Count was already ACKed; terminate the read with NACK and STOP. */
		status = i2c_master_seq_receive(config->base, addr, &discard, sizeof(discard),
						i2c_last_frame);
		if (status == status_success) {
			status = status_invalid_argument;
		}
		goto unlock;
	}

	if (*count == 0U) {
		if (pec != NULL) {
			status = i2c_master_seq_receive(config->base, addr, block, 1U,
							i2c_last_frame);
		} else {
			i2c_clear_status(config->base, I2C_STATUS_CMPL_MASK);
			i2c_master_enable_stop_phase(config->base);
			i2c_master_issue_data_transmission(config->base);
			status = status_success;
		}
	} else {
		status = i2c_master_seq_receive(config->base, addr, block, *count,
						pec != NULL ? i2c_next_frame : i2c_last_frame);
		if ((status == status_success) && (pec != NULL)) {
			status = i2c_master_seq_receive(config->base, addr, &block[*count], 1U,
							i2c_last_frame);
		}
	}
	if (status == status_success) {
		memcpy(rx_buf, block, *count);
		if (pec != NULL) {
			*pec = block[*count];
		}
	}

unlock:
#if defined(CONFIG_I2C_TARGET) || defined(CONFIG_I2C_HPMICRO_DMA)
	if (irq_was_enabled) {
		irq_enable(config->irq);
	}
#endif
	k_mutex_unlock(&data->lock);

	if (status == status_invalid_argument) {
		return -ENODATA;
	}

	return hpm_i2c_status_to_errno(status);
}

static int hpm_i2c_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t num_msgs,
			    uint16_t addr)
{
	const struct hpm_i2c_config *config = dev->config;
	struct hpm_i2c_data *data = dev->data;
	int ret;

	if ((msgs == NULL) || (num_msgs == 0U)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
#ifdef CONFIG_I2C_TARGET
	if (data->target != NULL) {
		ret = -EBUSY;
		goto unlock;
	}
#endif

	for (uint8_t i = 0U; i < num_msgs; i++) {
#ifdef CONFIG_I2C_HPMICRO_DMA
		if ((config->dma_dev != NULL) && (num_msgs == 1U) && i2c_is_stop_op(&msgs[i]) &&
		    !hpm_i2c_msg_is_10bit(data, &msgs[i])) {
			ret = hpm_i2c_transfer_frame_dma(dev, addr, &msgs[i]);
		} else
#endif
		{
			ret = hpm_i2c_transfer_frame(config, data, addr, &msgs[i]);
		}
		if (ret < 0) {
			goto unlock;
		}
	}

unlock:
	k_mutex_unlock(&data->lock);

	return ret;
}

static int hpm_i2c_recover_bus(const struct device *dev)
{
	const struct hpm_i2c_config *config = dev->config;
	struct hpm_i2c_data *data = dev->data;
	int ret;

#if defined(HPM_IP_FEATURE_I2C_SUPPORT_RESET) && (HPM_IP_FEATURE_I2C_SUPPORT_RESET == 1)
	k_mutex_lock(&data->lock, K_FOREVER);
#ifdef CONFIG_I2C_TARGET
	if (data->target != NULL) {
		ret = -EBUSY;
		goto unlock;
	}
#endif

	if (!i2c_get_line_scl_status(config->base)) {
		ret = -EBUSY;
		goto unlock;
	}

	if (!i2c_get_line_sda_status(config->base)) {
		i2c_gen_reset_signal(config->base, 9U);
		k_busy_wait(1000U);
	}

	ret = i2c_get_line_sda_status(config->base) ? 0 : -EBUSY;

unlock:
	k_mutex_unlock(&data->lock);
	return ret;
#else
	ARG_UNUSED(config);
	ARG_UNUSED(data);
	return -ENOTSUP;
#endif
}

#ifdef CONFIG_I2C_HPMICRO_DMA
static void hpm_i2c_master_isr(const struct device *dev)
{
	const struct hpm_i2c_config *config = dev->config;
	struct hpm_i2c_data *data = dev->data;
	uint32_t status = i2c_get_status(config->base);

	if ((status & I2C_EVENT_LOSS_ARBITRATION) != 0U) {
		i2c_clear_status(config->base, I2C_EVENT_LOSS_ARBITRATION);
		data->dma_error = true;
		if (data->dma_active) {
			(void)dma_stop(config->dma_dev,
				       i2c_get_direction(config->base) == I2C_DIR_MASTER_READ
					       ? config->rx_dma_channel
					       : config->tx_dma_channel);
			data->dma_active = false;
		}
		k_sem_give(&data->dma_done);
		k_sem_give(&data->i2c_done);
	}

	if ((status & I2C_EVENT_TRANSACTION_COMPLETE) != 0U) {
		i2c_clear_status(config->base, I2C_EVENT_TRANSACTION_COMPLETE);
		k_sem_give(&data->i2c_done);
	}
}
#endif /* CONFIG_I2C_HPMICRO_DMA */

#ifdef CONFIG_I2C_TARGET
static void hpm_i2c_target_end_transfer(struct hpm_i2c_data *data)
{
	data->target_write_active = false;
	data->target_read_active = false;
}

static void hpm_i2c_target_reset_state(struct hpm_i2c_data *data)
{
	data->target_selected = false;
	hpm_i2c_target_end_transfer(data);
}

static void hpm_i2c_target_drain_rx(const struct device *dev)
{
	const struct hpm_i2c_config *config = dev->config;
	struct hpm_i2c_data *data = dev->data;

	if (!data->target_write_active) {
		return;
	}

	while (!i2c_fifo_is_empty(config->base)) {
		uint8_t value = i2c_read_byte(config->base);

		if (data->target_write_active && (data->target != NULL) &&
		    (data->target->callbacks->write_received != NULL)) {
			(void)data->target->callbacks->write_received(data->target, value);
		}
	}
}

static void hpm_i2c_target_complete(const struct device *dev)
{
	const struct hpm_i2c_config *config = dev->config;
	struct hpm_i2c_data *data = dev->data;

	hpm_i2c_target_drain_rx(dev);

	i2c_disable_irq(config->base, I2C_EVENT_FIFO_FULL | I2C_EVENT_FIFO_EMPTY);
	hpm_i2c_target_end_transfer(data);
}

static void hpm_i2c_target_isr(const struct device *dev)
{
	const struct hpm_i2c_config *config = dev->config;
	struct hpm_i2c_data *data = dev->data;
	uint32_t status = i2c_get_status(config->base);
	bool stop = ((status & I2C_EVENT_STOP_CONDITION) != 0U) &&
		    ((status & I2C_STATUS_BUS_BUSY) == 0U);

	if ((status & I2C_EVENT_LOSS_ARBITRATION) != 0U) {
		i2c_clear_status(config->base, I2C_EVENT_LOSS_ARBITRATION);
		hpm_i2c_target_reset_state(data);
		if ((data->target != NULL) && (data->target->callbacks->error != NULL)) {
			data->target->callbacks->error(data->target, I2C_ERROR_ARBITRATION);
		}
		return;
	}

	if ((status & I2C_EVENT_ADDRESS_HIT) != 0U) {
		uint8_t direction = i2c_get_direction(config->base);
		int ret;

		i2c_clear_status(config->base, I2C_EVENT_ADDRESS_HIT);
		/* Completion/STOP are latched across a repeated START on this block. */
		i2c_clear_status(config->base, I2C_EVENT_TRANSACTION_COMPLETE |
						       I2C_EVENT_START_CONDITION |
						       I2C_EVENT_STOP_CONDITION);
		if (data->target == NULL) {
			return;
		}

		hpm_i2c_target_end_transfer(data);
		data->target_selected = true;
		if (direction == I2C_DIR_SLAVE_READ) {
			/* The master writes to this target. */
			data->target_write_active = true;
			ret = data->target->callbacks->write_requested != NULL
				      ? data->target->callbacks->write_requested(data->target)
				      : 0;
			if (ret < 0) {
				i2c_respond_Nack(config->base);
				hpm_i2c_target_reset_state(data);
			} else {
				i2c_enable_irq(config->base, I2C_EVENT_FIFO_FULL);
			}
		} else {
			/* The master reads from this target. */
			uint8_t value = 0U;

			data->target_read_active = true;
			ret = data->target->callbacks->read_requested != NULL
				      ? data->target->callbacks->read_requested(data->target,
										&value)
				      : -EINVAL;
			if (ret < 0) {
				i2c_respond_Nack(config->base);
				hpm_i2c_target_reset_state(data);
			} else {
				i2c_write_byte(config->base, value);
				i2c_enable_irq(config->base, I2C_EVENT_FIFO_EMPTY);
			}
		}
		return;
	}

	if ((status & I2C_EVENT_FIFO_FULL) != 0U) {
		while (!i2c_fifo_is_empty(config->base)) {
			uint8_t value = i2c_read_byte(config->base);

			if ((data->target != NULL) &&
			    (data->target->callbacks->write_received != NULL) &&
			    (data->target->callbacks->write_received(data->target, value) < 0)) {
				i2c_respond_Nack(config->base);
				i2c_disable_irq(config->base, I2C_EVENT_FIFO_FULL);
				data->target_write_active = false;
				break;
			}
		}
	}

	if ((status & I2C_EVENT_FIFO_EMPTY) != 0U) {
		uint8_t value = 0U;
		int ret;

		if ((data->target != NULL) && data->target_read_active) {
			ret = data->target->callbacks->read_processed != NULL
				      ? data->target->callbacks->read_processed(data->target,
										&value)
				      : -EINVAL;
			if (ret < 0) {
				i2c_respond_Nack(config->base);
				i2c_disable_irq(config->base, I2C_EVENT_FIFO_EMPTY);
				data->target_read_active = false;
			} else {
				i2c_write_byte(config->base, value);
			}
		}
	}

	if ((status & I2C_EVENT_TRANSACTION_COMPLETE) != 0U) {
		i2c_clear_status(config->base, I2C_EVENT_TRANSACTION_COMPLETE);
		if (!stop) {
			hpm_i2c_target_drain_rx(dev);
		} else {
			hpm_i2c_target_complete(dev);
		}
	}

	if ((status & I2C_EVENT_STOP_CONDITION) != 0U) {
		bool selected = data->target_selected;

		i2c_clear_status(config->base, I2C_EVENT_STOP_CONDITION);
		if (!stop) {
			/* A stale STOP status can accompany a repeated START. */
			i2c_clear_status(config->base, I2C_EVENT_STOP_CONDITION);
		} else if (data->target_write_active || data->target_read_active) {
			hpm_i2c_target_complete(dev);
		}
		if (stop && selected && (data->target != NULL) &&
		    (data->target->callbacks->stop != NULL)) {
			(void)data->target->callbacks->stop(data->target);
		}
		if (stop) {
			data->target_selected = false;
		}
	}
}

static int hpm_i2c_target_register(const struct device *dev, struct i2c_target_config *target)
{
	const struct hpm_i2c_config *config = dev->config;
	struct hpm_i2c_data *data = dev->data;
	i2c_config_t hal_config = {0};
	uint32_t clock_freq;
	uint8_t mode;
	int ret;

	if ((target == NULL) || (target->callbacks == NULL)) {
		return -EINVAL;
	}

	if (target->address >
	    ((target->flags & I2C_TARGET_FLAGS_ADDR_10_BITS) != 0U ? BIT_MASK(10) : BIT_MASK(7))) {
		return -EINVAL;
	}

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_get_rate(config->clock_dev, config->clock_subsys, &clock_freq);
	if (ret < 0) {
		return ret;
	}

	ret = hpm_i2c_speed_to_mode(config->bitrate, &mode);
	if (ret < 0) {
		return ret;
	}

	hal_config.is_10bit_addressing = (target->flags & I2C_TARGET_FLAGS_ADDR_10_BITS) != 0U;
	hal_config.i2c_mode = mode;

	k_mutex_lock(&data->lock, K_FOREVER);
	if ((data->target != NULL) || data->target_selected || data->target_write_active ||
	    data->target_read_active) {
		ret = -EBUSY;
		goto unlock;
	}

	if (i2c_init_slave(config->base, clock_freq, &hal_config, target->address) !=
	    status_success) {
		ret = -EIO;
		goto unlock;
	}

	data->target = target;
	hpm_i2c_target_reset_state(data);
	i2c_disable_irq(config->base, I2C_EVENT_ALL_MASK);
	i2c_clear_status(config->base, I2C_EVENT_ALL_MASK);
	irq_enable(config->irq);
	i2c_enable_irq(config->base, I2C_EVENT_ADDRESS_HIT | I2C_EVENT_TRANSACTION_COMPLETE |
					     I2C_EVENT_STOP_CONDITION | I2C_EVENT_LOSS_ARBITRATION);
	ret = 0;

unlock:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int hpm_i2c_target_unregister(const struct device *dev, struct i2c_target_config *target)
{
	const struct hpm_i2c_config *config = dev->config;
	struct hpm_i2c_data *data = dev->data;
	i2c_config_t hal_config = {0};
	uint32_t clock_freq;
	uint8_t mode;
	int ret = 0;

	if (target == NULL) {
		return -EINVAL;
	}

	ret = hpm_i2c_speed_to_mode(config->bitrate, &mode);
	if (ret < 0) {
		return ret;
	}

	ret = clock_control_get_rate(config->clock_dev, config->clock_subsys, &clock_freq);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	if (data->target != target) {
		ret = -EINVAL;
		goto unlock;
	}

	irq_disable(config->irq);
	i2c_disable_irq(config->base, I2C_EVENT_ALL_MASK);
	data->target = NULL;
	hpm_i2c_target_reset_state(data);
	i2c_reset(config->base);
	hal_config.i2c_mode = mode;
	hal_config.is_10bit_addressing = false;
	if (i2c_init_master(config->base, clock_freq, &hal_config) != status_success) {
		ret = -EIO;
	} else {
		data->dev_config = I2C_MODE_CONTROLLER | I2C_SPEED_SET(hpm_i2c_mode_to_speed(mode));
	}

unlock:
	k_mutex_unlock(&data->lock);
	return ret;
}
#endif /* CONFIG_I2C_TARGET */

#if defined(CONFIG_I2C_TARGET) || defined(CONFIG_I2C_HPMICRO_DMA)
static void hpm_i2c_isr(const struct device *dev)
{
	const struct hpm_i2c_config *config = dev->config;
	const struct hpm_i2c_data *data = dev->data;

#ifdef CONFIG_I2C_HPMICRO_DMA
	if (data->dma_transfer_active) {
		hpm_i2c_master_isr(dev);
		return;
	}
#endif
#ifdef CONFIG_I2C_TARGET
	if (data->target != NULL) {
		hpm_i2c_target_isr(dev);
		return;
	}
#endif

	/* Polling master transfers may briefly enable an IRQ for SDK flow control. */
	i2c_disable_irq(config->base, I2C_EVENT_ALL_MASK);
	i2c_clear_status(config->base, I2C_EVENT_ALL_MASK);
}
#endif

static int hpm_i2c_init(const struct device *dev)
{
	const struct hpm_i2c_config *config = dev->config;
	struct hpm_i2c_data *data = dev->data;
	uint8_t mode;
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

	ret = hpm_i2c_speed_to_mode(config->bitrate, &mode);
	if (ret < 0) {
		return ret;
	}

	k_mutex_init(&data->lock);
#if defined(CONFIG_I2C_TARGET) || defined(CONFIG_I2C_HPMICRO_DMA)
	config->irq_config_func(dev);
#endif
#ifdef CONFIG_I2C_HPMICRO_DMA
	if (config->dma_dev != NULL) {
		uint32_t tx_filter = BIT(config->tx_dma_channel);
		uint32_t rx_filter = BIT(config->rx_dma_channel);
		int channel;

		if (!device_is_ready(config->dma_dev)) {
			return -ENODEV;
		}
		channel = dma_request_channel(config->dma_dev, &tx_filter);
		if (channel < 0) {
			return channel;
		}
		channel = dma_request_channel(config->dma_dev, &rx_filter);
		if (channel < 0) {
			dma_release_channel(config->dma_dev, config->tx_dma_channel);
			return channel;
		}
		k_sem_init(&data->dma_done, 0, 1);
		k_sem_init(&data->i2c_done, 0, 1);
	}
#endif

	return hpm_i2c_configure(dev,
				 I2C_MODE_CONTROLLER | I2C_SPEED_SET(hpm_i2c_mode_to_speed(mode)));
}

static DEVICE_API(hpm_i2c, hpm_i2c_api) = {
	.i2c_api =
		{
			.configure = hpm_i2c_configure,
			.get_config = hpm_i2c_get_config,
			.transfer = hpm_i2c_transfer,
			.recover_bus = hpm_i2c_recover_bus,
#ifdef CONFIG_I2C_TARGET
			.target_register = hpm_i2c_target_register,
			.target_unregister = hpm_i2c_target_unregister,
#endif
		},
	.smbus_quick = hpm_i2c_smbus_quick_impl,
	.smbus_block_transfer = hpm_i2c_smbus_block_transfer_impl,
};

#ifdef CONFIG_I2C_HPMICRO_DMA
#define HPM_I2C_DMA_CONFIG(n)                                                                      \
	.dma_dev = COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, tx),                                       \
			       (DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(n, tx))), (NULL)),                          \
		 .tx_dma_channel = COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, tx),                                 \
				      (DT_INST_DMAS_CELL_BY_NAME(n, tx, channel)), (0)),                           \
			  .rx_dma_channel = COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, rx),                                 \
				      (DT_INST_DMAS_CELL_BY_NAME(n, rx, channel)), (0)),                  \
				   .dma_request = COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, tx),                                    \
				   (DT_INST_DMAS_CELL_BY_NAME(n, tx, request)), (0)),
#else
#define HPM_I2C_DMA_CONFIG(n)
#endif

#if defined(CONFIG_I2C_TARGET) || defined(CONFIG_I2C_HPMICRO_DMA)
#define HPM_I2C_IRQ_CONFIG(n)                                                                      \
	static void hpm_i2c_irq_config_##n(const struct device *dev)                               \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_i2c_isr,                \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}

#define HPM_I2C_IRQ_CONFIG_FIELD(n) .irq_config_func = hpm_i2c_irq_config_##n,
#define HPM_I2C_IRQ_NUM_FIELD(n)    .irq = DT_INST_IRQN(n),
#else
#define HPM_I2C_IRQ_CONFIG(n)
#define HPM_I2C_IRQ_CONFIG_FIELD(n)
#define HPM_I2C_IRQ_NUM_FIELD(n)
#endif

#define HPM_I2C_INIT(n)                                                                            \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	HPM_I2C_IRQ_CONFIG(n)                                                                      \
                                                                                                   \
	static const struct hpm_i2c_config hpm_i2c_config_##n = {                                  \
		.base = (I2C_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.clock_name = DT_INST_PROP(n, clock_name),                                         \
		.clock_src = DT_INST_PROP(n, clock_src),                                           \
		.clock_div = DT_INST_CLOCKS_CELL_BY_IDX(n, 0, divider),                            \
		.bitrate = DT_INST_PROP(n, clock_frequency),                                       \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
		HPM_I2C_DMA_CONFIG(n) HPM_I2C_IRQ_CONFIG_FIELD(n) HPM_I2C_IRQ_NUM_FIELD(n)};       \
                                                                                                   \
	static struct hpm_i2c_data hpm_i2c_data_##n;                                               \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_i2c_init, NULL, &hpm_i2c_data_##n, &hpm_i2c_config_##n,       \
			      POST_KERNEL, CONFIG_I2C_INIT_PRIORITY, &hpm_i2c_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_I2C_INIT)
