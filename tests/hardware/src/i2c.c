/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/smbus.h>
#include <zephyr/ztest.h>

static const struct device *const controller = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct device *const target = DEVICE_DT_GET(DT_NODELABEL(i2c1));
static uint8_t memory[256];
static uint8_t cursor;
static bool address_byte;
static bool registered;
static K_SEM_DEFINE(stopped, 0, 1);

static int write_requested(struct i2c_target_config *config)
{
	ARG_UNUSED(config);
	address_byte = true;
	return 0;
}

static int write_received(struct i2c_target_config *config, uint8_t value)
{
	ARG_UNUSED(config);
	if (address_byte) {
		cursor = value;
		address_byte = false;
	} else {
		memory[cursor++] = value;
	}
	return 0;
}

static int read_requested(struct i2c_target_config *config, uint8_t *value)
{
	ARG_UNUSED(config);
	*value = memory[cursor++];
	return 0;
}

static int stop_received(struct i2c_target_config *config)
{
	ARG_UNUSED(config);
	k_sem_give(&stopped);
	return 0;
}

static const struct i2c_target_callbacks callbacks = {
	.write_requested = write_requested,
	.write_received = write_received,
	.read_requested = read_requested,
	.read_processed = read_requested,
	.stop = stop_received,
};
static struct i2c_target_config target_config = {.address = 0x52, .callbacks = &callbacks};

static void i2c_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_true(device_is_ready(controller));
	zassert_true(device_is_ready(target));
	zassert_ok(i2c_target_register(target, &target_config));
	registered = true;
}

static void i2c_after(void *fixture)
{
	ARG_UNUSED(fixture);
	if (registered) {
		(void)i2c_target_unregister(target, &target_config);
		registered = false;
	}
	k_sem_reset(&stopped);
}

ZTEST(hpm_i2c_pair, test_dma_write_and_repeated_start_read)
{
	uint8_t tx[65] = {0x20};
	uint8_t rx[64];
	uint8_t reg = 0x20;

	for (size_t i = 1; i < sizeof(tx); ++i) {
		tx[i] = i * 29;
	}
	zassert_ok(i2c_write(controller, tx, sizeof(tx), target_config.address));
	zassert_ok(k_sem_take(&stopped, K_SECONDS(1)), "I2C target STOP interrupt missing");
	zassert_mem_equal(memory + reg, tx + 1, sizeof(rx));
	zassert_ok(i2c_write_read(controller, target_config.address, &reg, sizeof(reg), rx,
				  sizeof(rx)),
		   "I2C repeated-start transfer failed");
	zassert_ok(k_sem_take(&stopped, K_SECONDS(1)));
	zassert_mem_equal(rx, tx + 1, sizeof(rx));
}

ZTEST(hpm_i2c_pair, test_smbus_data_block_and_pec)
{
	const struct device *smbus = DEVICE_DT_GET(DT_NODELABEL(smbus0));
	uint8_t sent[SMBUS_BLOCK_BYTES_MAX];
	uint8_t received[SMBUS_BLOCK_BYTES_MAX];
	uint8_t count;
	uint8_t byte;
	uint16_t word;
	int ret;

	zassert_true(device_is_ready(smbus));
	zassert_ok(smbus_configure(smbus, SMBUS_MODE_CONTROLLER));
	zassert_ok(smbus_byte_data_write(smbus, 0x52, 0x40, 0x96));
	zassert_equal(memory[0x40], 0x96);
	zassert_ok(smbus_byte_data_read(smbus, 0x52, 0x40, &byte));
	zassert_equal(byte, 0x96);
	zassert_ok(smbus_word_data_write(smbus, 0x52, 0x50, 0x5361));
	zassert_equal(memory[0x50], 0x61, "SMBus word must be little endian");
	zassert_equal(memory[0x51], 0x53);
	zassert_ok(smbus_word_data_read(smbus, 0x52, 0x50, &word));
	zassert_equal(word, 0x5361);
	for (size_t i = 0; i < sizeof(sent); ++i) {
		sent[i] = i * 17 + 3;
	}
	zassert_ok(smbus_block_write(smbus, 0x52, 0x80, sizeof(sent), sent));
	zassert_equal(memory[0x80], sizeof(sent));
	zassert_mem_equal(memory + 0x81, sent, sizeof(sent));
	ret = smbus_block_read(smbus, 0x52, 0x80, &count, received);
	zassert_equal(ret, 0, "SMBus block read returned %d", ret);
	zassert_equal(count, sizeof(sent));
	zassert_mem_equal(received, sent, sizeof(sent));
	zassert_ok(smbus_configure(smbus, SMBUS_MODE_CONTROLLER | SMBUS_MODE_PEC));
	zassert_ok(smbus_word_data_write(smbus, 0x52, 0x60, 0x5361));
	/* CRC-8/SMBus of [address-W=0xa4, command=0x60, 0x61, 0x53]. */
	zassert_equal(memory[0x62], 0x3c, "SMBus write PEC mismatch");
	memory[0x70] = 0x6d;
	/* CRC-8/SMBus of [0xa4, 0x70, address-R=0xa5, data=0x6d]. */
	memory[0x71] = 0x9d;
	zassert_ok(smbus_byte_data_read(smbus, 0x52, 0x70, &byte));
	zassert_equal(byte, 0x6d);
	memory[0x71] ^= 1;
	zassert_equal(smbus_byte_data_read(smbus, 0x52, 0x70, &byte), -EIO,
		      "SMBus accepted corrupt PEC");
}

ZTEST(hpm_i2c_pair, test_smbus_block_pec_and_invalid_count)
{
	const struct device *smbus = DEVICE_DT_GET(DT_NODELABEL(smbus0));
	static const uint8_t lengths[] = {1, SMBUS_BLOCK_BYTES_MAX};
	static const uint8_t pecs[] = {0xbe, 0xbf};
	uint8_t sent[SMBUS_BLOCK_BYTES_MAX];
	uint8_t received[SMBUS_BLOCK_BYTES_MAX];
	uint8_t count;
	int ret;

	zassert_true(device_is_ready(smbus));
	zassert_ok(smbus_configure(smbus, SMBUS_MODE_CONTROLLER | SMBUS_MODE_PEC));
	for (size_t i = 0; i < sizeof(sent); ++i) {
		sent[i] = i * 17 + 3;
	}
	for (size_t i = 0; i < ARRAY_SIZE(lengths); ++i) {
		memory[0x90] = lengths[i];
		memcpy(memory + 0x91, sent, lengths[i]);
		/* CRC-8/SMBus of [0xa4, 0x90, 0xa5, count, sent[0..count-1]]. */
		memory[0x91 + lengths[i]] = pecs[i];
		ret = smbus_block_read(smbus, 0x52, 0x90, &count, received);
		zassert_equal(ret, 0, "SMBus %u-byte block PEC read returned %d", lengths[i], ret);
		zassert_equal(count, lengths[i]);
		zassert_mem_equal(received, sent, count);

		memory[0x91 + lengths[i]] ^= 1;
		ret = smbus_block_read(smbus, 0x52, 0x90, &count, received);
		zassert_equal(ret, -EIO, "SMBus %u-byte block accepted corrupt PEC: %d", lengths[i],
			      ret);
	}

	memory[0x90] = SMBUS_BLOCK_BYTES_MAX + 1;
	ret = smbus_block_read(smbus, 0x52, 0x90, &count, received);
	zassert_equal(ret, -ENODATA, "SMBus accepted oversized block count: %d", ret);
	memory[0x90] = sizeof(sent);
	memory[0x91 + sizeof(sent)] = pecs[1];
	ret = smbus_block_read(smbus, 0x52, 0x90, &count, received);
	zassert_equal(ret, 0, "SMBus block read after invalid count returned %d", ret);
	zassert_equal(count, sizeof(sent));
	zassert_mem_equal(received, sent, count);
}

ZTEST_SUITE(hpm_i2c_pair, NULL, NULL, i2c_before, i2c_after, NULL);
