/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_smbus

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/smbus.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <drivers/i2c/hpmicro_i2c.h>

#include <smbus_utils.h>

LOG_MODULE_REGISTER(smbus_hpmicro, CONFIG_SMBUS_HPMICRO_LOG_LEVEL);

struct hpm_smbus_config {
	const struct device *i2c;
};

struct hpm_smbus_data {
	uint32_t config;
};

static bool hpm_smbus_pec_enabled(const struct device *dev)
{
	const struct hpm_smbus_data *data = dev->data;

	return (data->config & SMBUS_MODE_PEC) != 0U;
}

static uint8_t hpm_smbus_pec_for_read(uint16_t addr, const uint8_t *write_data, size_t write_len,
				      const uint8_t *count, const uint8_t *data, size_t data_len)
{
	struct i2c_msg msgs[3];
	uint8_t msg_count = 0U;

	if ((write_data != NULL) && (write_len != 0U)) {
		msgs[msg_count].buf = (uint8_t *)write_data;
		msgs[msg_count].len = write_len;
		msgs[msg_count].flags = I2C_MSG_WRITE;
		msg_count++;
	}

	if (count != NULL) {
		msgs[msg_count].buf = (uint8_t *)count;
		msgs[msg_count].len = 1U;
		msgs[msg_count].flags = I2C_MSG_READ;
		msg_count++;
	}

	msgs[msg_count].buf = (uint8_t *)data;
	msgs[msg_count].len = data_len;
	msgs[msg_count].flags = I2C_MSG_READ;
	msg_count++;

	return smbus_pec(addr, msgs, msg_count);
}

static int hpm_smbus_write_buffer(const struct device *dev, uint16_t addr, uint8_t *buffer,
				  size_t data_len)
{
	const struct hpm_smbus_config *config = dev->config;
	struct i2c_msg msg = {
		.buf = buffer,
		.len = data_len,
		.flags = I2C_MSG_WRITE,
	};
	size_t write_len = data_len;

	if (hpm_smbus_pec_enabled(dev)) {
		buffer[data_len] = smbus_pec(addr, &msg, 1U);
		write_len++;
	}

	return i2c_write(config->i2c, buffer, write_len, addr);
}

static int hpm_smbus_read_buffer(const struct device *dev, uint16_t addr, uint8_t command,
				 bool has_command, uint8_t *buffer, size_t data_len)
{
	const struct hpm_smbus_config *config = dev->config;
	size_t read_len = data_len + (hpm_smbus_pec_enabled(dev) ? 1U : 0U);
	int ret;

	if (!hpm_smbus_pec_enabled(dev)) {
		if (has_command) {
			return i2c_write_read(config->i2c, addr, &command, 1U, buffer, data_len);
		}
		return i2c_read(config->i2c, buffer, data_len, addr);
	}

	if (has_command) {
		ret = i2c_write_read(config->i2c, addr, &command, 1U, buffer, read_len);
	} else {
		ret = i2c_read(config->i2c, buffer, read_len, addr);
	}
	if (ret < 0) {
		return ret;
	}

	if (buffer[data_len] != hpm_smbus_pec_for_read(addr, has_command ? &command : NULL,
						       has_command ? 1U : 0U, NULL, buffer,
						       data_len)) {
		return -EIO;
	}

	return 0;
}

static int hpm_smbus_configure_impl(const struct device *dev, uint32_t config_value)
{
	struct hpm_smbus_data *data = dev->data;

	if ((config_value & SMBUS_MODE_CONTROLLER) == 0U) {
		return -ENOTSUP;
	}

	if ((config_value & ~(SMBUS_MODE_CONTROLLER | SMBUS_MODE_PEC)) != 0U) {
		return -ENOTSUP;
	}

	data->config = config_value;

	return 0;
}

static int hpm_smbus_get_config_impl(const struct device *dev, uint32_t *config_value)
{
	struct hpm_smbus_data *data = dev->data;

	if (config_value == NULL) {
		return -EINVAL;
	}

	*config_value = data->config;

	return 0;
}

static int hpm_smbus_quick_impl(const struct device *dev, uint16_t addr,
				enum smbus_direction direction)
{
	const struct hpm_smbus_config *config = dev->config;

	if (direction == SMBUS_MSG_WRITE) {
		return hpm_i2c_smbus_quick(config->i2c, addr, false);
	}

	if (direction == SMBUS_MSG_READ) {
		return hpm_i2c_smbus_quick(config->i2c, addr, true);
	}

	return -EINVAL;
}

static int hpm_smbus_byte_write_impl(const struct device *dev, uint16_t addr, uint8_t byte)
{
	uint8_t buffer[2] = {byte, 0U};

	return hpm_smbus_write_buffer(dev, addr, buffer, 1U);
}

static int hpm_smbus_byte_read_impl(const struct device *dev, uint16_t addr, uint8_t *byte)
{
	uint8_t buffer[2];
	int ret;

	if (byte == NULL) {
		return -EINVAL;
	}

	ret = hpm_smbus_read_buffer(dev, addr, 0U, false, buffer, 1U);
	if (ret == 0) {
		*byte = buffer[0];
	}

	return ret;
}

static int hpm_smbus_byte_data_write_impl(const struct device *dev, uint16_t addr, uint8_t command,
					  uint8_t byte)
{
	uint8_t buffer[3] = {command, byte, 0U};

	return hpm_smbus_write_buffer(dev, addr, buffer, 2U);
}

static int hpm_smbus_byte_data_read_impl(const struct device *dev, uint16_t addr, uint8_t command,
					 uint8_t *byte)
{
	uint8_t buffer[2];
	int ret;

	if (byte == NULL) {
		return -EINVAL;
	}

	ret = hpm_smbus_read_buffer(dev, addr, command, true, buffer, 1U);
	if (ret == 0) {
		*byte = buffer[0];
	}

	return ret;
}

static int hpm_smbus_word_data_write_impl(const struct device *dev, uint16_t addr, uint8_t command,
					  uint16_t word)
{
	uint8_t buffer[4] = {command, 0U, 0U, 0U};

	sys_put_le16(word, &buffer[1]);

	return hpm_smbus_write_buffer(dev, addr, buffer, 3U);
}

static int hpm_smbus_word_data_read_impl(const struct device *dev, uint16_t addr, uint8_t command,
					 uint16_t *word)
{
	uint8_t buffer[3];
	int ret;

	if (word == NULL) {
		return -EINVAL;
	}

	ret = hpm_smbus_read_buffer(dev, addr, command, true, buffer, 2U);
	if (ret == 0) {
		*word = sys_get_le16(buffer);
	}

	return ret;
}

static int hpm_smbus_pcall_impl(const struct device *dev, uint16_t addr, uint8_t command,
				uint16_t send_word, uint16_t *recv_word)
{
	const struct hpm_smbus_config *config = dev->config;
	uint8_t write_buffer[3];
	uint8_t read_buffer[3];
	size_t read_len;
	int ret;

	if (recv_word == NULL) {
		return -EINVAL;
	}

	write_buffer[0] = command;
	sys_put_le16(send_word, &write_buffer[1]);
	read_len = sizeof(uint16_t) + (hpm_smbus_pec_enabled(dev) ? 1U : 0U);
	ret = i2c_write_read(config->i2c, addr, write_buffer, sizeof(write_buffer), read_buffer,
			     read_len);
	if (ret < 0) {
		return ret;
	}

	if (hpm_smbus_pec_enabled(dev) &&
	    (read_buffer[sizeof(uint16_t)] !=
	     hpm_smbus_pec_for_read(addr, write_buffer, sizeof(write_buffer), NULL, read_buffer,
				    sizeof(uint16_t)))) {
		return -EIO;
	}

	*recv_word = sys_get_le16(read_buffer);

	return 0;
}

static int hpm_smbus_block_write_impl(const struct device *dev, uint16_t addr, uint8_t command,
				      uint8_t count, uint8_t *buf)
{
	uint8_t buffer[SMBUS_BLOCK_BYTES_MAX + 3U];

	if ((count == 0U) || (count > SMBUS_BLOCK_BYTES_MAX) || (buf == NULL)) {
		return -EINVAL;
	}

	buffer[0] = command;
	buffer[1] = count;
	memcpy(&buffer[2], buf, count);

	return hpm_smbus_write_buffer(dev, addr, buffer, count + 2U);
}

static int hpm_smbus_block_read_impl(const struct device *dev, uint16_t addr, uint8_t command,
				     uint8_t *count, uint8_t *buf)
{
	const struct hpm_smbus_config *config = dev->config;
	uint8_t received_count = 0U;
	uint8_t node_buffer[SMBUS_BLOCK_BYTES_MAX];
	uint8_t pec = 0U;
	uint8_t *pec_ptr = hpm_smbus_pec_enabled(dev) ? &pec : NULL;
	int ret;

	if ((count == NULL) || (buf == NULL)) {
		return -EINVAL;
	}

	ret = hpm_i2c_smbus_block_transfer(config->i2c, addr, &command, sizeof(command),
					   &received_count, node_buffer, pec_ptr);
	if (ret < 0) {
		return ret;
	}

	if ((pec_ptr != NULL) &&
	    (pec != hpm_smbus_pec_for_read(addr, &command, sizeof(command), &received_count,
					   node_buffer, received_count))) {
		return -EIO;
	}

	memcpy(buf, node_buffer, received_count);
	*count = received_count;

	return 0;
}

static int hpm_smbus_block_pcall_impl(const struct device *dev, uint16_t addr, uint8_t command,
				      uint8_t send_count, uint8_t *send_buf, uint8_t *recv_count,
				      uint8_t *recv_buf)
{
	const struct hpm_smbus_config *config = dev->config;
	uint8_t tx_buf[SMBUS_BLOCK_BYTES_MAX + 2U];
	uint8_t received_count = 0U;
	uint8_t node_buffer[SMBUS_BLOCK_BYTES_MAX];
	uint8_t pec = 0U;
	uint8_t *pec_ptr = hpm_smbus_pec_enabled(dev) ? &pec : NULL;
	int ret;

	if ((send_count == 0U) || (send_count > SMBUS_BLOCK_BYTES_MAX) || (send_buf == NULL) ||
	    (recv_count == NULL) || (recv_buf == NULL)) {
		return -EINVAL;
	}

	tx_buf[0] = command;
	tx_buf[1] = send_count;
	memcpy(&tx_buf[2], send_buf, send_count);

	ret = hpm_i2c_smbus_block_transfer(config->i2c, addr, tx_buf, send_count + 2U,
					   &received_count, node_buffer, pec_ptr);
	if (ret < 0) {
		return ret;
	}

	if ((pec_ptr != NULL) &&
	    (pec != hpm_smbus_pec_for_read(addr, tx_buf, send_count + 2U, &received_count,
					   node_buffer, received_count))) {
		return -EIO;
	}

	memcpy(recv_buf, node_buffer, received_count);
	*recv_count = received_count;

	return 0;
}

static DEVICE_API(smbus, hpm_smbus_api) = {
	.configure = hpm_smbus_configure_impl,
	.get_config = hpm_smbus_get_config_impl,
	.smbus_quick = hpm_smbus_quick_impl,
	.smbus_byte_write = hpm_smbus_byte_write_impl,
	.smbus_byte_read = hpm_smbus_byte_read_impl,
	.smbus_byte_data_write = hpm_smbus_byte_data_write_impl,
	.smbus_byte_data_read = hpm_smbus_byte_data_read_impl,
	.smbus_word_data_write = hpm_smbus_word_data_write_impl,
	.smbus_word_data_read = hpm_smbus_word_data_read_impl,
	.smbus_pcall = hpm_smbus_pcall_impl,
	.smbus_block_write = hpm_smbus_block_write_impl,
	.smbus_block_read = hpm_smbus_block_read_impl,
	.smbus_block_pcall = hpm_smbus_block_pcall_impl,
};

static int hpm_smbus_init(const struct device *dev)
{
	const struct hpm_smbus_config *config = dev->config;

	if (!device_is_ready(config->i2c)) {
		return -ENODEV;
	}

	return 0;
}

#define HPM_SMBUS_INIT(n)                                                                          \
	static const struct hpm_smbus_config hpm_smbus_config_##n = {                              \
		.i2c = DEVICE_DT_GET(DT_INST_PHANDLE(n, i2c)),                                     \
	};                                                                                         \
	static struct hpm_smbus_data hpm_smbus_data_##n;                                           \
                                                                                                   \
	SMBUS_DEVICE_DT_INST_DEFINE(n, hpm_smbus_init, NULL, &hpm_smbus_data_##n,                  \
				    &hpm_smbus_config_##n, POST_KERNEL,                            \
				    CONFIG_SMBUS_INIT_PRIORITY, &hpm_smbus_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_SMBUS_INIT)
