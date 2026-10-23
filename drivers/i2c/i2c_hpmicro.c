/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_i2c

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>

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
};

struct hpm_i2c_data {
	struct k_mutex lock;
	uint32_t dev_config;
};

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

static int hpm_i2c_configure(const struct device *dev, uint32_t dev_config)
{
	const struct hpm_i2c_config *config = dev->config;
	struct hpm_i2c_data *data = dev->data;
	i2c_config_t hal_config = {0};
	uint32_t clock_freq;
	uint32_t bitrate;
	int ret;

	if ((dev_config & I2C_MODE_CONTROLLER) == 0U) {
		return -ENOTSUP;
	}

	if ((dev_config & I2C_ADDR_10_BITS) != 0U) {
		return -ENOTSUP;
	}

	switch (I2C_SPEED_GET(dev_config)) {
	case I2C_SPEED_STANDARD:
		bitrate = 100000U;
		break;
	case I2C_SPEED_FAST:
		bitrate = 400000U;
		break;
	case I2C_SPEED_FAST_PLUS:
		bitrate = 1000000U;
		break;
	default:
		return -ENOTSUP;
	}

	ret = clock_control_get_rate(config->clock_dev, config->clock_subsys, &clock_freq);
	if (ret < 0) {
		return ret;
	}

	hal_config.is_10bit_addressing = false;
	ret = hpm_i2c_speed_to_mode(bitrate, &hal_config.i2c_mode);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	if (i2c_init_master(config->base, clock_freq, &hal_config) != status_success) {
		k_mutex_unlock(&data->lock);
		return -EIO;
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

static int hpm_i2c_transfer_frame(const struct hpm_i2c_config *config, uint16_t addr,
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

	return hpm_i2c_status_to_errno(
		i2c_master_transfer(config->base, addr, msg->buf, msg->len, flags));
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

	for (uint8_t i = 0U; i < num_msgs; i++) {
		if ((msgs[i].flags & I2C_MSG_ADDR_10_BITS) != 0U) {
			ret = -ENOTSUP;
			goto unlock;
		}
		if ((msgs[i].len > I2C_SOC_TRANSFER_COUNT_MAX) ||
		    ((msgs[i].buf == NULL) && (msgs[i].len != 0U))) {
			ret = -EINVAL;
			goto unlock;
		}
		if ((msgs[i].len == 0U) && (i != (uint8_t)(num_msgs - 1U))) {
			ret = -ENOTSUP;
			goto unlock;
		}
	}

	if (num_msgs == 1U) {
		uint16_t hal_flags = i2c_is_read_op(&msgs[0]) ? I2C_RD : I2C_WRITE_CHECK_ACK;

		if (msgs[0].len == 0U) {
			ret = 0;
			goto unlock;
		}
		if (!i2c_is_stop_op(&msgs[0])) {
			hal_flags |= I2C_NO_STOP;
		}
		ret = hpm_i2c_status_to_errno(i2c_master_transfer(config->base, addr, msgs[0].buf,
								  msgs[0].len, hal_flags));
		goto unlock;
	}

	for (uint8_t i = 0U; i < num_msgs; i++) {
		ret = hpm_i2c_transfer_frame(config, addr, &msgs[i]);
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

	return hpm_i2c_configure(dev,
				 I2C_MODE_CONTROLLER | I2C_SPEED_SET(hpm_i2c_mode_to_speed(mode)));
}

static const struct i2c_driver_api hpm_i2c_api = {
	.configure = hpm_i2c_configure,
	.get_config = hpm_i2c_get_config,
	.transfer = hpm_i2c_transfer,
	.recover_bus = hpm_i2c_recover_bus,
};

#define HPM_I2C_INIT(n)                                                                            \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
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
	};                                                                                         \
                                                                                                   \
	static struct hpm_i2c_data hpm_i2c_data_##n;                                               \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_i2c_init, NULL, &hpm_i2c_data_##n, &hpm_i2c_config_##n,       \
			      POST_KERNEL, CONFIG_I2C_INIT_PRIORITY, &hpm_i2c_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_I2C_INIT)
