/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_gptmr

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hpm_clock_drv.h>
#include <hpm_gptmr_drv.h>

LOG_MODULE_REGISTER(counter_hpmicro, CONFIG_COUNTER_LOG_LEVEL);

#define HPM_GPTMR_CHANNELS 4U
#define HPM_GPTMR_TOP_CH   0U
#define HPM_GPTMR_MAX_TOP  UINT32_MAX

struct hpm_gptmr_alarm {
	counter_alarm_callback_t callback;
	void *user_data;
};

struct hpm_gptmr_config {
	struct counter_config_info info;
	GPTMR_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	clock_name_t clock_name;
	clock_source_t clock_src;
	uint32_t clock_div;
	void (*irq_config_func)(const struct device *dev);
};

struct hpm_gptmr_data {
	struct k_spinlock lock;
	struct hpm_gptmr_alarm alarms[HPM_GPTMR_CHANNELS];
	counter_top_callback_t top_callback;
	void *top_user_data;
	uint32_t top;
	uint32_t guard_period;
	uint32_t frequency;
	bool running;
};

static uint32_t hpm_gptmr_counter_ticks(const struct device *dev, uint32_t channel)
{
	const struct hpm_gptmr_config *config = dev->config;

	return gptmr_channel_get_counter(config->base, channel, gptmr_counter_type_normal);
}

static void hpm_gptmr_isr(const struct device *dev)
{
	const struct hpm_gptmr_config *config = dev->config;
	struct hpm_gptmr_data *data = dev->data;
	struct hpm_gptmr_alarm pending_alarms[HPM_GPTMR_CHANNELS] = {0};
	counter_top_callback_t top_callback = NULL;
	void *top_user_data = NULL;
	uint32_t status;
	uint32_t channel;
	k_spinlock_key_t key;

	status = gptmr_get_status(config->base);
	gptmr_clear_status(config->base, status);

	key = k_spin_lock(&data->lock);
	for (channel = 0U; channel < HPM_GPTMR_CHANNELS; channel++) {
		uint32_t cmp_status = status & GPTMR_CH_CMP_STAT_MASK(channel, 0U);
		if (cmp_status == 0U) {
			continue;
		}

		pending_alarms[channel] = data->alarms[channel];
		data->alarms[channel].callback = NULL;
		data->alarms[channel].user_data = NULL;
		gptmr_disable_irq(config->base, GPTMR_CH_CMP_IRQ_MASK(channel, 0U));
	}

	if ((status & GPTMR_CH_RLD_STAT_MASK(HPM_GPTMR_TOP_CH)) != 0U) {
		top_callback = data->top_callback;
		top_user_data = data->top_user_data;
	}
	k_spin_unlock(&data->lock, key);

	if (top_callback != NULL) {
		top_callback(dev, top_user_data);
	}

	for (channel = 0U; channel < HPM_GPTMR_CHANNELS; channel++) {
		if (pending_alarms[channel].callback != NULL) {
			pending_alarms[channel].callback(dev, channel,
							 hpm_gptmr_counter_ticks(dev, channel),
							 pending_alarms[channel].user_data);
		}
	}
}

static int hpm_gptmr_start(const struct device *dev)
{
	const struct hpm_gptmr_config *config = dev->config;
	struct hpm_gptmr_data *data = dev->data;
	uint32_t channel;

	for (channel = 0U; channel < HPM_GPTMR_CHANNELS; channel++) {
		gptmr_start_counter(config->base, channel);
	}
	data->running = true;

	return 0;
}

static int hpm_gptmr_stop(const struct device *dev)
{
	const struct hpm_gptmr_config *config = dev->config;
	struct hpm_gptmr_data *data = dev->data;
	uint32_t channel;

	for (channel = 0U; channel < HPM_GPTMR_CHANNELS; channel++) {
		gptmr_stop_counter(config->base, channel);
	}
	data->running = false;

	return 0;
}

static int hpm_gptmr_get_value(const struct device *dev, uint32_t *ticks)
{
	*ticks = hpm_gptmr_counter_ticks(dev, HPM_GPTMR_TOP_CH);

	return 0;
}

static int hpm_gptmr_reset(const struct device *dev)
{
	const struct hpm_gptmr_config *config = dev->config;
	uint32_t channel;

	for (channel = 0U; channel < HPM_GPTMR_CHANNELS; channel++) {
		gptmr_channel_reset_count(config->base, channel);
	}

	return 0;
}

static int hpm_gptmr_set_value(const struct device *dev, uint32_t ticks)
{
	const struct hpm_gptmr_config *config = dev->config;
	struct hpm_gptmr_data *data = dev->data;
	uint32_t channel;

	if (ticks > data->top) {
		return -EINVAL;
	}

	for (channel = 0U; channel < HPM_GPTMR_CHANNELS; channel++) {
		gptmr_channel_update_count(config->base, channel, ticks);
	}

	return 0;
}

static int hpm_gptmr_set_top_value(const struct device *dev, const struct counter_top_cfg *cfg)
{
	const struct hpm_gptmr_config *config = dev->config;
	struct hpm_gptmr_data *data = dev->data;
	uint32_t current;
	uint32_t channel;
	k_spinlock_key_t key;

	if ((cfg == NULL) || (cfg->ticks == 0U)) {
		return -EINVAL;
	}

	if ((cfg->flags & ~(COUNTER_TOP_CFG_DONT_RESET | COUNTER_TOP_CFG_RESET_WHEN_LATE)) != 0U) {
		return -ENOTSUP;
	}

	key = k_spin_lock(&data->lock);
	for (channel = 0U; channel < HPM_GPTMR_CHANNELS; channel++) {
		if (data->alarms[channel].callback != NULL) {
			k_spin_unlock(&data->lock, key);
			return -EBUSY;
		}
	}

	current = hpm_gptmr_counter_ticks(dev, HPM_GPTMR_TOP_CH);
	if ((cfg->flags & COUNTER_TOP_CFG_DONT_RESET) != 0U) {
		if (current >= cfg->ticks) {
			if ((cfg->flags & COUNTER_TOP_CFG_RESET_WHEN_LATE) == 0U) {
				k_spin_unlock(&data->lock, key);
				return -ETIME;
			}
			for (channel = 0U; channel < HPM_GPTMR_CHANNELS; channel++) {
				gptmr_channel_reset_count(config->base, channel);
			}
		}
	} else {
		for (channel = 0U; channel < HPM_GPTMR_CHANNELS; channel++) {
			gptmr_channel_reset_count(config->base, channel);
		}
	}

	for (channel = 0U; channel < HPM_GPTMR_CHANNELS; channel++) {
		gptmr_channel_config_update_reload(config->base, channel, cfg->ticks);
	}
	gptmr_disable_irq(config->base, GPTMR_CH_RLD_IRQ_MASK(HPM_GPTMR_TOP_CH));
	gptmr_clear_status(config->base, GPTMR_CH_RLD_STAT_MASK(HPM_GPTMR_TOP_CH));

	data->top = cfg->ticks;
	data->top_callback = cfg->callback;
	data->top_user_data = cfg->user_data;
	if (cfg->callback != NULL) {
		gptmr_enable_irq(config->base, GPTMR_CH_RLD_IRQ_MASK(HPM_GPTMR_TOP_CH));
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static uint32_t hpm_gptmr_get_top_value(const struct device *dev)
{
	struct hpm_gptmr_data *data = dev->data;

	return data->top;
}

static int hpm_gptmr_set_alarm(const struct device *dev, uint8_t chan_id,
			       const struct counter_alarm_cfg *alarm_cfg)
{
	const struct hpm_gptmr_config *config = dev->config;
	struct hpm_gptmr_data *data = dev->data;
	uint64_t period;
	uint32_t now;
	uint32_t target;
	k_spinlock_key_t key;

	if (chan_id >= HPM_GPTMR_CHANNELS) {
		return -ENOTSUP;
	}

	if ((alarm_cfg == NULL) || (alarm_cfg->callback == NULL)) {
		return -EINVAL;
	}

	if ((alarm_cfg->flags & ~COUNTER_ALARM_CFG_ABSOLUTE) != 0U) {
		return -ENOTSUP;
	}

	key = k_spin_lock(&data->lock);
	if (data->alarms[chan_id].callback != NULL) {
		k_spin_unlock(&data->lock, key);
		return -EBUSY;
	}

	if (alarm_cfg->ticks > data->top) {
		k_spin_unlock(&data->lock, key);
		return -EINVAL;
	}

	period = (uint64_t)data->top + 1U;
	now = hpm_gptmr_counter_ticks(dev, chan_id);
	if ((alarm_cfg->flags & COUNTER_ALARM_CFG_ABSOLUTE) != 0U) {
		target = alarm_cfg->ticks;
		if ((now >= target) && ((now - target) < data->guard_period)) {
			k_spin_unlock(&data->lock, key);
			return -ETIME;
		}
	} else {
		target = (uint32_t)(((uint64_t)now + alarm_cfg->ticks) % period);
	}

	gptmr_disable_irq(config->base, GPTMR_CH_CMP_IRQ_MASK(chan_id, 0U));
	gptmr_clear_status(config->base, GPTMR_CH_CMP_STAT_MASK(chan_id, 0U));
	gptmr_update_cmp(config->base, chan_id, 0U, target);
	data->alarms[chan_id].callback = alarm_cfg->callback;
	data->alarms[chan_id].user_data = alarm_cfg->user_data;
	gptmr_enable_irq(config->base, GPTMR_CH_CMP_IRQ_MASK(chan_id, 0U));
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_gptmr_cancel_alarm(const struct device *dev, uint8_t chan_id)
{
	const struct hpm_gptmr_config *config = dev->config;
	struct hpm_gptmr_data *data = dev->data;
	k_spinlock_key_t key;

	if (chan_id >= HPM_GPTMR_CHANNELS) {
		return -ENOTSUP;
	}

	key = k_spin_lock(&data->lock);
	gptmr_disable_irq(config->base, GPTMR_CH_CMP_IRQ_MASK(chan_id, 0U));
	gptmr_clear_status(config->base, GPTMR_CH_CMP_STAT_MASK(chan_id, 0U));
	data->alarms[chan_id].callback = NULL;
	data->alarms[chan_id].user_data = NULL;
	k_spin_unlock(&data->lock, key);

	return 0;
}

static uint32_t hpm_gptmr_get_pending_int(const struct device *dev)
{
	const struct hpm_gptmr_config *config = dev->config;

	return config->base->SR != 0U ? 1U : 0U;
}

static uint32_t hpm_gptmr_get_guard_period(const struct device *dev, uint32_t flags)
{
	struct hpm_gptmr_data *data = dev->data;

	if ((flags & ~COUNTER_GUARD_PERIOD_LATE_TO_SET) != 0U) {
		return 0U;
	}

	return data->guard_period;
}

static int hpm_gptmr_set_guard_period(const struct device *dev, uint32_t guard, uint32_t flags)
{
	struct hpm_gptmr_data *data = dev->data;

	if ((flags & ~COUNTER_GUARD_PERIOD_LATE_TO_SET) != 0U) {
		return -ENOTSUP;
	}

	data->guard_period = guard;

	return 0;
}

static uint32_t hpm_gptmr_get_frequency(const struct device *dev)
{
	struct hpm_gptmr_data *data = dev->data;

	return data->frequency;
}

static int hpm_gptmr_init(const struct device *dev)
{
	const struct hpm_gptmr_config *config = dev->config;
	struct hpm_gptmr_data *data = dev->data;
	gptmr_channel_config_t channel_cfg;
	uint32_t frequency;
	uint32_t channel;
	hpm_stat_t status;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		return ret;
	}

	status = clock_set_source_divider(config->clock_name, config->clock_src, config->clock_div);
	if ((status != status_success) && (status != status_clk_fixed) &&
	    (status != status_clk_shared_cpu0)) {
		return -EIO;
	}

	ret = clock_control_get_rate(config->clock_dev, config->clock_subsys, &frequency);
	if (ret < 0) {
		return ret;
	}

	data->frequency = frequency;
	data->top = HPM_GPTMR_MAX_TOP;

	for (channel = 0U; channel < HPM_GPTMR_CHANNELS; channel++) {
		gptmr_stop_counter(config->base, channel);
		gptmr_channel_reset_count(config->base, channel);
		gptmr_clear_status(config->base, GPTMR_CH_RLD_STAT_MASK(channel) |
							 GPTMR_CH_CAP_STAT_MASK(channel) |
							 GPTMR_CH_CMP_STAT_MASK(channel, 0U) |
							 GPTMR_CH_CMP_STAT_MASK(channel, 1U));
		gptmr_disable_irq(config->base, GPTMR_CH_RLD_IRQ_MASK(channel) |
							GPTMR_CH_CAP_IRQ_MASK(channel) |
							GPTMR_CH_CMP_IRQ_MASK(channel, 0U) |
							GPTMR_CH_CMP_IRQ_MASK(channel, 1U));
	}

	gptmr_channel_get_default_config(config->base, &channel_cfg);
	channel_cfg.enable_cmp_output = false;
	channel_cfg.enable_software_sync = false;
	for (channel = 0U; channel < HPM_GPTMR_CHANNELS; channel++) {
		channel_cfg.reload = data->top;
		if (gptmr_channel_config(config->base, channel, &channel_cfg, false) !=
		    status_success) {
			return -EIO;
		}
	}

	config->irq_config_func(dev);

	return 0;
}

static DEVICE_API(counter, hpm_gptmr_api) = {
	.start = hpm_gptmr_start,
	.stop = hpm_gptmr_stop,
	.get_value = hpm_gptmr_get_value,
	.reset = hpm_gptmr_reset,
	.set_value = hpm_gptmr_set_value,
	.set_alarm = hpm_gptmr_set_alarm,
	.cancel_alarm = hpm_gptmr_cancel_alarm,
	.set_top_value = hpm_gptmr_set_top_value,
	.get_pending_int = hpm_gptmr_get_pending_int,
	.get_top_value = hpm_gptmr_get_top_value,
	.get_guard_period = hpm_gptmr_get_guard_period,
	.set_guard_period = hpm_gptmr_set_guard_period,
	.get_freq = hpm_gptmr_get_frequency,
};

#define HPM_GPTMR_IRQ_CONFIG(n)                                                                    \
	static void hpm_gptmr_irq_config_##n(const struct device *dev)                             \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_gptmr_isr,              \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}

#define HPM_GPTMR_INIT(n)                                                                          \
	HPM_GPTMR_IRQ_CONFIG(n)                                                                    \
                                                                                                   \
	static struct hpm_gptmr_data hpm_gptmr_data_##n;                                           \
                                                                                                   \
	static const struct hpm_gptmr_config hpm_gptmr_config_##n = {                              \
		.info =                                                                            \
			{                                                                          \
				.max_top_value = UINT32_MAX,                                       \
				.flags = COUNTER_CONFIG_INFO_COUNT_UP,                             \
				.channels = HPM_GPTMR_CHANNELS,                                    \
			},                                                                         \
		.base = (GPTMR_Type *)DT_INST_REG_ADDR(n),                                         \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.clock_name = DT_INST_PROP(n, clock_name),                                         \
		.clock_src = DT_INST_PROP(n, clock_src),                                           \
		.clock_div = DT_INST_CLOCKS_CELL_BY_IDX(n, 0, divider),                            \
		.irq_config_func = hpm_gptmr_irq_config_##n,                                       \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_gptmr_init, NULL, &hpm_gptmr_data_##n, &hpm_gptmr_config_##n, \
			      POST_KERNEL, CONFIG_COUNTER_INIT_PRIORITY, &hpm_gptmr_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_GPTMR_INIT)
