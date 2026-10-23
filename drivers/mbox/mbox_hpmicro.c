/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_mbx

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hpm_mbx_drv.h>

LOG_MODULE_REGISTER(mbox_hpmicro, CONFIG_MBOX_LOG_LEVEL);

#define HPM_MBX_CHANNELS 1U
#define HPM_MBX_MSG_SIZE sizeof(uint32_t)
#define HPM_MBX_CHANNEL  0U

struct hpm_mbx_config {
	MBX_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	void (*irq_config_func)(const struct device *dev);
};

struct hpm_mbx_data {
	struct k_spinlock lock;
	mbox_callback_t callback;
	void *user_data;
	bool enabled;
};

static bool hpm_mbx_channel_is_valid(mbox_channel_id_t channel)
{
	return channel < HPM_MBX_CHANNELS;
}

static void hpm_mbx_dispatch(const struct device *dev)
{
	const struct hpm_mbx_config *config = dev->config;
	struct hpm_mbx_data *data = dev->data;
	mbox_callback_t callback;
	void *user_data;
	uint32_t message;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);
	if (!data->enabled) {
		k_spin_unlock(&data->lock, key);
		return;
	}

	if (mbx_retrieve_message(config->base, &message) != status_success) {
		k_spin_unlock(&data->lock, key);
		return;
	}

	callback = data->callback;
	user_data = data->user_data;
	k_spin_unlock(&data->lock, key);

	if (callback != NULL) {
		struct mbox_msg msg = {
			.data = &message,
			.size = HPM_MBX_MSG_SIZE,
		};

		callback(dev, HPM_MBX_CHANNEL, user_data, &msg);
	}
}

static int hpm_mbx_send(const struct device *dev, mbox_channel_id_t channel,
			const struct mbox_msg *msg)
{
	const struct hpm_mbx_config *config = dev->config;
	struct hpm_mbx_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t word;
	int ret;

	if (!hpm_mbx_channel_is_valid(channel)) {
		return -EINVAL;
	}

	if (msg == NULL) {
		return -EINVAL;
	}

	if (msg->data == NULL) {
		return -EINVAL;
	}

	if (msg->size != HPM_MBX_MSG_SIZE) {
		return -EMSGSIZE;
	}

	memcpy(&word, msg->data, sizeof(word));

	key = k_spin_lock(&data->lock);
	ret = mbx_send_message(config->base, word) == status_success ? 0 : -EBUSY;
	k_spin_unlock(&data->lock, key);

	return ret;
}

static int hpm_mbx_register_callback(const struct device *dev, mbox_channel_id_t channel,
				     mbox_callback_t callback, void *user_data)
{
	struct hpm_mbx_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_mbx_channel_is_valid(channel)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	data->callback = callback;
	data->user_data = user_data;
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int hpm_mbx_mtu_get(const struct device *dev)
{
	ARG_UNUSED(dev);

	return HPM_MBX_MSG_SIZE;
}

static uint32_t hpm_mbx_max_channels_get(const struct device *dev)
{
	ARG_UNUSED(dev);

	return HPM_MBX_CHANNELS;
}

static int hpm_mbx_set_enabled(const struct device *dev, mbox_channel_id_t channel, bool enabled)
{
	const struct hpm_mbx_config *config = dev->config;
	struct hpm_mbx_data *data = dev->data;
	k_spinlock_key_t key;

	if (!hpm_mbx_channel_is_valid(channel)) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);
	if (data->enabled == enabled) {
		k_spin_unlock(&data->lock, key);
		return -EALREADY;
	}

	if (enabled) {
		if (data->callback == NULL) {
			LOG_WRN("Enabling mailbox channel without a callback");
		}
		data->enabled = true;
		mbx_enable_intr(config->base, MBX_CR_RWMVIE_MASK);
	} else {
		mbx_disable_intr(config->base, MBX_CR_RWMVIE_MASK);
		data->enabled = false;
	}
	k_spin_unlock(&data->lock, key);

	return 0;
}

static const DEVICE_API(mbox, hpm_mbx_api) = {
	.send = hpm_mbx_send,
	.register_callback = hpm_mbx_register_callback,
	.mtu_get = hpm_mbx_mtu_get,
	.max_channels_get = hpm_mbx_max_channels_get,
	.set_enabled = hpm_mbx_set_enabled,
};

static void hpm_mbx_isr(const struct device *dev)
{
	hpm_mbx_dispatch(dev);
}

#define HPM_MBX_IRQ_CONFIG(n)                                                                      \
	static void hpm_mbx_irq_config_##n(const struct device *dev)                               \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), hpm_mbx_isr,                \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}

#define HPM_MBX_INIT(n)                                                                            \
	HPM_MBX_IRQ_CONFIG(n)                                                                      \
                                                                                                   \
	static struct hpm_mbx_data hpm_mbx_data_##n;                                               \
                                                                                                   \
	static const struct hpm_mbx_config hpm_mbx_config_##n = {                                  \
		.base = (MBX_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.irq_config_func = hpm_mbx_irq_config_##n,                                         \
	};                                                                                         \
                                                                                                   \
	static int hpm_mbx_init_##n(const struct device *dev)                                      \
	{                                                                                          \
		const struct hpm_mbx_config *config = dev->config;                                 \
		int ret;                                                                           \
                                                                                                   \
		if (!device_is_ready(config->clock_dev)) {                                         \
			return -ENODEV;                                                            \
		}                                                                                  \
                                                                                                   \
		ret = clock_control_on(config->clock_dev, config->clock_subsys);                   \
		if (ret < 0) {                                                                     \
			return ret;                                                                \
		}                                                                                  \
                                                                                                   \
		mbx_init(config->base);                                                            \
		config->irq_config_func(dev);                                                      \
                                                                                                   \
		return 0;                                                                          \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_mbx_init_##n, NULL, &hpm_mbx_data_##n, &hpm_mbx_config_##n,   \
			      POST_KERNEL, CONFIG_MBOX_INIT_PRIORITY, &hpm_mbx_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_MBX_INIT)
