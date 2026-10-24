/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_ptpc

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/ptp_clock.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/ptp_clock/hpmicro_ptpc.h>
#include <hpm_ptpc_drv.h>

LOG_MODULE_REGISTER(ptp_clock_hpmicro, CONFIG_PTP_CLOCK_HPMICRO_LOG_LEVEL);

struct hpm_ptpc_config {
	PTPC_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	uint8_t index;
	void (*irq_config_func)(const struct device *dev);
};

struct hpm_ptpc_data {
	struct k_mutex lock;
	struct k_spinlock event_lock;
	uint32_t nominal_addend;
	uint32_t latched_events;
	uint32_t event_mask;
	hpm_ptpc_event_callback_t callback;
	void *callback_user_data;
};

/* These routing registers are global to both PTPC timestamp channels. */
static struct k_spinlock hpm_ptpc_routing_lock;

static uint32_t hpm_ptpc_channel_event_mask(const struct hpm_ptpc_config *config, uint32_t events)
{
	uint32_t shift = config->index == 0U ? 0U : 16U;
	uint32_t mask = 0U;

	if ((events & HPM_PTPC_EVENT_PPS) != 0U) {
		mask |= PTPC_EVENT_PPS0_MASK << shift;
	}
	if ((events & HPM_PTPC_EVENT_CAPTURE) != 0U) {
		mask |= PTPC_EVENT_CAPTURE0_MASK << shift;
	}
	if ((events & HPM_PTPC_EVENT_COMPARE) != 0U) {
		mask |= PTPC_EVENT_COMPARE0_MASK << shift;
	}

	return mask;
}

static uint32_t hpm_ptpc_hw_events_to_channel(const struct hpm_ptpc_config *config,
					      uint32_t hw_events)
{
	uint32_t shift = config->index == 0U ? 0U : 16U;
	uint32_t events = 0U;

	if ((hw_events & (PTPC_EVENT_PPS0_MASK << shift)) != 0U) {
		events |= HPM_PTPC_EVENT_PPS;
	}
	if ((hw_events & (PTPC_EVENT_CAPTURE0_MASK << shift)) != 0U) {
		events |= HPM_PTPC_EVENT_CAPTURE;
	}
	if ((hw_events & (PTPC_EVENT_COMPARE0_MASK << shift)) != 0U) {
		events |= HPM_PTPC_EVENT_COMPARE;
	}

	return events;
}

static int hpm_ptpc_status_to_errno(hpm_stat_t status)
{
	switch (status) {
	case status_success:
		return 0;
	case status_invalid_argument:
		return -EINVAL;
	default:
		return -EIO;
	}
}

static void hpm_ptpc_read_time(const struct hpm_ptpc_config *config, uint32_t *seconds,
			       uint32_t *nanoseconds)
{
	uint32_t sec_before;
	uint32_t sec_after;
	uint32_t nsec;

	do {
		sec_before = ptpc_get_timestamp_second(config->base, config->index);
		nsec = ptpc_get_timestamp_ns(config->base, config->index);
		sec_after = ptpc_get_timestamp_second(config->base, config->index);
	} while (sec_before != sec_after);

	*seconds = sec_after;
	*nanoseconds = nsec;
}

static int hpm_ptpc_set(const struct device *dev, struct net_ptp_time *tm)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	hpm_stat_t status;

	if ((tm == NULL) || (tm->second > UINT32_MAX) || (tm->nanosecond >= NSEC_PER_SEC)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	ptpc_disable_timer(config->base, config->index);
	status = ptpc_init_timer_with_initial(config->base, config->index, (uint32_t)tm->second,
					      tm->nanosecond, ptpc_counting_increment);
	if (status == status_success) {
		ptpc_enable_timer(config->base, config->index);
	}
	k_mutex_unlock(&data->lock);

	return hpm_ptpc_status_to_errno(status);
}

static int hpm_ptpc_get(const struct device *dev, struct net_ptp_time *tm)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	uint32_t seconds;
	uint32_t nanoseconds;

	if (tm == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	hpm_ptpc_read_time(config, &seconds, &nanoseconds);
	k_mutex_unlock(&data->lock);

	tm->second = seconds;
	tm->nanosecond = nanoseconds;

	return 0;
}

static int hpm_ptpc_adjust(const struct device *dev, int increment)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	ptpc_counting_mode mode;
	uint64_t magnitude;
	uint32_t seconds;
	uint32_t nanoseconds;
	hpm_stat_t status;

	if (increment == 0) {
		return 0;
	}

	if (increment < 0) {
		mode = ptpc_counting_decrement;
		magnitude = (uint64_t)(-(int64_t)increment);
	} else {
		mode = ptpc_counting_increment;
		magnitude = (uint64_t)increment;
	}

	seconds = (uint32_t)(magnitude / NSEC_PER_SEC);
	nanoseconds = (uint32_t)(magnitude % NSEC_PER_SEC);

	k_mutex_lock(&data->lock, K_FOREVER);
	status = ptpc_update_timer(config->base, config->index, seconds, nanoseconds, mode);
	k_mutex_unlock(&data->lock);

	return hpm_ptpc_status_to_errno(status);
}

static int hpm_ptpc_rate_adjust(const struct device *dev, double ratio)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	double adjusted;
	uint32_t addend;

	if ((ratio <= 0.0) || (ratio > (double)UINT32_MAX / data->nominal_addend)) {
		return -ERANGE;
	}

	adjusted = (double)data->nominal_addend * ratio;
	addend = (uint32_t)(adjusted + 0.5);

	if (addend == 0U) {
		return -ERANGE;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	config->base->PTPC[config->index].ADDEND = addend;
	k_mutex_unlock(&data->lock);

	return 0;
}

static int hpm_ptpc_drv_configure_capture(const struct device *dev,
					  const struct hpm_ptpc_capture_config *capture)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	ptpc_capture_trigger_type_t trigger;
	k_spinlock_key_t key;

	if ((capture == NULL) || ((unsigned int)capture->edge > HPM_PTPC_CAPTURE_BOTH)) {
		return -EINVAL;
	}

	switch (capture->edge) {
	case HPM_PTPC_CAPTURE_DISABLED:
		trigger = ptpc_capture_trigger_none;
		break;
	case HPM_PTPC_CAPTURE_RISING:
		trigger = ptpc_capture_trigger_on_rising_edge;
		break;
	case HPM_PTPC_CAPTURE_FALLING:
		trigger = ptpc_capture_trigger_on_failing_edge;
		break;
	case HPM_PTPC_CAPTURE_BOTH:
		trigger = ptpc_capture_trigger_on_both_edges;
		break;
	default:
		return -EINVAL;
	}

	key = k_spin_lock(&data->event_lock);
	ptpc_config_capture(config->base, config->index, trigger);
	if (capture->keep) {
		ptpc_enable_capture_keep(config->base, config->index);
	} else {
		ptpc_disable_capture_keep(config->base, config->index);
	}
	k_spin_unlock(&data->event_lock, key);

	return 0;
}

static int hpm_ptpc_drv_read_capture(const struct device *dev, struct net_ptp_time *capture)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	k_spinlock_key_t key;

	if (capture == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->event_lock);
	capture->second = ptpc_get_capture_second(config->base, config->index);
	capture->nanosecond = ptpc_get_capture_ns(config->base, config->index);
	k_spin_unlock(&data->event_lock, key);

	return 0;
}

static int hpm_ptpc_drv_configure_compare(const struct device *dev,
					  const struct hpm_ptpc_compare_config *compare)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	k_spinlock_key_t key;

	if ((compare == NULL) || (compare->target.second > UINT32_MAX) ||
	    (compare->target.nanosecond >= NSEC_PER_SEC)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->event_lock);
	if (compare->enable) {
		ptpc_config_compare(config->base, config->index, (uint32_t)compare->target.second,
				    compare->target.nanosecond);
	} else {
		config->base->PTPC[config->index].CTRL0 &= ~PTPC_PTPC_CTRL0_COMP_EN_MASK;
	}
	k_spin_unlock(&data->event_lock, key);

	return 0;
}

static int hpm_ptpc_drv_configure_pps(const struct device *dev, uint8_t frequency_exponent)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	k_spinlock_key_t key;
	hpm_stat_t status;

	if (frequency_exponent > 15U) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->event_lock);
	status = ptpc_set_pps(config->base, config->index, frequency_exponent);
	k_spin_unlock(&data->event_lock, key);

	return hpm_ptpc_status_to_errno(status);
}

static int hpm_ptpc_drv_configure_can_time_source(const struct device *dev, uint8_t can_port,
						  enum hpm_ptpc_can_time_source source)
{
	const struct hpm_ptpc_config *config = dev->config;
	k_spinlock_key_t key;

	if ((can_port >= HPM_PTPC_CAN_PORT_COUNT) || ((source != HPM_PTPC_CAN_TIME_SOURCE_PTPC0) &&
						      (source != HPM_PTPC_CAN_TIME_SOURCE_PTPC1))) {
		return -EINVAL;
	}

	key = k_spin_lock(&hpm_ptpc_routing_lock);
	config->base->TIME_SEL =
		(config->base->TIME_SEL & ~BIT(can_port)) | ((uint32_t)source << can_port);
	k_spin_unlock(&hpm_ptpc_routing_lock, key);

	return 0;
}

static int hpm_ptpc_drv_get_can_time_source(const struct device *dev, uint8_t can_port,
					    enum hpm_ptpc_can_time_source *source)
{
	const struct hpm_ptpc_config *config = dev->config;

	if ((can_port >= HPM_PTPC_CAN_PORT_COUNT) || (source == NULL)) {
		return -EINVAL;
	}

	*source = (enum hpm_ptpc_can_time_source)((config->base->TIME_SEL >> can_port) & 1U);

	return 0;
}

static uint8_t hpm_ptpc_tsu_input_shift(uint8_t tsu_input)
{
	return (uint8_t)(8U + (tsu_input * 6U));
}

static int hpm_ptpc_drv_configure_tsu_input(const struct device *dev, uint8_t tsu_input,
					    enum hpm_ptpc_tsu_input_source source)
{
	const struct hpm_ptpc_config *config = dev->config;
	k_spinlock_key_t key;
	uint32_t shift;

	if ((tsu_input >= HPM_PTPC_TSU_INPUT_COUNT) ||
	    ((source != HPM_PTPC_TSU_INPUT_PTPC0) && (source != HPM_PTPC_TSU_INPUT_PTPC1))) {
		return -EINVAL;
	}

	shift = hpm_ptpc_tsu_input_shift(tsu_input);
	key = k_spin_lock(&hpm_ptpc_routing_lock);
	config->base->PTPC_CAN_TS_SEL =
		(config->base->PTPC_CAN_TS_SEL & ~(0x3FU << shift)) | ((uint32_t)source << shift);
	k_spin_unlock(&hpm_ptpc_routing_lock, key);

	return 0;
}

static int hpm_ptpc_drv_get_tsu_input(const struct device *dev, uint8_t tsu_input,
				      enum hpm_ptpc_tsu_input_source *source)
{
	const struct hpm_ptpc_config *config = dev->config;
	uint32_t value;

	if ((tsu_input >= HPM_PTPC_TSU_INPUT_COUNT) || (source == NULL)) {
		return -EINVAL;
	}

	value = (config->base->PTPC_CAN_TS_SEL >> hpm_ptpc_tsu_input_shift(tsu_input)) & 0x3FU;
	if ((value != HPM_PTPC_TSU_INPUT_PTPC0) && (value != HPM_PTPC_TSU_INPUT_PTPC1)) {
		return -EIO;
	}

	*source = (enum hpm_ptpc_tsu_input_source)value;

	return 0;
}

static int hpm_ptpc_drv_get_events(const struct device *dev, uint32_t *events)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	k_spinlock_key_t key;

	if (events == NULL) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->event_lock);
	*events = data->latched_events |
		  hpm_ptpc_hw_events_to_channel(config, ptpc_get_irq_status(config->base));
	data->latched_events = 0U;
	k_spin_unlock(&data->event_lock, key);

	return 0;
}

static int hpm_ptpc_drv_clear_events(const struct device *dev, uint32_t events)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t hw_mask;

	if ((events & ~HPM_PTPC_EVENT_ALL) != 0U) {
		return -EINVAL;
	}

	hw_mask = hpm_ptpc_channel_event_mask(config, events);
	key = k_spin_lock(&data->event_lock);
	data->latched_events &= ~events;
	ptpc_clear_irq_status(config->base, hw_mask);
	k_spin_unlock(&data->event_lock, key);

	return 0;
}

static int hpm_ptpc_drv_set_event_callback(const struct device *dev, uint32_t event_mask,
					   hpm_ptpc_event_callback_t callback, void *user_data)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t hw_mask;

	if ((event_mask & ~HPM_PTPC_EVENT_ALL) != 0U) {
		return -EINVAL;
	}

	hw_mask = hpm_ptpc_channel_event_mask(config, HPM_PTPC_EVENT_ALL);
	key = k_spin_lock(&data->event_lock);
	ptpc_irq_disable(config->base, hw_mask);
	ptpc_clear_irq_status(config->base, hw_mask);
	data->latched_events = 0U;
	if ((callback == NULL) || (event_mask == 0U)) {
		data->callback = NULL;
		data->callback_user_data = NULL;
		data->event_mask = 0U;
	} else {
		data->callback = callback;
		data->callback_user_data = user_data;
		data->event_mask = event_mask;
		ptpc_irq_enable(config->base, hpm_ptpc_channel_event_mask(config, event_mask));
	}
	k_spin_unlock(&data->event_lock, key);

	return 0;
}

static void hpm_ptpc_isr(const struct device *dev)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	hpm_ptpc_event_callback_t callback;
	void *user_data;
	k_spinlock_key_t key;
	uint32_t events;

	key = k_spin_lock(&data->event_lock);
	events = hpm_ptpc_hw_events_to_channel(config, ptpc_get_irq_status(config->base)) &
		 data->event_mask;
	if (events == 0U) {
		k_spin_unlock(&data->event_lock, key);
		return;
	}

	ptpc_clear_irq_status(config->base, hpm_ptpc_channel_event_mask(config, events));
	data->latched_events |= events;
	callback = data->callback;
	user_data = data->callback_user_data;
	k_spin_unlock(&data->event_lock, key);

	if (callback != NULL) {
		callback(dev, events, user_data);
	}
}

static DEVICE_API(hpm_ptpc, hpm_ptpc_api) = {
	.ptp_clock_api =
		{
			.set = hpm_ptpc_set,
			.get = hpm_ptpc_get,
			.adjust = hpm_ptpc_adjust,
			.rate_adjust = hpm_ptpc_rate_adjust,
		},
	.configure_capture = hpm_ptpc_drv_configure_capture,
	.read_capture = hpm_ptpc_drv_read_capture,
	.configure_compare = hpm_ptpc_drv_configure_compare,
	.configure_pps = hpm_ptpc_drv_configure_pps,
	.configure_can_time_source = hpm_ptpc_drv_configure_can_time_source,
	.get_can_time_source = hpm_ptpc_drv_get_can_time_source,
	.configure_tsu_input = hpm_ptpc_drv_configure_tsu_input,
	.get_tsu_input = hpm_ptpc_drv_get_tsu_input,
	.get_events = hpm_ptpc_drv_get_events,
	.clear_events = hpm_ptpc_drv_clear_events,
	.set_event_callback = hpm_ptpc_drv_set_event_callback,
};

static int hpm_ptpc_init(const struct device *dev)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	ptpc_config_t ptpc_config;
	uint32_t clock_rate;
	uint32_t counter_incr;
	uint64_t denominator;
	uint64_t nominal_addend;
	hpm_stat_t status;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		return ret;
	}

	ret = clock_control_get_rate(config->clock_dev, config->clock_subsys, &clock_rate);
	if ((ret < 0) || (clock_rate == 0U)) {
		return ret < 0 ? ret : -EINVAL;
	}

	ptpc_get_default_config(config->base, &ptpc_config);
	ptpc_config.capture_trigger = ptpc_capture_trigger_none;
	ptpc_config.capture_keep = false;
	ptpc_config.ns_rollover_mode = ptpc_ns_counter_rollover_digital;
	ptpc_config.src_frequency = clock_rate;
	/*
	 * The SDK field selects CTRL0.FINE_COARSE_SEL directly despite its name.
	 * Set it so ADDEND controls the fractional nanosecond accumulation.
	 */
	ptpc_config.coarse_increment = true;

	/*
	 * Use two nominal increments per clock. The fractional accumulator then
	 * runs at half rate, leaving headroom for both faster and slower tuning.
	 */
	counter_incr = 2U * DIV_ROUND_UP(NSEC_PER_SEC, clock_rate);
	if ((counter_incr == 0U) || (counter_incr > UINT8_MAX)) {
		return -ERANGE;
	}

	denominator = (uint64_t)clock_rate * counter_incr;
	nominal_addend = (((uint64_t)NSEC_PER_SEC << 32) + denominator / 2U) / denominator;
	if ((nominal_addend == 0U) || (nominal_addend > UINT32_MAX)) {
		return -ERANGE;
	}

	status = ptpc_init(config->base, config->index, &ptpc_config);
	if (status != status_success) {
		return hpm_ptpc_status_to_errno(status);
	}

	/* ptpc_init() derives SS_INCR for one increment per clock; override it. */
	ptpc_disable_timer(config->base, config->index);
	ptpc_set_ns_counter_step(config->base, config->index, counter_incr);
	config->base->PTPC[config->index].ADDEND = (uint32_t)nominal_addend;
	ptpc_init_timer(config->base, config->index);
	ptpc_enable_timer(config->base, config->index);
	data->nominal_addend = (uint32_t)nominal_addend;
	k_mutex_init(&data->lock);
	config->irq_config_func(dev);

	return 0;
}

#define HPM_PTPC_IRQ_CONFIG(n)                                                                     \
	static void hpm_ptpc_irq_config_##n(const struct device *dev)                              \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_ptpc_isr,               \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}

#define HPM_PTPC_INIT(n)                                                                           \
	HPM_PTPC_IRQ_CONFIG(n)                                                                     \
	BUILD_ASSERT(DT_INST_REG_ADDR(n) < 2U, "invalid PTPC counter index");                      \
                                                                                                   \
	static struct hpm_ptpc_data hpm_ptpc_data_##n;                                             \
                                                                                                   \
	static const struct hpm_ptpc_config hpm_ptpc_config_##n = {                                \
		.base = (PTPC_Type *)DT_REG_ADDR(DT_INST_PARENT(n)),                               \
		.clock_dev = DEVICE_DT_GET(DT_CLOCKS_CTLR(DT_INST_PARENT(n))),                     \
		.clock_subsys = (clock_control_subsys_t)DT_CLOCKS_CELL(DT_INST_PARENT(n), name),   \
		.index = DT_INST_REG_ADDR(n),                                                      \
		.irq_config_func = hpm_ptpc_irq_config_##n,                                        \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_ptpc_init, NULL, &hpm_ptpc_data_##n, &hpm_ptpc_config_##n,    \
			      POST_KERNEL, CONFIG_PTP_CLOCK_INIT_PRIORITY, &hpm_ptpc_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_PTPC_INIT)
