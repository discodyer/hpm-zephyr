/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * Copyright (c) 2026 Cody Gu <gujiaqi@iscas.ac.cn>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_uart_lin

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/lin.h>
#include <zephyr/drivers/lin/transceiver.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/util.h>

#include <hpm_clock_drv.h>
#include <hpm_soc.h>
#include <hpm_uart_drv.h>

#define HPM_LIN_MIN_BITRATE   1000U
#define HPM_LIN_MAX_BITRATE   20000U
#define HPM_LIN_WAKEUP_US     400U
#define PINCTRL_STATE_TX_GPIO 1U

enum hpm_lin_state {
	HPM_LIN_STATE_IDLE,
	HPM_LIN_STATE_WAIT_SYNC,
	HPM_LIN_STATE_WAIT_PID,
	HPM_LIN_STATE_WAIT_RESPONSE,
	HPM_LIN_STATE_RX_DATA,
	HPM_LIN_STATE_TX_DATA,
};

struct hpm_lin_config {
	struct lin_driver_config common;
	UART_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	clock_name_t clock_name;
	clock_source_t clock_src;
	uint32_t clock_div;
	const struct pinctrl_dev_config *pincfg;
	struct gpio_dt_spec tx_gpio;
	bool rx_header_echo;
	void (*irq_config_func)(void);
};

struct hpm_lin_data {
	struct lin_driver_data common;
	const struct device *dev;
	struct k_spinlock lock;
	struct k_work transfer_work;
	struct k_work_delayable timeout_work;
	struct lin_msg tx_msg;
	struct lin_msg *receive_msg;
	struct lin_msg *responder_msg;
	struct lin_filter filter;
	k_timeout_t transfer_timeout;
	uint32_t clock_freq;
	uint8_t frame_pid;
	uint8_t tx_frame[LIN_MAX_DLEN + 1U];
	uint8_t tx_index;
	uint8_t tx_len;
	uint8_t rx_index;
	uint8_t rx_checksum;
	uint8_t tx_checksum;
	enum hpm_lin_state state;
	atomic_t busy;
	bool commander_receive;
	bool filter_enabled;
	bool header_received;
	bool cancel;
};

static uint32_t hpm_lin_line_errors(uint32_t status)
{
	uint32_t errors = 0;

	if ((status & UART_LSR_OE_MASK) != 0U) {
		errors |= LIN_ERR_OVERRUN;
	}
	if ((status & UART_LSR_PE_MASK) != 0U) {
		errors |= LIN_ERR_PARITY;
	}
	if ((status & UART_LSR_FE_MASK) != 0U) {
		errors |= LIN_ERR_FRAMING;
	}

	return errors;
}

static void hpm_lin_emit_event(const struct device *dev, const struct lin_event *event)
{
	struct hpm_lin_data *data = dev->data;
	k_spinlock_key_t key;
	lin_event_callback_t callback;
	void *callback_data;

	key = k_spin_lock(&data->lock);
	callback = data->common.callback;
	callback_data = data->common.callback_data;
	k_spin_unlock(&data->lock, key);

	if (callback != NULL) {
		callback(dev, event, callback_data);
	}
}

static void hpm_lin_emit_error(const struct device *dev, uint32_t error_flags)
{
	struct lin_event event = {
		.type = LIN_EVT_ERR,
		.error_flags = error_flags,
	};

	hpm_lin_emit_event(dev, &event);
}

static uint8_t hpm_lin_checksum(uint8_t pid, const uint8_t *data, uint8_t data_len,
				enum lin_checksum_type type)
{
	uint16_t sum = 0;

	for (uint8_t i = 0; i < data_len; i++) {
		sum += data[i];
	}
	if (type == LIN_CHECKSUM_ENHANCED) {
		sum += pid;
	}

	return (uint8_t)~((sum & 0xffU) + (sum >> 8U));
}

static int hpm_lin_msg_validate(const struct lin_msg *msg)
{
	if (msg == NULL) {
		return -EINVAL;
	}
	if ((msg->id & (uint8_t)~LIN_ID_MASK) != 0U) {
		return -EINVAL;
	}
	if (msg->data_len > LIN_MAX_DLEN) {
		return -EINVAL;
	}
	if (msg->checksum_type != LIN_CHECKSUM_CLASSIC &&
	    msg->checksum_type != LIN_CHECKSUM_ENHANCED) {
		return -EINVAL;
	}

	return 0;
}

static bool hpm_lin_filter_match(const struct device *dev, uint8_t pid)
{
	struct hpm_lin_data *data = dev->data;
	bool match = true;

	if (data->filter_enabled) {
		match = ((pid & data->filter.mask) ==
			 (data->filter.primary_pid & data->filter.mask)) ||
			((data->filter.secondary_pid != 0U) &&
			 ((pid & data->filter.mask) ==
			  (data->filter.secondary_pid & data->filter.mask)));
	}

	return match;
}

static int hpm_lin_wait_tx_slot(UART_Type *base, k_timepoint_t deadline)
{
	while (!uart_check_status(base, uart_stat_tx_slot_avail)) {
		if (sys_timepoint_expired(deadline)) {
			return -ETIMEDOUT;
		}
		k_busy_wait(1);
	}

	return 0;
}

static int hpm_lin_write_byte(UART_Type *base, uint8_t byte, k_timepoint_t deadline)
{
	int ret = hpm_lin_wait_tx_slot(base, deadline);

	if (ret == 0) {
		uart_write_byte(base, byte);
	}

	return ret;
}

static int hpm_lin_wait_tx_empty(UART_Type *base, k_timepoint_t deadline)
{
	while (!uart_check_status(base, uart_stat_transmitter_empty)) {
		if (sys_timepoint_expired(deadline)) {
			return -ETIMEDOUT;
		}
		k_busy_wait(1);
	}

	return 0;
}

static int hpm_lin_drive_tx_gpio(const struct device *dev, uint32_t dominant_us,
				 uint32_t recessive_us)
{
	const struct hpm_lin_config *config = dev->config;
	int ret;

	ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_TX_GPIO);
	if (ret < 0) {
		return ret;
	}

	(void)gpio_pin_set_raw(config->tx_gpio.port, config->tx_gpio.pin, 0);
	k_busy_wait(dominant_us);
	(void)gpio_pin_set_raw(config->tx_gpio.port, config->tx_gpio.pin, 1);
	k_busy_wait(recessive_us);

	return pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
}

static int hpm_lin_send_break(const struct device *dev)
{
	struct hpm_lin_data *data = dev->data;
	uint32_t bit_us = DIV_ROUND_UP(1000000U, data->common.config.baudrate);

	return hpm_lin_drive_tx_gpio(dev, bit_us * data->common.config.break_len,
				     bit_us * data->common.config.break_delimiter_len);
}

static int hpm_lin_write_header(const struct device *dev, uint8_t pid, k_timepoint_t deadline)
{
	const struct hpm_lin_config *config = dev->config;
	int ret;

	ret = hpm_lin_write_byte(config->base, 0x55U, deadline);
	if (ret == 0) {
		ret = hpm_lin_write_byte(config->base, pid, deadline);
	}

	return ret;
}

static int hpm_lin_write_payload(const struct device *dev, const struct lin_msg *msg, uint8_t pid,
				 k_timepoint_t deadline)
{
	const struct hpm_lin_config *config = dev->config;
	uint8_t checksum = hpm_lin_checksum(pid, msg->data, msg->data_len, msg->checksum_type);
	int ret;

	for (uint8_t i = 0; i < msg->data_len; i++) {
		ret = hpm_lin_write_byte(config->base, msg->data[i], deadline);
		if (ret < 0) {
			return ret;
		}
	}

	return hpm_lin_write_byte(config->base, checksum, deadline);
}

static int hpm_lin_receive_response(const struct device *dev, struct lin_msg *msg, uint8_t pid,
				    k_timepoint_t deadline, uint8_t *checksum)
{
	const struct hpm_lin_config *config = dev->config;
	uint32_t errors = 0;
	uint8_t header_index = 0;
	uint8_t count = 0;

	if (msg->data_len == 0U) {
		return -EINVAL;
	}

	while (count < msg->data_len + 1U) {
		uint32_t status = uart_get_status(config->base);

		errors |= hpm_lin_line_errors(status);
		if (uart_check_status(config->base, uart_stat_data_ready)) {
			uint8_t byte = uart_read_byte(config->base);

			if (config->rx_header_echo && (header_index < 2U)) {
				if ((header_index == 0U && byte != 0x55U) ||
				    (header_index == 1U && byte != pid)) {
					return -EBADMSG;
				}
				header_index++;
			} else if (count < msg->data_len) {
				msg->data[count++] = byte;
			} else {
				*checksum = byte;
				count++;
			}
		}

		if (sys_timepoint_expired(deadline)) {
			return -ETIMEDOUT;
		}
		k_busy_wait(1);
	}

	if (errors != 0U) {
		hpm_lin_emit_error(dev, errors);
		return -EIO;
	}
	if (*checksum != hpm_lin_checksum(pid, msg->data, msg->data_len, msg->checksum_type)) {
		hpm_lin_emit_error(dev, LIN_ERR_INVALID_CHECKSUM);
		return -EBADMSG;
	}

	return 0;
}

static void hpm_lin_transfer_work(struct k_work *work)
{
	struct hpm_lin_data *data = CONTAINER_OF(work, struct hpm_lin_data, transfer_work);
	const struct device *dev = data->dev;
	k_timepoint_t deadline;
	struct lin_event event = {0};
	struct lin_msg rx_msg;
	struct lin_msg *rx_target = NULL;
	bool receive;
	uint8_t checksum = 0;
	uint8_t pid;
	int ret;

	if (data->cancel) {
		atomic_clear(&data->busy);
		return;
	}

	rx_msg = data->tx_msg;
	receive = data->commander_receive;
	rx_target = data->receive_msg;
	deadline = sys_timepoint_calc(data->transfer_timeout);
	pid = lin_compute_pid(data->tx_msg.id);

	ret = hpm_lin_wait_tx_empty(((const struct hpm_lin_config *)dev->config)->base, deadline);
	if (ret == 0) {
		ret = hpm_lin_send_break(dev);
	}
	if (ret == 0) {
		ret = hpm_lin_write_header(dev, pid, deadline);
	}
	if (ret == 0 && !receive) {
		ret = hpm_lin_write_payload(dev, &data->tx_msg, pid, deadline);
	}
	if (ret == 0) {
		ret = hpm_lin_wait_tx_empty(((const struct hpm_lin_config *)dev->config)->base,
					    deadline);
	}

	if (ret == 0) {
		event.type = LIN_EVT_TX_HEADER;
		event.header.pid = pid;
		hpm_lin_emit_event(dev, &event);

		if (!receive) {
			event = (struct lin_event){0};
			event.type = LIN_EVT_TX_DATA;
			event.data.pid = pid;
			event.data.bytes_received = rx_msg.data_len;
			event.data.checksum = hpm_lin_checksum(pid, rx_msg.data, rx_msg.data_len,
							       rx_msg.checksum_type);
			hpm_lin_emit_event(dev, &event);
		} else if (rx_target != NULL) {
			ret = hpm_lin_receive_response(dev, rx_target, pid, deadline, &checksum);
			if (ret == 0) {
				event = (struct lin_event){0};
				event.type = LIN_EVT_RX_DATA;
				event.data.pid = pid;
				event.data.bytes_received = rx_target->data_len;
				event.data.checksum = checksum;
				hpm_lin_emit_event(dev, &event);
			} else if (ret != -EBADMSG) {
				event = (struct lin_event){0};
				event.type = LIN_EVT_RX_DATA;
				event.data.pid = pid;
				event.status = ret;
				hpm_lin_emit_event(dev, &event);
			}
		}
	} else if (!receive) {
		event = (struct lin_event){0};
		event.type = LIN_EVT_TX_DATA;
		event.data.pid = pid;
		event.status = ret;
		hpm_lin_emit_event(dev, &event);
	} else {
		event = (struct lin_event){0};
		event.type = LIN_EVT_RX_DATA;
		event.data.pid = pid;
		event.status = ret;
		hpm_lin_emit_event(dev, &event);
	}

	data->receive_msg = NULL;
	data->commander_receive = false;
	atomic_clear(&data->busy);
}

static void hpm_lin_cancel_timeout(const struct device *dev)
{
	struct hpm_lin_data *data = dev->data;

	(void)k_work_cancel_delayable(&data->timeout_work);
}

static void hpm_lin_schedule_timeout(const struct device *dev, k_timeout_t timeout)
{
	struct hpm_lin_data *data = dev->data;

	if (!K_TIMEOUT_EQ(timeout, K_FOREVER)) {
		(void)k_work_reschedule(&data->timeout_work, timeout);
	}
}

static void hpm_lin_timeout_work(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct hpm_lin_data *data = CONTAINER_OF(dwork, struct hpm_lin_data, timeout_work);
	const struct device *dev = data->dev;
	struct lin_event event = {0};
	enum hpm_lin_state state;
	uint8_t pid;

	state = data->state;
	if (state != HPM_LIN_STATE_RX_DATA && state != HPM_LIN_STATE_TX_DATA) {
		return;
	}

	pid = data->frame_pid;
	data->state = HPM_LIN_STATE_IDLE;
	data->header_received = false;
	data->responder_msg = NULL;

	event.type = state == HPM_LIN_STATE_RX_DATA ? LIN_EVT_RX_DATA : LIN_EVT_TX_DATA;
	event.status = -ETIMEDOUT;
	event.data.pid = pid;
	hpm_lin_emit_event(dev, &event);
}

static void hpm_lin_reset_responder(const struct device *dev)
{
	const struct hpm_lin_config *config = dev->config;
	struct hpm_lin_data *data = dev->data;

	uart_disable_irq(config->base, uart_intr_rx_data_avail_or_timeout);
	uart_disable_irq(config->base, uart_intr_tx_slot_avail);
	uart_disable_irq(config->base, uart_intr_tx_line_idle);
	uart_disable_txline_idle_detection(config->base);
	hpm_lin_cancel_timeout(dev);

	data->state = HPM_LIN_STATE_IDLE;
	data->header_received = false;
	data->responder_msg = NULL;
}

static void hpm_lin_finish_response(const struct device *dev)
{
	const struct hpm_lin_config *config = dev->config;
	struct hpm_lin_data *data = dev->data;
	struct lin_event event = {
		.type = LIN_EVT_TX_DATA,
	};

	event.data.pid = data->frame_pid;
	event.data.checksum = data->tx_checksum;

	uart_disable_irq(config->base, uart_intr_tx_slot_avail);
	uart_disable_irq(config->base, uart_intr_tx_line_idle);
	uart_disable_txline_idle_detection(config->base);
	hpm_lin_cancel_timeout(dev);
	data->state = HPM_LIN_STATE_IDLE;
	data->header_received = false;
	hpm_lin_emit_event(dev, &event);
}

static void hpm_lin_tx_advance(const struct device *dev)
{
	const struct hpm_lin_config *config = dev->config;
	struct hpm_lin_data *data = dev->data;
	uart_rxline_idle_config_t idle = {
		.detect_enable = true,
		.detect_irq_enable = true,
		.idle_cond = uart_rxline_idle_cond_rxline_logic_one,
		.threshold = 10U,
	};

	while ((data->tx_index < data->tx_len) &&
	       uart_check_status(config->base, uart_stat_tx_slot_avail)) {
		uart_write_byte(config->base, data->tx_frame[data->tx_index++]);
	}

	if (data->tx_index == data->tx_len) {
		uart_disable_irq(config->base, uart_intr_tx_slot_avail);
		uart_clear_txline_idle_flag(config->base);
		(void)uart_init_txline_idle_detection(config->base, idle);
	}
}

static void hpm_lin_handle_break(const struct device *dev)
{
	const struct hpm_lin_config *config = dev->config;
	struct hpm_lin_data *data = dev->data;

	hpm_lin_reset_responder(dev);
	uart_clear_rx_fifo(config->base);
	data->state = HPM_LIN_STATE_WAIT_SYNC;
	uart_enable_irq(config->base, uart_intr_rx_data_avail_or_timeout);
}

static void hpm_lin_handle_header(const struct device *dev, uint8_t pid)
{
	const struct hpm_lin_config *config = dev->config;
	struct hpm_lin_data *data = dev->data;
	struct lin_event event = {
		.type = LIN_EVT_RX_HEADER,
		.header.pid = pid,
	};

	if (!lin_verify_pid(pid)) {
		hpm_lin_emit_error(dev, LIN_ERR_PARITY);
		data->state = HPM_LIN_STATE_IDLE;
		return;
	}
	if (!hpm_lin_filter_match(dev, pid)) {
		data->state = HPM_LIN_STATE_IDLE;
		return;
	}

	data->frame_pid = pid;
	data->header_received = true;
	data->state = HPM_LIN_STATE_WAIT_RESPONSE;
	uart_disable_irq(config->base, uart_intr_rx_data_avail_or_timeout);
	hpm_lin_emit_event(dev, &event);
}

static void hpm_lin_finish_read(const struct device *dev)
{
	const struct hpm_lin_config *config = dev->config;
	struct hpm_lin_data *data = dev->data;
	struct lin_event event = {
		.type = LIN_EVT_RX_DATA,
	};

	event.data.pid = data->frame_pid;
	event.data.bytes_received = data->responder_msg->data_len;
	event.data.checksum = data->rx_checksum;

	uart_disable_irq(config->base, uart_intr_rx_data_avail_or_timeout);
	hpm_lin_cancel_timeout(dev);

	if (data->rx_checksum != hpm_lin_checksum(data->frame_pid, data->responder_msg->data,
						  data->responder_msg->data_len,
						  data->responder_msg->checksum_type)) {
		data->state = HPM_LIN_STATE_IDLE;
		data->header_received = false;
		data->responder_msg = NULL;
		hpm_lin_emit_error(dev, LIN_ERR_INVALID_CHECKSUM);
		return;
	}

	data->state = HPM_LIN_STATE_IDLE;
	data->header_received = false;
	data->responder_msg = NULL;
	hpm_lin_emit_event(dev, &event);
}

static void hpm_lin_handle_rx_data(const struct device *dev)
{
	const struct hpm_lin_config *config = dev->config;
	struct hpm_lin_data *data = dev->data;

	while (uart_check_status(config->base, uart_stat_data_ready)) {
		uint8_t byte = uart_read_byte(config->base);

		switch (data->state) {
		case HPM_LIN_STATE_WAIT_SYNC:
			if (byte != 0x55U) {
				hpm_lin_emit_error(dev, LIN_ERR_FRAMING);
				hpm_lin_reset_responder(dev);
				return;
			}
			data->state = HPM_LIN_STATE_WAIT_PID;
			break;
		case HPM_LIN_STATE_WAIT_PID:
			hpm_lin_handle_header(dev, byte);
			break;
		case HPM_LIN_STATE_RX_DATA:
			if (data->responder_msg == NULL) {
				hpm_lin_reset_responder(dev);
				return;
			}
			if (data->rx_index < data->responder_msg->data_len) {
				data->responder_msg->data[data->rx_index++] = byte;
			} else {
				data->rx_checksum = byte;
				hpm_lin_finish_read(dev);
			}
			break;
		case HPM_LIN_STATE_TX_DATA:
		case HPM_LIN_STATE_WAIT_RESPONSE:
		case HPM_LIN_STATE_IDLE:
		default:
			break;
		}
	}
}

static void hpm_lin_isr(const struct device *dev)
{
	const struct hpm_lin_config *config = dev->config;
	struct hpm_lin_data *data = dev->data;
	uint32_t status;

	if (uart_is_txline_idle(config->base)) {
		uart_clear_txline_idle_flag(config->base);
		hpm_lin_finish_response(dev);
		return;
	}

	status = uart_get_status(config->base);
	if ((status & UART_LSR_LBREAK_MASK) != 0U) {
		hpm_lin_handle_break(dev);
		return;
	}

	if ((status & (UART_LSR_OE_MASK | UART_LSR_PE_MASK | UART_LSR_FE_MASK)) != 0U) {
		hpm_lin_emit_error(dev, hpm_lin_line_errors(status));
		hpm_lin_reset_responder(dev);
		return;
	}

	if (data->state == HPM_LIN_STATE_TX_DATA) {
		if ((status & UART_LSR_THRE_MASK) != 0U) {
			hpm_lin_tx_advance(dev);
		}
		return;
	}

	if (data->common.config.mode == LIN_MODE_RESPONDER) {
		hpm_lin_handle_rx_data(dev);
	}
}

static int hpm_lin_configure_uart(const struct device *dev)
{
	const struct hpm_lin_config *config = dev->config;
	struct hpm_lin_data *data = dev->data;
	uart_config_t cfg;
	hpm_stat_t status;

	uart_default_config(config->base, &cfg);
	cfg.src_freq_in_hz = data->clock_freq;
	cfg.baudrate = data->common.config.baudrate;
	cfg.word_length = word_length_8_bits;
	cfg.parity = parity_none;
	cfg.num_of_stop_bits = stop_bits_1;
	cfg.fifo_enable = true;
	cfg.rx_fifo_level = uart_rx_fifo_trg_not_empty;
	cfg.tx_fifo_level = uart_tx_fifo_trg_not_full;
	cfg.dma_enable = false;
	cfg.modem_config.auto_flow_ctrl_en = false;
	cfg.modem_config.loop_back_en = false;

	status = uart_init(config->base, &cfg);
	if (status != status_success) {
		return -EINVAL;
	}

	uart_enable_rx_function(config->base, true);
	return 0;
}

static int hpm_lin_configure(const struct device *dev, const struct lin_config *config)
{
	struct hpm_lin_data *data = dev->data;
	k_spinlock_key_t key;
	int ret;

	if (config == NULL) {
		return -EINVAL;
	}
	if ((config->mode != LIN_MODE_COMMANDER) && (config->mode != LIN_MODE_RESPONDER)) {
		return -EINVAL;
	}
	if (config->flags != 0U) {
		return -ENOTSUP;
	}
	if ((config->break_delimiter_len != 1U) && (config->break_delimiter_len != 2U)) {
		return -EINVAL;
	}
	if ((config->break_len == 0U) || (config->break_len > UINT8_MAX)) {
		return -EINVAL;
	}
	if ((config->baudrate < ((const struct hpm_lin_config *)dev->config)->common.min_bitrate) ||
	    (config->baudrate > ((const struct hpm_lin_config *)dev->config)->common.max_bitrate)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	if (data->common.started) {
		k_spin_unlock(&data->lock, key);
		return -EBUSY;
	}
	data->common.config = *config;
	data->filter_enabled = false;
	k_spin_unlock(&data->lock, key);

	ret = hpm_lin_configure_uart(dev);
	return ret;
}

static int hpm_lin_get_config(const struct device *dev, struct lin_config *config)
{
	struct hpm_lin_data *data = dev->data;
	k_spinlock_key_t key;

	if (config == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	*config = data->common.config;
	k_spin_unlock(&data->lock, key);
	return 0;
}

static int hpm_lin_start(const struct device *dev)
{
	const struct hpm_lin_config *config = dev->config;
	struct hpm_lin_data *data = dev->data;
	const struct device *phy = lin_get_transceiver(dev);
	k_spinlock_key_t key;
	int ret;

	key = k_spin_lock(&data->lock);
	if (data->common.started) {
		k_spin_unlock(&data->lock, key);
		return -EALREADY;
	}
	k_spin_unlock(&data->lock, key);

	if (phy != NULL) {
		ret = device_is_ready(phy) ? lin_transceiver_enable(phy, 0) : -ENODEV;
		if (ret < 0 && ret != -EALREADY) {
			return ret;
		}
	}

	ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		goto disable_phy;
	}

	ret = hpm_lin_configure_uart(dev);
	if (ret < 0) {
		goto disable_phy;
	}

	uart_disable_irq(config->base, uart_intr_rx_line_stat | uart_intr_rx_data_avail_or_timeout |
					       uart_intr_tx_slot_avail | uart_intr_tx_line_idle);
	if (data->common.config.mode == LIN_MODE_RESPONDER) {
		uart_enable_irq(config->base, uart_intr_rx_line_stat);
	}

	key = k_spin_lock(&data->lock);
	data->common.started = true;
	data->cancel = false;
	atomic_clear(&data->busy);
	k_spin_unlock(&data->lock, key);
	return 0;

disable_phy:
	if (phy != NULL) {
		(void)lin_transceiver_disable(phy);
	}
	return ret;
}

static int hpm_lin_stop(const struct device *dev)
{
	const struct hpm_lin_config *config = dev->config;
	struct hpm_lin_data *data = dev->data;
	const struct device *phy = lin_get_transceiver(dev);
	k_spinlock_key_t key;
	int ret = 0;

	key = k_spin_lock(&data->lock);
	if (!data->common.started) {
		k_spin_unlock(&data->lock, key);
		return -EALREADY;
	}
	data->cancel = true;
	data->common.started = false;
	k_spin_unlock(&data->lock, key);

	uart_disable_irq(config->base, uart_intr_rx_line_stat | uart_intr_rx_data_avail_or_timeout |
					       uart_intr_tx_slot_avail | uart_intr_tx_line_idle);
	uart_enable_rx_function(config->base, false);
	(void)k_work_cancel(&data->transfer_work);
	hpm_lin_reset_responder(dev);

	if (phy != NULL) {
		ret = lin_transceiver_disable(phy);
		if (ret == -EALREADY) {
			ret = 0;
		}
	}

	return ret;
}

static int hpm_lin_set_event_callback(const struct device *dev, lin_event_callback_t callback,
				      void *user_data)
{
	struct hpm_lin_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	data->common.callback = callback;
	data->common.callback_data = user_data;
	k_spin_unlock(&data->lock, key);
	return 0;
}

static int hpm_lin_send(const struct device *dev, const struct lin_msg *msg, k_timeout_t timeout)
{
	struct hpm_lin_data *data = dev->data;
	k_spinlock_key_t key;
	int ret;

	ret = hpm_lin_msg_validate(msg);
	if (ret < 0) {
		return ret;
	}
	if (!data->common.started) {
		return -EIO;
	}
	if (data->common.config.mode != LIN_MODE_COMMANDER) {
		return -EPERM;
	}
	if (!atomic_cas(&data->busy, 0, 1)) {
		return -EBUSY;
	}

	key = k_spin_lock(&data->lock);
	data->tx_msg = *msg;
	data->transfer_timeout = timeout;
	data->commander_receive = false;
	k_spin_unlock(&data->lock, key);
	(void)k_work_submit(&data->transfer_work);
	return 0;
}

static int hpm_lin_receive(const struct device *dev, struct lin_msg *msg, k_timeout_t timeout)
{
	struct hpm_lin_data *data = dev->data;
	k_spinlock_key_t key;
	int ret;

	ret = hpm_lin_msg_validate(msg);
	if (ret < 0) {
		return ret;
	}
	if (msg->data_len == 0U) {
		return -EINVAL;
	}
	if (!data->common.started) {
		return -EIO;
	}
	if (data->common.config.mode != LIN_MODE_COMMANDER) {
		return -EPERM;
	}
	if (!atomic_cas(&data->busy, 0, 1)) {
		return -EBUSY;
	}

	key = k_spin_lock(&data->lock);
	data->tx_msg = *msg;
	data->receive_msg = msg;
	data->transfer_timeout = timeout;
	data->commander_receive = true;
	k_spin_unlock(&data->lock, key);
	(void)k_work_submit(&data->transfer_work);
	return 0;
}

static int hpm_lin_response(const struct device *dev, const struct lin_msg *msg,
			    k_timeout_t timeout)
{
	const struct hpm_lin_config *config = dev->config;
	struct hpm_lin_data *data = dev->data;
	uint8_t checksum;
	int ret;

	ret = hpm_lin_msg_validate(msg);
	if (ret < 0) {
		return ret;
	}
	if (msg->data_len == 0U) {
		return -EINVAL;
	}
	if (data->common.config.mode != LIN_MODE_RESPONDER) {
		return -EPERM;
	}
	if (!data->header_received || data->state != HPM_LIN_STATE_WAIT_RESPONSE) {
		return -EFAULT;
	}
	if (msg->id != lin_get_frame_id(data->frame_pid)) {
		return -EINVAL;
	}

	checksum = hpm_lin_checksum(data->frame_pid, msg->data, msg->data_len, msg->checksum_type);
	memcpy(data->tx_frame, msg->data, msg->data_len);
	data->tx_frame[msg->data_len] = checksum;
	data->tx_len = msg->data_len + 1U;
	data->tx_index = 0U;
	data->tx_checksum = checksum;
	data->state = HPM_LIN_STATE_TX_DATA;

	uart_enable_irq(config->base, uart_intr_tx_slot_avail);
	hpm_lin_tx_advance(dev);
	hpm_lin_cancel_timeout(dev);
	hpm_lin_schedule_timeout(dev, timeout);
	return 0;
}

static int hpm_lin_read(const struct device *dev, struct lin_msg *msg, k_timeout_t timeout)
{
	const struct hpm_lin_config *config = dev->config;
	struct hpm_lin_data *data = dev->data;
	int ret;

	ret = hpm_lin_msg_validate(msg);
	if (ret < 0) {
		return ret;
	}
	if (msg->data_len == 0U) {
		return -EINVAL;
	}
	if (data->common.config.mode != LIN_MODE_RESPONDER) {
		return -EPERM;
	}
	if (!data->header_received || data->state != HPM_LIN_STATE_WAIT_RESPONSE) {
		return -EFAULT;
	}
	if (msg->id != lin_get_frame_id(data->frame_pid)) {
		return -EINVAL;
	}

	data->responder_msg = msg;
	data->rx_index = 0U;
	data->rx_checksum = 0U;
	data->state = HPM_LIN_STATE_RX_DATA;
	uart_enable_irq(config->base, uart_intr_rx_data_avail_or_timeout);
	hpm_lin_cancel_timeout(dev);
	hpm_lin_schedule_timeout(dev, timeout);
	return 0;
}

static int hpm_lin_set_rx_filter(const struct device *dev, const struct lin_filter *filter)
{
	struct hpm_lin_data *data = dev->data;
	k_spinlock_key_t key;

	if (filter == NULL) {
		return -EINVAL;
	}
	if (data->common.config.mode != LIN_MODE_RESPONDER) {
		return -EPERM;
	}

	key = k_spin_lock(&data->lock);
	data->filter = *filter;
	data->filter_enabled = true;
	k_spin_unlock(&data->lock, key);
	return 0;
}

static int hpm_lin_wakeup_send(const struct device *dev)
{
	struct hpm_lin_data *data = dev->data;
	struct lin_event event = {
		.type = LIN_EVT_TX_WAKEUP,
	};
	uint32_t bit_us;
	int ret;

	if (!data->common.started) {
		return -EIO;
	}
	if (!atomic_cas(&data->busy, 0, 1)) {
		return -EBUSY;
	}

	bit_us = DIV_ROUND_UP(1000000U, data->common.config.baudrate);
	ret = hpm_lin_drive_tx_gpio(dev, HPM_LIN_WAKEUP_US, bit_us);
	if (ret == 0) {
		hpm_lin_emit_event(dev, &event);
	}
	atomic_clear(&data->busy);
	return ret;
}

static int hpm_lin_init(const struct device *dev)
{
	const struct hpm_lin_config *config = dev->config;
	struct hpm_lin_data *data = dev->data;
	uint32_t clock_freq;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&config->tx_gpio)) {
		return -ENODEV;
	}

	ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	ret = clock_set_source_divider(config->clock_name, config->clock_src, config->clock_div);
	if (ret != 0) {
		return -EINVAL;
	}
	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		return ret;
	}
	ret = clock_control_get_rate(config->clock_dev, config->clock_subsys, &clock_freq);
	if (ret < 0) {
		return ret;
	}

	ret = gpio_pin_configure(config->tx_gpio.port, config->tx_gpio.pin, GPIO_OUTPUT);
	if (ret < 0) {
		return ret;
	}
	(void)gpio_pin_set_raw(config->tx_gpio.port, config->tx_gpio.pin, 1);

	data->dev = dev;
	data->clock_freq = clock_freq;
	k_work_init(&data->transfer_work, hpm_lin_transfer_work);
	k_work_init_delayable(&data->timeout_work, hpm_lin_timeout_work);
	config->irq_config_func();
	return 0;
}

static DEVICE_API(lin, hpm_lin_api) = {
	.start = hpm_lin_start,
	.stop = hpm_lin_stop,
	.configure = hpm_lin_configure,
	.get_config = hpm_lin_get_config,
	.set_rx_filter = hpm_lin_set_rx_filter,
	.send = hpm_lin_send,
	.receive = hpm_lin_receive,
	.response = hpm_lin_response,
	.read = hpm_lin_read,
	.wakeup_send = hpm_lin_wakeup_send,
	.set_event_callback = hpm_lin_set_event_callback,
};

#define HPM_LIN_UART_NODE(n) DT_PHANDLE(DT_DRV_INST(n), uart)

#define HPM_LIN_IRQ_CONFIG(n)                                                                      \
	static void hpm_lin_irq_config_##n(void)                                                   \
	{                                                                                          \
		IRQ_CONNECT(DT_IRQN(HPM_LIN_UART_NODE(n)), DT_IRQ(HPM_LIN_UART_NODE(n), priority), \
			    hpm_lin_isr, DEVICE_DT_INST_GET(n), 0);                                \
		irq_enable(DT_IRQN(HPM_LIN_UART_NODE(n)));                                         \
	}

#define HPM_LIN_DEVICE_INIT(n)                                                                     \
	BUILD_ASSERT(!DT_INST_PROP(n, auto_sync), "UART LIN does not support auto sync");          \
	BUILD_ASSERT(!DT_INST_PROP(n, conflict_detection),                                         \
		     "UART LIN does not support bus conflict detection");                          \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	HPM_LIN_IRQ_CONFIG(n)                                                                      \
	static struct hpm_lin_data hpm_lin_data_##n = {                                            \
		.common =                                                                          \
			{                                                                          \
				.config =                                                          \
					{                                                          \
						.mode = DT_INST_PROP(n, commander)                 \
								? LIN_MODE_COMMANDER               \
								: LIN_MODE_RESPONDER,              \
						.baudrate = DT_INST_PROP(n, bitrate),              \
						.break_len = DT_INST_PROP(n, break_len),           \
						.break_delimiter_len =                             \
							DT_INST_PROP(n, break_delimiter),          \
					},                                                         \
			},                                                                         \
	};                                                                                         \
	static const struct hpm_lin_config hpm_lin_config_##n = {                                  \
		.common = LIN_DT_DRIVER_CONFIG_INST_GET(n, HPM_LIN_MIN_BITRATE,                    \
							HPM_LIN_MAX_BITRATE),                      \
		.base = (UART_Type *)DT_REG_ADDR(HPM_LIN_UART_NODE(n)),                            \
		.clock_dev = DEVICE_DT_GET(DT_CLOCKS_CTLR(HPM_LIN_UART_NODE(n))),                  \
		.clock_subsys =                                                                    \
			(clock_control_subsys_t)DT_CLOCKS_CELL(HPM_LIN_UART_NODE(n), name),        \
		.clock_name = DT_PROP(HPM_LIN_UART_NODE(n), clock_name),                           \
		.clock_src = DT_PROP(HPM_LIN_UART_NODE(n), clock_src),                             \
		.clock_div = DT_CLOCKS_CELL_BY_IDX(HPM_LIN_UART_NODE(n), 0, divider),              \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
		.tx_gpio = GPIO_DT_SPEC_INST_GET(n, tx_gpios),                                     \
		.rx_header_echo = DT_INST_PROP(n, rx_header_echo),                                 \
		.irq_config_func = hpm_lin_irq_config_##n,                                         \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, hpm_lin_init, NULL, &hpm_lin_data_##n, &hpm_lin_config_##n,       \
			      POST_KERNEL, CONFIG_LIN_INIT_PRIORITY, &hpm_lin_api)

DT_INST_FOREACH_STATUS_OKAY(HPM_LIN_DEVICE_INIT);
