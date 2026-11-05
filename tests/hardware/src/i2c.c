/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/smbus.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

PINCTRL_DT_DEFINE(DT_NODELABEL(i2c_recovery));
PINCTRL_DT_DEFINE(DT_NODELABEL(i2c1));

static const struct device *const controller = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct device *const target = DEVICE_DT_GET(DT_NODELABEL(i2c1));
static const struct gpio_dt_spec recovery_scl =
	GPIO_DT_SPEC_GET(DT_NODELABEL(i2c_recovery), scl_gpios);
static const struct gpio_dt_spec recovery_sda =
	GPIO_DT_SPEC_GET(DT_NODELABEL(i2c_recovery), sda_gpios);
static uint8_t memory[256];
static uint8_t cursor;
static bool address_byte;
static bool registered;
static bool recovery_callback_added;
static atomic_t write_requests;
static atomic_t received_bytes;
static atomic_t read_requests;
static atomic_t stop_events;
static atomic_t recovery_rising_edges;
static atomic_t recovery_callback_errors;
static struct gpio_callback recovery_scl_callback;
static K_SEM_DEFINE(stopped, 0, 1);

static void recovery_scl_rising_edge(const struct device *port, struct gpio_callback *callback,
				     gpio_port_pins_t pins)
{
	atomic_val_t edge;

	if (!k_is_in_isr() || (port != recovery_scl.port) || (callback != &recovery_scl_callback) ||
	    (pins != BIT(recovery_scl.pin))) {
		atomic_inc(&recovery_callback_errors);
	}

	edge = atomic_inc(&recovery_rising_edges) + 1;
	if ((edge == 9) && (gpio_pin_set_dt(&recovery_sda, 1) < 0)) {
		atomic_inc(&recovery_callback_errors);
	}
}

static int write_requested(struct i2c_target_config *config)
{
	ARG_UNUSED(config);
	atomic_inc(&write_requests);
	address_byte = true;
	return 0;
}

static int write_received(struct i2c_target_config *config, uint8_t value)
{
	ARG_UNUSED(config);
	atomic_inc(&received_bytes);
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
	atomic_inc(&read_requests);
	*value = memory[cursor++];
	return 0;
}

static int read_processed(struct i2c_target_config *config, uint8_t *value)
{
	ARG_UNUSED(config);
	*value = memory[cursor++];
	return 0;
}

static int stop_received(struct i2c_target_config *config)
{
	ARG_UNUSED(config);
	atomic_inc(&stop_events);
	k_sem_give(&stopped);
	return 0;
}

static const struct i2c_target_callbacks callbacks = {
	.write_requested = write_requested,
	.write_received = write_received,
	.read_requested = read_requested,
	.read_processed = read_processed,
	.stop = stop_received,
};
static struct i2c_target_config target_config = {.address = 0x52, .callbacks = &callbacks};

static void select_target(uint16_t address, uint8_t flags)
{
	if (registered) {
		zassert_ok(i2c_target_unregister(target, &target_config));
		registered = false;
	}

	target_config.address = address;
	target_config.flags = flags;
	zassert_ok(i2c_target_register(target, &target_config));
	registered = true;
	k_sem_reset(&stopped);
	atomic_clear(&write_requests);
	atomic_clear(&received_bytes);
	atomic_clear(&read_requests);
	atomic_clear(&stop_events);
}

static void i2c_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_true(device_is_ready(controller));
	zassert_true(device_is_ready(target));
	zassert_ok(
		i2c_configure(controller, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD)));
	select_target(0x52, 0U);
}

static void i2c_after(void *fixture)
{
	ARG_UNUSED(fixture);
	if (registered) {
		(void)i2c_target_unregister(target, &target_config);
		registered = false;
	}
	if (recovery_callback_added) {
		(void)gpio_pin_interrupt_configure_dt(&recovery_scl, GPIO_INT_DISABLE);
		(void)gpio_remove_callback(recovery_scl.port, &recovery_scl_callback);
		recovery_callback_added = false;
	}
	(void)gpio_pin_configure_dt(&recovery_scl, GPIO_INPUT);
	(void)gpio_pin_configure_dt(&recovery_sda, GPIO_INPUT);
	zassert_ok(pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(i2c1)),
				       PINCTRL_STATE_DEFAULT));
	zassert_ok(
		i2c_configure(controller, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD)));
	k_sem_reset(&stopped);
}

ZTEST(hpm_i2c_pair, test_10bit_write_with_stop)
{
	uint8_t sent[] = {0x30, 0x5a, 0xa5, 0x13, 0x37, 0x42, 0x81, 0xfe, 0x01};
	struct i2c_msg message = {
		.buf = sent,
		.len = sizeof(sent),
		.flags = I2C_MSG_ADDR_10_BITS | I2C_MSG_STOP,
	};
	int ret;

	select_target(0x155, I2C_TARGET_FLAGS_ADDR_10_BITS);
	ret = i2c_transfer(controller, &message, 1U, target_config.address);
	zassert_ok(ret, "10-bit write returned %d", ret);
	zassert_ok(k_sem_take(&stopped, K_SECONDS(1)));
	zassert_mem_equal(memory + sent[0], sent + 1, sizeof(sent) - 1U);
	zassert_equal(atomic_get(&write_requests), 1);
	zassert_equal(atomic_get(&received_bytes), sizeof(sent));
	zassert_equal(atomic_get(&stop_events), 1);
}

ZTEST(hpm_i2c_pair, test_10bit_read_with_stop)
{
	uint8_t expected[] = {0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x78, 0x89};
	uint8_t received[sizeof(expected)] = {0};
	struct i2c_msg message = {
		.buf = received,
		.len = sizeof(received),
		.flags = I2C_MSG_ADDR_10_BITS | I2C_MSG_READ | I2C_MSG_STOP,
	};
	int ret;

	select_target(0x155, I2C_TARGET_FLAGS_ADDR_10_BITS);
	cursor = 0x40;
	memcpy(memory + cursor, expected, sizeof(expected));
	ret = i2c_transfer(controller, &message, 1U, target_config.address);
	zassert_ok(ret, "10-bit read returned %d", ret);
	zassert_ok(k_sem_take(&stopped, K_SECONDS(1)));
	zassert_mem_equal(received, expected, sizeof(expected));
	zassert_equal(atomic_get(&read_requests), 1);
	zassert_equal(atomic_get(&stop_events), 1);
}

ZTEST(hpm_i2c_pair, test_10bit_write_restart_read)
{
	uint8_t expected[] = {0x5a, 0x6b, 0x7c, 0x8d, 0x9e, 0xaf, 0xb0, 0xc1};
	uint8_t command = 0x50;
	uint8_t received[sizeof(expected)] = {0};
	struct i2c_msg messages[] = {
		{
			.buf = &command,
			.len = sizeof(command),
			.flags = I2C_MSG_ADDR_10_BITS,
		},
		{
			.buf = received,
			.len = sizeof(received),
			.flags = I2C_MSG_ADDR_10_BITS | I2C_MSG_READ | I2C_MSG_RESTART |
				 I2C_MSG_STOP,
		},
	};
	int ret;

	select_target(0x155, I2C_TARGET_FLAGS_ADDR_10_BITS);
	memcpy(memory + command, expected, sizeof(expected));
	ret = i2c_transfer(controller, messages, ARRAY_SIZE(messages), target_config.address);
	zassert_ok(ret, "10-bit restart returned %d; write=%ld bytes=%ld read=%ld stop=%ld", ret,
		   atomic_get(&write_requests), atomic_get(&received_bytes),
		   atomic_get(&read_requests), atomic_get(&stop_events));
	zassert_ok(k_sem_take(&stopped, K_SECONDS(1)));
	zassert_mem_equal(received, expected, sizeof(expected));
	zassert_equal(atomic_get(&write_requests), 1);
	zassert_equal(atomic_get(&received_bytes), 1);
	zassert_equal(atomic_get(&read_requests), 1);
	zassert_equal(atomic_get(&stop_events), 1);
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

ZTEST(hpm_i2c_pair, test_bus_recovery_with_stuck_sda)
{
	uint8_t sent[] = {0x22, 0x5a, 0xa5};
	int ret;

	if (registered) {
		zassert_ok(i2c_target_unregister(target, &target_config));
		registered = false;
	}

	zassert_ok(pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(i2c_recovery)),
				       PINCTRL_STATE_DEFAULT));
	zassert_ok(gpio_pin_configure_dt(&recovery_scl, GPIO_INPUT));
	zassert_ok(gpio_pin_configure_dt(&recovery_sda, GPIO_OUTPUT_HIGH));
	zassert_equal(gpio_pin_get_dt(&recovery_scl), 1, "SCL must idle high");
	zassert_equal(gpio_pin_get_dt(&recovery_sda), 1, "SDA must idle high");

	atomic_clear(&recovery_rising_edges);
	atomic_clear(&recovery_callback_errors);
	gpio_init_callback(&recovery_scl_callback, recovery_scl_rising_edge, BIT(recovery_scl.pin));
	zassert_ok(gpio_add_callback(recovery_scl.port, &recovery_scl_callback));
	recovery_callback_added = true;
	zassert_ok(gpio_pin_interrupt_configure_dt(&recovery_scl, GPIO_INT_EDGE_RISING));

	zassert_ok(gpio_pin_set_dt(&recovery_sda, 0));
	k_busy_wait(10);
	zassert_equal(gpio_pin_get_dt(&recovery_sda), 0, "SDA did not stay low");

	ret = i2c_recover_bus(controller);
	zassert_ok(ret, "i2c_recover_bus returned %d", ret);
	zassert_equal(atomic_get(&recovery_rising_edges), 9,
		      "expected exactly 9 recovery clocks, got %ld",
		      atomic_get(&recovery_rising_edges));
	zassert_equal(atomic_get(&recovery_callback_errors), 0,
		      "invalid recovery callback context/source");
	zassert_equal(gpio_pin_get_dt(&recovery_scl), 1, "SCL did not return high");
	zassert_equal(gpio_pin_get_dt(&recovery_sda), 1, "SDA did not return high");

	zassert_ok(gpio_pin_interrupt_configure_dt(&recovery_scl, GPIO_INT_DISABLE));
	zassert_ok(gpio_remove_callback(recovery_scl.port, &recovery_scl_callback));
	recovery_callback_added = false;
	zassert_ok(pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(i2c1)),
				       PINCTRL_STATE_DEFAULT));

	select_target(0x52, 0U);
	zassert_ok(i2c_write(controller, sent, sizeof(sent), target_config.address),
		   "transfer after bus recovery failed");
	zassert_ok(k_sem_take(&stopped, K_SECONDS(1)));
	zassert_mem_equal(memory + sent[0], sent + 1, sizeof(sent) - 1U);
}

ZTEST(hpm_i2c_pair, test_bus_recovery_with_stuck_scl)
{
	uint8_t sent[] = {0x24, 0xa5, 0x5a};
	int ret;

	if (registered) {
		zassert_ok(i2c_target_unregister(target, &target_config));
		registered = false;
	}

	zassert_ok(pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(i2c_recovery)),
				       PINCTRL_STATE_DEFAULT));
	zassert_ok(gpio_pin_configure_dt(&recovery_scl, GPIO_OUTPUT_HIGH));
	zassert_ok(gpio_pin_configure_dt(&recovery_sda, GPIO_OUTPUT_HIGH));
	zassert_equal(gpio_pin_get_dt(&recovery_scl), 1, "SCL must idle high");
	zassert_equal(gpio_pin_get_dt(&recovery_sda), 1, "SDA must idle high");

	atomic_clear(&recovery_rising_edges);
	atomic_clear(&recovery_callback_errors);
	gpio_init_callback(&recovery_scl_callback, recovery_scl_rising_edge, BIT(recovery_scl.pin));
	zassert_ok(gpio_add_callback(recovery_scl.port, &recovery_scl_callback));
	recovery_callback_added = true;
	zassert_ok(gpio_pin_interrupt_configure_dt(&recovery_scl, GPIO_INT_EDGE_RISING));

	zassert_ok(gpio_pin_set_dt(&recovery_scl, 0));
	zassert_ok(gpio_pin_set_dt(&recovery_sda, 0));
	k_busy_wait(10);
	zassert_equal(gpio_pin_get_dt(&recovery_scl), 0, "SCL did not stay low");
	zassert_equal(gpio_pin_get_dt(&recovery_sda), 0, "SDA did not stay low");

	ret = i2c_recover_bus(controller);
	zassert_equal(ret, -EBUSY, "stuck SCL recovery returned %d", ret);
	zassert_equal(atomic_get(&recovery_rising_edges), 0,
		      "recovery generated clocks while SCL was held low");

	zassert_ok(gpio_pin_set_dt(&recovery_scl, 1));
	k_busy_wait(10);
	zassert_equal(gpio_pin_get_dt(&recovery_scl), 1, "SCL did not release high");
	zassert_equal(gpio_pin_get_dt(&recovery_sda), 0, "SDA should still be held low");

	atomic_clear(&recovery_rising_edges);
	atomic_clear(&recovery_callback_errors);
	ret = i2c_recover_bus(controller);
	zassert_ok(ret, "recovery after releasing SCL returned %d", ret);
	zassert_equal(atomic_get(&recovery_rising_edges), 9,
		      "expected exactly 9 recovery clocks, got %ld",
		      atomic_get(&recovery_rising_edges));
	zassert_equal(atomic_get(&recovery_callback_errors), 0,
		      "invalid recovery callback context/source");
	zassert_equal(gpio_pin_get_dt(&recovery_scl), 1, "SCL did not return high");
	zassert_equal(gpio_pin_get_dt(&recovery_sda), 1, "SDA did not return high");

	zassert_ok(gpio_pin_interrupt_configure_dt(&recovery_scl, GPIO_INT_DISABLE));
	zassert_ok(gpio_remove_callback(recovery_scl.port, &recovery_scl_callback));
	recovery_callback_added = false;
	zassert_ok(gpio_pin_configure_dt(&recovery_scl, GPIO_INPUT));
	zassert_ok(gpio_pin_configure_dt(&recovery_sda, GPIO_INPUT));
	zassert_ok(pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(i2c1)),
				       PINCTRL_STATE_DEFAULT));

	select_target(0x52, 0U);
	zassert_ok(i2c_write(controller, sent, sizeof(sent), target_config.address),
		   "transfer after SCL recovery failed");
	zassert_ok(k_sem_take(&stopped, K_SECONDS(1)));
	zassert_mem_equal(memory + sent[0], sent + 1, sizeof(sent) - 1U);
}

ZTEST_SUITE(hpm_i2c_pair, NULL, NULL, i2c_before, i2c_after, NULL);
