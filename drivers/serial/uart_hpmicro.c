/*
 * Copyright (c) 2022-2026 HPMicro
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_uart

#include <errno.h>

#include <zephyr/cache.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <drivers/serial/hpmicro_uart.h>

#include <hpm_common.h>
#include <hpm_clock_drv.h>
#include <hpm_soc.h>
#include <hpm_uart_drv.h>

#define PINCTRL_STATE_FLOW_CONTROL PINCTRL_STATE_PRIV_START
#define PINCTRL_STATE_RS485        (PINCTRL_STATE_PRIV_START + 1U)

struct hpm_zephyr_uart_config {
	UART_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	clock_name_t clock_name;
	clock_source_t clock_src;
	uint32_t clock_div;
	uint32_t current_speed;
	const struct pinctrl_dev_config *pincfg;
#if defined(CONFIG_UART_INTERRUPT_DRIVEN) || defined(CONFIG_UART_ASYNC_API)
	uart_irq_config_func_t irq_config_func;
#endif
#ifdef CONFIG_UART_ASYNC_API
	const struct device *dma_dev;
	uint32_t tx_dma_channel;
	uint32_t rx_dma_channel;
	uint32_t tx_dma_request;
	uint32_t rx_dma_request;
#endif
};

struct hpm_zephyr_uart_data {
	struct k_spinlock lock;
	uint32_t clock_freq;
	uint32_t baudrate;
	enum uart_config_flow_control flow_ctrl;
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	uart_irq_callback_user_data_t callback;
	void *callback_data;
#endif
#ifdef CONFIG_UART_ASYNC_API
	const struct device *dev;
	uart_callback_t async_callback;
	void *async_user_data;
	const uint8_t *tx_buf;
	size_t tx_len;
	int32_t tx_timeout_us;
	k_timepoint_t tx_timepoint;
	bool tx_dma_done;
	uint8_t *rx_buf;
	size_t rx_len;
	size_t rx_offset;
	uint8_t *rx_next_buf;
	size_t rx_next_len;
	struct dma_block_config tx_block;
	struct dma_block_config rx_block;
	struct k_work_delayable tx_done_work;
	struct k_work_delayable rx_timeout_work;
	int32_t rx_timeout_us;
	bool rx_active;
#endif
};

#ifdef CONFIG_UART_ASYNC_API
static void hpm_uart_update_dma_mode(const struct device *dev);
static void hpm_uart_tx_done_handler(struct k_work *work);
static void hpm_uart_rx_timeout_handler(struct k_work *work);
static void hpm_uart_rx_ready(const struct device *dev);
#endif

static word_length_t hpm_uart_word_length(enum uart_config_data_bits data_bits)
{
	switch (data_bits) {
	case UART_CFG_DATA_BITS_5:
		return word_length_5_bits;
	case UART_CFG_DATA_BITS_6:
		return word_length_6_bits;
	case UART_CFG_DATA_BITS_7:
		return word_length_7_bits;
	case UART_CFG_DATA_BITS_8:
		return word_length_8_bits;
	default:
		return (word_length_t)UINT8_MAX;
	}
}

static parity_setting_t hpm_uart_parity(enum uart_config_parity parity)
{
	switch (parity) {
	case UART_CFG_PARITY_NONE:
		return parity_none;
	case UART_CFG_PARITY_ODD:
		return parity_odd;
	case UART_CFG_PARITY_EVEN:
		return parity_even;
	case UART_CFG_PARITY_MARK:
		return parity_always_1;
	case UART_CFG_PARITY_SPACE:
		return parity_always_0;
	default:
		return (parity_setting_t)UINT8_MAX;
	}
}

static num_of_stop_bits_t hpm_uart_stop_bits(enum uart_config_stop_bits stop_bits)
{
	switch (stop_bits) {
	case UART_CFG_STOP_BITS_1:
		return stop_bits_1;
	case UART_CFG_STOP_BITS_1_5:
		return stop_bits_1_5;
	case UART_CFG_STOP_BITS_2:
		return stop_bits_2;
	default:
		return (num_of_stop_bits_t)UINT8_MAX;
	}
}

static int hpm_uart_apply_flow_pinctrl(const struct device *dev,
				       enum uart_config_flow_control flow_ctrl)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	uint8_t state;
	int ret;

	switch (flow_ctrl) {
	case UART_CFG_FLOW_CTRL_NONE:
		state = PINCTRL_STATE_DEFAULT;
		break;
	case UART_CFG_FLOW_CTRL_RTS_CTS:
		state = PINCTRL_STATE_FLOW_CONTROL;
		break;
	case UART_CFG_FLOW_CTRL_RS485:
		state = PINCTRL_STATE_RS485;
		break;
	default:
		return -ENOTSUP;
	}

	ret = pinctrl_apply_state(config->pincfg, state);
	if (ret == -ENOENT) {
		return -ENOTSUP;
	}

	return ret;
}

static int hpm_uart_configure(const struct device *dev, const struct uart_config *cfg)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	struct hpm_zephyr_uart_data *data = dev->data;
	word_length_t word_length;
	parity_setting_t parity;
	num_of_stop_bits_t stop_bits;
	k_spinlock_key_t key;
	hpm_stat_t status;
	int ret;

	if (cfg == NULL) {
		return -EINVAL;
	}

	if ((cfg->data_bits == UART_CFG_DATA_BITS_9) ||
	    ((cfg->data_bits == UART_CFG_DATA_BITS_5) &&
	     (cfg->stop_bits == UART_CFG_STOP_BITS_2))) {
		return -ENOTSUP;
	}

	if ((cfg->flow_ctrl != UART_CFG_FLOW_CTRL_NONE) &&
	    (cfg->flow_ctrl != UART_CFG_FLOW_CTRL_RTS_CTS) &&
	    (cfg->flow_ctrl != UART_CFG_FLOW_CTRL_RS485)) {
		return -ENOTSUP;
	}

	word_length = hpm_uart_word_length(cfg->data_bits);
	parity = hpm_uart_parity(cfg->parity);
	stop_bits = hpm_uart_stop_bits(cfg->stop_bits);
	if ((word_length == (word_length_t)UINT8_MAX) || (parity == (parity_setting_t)UINT8_MAX) ||
	    (stop_bits == (num_of_stop_bits_t)UINT8_MAX)) {
		return -ENOTSUP;
	}

	if (cfg->baudrate == 0U) {
		return -EINVAL;
	}

	ret = hpm_uart_apply_flow_pinctrl(dev, cfg->flow_ctrl);
	if (ret < 0) {
		return ret;
	}

	key = k_spin_lock(&data->lock);
	status = uart_set_baudrate(config->base, cfg->baudrate, data->clock_freq);
	if (status != status_success) {
		k_spin_unlock(&data->lock, key);
		return -EINVAL;
	}
	uart_set_data_bits(config->base, word_length);
	uart_set_parity(config->base, parity);
	uart_set_stop_bits(config->base, stop_bits);
	if (cfg->flow_ctrl == UART_CFG_FLOW_CTRL_RTS_CTS) {
		/* AFE with RTS=1 enables both auto-CTS and auto-RTS. */
		config->base->MCR |= UART_MCR_RTS_MASK;
		uart_enable_flow_control(config->base);
	} else {
		uart_disable_flow_control(config->base);
	}
	data->flow_ctrl = cfg->flow_ctrl;
	data->baudrate = cfg->baudrate;
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_uart_init(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	struct hpm_zephyr_uart_data *data = dev->data;
	uart_config_t uart_config;
	uint32_t clock_freq;
	hpm_stat_t status;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	status = clock_set_source_divider(config->clock_name, config->clock_src, config->clock_div);
	if (status != status_success) {
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

	uart_default_config(config->base, &uart_config);
	uart_config.src_freq_in_hz = clock_freq;
	uart_config.baudrate = config->current_speed;

	status = uart_init(config->base, &uart_config);
	if (status != status_success) {
		return -EINVAL;
	}

#if defined(CONFIG_UART_INTERRUPT_DRIVEN) || defined(CONFIG_UART_ASYNC_API)
	config->irq_config_func(dev);
#endif

	k_spinlock_key_t key;

#ifdef CONFIG_UART_ASYNC_API
	data->dev = dev;
	k_work_init_delayable(&data->tx_done_work, hpm_uart_tx_done_handler);
	k_work_init_delayable(&data->rx_timeout_work, hpm_uart_rx_timeout_handler);
	if (config->dma_dev != NULL) {
		uint32_t rx_filter = BIT(config->rx_dma_channel);
		uint32_t tx_filter = BIT(config->tx_dma_channel);
		int channel;

		if (!device_is_ready(config->dma_dev)) {
			return -ENODEV;
		}
		channel = dma_request_channel(config->dma_dev, &rx_filter);
		if (channel < 0) {
			return channel;
		}
		channel = dma_request_channel(config->dma_dev, &tx_filter);
		if (channel < 0) {
			dma_release_channel(config->dma_dev, config->rx_dma_channel);
			return channel;
		}
	}
#endif

	key = k_spin_lock(&data->lock);
	data->clock_freq = clock_freq;
	data->baudrate = config->current_speed;
	data->flow_ctrl = uart_is_flow_control_enabled(config->base) ? UART_CFG_FLOW_CTRL_RTS_CTS
								     : UART_CFG_FLOW_CTRL_NONE;
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_uart_poll_in(const struct device *dev, unsigned char *c)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
#ifdef CONFIG_UART_ASYNC_API
	const struct hpm_zephyr_uart_data *data = dev->data;

	if (data->rx_active) {
		return -EBUSY;
	}
#endif

	if (uart_check_status(config->base, uart_stat_data_ready)) {
		*c = uart_read_byte(config->base);
		return 0;
	}

	return -1;
}

static void hpm_uart_poll_out(const struct device *dev, unsigned char c)
{
	const struct hpm_zephyr_uart_config *config = dev->config;

	while (!uart_check_status(config->base, uart_stat_tx_slot_avail)) {
	}

	uart_write_byte(config->base, c);
}

static int hpm_uart_err_check(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	uint32_t status = uart_get_status(config->base);
	int err = 0;

	if ((status & UART_LSR_OE_MASK) != 0U) {
		err |= UART_ERROR_OVERRUN;
	}
	if ((status & UART_LSR_PE_MASK) != 0U) {
		err |= UART_ERROR_PARITY;
	}
	if ((status & UART_LSR_FE_MASK) != 0U) {
		err |= UART_ERROR_FRAMING;
	}
	if ((status & UART_LSR_LBREAK_MASK) != 0U) {
		err |= UART_BREAK;
	}

	return err;
}

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static int hpm_uart_fifo_fill(const struct device *dev, const uint8_t *tx_data, int size)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	int count = 0;

	while ((count < size) && uart_check_status(config->base, uart_stat_tx_slot_avail)) {
		uart_write_byte(config->base, tx_data[count]);
		count++;
	}

	return count;
}

static int hpm_uart_fifo_read(const struct device *dev, uint8_t *rx_data, const int size)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	int count = 0;

	while ((count < size) && uart_check_status(config->base, uart_stat_data_ready)) {
		rx_data[count] = uart_read_byte(config->base);
		count++;
	}

	return count;
}

static void hpm_uart_irq_tx_enable(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;

	uart_enable_irq(config->base, uart_intr_tx_slot_avail);
}

static void hpm_uart_irq_tx_disable(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;

	uart_disable_irq(config->base, uart_intr_tx_slot_avail);
}

static int hpm_uart_irq_tx_ready(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;

	return uart_check_status(config->base, uart_stat_tx_slot_avail) ? 1 : 0;
}

static void hpm_uart_irq_rx_enable(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;

	uart_enable_irq(config->base, uart_intr_rx_data_avail_or_timeout);
}

static void hpm_uart_irq_rx_disable(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;

	uart_disable_irq(config->base, uart_intr_rx_data_avail_or_timeout);
}

static int hpm_uart_irq_tx_complete(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;

	return uart_check_status(config->base, uart_stat_transmitter_empty) ? 1 : 0;
}

static int hpm_uart_irq_rx_ready(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;

	return uart_check_status(config->base, uart_stat_data_ready) ? 1 : 0;
}

static void hpm_uart_irq_err_enable(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;

	uart_enable_irq(config->base, uart_intr_rx_line_stat);
}

static void hpm_uart_irq_err_disable(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;

	uart_disable_irq(config->base, uart_intr_rx_line_stat);
}

static int hpm_uart_irq_is_pending(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;

#if defined(HPM_IP_FEATURE_UART_IIR2) && (HPM_IP_FEATURE_UART_IIR2 == 1)
	return (config->base->IIR2 & UART_IIR2_INTRID_MASK) != 1U;
#else
	return (uart_get_irq_id(config->base) & UART_IIR_INTRID_MASK) != 1U;
#endif
}

static void hpm_uart_irq_update(const struct device *dev)
{
	ARG_UNUSED(dev);
}

static void hpm_uart_irq_callback_set(const struct device *dev,
				      uart_irq_callback_user_data_t callback, void *user_data)
{
	struct hpm_zephyr_uart_data *data = dev->data;

	data->callback = callback;
	data->callback_data = user_data;
#ifdef CONFIG_UART_EXCLUSIVE_API_CALLBACKS
	data->async_callback = NULL;
	data->async_user_data = NULL;
#endif
}

#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

#ifdef CONFIG_UART_ASYNC_API
#if defined(HPM_IP_FEATURE_UART_RX_IDLE_DETECT) && (HPM_IP_FEATURE_UART_RX_IDLE_DETECT == 1)
static uint8_t hpm_uart_rx_idle_threshold(const struct hpm_zephyr_uart_data *data)
{
	uint64_t bits;

	if (data->rx_timeout_us == SYS_FOREVER_US) {
		return 0U;
	}

	bits = ((uint64_t)data->baudrate * MAX(data->rx_timeout_us, 1)) + 999999U;
	bits /= 1000000U;
	return (uint8_t)CLAMP(bits, 1U, UINT8_MAX);
}

static void hpm_uart_rx_idle_configure(const struct device *dev, bool enable)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	struct hpm_zephyr_uart_data *data = dev->data;
	uart_rxline_idle_config_t idle = {
		.detect_enable = enable,
		.detect_irq_enable = enable,
		.idle_cond = uart_rxline_idle_cond_rxline_logic_one,
		.threshold = hpm_uart_rx_idle_threshold(data),
	};

	(void)uart_init_rxline_idle_detection(config->base, idle);
	if (!enable) {
		uart_clear_rxline_idle_flag(config->base);
	}
}
#endif
#endif

#if defined(CONFIG_UART_INTERRUPT_DRIVEN) || defined(CONFIG_UART_ASYNC_API)
static void hpm_uart_isr(const struct device *dev)
{
#ifdef CONFIG_UART_ASYNC_API
	const struct hpm_zephyr_uart_config *config = dev->config;
#endif
	struct hpm_zephyr_uart_data *data = dev->data;

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	if (data->callback != NULL) {
		data->callback(dev, data->callback_data);
	}
#endif
#ifdef CONFIG_UART_ASYNC_API
	if (data->rx_active) {
#if defined(HPM_IP_FEATURE_UART_RX_IDLE_DETECT) && (HPM_IP_FEATURE_UART_RX_IDLE_DETECT == 1)
		if (uart_is_rxline_idle(config->base)) {
			uart_clear_rxline_idle_flag(config->base);
			hpm_uart_rx_ready(dev);
		}
#else
		if (data->rx_timeout_us != SYS_FOREVER_US) {
			(void)k_work_reschedule(&data->rx_timeout_work,
						K_USEC(data->rx_timeout_us));
		}
#endif
	}
#endif
}
#endif

#ifdef CONFIG_UART_ASYNC_API
static void hpm_uart_async_callback(const struct device *dev, struct uart_event *event)
{
	struct hpm_zephyr_uart_data *data = dev->data;

	if (data->async_callback != NULL) {
		data->async_callback(dev, event, data->async_user_data);
	}
}

static void hpm_uart_dma_rx_callback(const struct device *dma_dev, void *user_data,
				     uint32_t channel, int status);

static void hpm_uart_dma_tx_callback(const struct device *dma_dev, void *user_data,
				     uint32_t channel, int status)
{
	const struct device *dev = user_data;
	struct hpm_zephyr_uart_data *data = dev->data;
	struct uart_event event;

	ARG_UNUSED(dma_dev);
	ARG_UNUSED(channel);

	if (status == 0) {
		data->tx_dma_done = true;
		(void)k_work_reschedule(&data->tx_done_work, K_NO_WAIT);
		return;
	}

	(void)k_work_cancel_delayable(&data->tx_done_work);
	event.type = UART_TX_ABORTED;
	event.data.tx.buf = data->tx_buf;
	event.data.tx.len = 0U;
	data->tx_buf = NULL;
	data->tx_len = 0U;
	data->tx_dma_done = false;
	hpm_uart_update_dma_mode(dev);
	hpm_uart_async_callback(dev, &event);
}

static void hpm_uart_update_dma_mode(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	struct hpm_zephyr_uart_data *data = dev->data;
	bool dma_enable = (data->tx_buf != NULL) || data->rx_active;

	uart_config_fifo_ctrl(config->base, &(uart_fifo_ctrl_t){
						    .fifo_enable = true,
						    .tx_fifo_level = uart_tx_fifo_trg_not_full,
						    .rx_fifo_level = uart_rx_fifo_trg_not_empty,
						    .dma_enable = dma_enable,
					    });
}

static void hpm_uart_tx_complete(const struct device *dev, bool aborted)
{
	struct hpm_zephyr_uart_data *data = dev->data;
	struct uart_event event = {
		.type = aborted ? UART_TX_ABORTED : UART_TX_DONE,
		.data.tx.buf = data->tx_buf,
		.data.tx.len = aborted ? 0U : data->tx_len,
	};

	data->tx_buf = NULL;
	data->tx_len = 0U;
	data->tx_dma_done = false;
	hpm_uart_update_dma_mode(dev);
	hpm_uart_async_callback(dev, &event);
}

static void hpm_uart_tx_done_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct hpm_zephyr_uart_data *data =
		CONTAINER_OF(dwork, struct hpm_zephyr_uart_data, tx_done_work);
	const struct device *dev = data->dev;
	const struct hpm_zephyr_uart_config *config = dev->config;

	if (data->tx_buf == NULL) {
		return;
	}
	if (data->tx_dma_done && uart_check_status(config->base, uart_stat_transmitter_empty)) {
		hpm_uart_tx_complete(dev, false);
		return;
	}
	if ((data->tx_timeout_us != SYS_FOREVER_US) && sys_timepoint_expired(data->tx_timepoint)) {
		(void)dma_stop(config->dma_dev, config->tx_dma_channel);
		hpm_uart_tx_complete(dev, true);
		return;
	}
	(void)k_work_reschedule(&data->tx_done_work, K_MSEC(1));
}

static void hpm_uart_rx_release(const struct device *dev)
{
	struct hpm_zephyr_uart_data *data = dev->data;
	uint8_t *rx_buf = data->rx_buf;
	struct uart_event event = {
		.type = UART_RX_BUF_RELEASED,
		.data.rx_buf.buf = rx_buf,
	};

	data->rx_buf = NULL;
	data->rx_len = 0U;
	data->rx_offset = 0U;
	if (rx_buf != NULL) {
		hpm_uart_async_callback(dev, &event);
	}
}

static void hpm_uart_rx_disable_internal(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	struct hpm_zephyr_uart_data *data = dev->data;
	struct uart_event disabled = {.type = UART_RX_DISABLED};

	(void)k_work_cancel_delayable(&data->rx_timeout_work);
	if (!data->rx_active) {
		return;
	}

	uart_disable_irq(config->base, uart_intr_rx_data_avail_or_timeout | uart_intr_rx_line_stat);
#if defined(HPM_IP_FEATURE_UART_RX_IDLE_DETECT) && (HPM_IP_FEATURE_UART_RX_IDLE_DETECT == 1)
	hpm_uart_rx_idle_configure(dev, false);
#endif
	(void)dma_stop(config->dma_dev, config->rx_dma_channel);
	data->rx_active = false;
	hpm_uart_update_dma_mode(dev);
	hpm_uart_rx_release(dev);
	if (data->rx_next_buf != NULL) {
		struct uart_event released = {
			.type = UART_RX_BUF_RELEASED,
			.data.rx_buf.buf = data->rx_next_buf,
		};

		data->rx_next_buf = NULL;
		data->rx_next_len = 0U;
		hpm_uart_async_callback(dev, &released);
	}
	hpm_uart_async_callback(dev, &disabled);
}

static size_t hpm_uart_rx_received(const struct hpm_zephyr_uart_config *config,
				   const struct hpm_zephyr_uart_data *data)
{
	struct dma_status status;
	size_t received;

	if (!data->rx_active || data->rx_buf == NULL ||
	    dma_get_status(config->dma_dev, config->rx_dma_channel, &status) < 0) {
		return 0U;
	}

	received = data->rx_len - MIN(status.pending_length, data->rx_len);
	return received;
}

static void hpm_uart_rx_ready(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	struct hpm_zephyr_uart_data *data = dev->data;
	size_t received = hpm_uart_rx_received(config, data);
	struct uart_event event;

	if ((data->rx_buf == NULL) || (received <= data->rx_offset)) {
		return;
	}

	(void)sys_cache_data_invd_range(data->rx_buf, received);
	event.type = UART_RX_RDY;
	event.data.rx.buf = data->rx_buf;
	event.data.rx.offset = data->rx_offset;
	event.data.rx.len = received - data->rx_offset;
	data->rx_offset = received;
	hpm_uart_async_callback(dev, &event);
}

static int hpm_uart_dma_rx_start(const struct device *dev, uint8_t *buf, size_t len)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	struct hpm_zephyr_uart_data *data = dev->data;
	struct dma_config dma_cfg = {0};
	int ret;

	data->rx_block = (struct dma_block_config){
		.source_address = (uint32_t)&config->base->RBR,
		.dest_address = (uint32_t)buf,
		.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
		.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.block_size = len,
	};
	dma_cfg.channel_direction = PERIPHERAL_TO_MEMORY;
	dma_cfg.dma_slot = config->rx_dma_request;
	dma_cfg.source_data_size = 1U;
	dma_cfg.dest_data_size = 1U;
	dma_cfg.source_burst_length = 1U;
	dma_cfg.dest_burst_length = 1U;
	dma_cfg.block_count = 1U;
	dma_cfg.head_block = &data->rx_block;
	dma_cfg.user_data = (void *)dev;
	dma_cfg.dma_callback = hpm_uart_dma_rx_callback;

	(void)sys_cache_data_invd_range(buf, len);
	uart_config_fifo_ctrl(config->base, &(uart_fifo_ctrl_t){
						    .fifo_enable = true,
						    .tx_fifo_level = uart_tx_fifo_trg_not_full,
						    .rx_fifo_level = uart_rx_fifo_trg_not_empty,
						    .dma_enable = true,
					    });
	ret = dma_config(config->dma_dev, config->rx_dma_channel, &dma_cfg);
	if (ret < 0) {
		hpm_uart_update_dma_mode(dev);
		return ret;
	}
	data->rx_buf = buf;
	data->rx_len = len;
	data->rx_offset = 0U;
	data->rx_active = true;
	ret = dma_start(config->dma_dev, config->rx_dma_channel);
	if (ret < 0) {
		data->rx_buf = NULL;
		data->rx_len = 0U;
		data->rx_offset = 0U;
		data->rx_active = false;
		hpm_uart_update_dma_mode(dev);
		return ret;
	}

#if defined(HPM_IP_FEATURE_UART_RX_IDLE_DETECT) && (HPM_IP_FEATURE_UART_RX_IDLE_DETECT == 1)
	if (data->rx_timeout_us != SYS_FOREVER_US) {
		hpm_uart_rx_idle_configure(dev, true);
	}
#endif
	uart_enable_irq(config->base, uart_intr_rx_data_avail_or_timeout | uart_intr_rx_line_stat);
	return 0;
}

static void hpm_uart_dma_rx_callback(const struct device *dma_dev, void *user_data,
				     uint32_t channel, int status)
{
	const struct device *dev = user_data;
	struct hpm_zephyr_uart_data *data = dev->data;

	ARG_UNUSED(dma_dev);
	ARG_UNUSED(channel);

	(void)k_work_cancel_delayable(&data->rx_timeout_work);

	if (status < 0) {
		struct uart_event stopped = {
			.type = UART_RX_STOPPED,
			.data.rx_stop.reason = UART_ERROR_OVERRUN,
			.data.rx_stop.data.buf = data->rx_buf,
			.data.rx_stop.data.offset = data->rx_offset,
			.data.rx_stop.data.len = 0U,
		};

		hpm_uart_async_callback(dev, &stopped);
		hpm_uart_rx_disable_internal(dev);
		return;
	}

	uint8_t *next_buf = data->rx_next_buf;
	size_t next_len = data->rx_next_len;

	data->rx_next_buf = NULL;
	data->rx_next_len = 0U;

	hpm_uart_rx_ready(dev);
	hpm_uart_rx_release(dev);

	if (next_buf == NULL) {
		hpm_uart_rx_disable_internal(dev);
		return;
	}

	if (hpm_uart_dma_rx_start(dev, next_buf, next_len) < 0) {
		data->rx_buf = next_buf;
		data->rx_len = next_len;
		data->rx_active = true;
		hpm_uart_rx_disable_internal(dev);
		return;
	}

	{
		struct uart_event request = {.type = UART_RX_BUF_REQUEST};

		hpm_uart_async_callback(dev, &request);
	}
}

static void hpm_uart_rx_timeout_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct hpm_zephyr_uart_data *data =
		CONTAINER_OF(dwork, struct hpm_zephyr_uart_data, rx_timeout_work);
	const struct device *dev = data->dev;
	int err = hpm_uart_err_check(dev);

	hpm_uart_rx_ready(dev);
	if (err != 0) {
		struct uart_event stopped = {
			.type = UART_RX_STOPPED,
			.data.rx_stop.reason = err,
			.data.rx_stop.data.buf = data->rx_buf,
			.data.rx_stop.data.offset = data->rx_offset,
			.data.rx_stop.data.len = 0U,
		};

		hpm_uart_async_callback(dev, &stopped);
		hpm_uart_rx_disable_internal(dev);
		return;
	}

	if (data->rx_timeout_us != SYS_FOREVER_US && data->rx_buf != NULL) {
#if !defined(HPM_IP_FEATURE_UART_RX_IDLE_DETECT) || (HPM_IP_FEATURE_UART_RX_IDLE_DETECT != 1)
		(void)k_work_reschedule(&data->rx_timeout_work, K_USEC(data->rx_timeout_us));
#endif
	}
}

static int hpm_uart_async_callback_set(const struct device *dev, uart_callback_t callback,
				       void *user_data)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	struct hpm_zephyr_uart_data *data = dev->data;

	if (config->dma_dev == NULL) {
		return -ENODEV;
	}
	data->async_callback = callback;
	data->async_user_data = user_data;
#ifdef CONFIG_UART_EXCLUSIVE_API_CALLBACKS
	data->callback = NULL;
	data->callback_data = NULL;
#endif
	return 0;
}

static int hpm_uart_async_tx(const struct device *dev, const uint8_t *buf, size_t len,
			     int32_t timeout)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	struct hpm_zephyr_uart_data *data = dev->data;
	struct dma_config dma_cfg = {0};
	int ret;

	if ((buf == NULL) || (len == 0U)) {
		return -EINVAL;
	}
	if (config->dma_dev == NULL) {
		return -ENODEV;
	}
	if (data->tx_buf != NULL) {
		return -EBUSY;
	}

	data->tx_block = (struct dma_block_config){
		.source_address = (uint32_t)buf,
		.dest_address = (uint32_t)&config->base->THR,
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
		.block_size = len,
	};
	dma_cfg.channel_direction = MEMORY_TO_PERIPHERAL;
	dma_cfg.dma_slot = config->tx_dma_request;
	dma_cfg.source_data_size = 1U;
	dma_cfg.dest_data_size = 1U;
	dma_cfg.source_burst_length = 1U;
	dma_cfg.dest_burst_length = 1U;
	dma_cfg.block_count = 1U;
	dma_cfg.head_block = &data->tx_block;
	dma_cfg.user_data = (void *)dev;
	dma_cfg.dma_callback = hpm_uart_dma_tx_callback;

	(void)sys_cache_data_flush_range((void *)buf, len);
	ret = dma_config(config->dma_dev, config->tx_dma_channel, &dma_cfg);
	if (ret < 0) {
		return ret;
	}
	data->tx_buf = buf;
	data->tx_len = len;
	data->tx_dma_done = false;
	(void)uart_config_fifo_ctrl(config->base,
				    &(uart_fifo_ctrl_t){
					    .fifo_enable = true,
					    .tx_fifo_level = uart_tx_fifo_trg_not_full,
					    .rx_fifo_level = uart_rx_fifo_trg_not_empty,
					    .dma_enable = true,
				    });
	data->tx_timeout_us = timeout;
	data->tx_timepoint =
		sys_timepoint_calc(timeout == SYS_FOREVER_US ? K_FOREVER : K_USEC(timeout));
	(void)k_work_reschedule(&data->tx_done_work, K_MSEC(1));

	ret = dma_start(config->dma_dev, config->tx_dma_channel);
	if (ret < 0) {
		data->tx_buf = NULL;
		data->tx_len = 0U;
		hpm_uart_update_dma_mode(dev);
		return ret;
	}
	return 0;
}

static int hpm_uart_async_tx_abort(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	struct hpm_zephyr_uart_data *data = dev->data;
	struct uart_event event;

	if (data->tx_buf == NULL) {
		return -EFAULT;
	}

	(void)k_work_cancel_delayable(&data->tx_done_work);
	(void)dma_stop(config->dma_dev, config->tx_dma_channel);
	event.type = UART_TX_ABORTED;
	event.data.tx.buf = data->tx_buf;
	event.data.tx.len = 0U;
	data->tx_buf = NULL;
	data->tx_len = 0U;
	data->tx_dma_done = false;
	hpm_uart_update_dma_mode(dev);
	hpm_uart_async_callback(dev, &event);
	return 0;
}

static int hpm_uart_async_rx_enable(const struct device *dev, uint8_t *buf, size_t len,
				    int32_t timeout)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	struct hpm_zephyr_uart_data *data = dev->data;
	struct uart_event request = {.type = UART_RX_BUF_REQUEST};
	int ret;

	if ((buf == NULL) || (len == 0U)) {
		return -EINVAL;
	}
	if (config->dma_dev == NULL) {
		return -ENODEV;
	}
	if (data->rx_active) {
		return -EBUSY;
	}

	data->rx_timeout_us = timeout;
	ret = hpm_uart_dma_rx_start(dev, buf, len);
	if (ret < 0) {
		data->rx_timeout_us = 0;
		return ret;
	}
#if !defined(HPM_IP_FEATURE_UART_RX_IDLE_DETECT) || (HPM_IP_FEATURE_UART_RX_IDLE_DETECT != 1)
	if (timeout != SYS_FOREVER_US) {
		(void)k_work_reschedule(&data->rx_timeout_work, K_USEC(timeout));
	}
#endif
	hpm_uart_async_callback(dev, &request);
	return 0;
}

static int hpm_uart_async_rx_buf_rsp(const struct device *dev, uint8_t *buf, size_t len)
{
	struct hpm_zephyr_uart_data *data = dev->data;

	if (!data->rx_active || data->rx_next_buf != NULL) {
		return -EBUSY;
	}
	if ((buf == NULL) || (len == 0U)) {
		return -EINVAL;
	}

	data->rx_next_buf = buf;
	data->rx_next_len = len;
#if !defined(HPM_IP_FEATURE_UART_RX_IDLE_DETECT) || (HPM_IP_FEATURE_UART_RX_IDLE_DETECT != 1)
	if (data->rx_timeout_us != SYS_FOREVER_US) {
		(void)k_work_reschedule(&data->rx_timeout_work, K_USEC(data->rx_timeout_us));
	}
#endif
	return 0;
}

static int hpm_uart_async_rx_disable(const struct device *dev)
{
	struct hpm_zephyr_uart_data *data = dev->data;

	if (!data->rx_active) {
		return -EFAULT;
	}
	hpm_uart_rx_ready(dev);
	hpm_uart_rx_disable_internal(dev);
	return 0;
}
#endif /* CONFIG_UART_ASYNC_API */

static int hpm_uart_config_get(const struct device *dev, struct uart_config *cfg)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	struct hpm_zephyr_uart_data *data = dev->data;
	word_length_t word_length;
	parity_setting_t parity;
	num_of_stop_bits_t stop_bits;
	k_spinlock_key_t key;

	if (cfg == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	word_length = uart_get_data_bits(config->base);
	parity = uart_get_parity(config->base);
	stop_bits = uart_get_stop_bits(config->base);
	cfg->baudrate = data->baudrate;
	cfg->flow_ctrl = data->flow_ctrl;
	k_spin_unlock(&data->lock, key);

	switch (word_length) {
	case word_length_5_bits:
		cfg->data_bits = UART_CFG_DATA_BITS_5;
		break;
	case word_length_6_bits:
		cfg->data_bits = UART_CFG_DATA_BITS_6;
		break;
	case word_length_7_bits:
		cfg->data_bits = UART_CFG_DATA_BITS_7;
		break;
	case word_length_8_bits:
		cfg->data_bits = UART_CFG_DATA_BITS_8;
		break;
	default:
		return -EIO;
	}

	switch (parity) {
	case parity_none:
		cfg->parity = UART_CFG_PARITY_NONE;
		break;
	case parity_odd:
		cfg->parity = UART_CFG_PARITY_ODD;
		break;
	case parity_even:
		cfg->parity = UART_CFG_PARITY_EVEN;
		break;
	case parity_always_1:
		cfg->parity = UART_CFG_PARITY_MARK;
		break;
	case parity_always_0:
		cfg->parity = UART_CFG_PARITY_SPACE;
		break;
	default:
		return -EIO;
	}

	switch (stop_bits) {
	case stop_bits_1:
		cfg->stop_bits = UART_CFG_STOP_BITS_1;
		break;
	case stop_bits_1_5:
		cfg->stop_bits = UART_CFG_STOP_BITS_1_5;
		break;
	case stop_bits_2:
		cfg->stop_bits = UART_CFG_STOP_BITS_2;
		break;
	default:
		return -EIO;
	}

	return 0;
}

static int hpm_uart_configure_address_match_impl(const struct device *dev,
						 const struct hpm_uart_address_match_config *config)
{
#if defined(HPM_IP_FEATURE_UART_ADDR_MATCH) && (HPM_IP_FEATURE_UART_ADDR_MATCH == 1)
	const struct hpm_zephyr_uart_config *uart = dev->config;
	struct hpm_zephyr_uart_data *data = dev->data;
	uart_rxline_idle_config_t idle = {
		.idle_cond = uart_rxline_idle_cond_state_machine_idle,
		.threshold = 4U,
	};
	k_spinlock_key_t key;

	if (config == NULL) {
		return -EINVAL;
	}
	idle.detect_enable = config->enable;
	if (config->nine_bit && !config->enable) {
		return -EINVAL;
	}
	if (!config->nine_bit &&
	    ((config->address0 & BIT(7)) != 0U || (config->address1 & BIT(7)) != 0U)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	if (config->nine_bit && ((uart_get_data_bits(uart->base) != word_length_8_bits) ||
				 (uart_get_parity(uart->base) != parity_none))) {
		k_spin_unlock(&data->lock, key);
		return -ENOTSUP;
	}

	uart_disable_address_match(uart->base);
	uart->base->ADDR_CFG &= ~(UART_ADDR_CFG_TXEN_9BIT_MASK | UART_ADDR_CFG_RXEN_ADDR_MSB_MASK |
				  UART_ADDR_CFG_RXEN_9BIT_MASK);
	if (config->enable) {
		uint32_t addr_cfg = UART_ADDR_CFG_TXEN_9BIT_MASK | UART_ADDR_CFG_RXEN_ADDR_MSB_MASK;

		if (config->nine_bit) {
			addr_cfg |= UART_ADDR_CFG_RXEN_9BIT_MASK;
		}
		uart->base->ADDR_CFG |= addr_cfg;
		uint8_t address0 = config->address0;
		uint8_t address1 = config->address1;

		if (!config->nine_bit) {
			address0 |= BIT(7);
			address1 |= BIT(7);
		}
		if (config->address1_enable) {
			uart_enable_address1_match(uart->base, address1);
		}
		if (config->address0_enable) {
			uart_enable_address0_match(uart->base, address0);
		}
	}
	(void)uart_init_rxline_idle_detection(uart->base, idle);
	k_spin_unlock(&data->lock, key);

	return 0;
#else
	ARG_UNUSED(dev);
	ARG_UNUSED(config);

	return -ENOTSUP;
#endif
}

static int hpm_uart_get_address_match_impl(const struct device *dev, bool *matched)
{
#if defined(HPM_IP_FEATURE_UART_ADDR_MATCH) && (HPM_IP_FEATURE_UART_ADDR_MATCH == 1)
	const struct hpm_zephyr_uart_config *uart = dev->config;
	struct hpm_zephyr_uart_data *data = dev->data;
	k_spinlock_key_t key;

	if (matched == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	*matched = uart_is_addr_match(uart->base);
	uart_clear_addr_match_flag(uart->base);
	k_spin_unlock(&data->lock, key);

	return 0;
#else
	ARG_UNUSED(dev);
	ARG_UNUSED(matched);

	return -ENOTSUP;
#endif
}

static int hpm_uart_get_address_match_idle_impl(const struct device *dev, bool *matched)
{
#if defined(HPM_IP_FEATURE_UART_ADDR_MATCH) && (HPM_IP_FEATURE_UART_ADDR_MATCH == 1)
	const struct hpm_zephyr_uart_config *uart = dev->config;
	struct hpm_zephyr_uart_data *data = dev->data;
	k_spinlock_key_t key;

	if (matched == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	*matched = uart_is_addr_match_and_rxidle(uart->base);
	uart_clear_addr_match_and_rxidle_flag(uart->base);
	k_spin_unlock(&data->lock, key);

	return 0;
#else
	ARG_UNUSED(dev);
	ARG_UNUSED(matched);

	return -ENOTSUP;
#endif
}

static DEVICE_API(hpm_uart, hpm_uart_api) = {
	.uart_api =
		{
			.poll_in = hpm_uart_poll_in,
			.poll_out = hpm_uart_poll_out,
			.err_check = hpm_uart_err_check,
			.configure = hpm_uart_configure,
			.config_get = hpm_uart_config_get,
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
			.fifo_fill = hpm_uart_fifo_fill,
			.fifo_read = hpm_uart_fifo_read,
			.irq_tx_enable = hpm_uart_irq_tx_enable,
			.irq_tx_disable = hpm_uart_irq_tx_disable,
			.irq_tx_ready = hpm_uart_irq_tx_ready,
			.irq_rx_enable = hpm_uart_irq_rx_enable,
			.irq_rx_disable = hpm_uart_irq_rx_disable,
			.irq_tx_complete = hpm_uart_irq_tx_complete,
			.irq_rx_ready = hpm_uart_irq_rx_ready,
			.irq_err_enable = hpm_uart_irq_err_enable,
			.irq_err_disable = hpm_uart_irq_err_disable,
			.irq_is_pending = hpm_uart_irq_is_pending,
			.irq_update = hpm_uart_irq_update,
			.irq_callback_set = hpm_uart_irq_callback_set,
#endif
#ifdef CONFIG_UART_ASYNC_API
			.callback_set = hpm_uart_async_callback_set,
			.tx = hpm_uart_async_tx,
			.tx_abort = hpm_uart_async_tx_abort,
			.rx_enable = hpm_uart_async_rx_enable,
			.rx_buf_rsp = hpm_uart_async_rx_buf_rsp,
			.rx_disable = hpm_uart_async_rx_disable,
#endif
		},
	.configure_address_match = hpm_uart_configure_address_match_impl,
	.get_address_match = hpm_uart_get_address_match_impl,
	.get_address_match_idle = hpm_uart_get_address_match_idle_impl,
};

#if defined(CONFIG_UART_INTERRUPT_DRIVEN) || defined(CONFIG_UART_ASYNC_API)
#define HPM_UART_IRQ_CONFIG(n)                                                                     \
	static void hpm_uart_irq_config_##n(const struct device *dev)                              \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_uart_isr,               \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}

#define HPM_UART_IRQ_CONFIG_FIELD(n) .irq_config_func = hpm_uart_irq_config_##n,
#else
#define HPM_UART_IRQ_CONFIG(n)
#define HPM_UART_IRQ_CONFIG_FIELD(n)
#endif

#ifdef CONFIG_UART_ASYNC_API
#define HPM_UART_DMA_CONFIG(n)                                                                     \
	.dma_dev = COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, rx),                                      \
			       (DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(n, rx))), (NULL)),                          \
		 .rx_dma_channel = COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, rx),                                \
				      (DT_INST_DMAS_CELL_BY_NAME(n, rx, channel)), (0)),                           \
			  .tx_dma_channel = COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, tx),                                \
				      (DT_INST_DMAS_CELL_BY_NAME(n, tx, channel)), (0)),                  \
				   .rx_dma_request = COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, rx),                                \
				      (DT_INST_DMAS_CELL_BY_NAME(n, rx, request)), (0)),         \
					    .tx_dma_request = COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, tx),                                \
				      (DT_INST_DMAS_CELL_BY_NAME(n, tx, request)), (0)),
#else
#define HPM_UART_DMA_CONFIG(n)
#endif

#define HPM_UART_INIT(n)                                                                           \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	HPM_UART_IRQ_CONFIG(n)                                                                     \
                                                                                                   \
	static struct hpm_zephyr_uart_data hpm_uart_data_##n;                                      \
                                                                                                   \
	static const struct hpm_zephyr_uart_config hpm_uart_config_##n = {                         \
		.base = (UART_Type *)DT_INST_REG_ADDR(n),                                          \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.clock_name = DT_INST_PROP(n, clock_name),                                         \
		.clock_src = DT_INST_PROP(n, clock_src),                                           \
		.clock_div = DT_INST_CLOCKS_CELL_BY_IDX(n, 0, divider),                            \
		.current_speed = DT_INST_PROP(n, current_speed),                                   \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
		HPM_UART_DMA_CONFIG(n) HPM_UART_IRQ_CONFIG_FIELD(n)};                              \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_uart_init, NULL, &hpm_uart_data_##n, &hpm_uart_config_##n,    \
			      PRE_KERNEL_1, CONFIG_SERIAL_INIT_PRIORITY, &hpm_uart_api)

DT_INST_FOREACH_STATUS_OKAY(HPM_UART_INIT);
