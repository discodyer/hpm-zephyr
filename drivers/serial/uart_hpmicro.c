/*
 * Copyright (c) 2022-2026 HPMicro
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_uart

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#include <hpm_common.h>
#include <hpm_clock_drv.h>
#include <hpm_soc.h>
#include <hpm_uart_drv.h>

struct hpm_zephyr_uart_config {
	UART_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	clock_name_t clock_name;
	clock_source_t clock_src;
	uint32_t clock_div;
	uint32_t current_speed;
	const struct pinctrl_dev_config *pincfg;
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	uart_irq_config_func_t irq_config_func;
#endif
};

struct hpm_zephyr_uart_data {
	struct k_spinlock lock;
	uint32_t clock_freq;
	uint32_t baudrate;
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	uart_irq_callback_user_data_t callback;
	void *callback_data;
#endif
};

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

static int hpm_uart_configure(const struct device *dev, const struct uart_config *cfg)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
	struct hpm_zephyr_uart_data *data = dev->data;
	word_length_t word_length;
	parity_setting_t parity;
	num_of_stop_bits_t stop_bits;
	k_spinlock_key_t key;
	hpm_stat_t status;

	if (cfg == NULL) {
		return -EINVAL;
	}

	if ((cfg->flow_ctrl != UART_CFG_FLOW_CTRL_NONE) ||
	    (cfg->data_bits == UART_CFG_DATA_BITS_9) ||
	    ((cfg->data_bits == UART_CFG_DATA_BITS_5) &&
	     (cfg->stop_bits == UART_CFG_STOP_BITS_2))) {
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

	key = k_spin_lock(&data->lock);
	status = uart_set_baudrate(config->base, cfg->baudrate, data->clock_freq);
	if (status != status_success) {
		k_spin_unlock(&data->lock, key);
		return -EINVAL;
	}
	uart_set_data_bits(config->base, word_length);
	uart_set_parity(config->base, parity);
	uart_set_stop_bits(config->base, stop_bits);
	data->baudrate = cfg->baudrate;
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_uart_init(const struct device *dev)
{
	const struct hpm_zephyr_uart_config *config = dev->config;
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

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	config->irq_config_func(dev);
#endif

	struct hpm_zephyr_uart_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	data->clock_freq = clock_freq;
	data->baudrate = config->current_speed;
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_uart_poll_in(const struct device *dev, unsigned char *c)
{
	const struct hpm_zephyr_uart_config *config = dev->config;

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
}

static void hpm_uart_isr(const struct device *dev)
{
	struct hpm_zephyr_uart_data *data = dev->data;

	if (data->callback != NULL) {
		data->callback(dev, data->callback_data);
	}
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

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

	cfg->flow_ctrl = UART_CFG_FLOW_CTRL_NONE;
	return 0;
}

static const struct uart_driver_api hpm_uart_api = {
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
};

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
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
		HPM_UART_IRQ_CONFIG_FIELD(n)};                                                     \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_uart_init, NULL, &hpm_uart_data_##n, &hpm_uart_config_##n,    \
			      PRE_KERNEL_1, CONFIG_SERIAL_INIT_PRIORITY, &hpm_uart_api)

DT_INST_FOREACH_STATUS_OKAY(HPM_UART_INIT);
