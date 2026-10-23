/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_H_
#define ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro serial encoder interface
 * @defgroup hpm_sei_interface HPMicro SEI
 * @ingroup io_interfaces
 * @{
 */

#define HPM_SEI_MAX_INSTRUCTIONS 64U
#define HPM_SEI_DATA_REGISTERS   10U
#define HPM_SEI_LATCH_COUNT      4U
#define HPM_SEI_TRANSITION_COUNT 4U

/** SEI controller index. HPM5361 exposes two channels in one block. */
enum hpm_sei_channel {
	HPM_SEI_CHANNEL_0 = 0,
	HPM_SEI_CHANNEL_1 = 1,
};

enum hpm_sei_mode {
	HPM_SEI_MODE_SYNC_MASTER = 0,
	HPM_SEI_MODE_SYNC_SLAVE = 1,
	HPM_SEI_MODE_ASYNC = 2,
};

enum hpm_sei_idle_state {
	HPM_SEI_IDLE_LOW = 0,
	HPM_SEI_IDLE_HIGH = 1,
};

enum hpm_sei_data_mode {
	HPM_SEI_DATA_MODE_DATA = 0,
	HPM_SEI_DATA_MODE_CHECK = 1,
	HPM_SEI_DATA_MODE_CRC = 2,
};

enum hpm_sei_bit_order {
	HPM_SEI_BIT_ORDER_LSB_FIRST = 0,
	HPM_SEI_BIT_ORDER_MSB_FIRST = 1,
};

enum hpm_sei_word_order {
	HPM_SEI_WORD_ORDER_NORMAL = 0,
	HPM_SEI_WORD_ORDER_REVERSED = 1,
};

enum hpm_sei_instruction_operation {
	HPM_SEI_INSTRUCTION_HALT = 0,
	HPM_SEI_INSTRUCTION_JUMP = 1,
	HPM_SEI_INSTRUCTION_SEND_WATCHDOG = 2,
	HPM_SEI_INSTRUCTION_SEND = 3,
	HPM_SEI_INSTRUCTION_WAIT_WATCHDOG = 4,
	HPM_SEI_INSTRUCTION_WAIT = 5,
	HPM_SEI_INSTRUCTION_RECEIVE_WATCHDOG = 6,
	HPM_SEI_INSTRUCTION_RECEIVE = 7,
};

enum hpm_sei_master_clock {
	HPM_SEI_MASTER_CLOCK_LOW = 0,
	HPM_SEI_MASTER_CLOCK_RISE_FALL = 1,
	HPM_SEI_MASTER_CLOCK_FALL_RISE = 2,
	HPM_SEI_MASTER_CLOCK_HIGH = 3,
};

enum hpm_sei_slave_clock {
	HPM_SEI_SLAVE_CLOCK_DEFAULT = 0,
	HPM_SEI_SLAVE_CLOCK_EXCHANGE = 1,
	HPM_SEI_SLAVE_CLOCK_TIMEOUT = 2,
	HPM_SEI_SLAVE_CLOCK_EXCHANGE_TIMEOUT = 3,
};

enum hpm_sei_transition_condition {
	HPM_SEI_TRANSITION_HIGH_MATCH = 0,
	HPM_SEI_TRANSITION_LOW_DISMATCH = 1,
	HPM_SEI_TRANSITION_RISE_ENTRY = 2,
	HPM_SEI_TRANSITION_FALL_LEAVE = 3,
};

/** SEI data register index. Registers 30 and 31 are constants. */
enum hpm_sei_data_register {
	HPM_SEI_DATA_REGISTER_0 = 0,
	HPM_SEI_DATA_REGISTER_1,
	HPM_SEI_DATA_REGISTER_2,
	HPM_SEI_DATA_REGISTER_3,
	HPM_SEI_DATA_REGISTER_4,
	HPM_SEI_DATA_REGISTER_5,
	HPM_SEI_DATA_REGISTER_6,
	HPM_SEI_DATA_REGISTER_7,
	HPM_SEI_DATA_REGISTER_8,
	HPM_SEI_DATA_REGISTER_9,
	HPM_SEI_DATA_REGISTER_CONST_0 = 30,
	HPM_SEI_DATA_REGISTER_CONST_1 = 31,
};

struct hpm_sei_pin_config {
	enum hpm_sei_idle_state idle_state;
	bool idle_high_z;
};

struct hpm_sei_transceiver_config {
	enum hpm_sei_mode mode;
	bool tri_sample;
	uint32_t baudrate;
	struct hpm_sei_pin_config data;
	struct hpm_sei_pin_config clock;
	uint16_t ck0_timeout_us;
	uint16_t ck1_timeout_us;
	uint8_t async_data_bits;
	uint8_t async_wait_bits;
	bool async_parity_enable;
	bool async_parity_odd;
};

struct hpm_sei_data_format {
	enum hpm_sei_data_mode mode;
	bool is_signed;
	enum hpm_sei_bit_order bit_order;
	enum hpm_sei_word_order word_order;
	uint8_t word_length;
	bool crc_invert;
	bool crc_shift;
	uint8_t crc_length;
	uint8_t first_bit;
	uint8_t last_bit;
	uint8_t min_bit;
	uint8_t max_bit;
	uint32_t gold_value;
	uint32_t crc_initial_value;
	uint32_t crc_polynomial;
};

struct hpm_sei_instruction {
	enum hpm_sei_instruction_operation operation;
	uint8_t clock;
	enum hpm_sei_data_register crc_register;
	enum hpm_sei_data_register data_register;
	uint8_t operand;
};

struct hpm_sei_latch_transition {
	bool disable_instr_ptr_check;
	enum hpm_sei_transition_condition instr_ptr_cfg;
	uint8_t instr_ptr_value;
	bool disable_clock_check;
	enum hpm_sei_transition_condition clock_cfg;
	bool disable_tx_check;
	enum hpm_sei_transition_condition tx_cfg;
	bool disable_rx_check;
	enum hpm_sei_transition_condition rx_cfg;
	bool disable_timeout_check;
	enum hpm_sei_transition_condition timeout_cfg;
};

struct hpm_sei_sample_config {
	uint8_t latch;
	bool sample_once;
	uint16_t window;
	enum hpm_sei_data_register acc_data_register;
	enum hpm_sei_data_register speed_data_register;
	enum hpm_sei_data_register revolution_data_register;
	enum hpm_sei_data_register position_data_register;
	bool use_acc_rx;
	bool use_speed_rx;
	bool use_revolution_rx;
	bool use_position_rx;
	uint32_t data_register_select;
};

struct hpm_sei_update_config {
	uint8_t latch;
	bool update_on_error;
	bool use_time_override;
	enum hpm_sei_data_register acc_data_register;
	enum hpm_sei_data_register speed_data_register;
	enum hpm_sei_data_register revolution_data_register;
	enum hpm_sei_data_register position_data_register;
	bool use_acc_rx;
	bool use_speed_rx;
	bool use_revolution_rx;
	bool use_position_rx;
	uint32_t data_register_select;
};

struct hpm_sei_engine_config {
	bool arm_on_trigger;
	enum hpm_sei_data_register command_data_register;
	enum hpm_sei_data_register data_base_register;
	uint8_t initial_instruction;
	bool watchdog_enable;
	bool watchdog_exception;
	uint8_t watchdog_instruction;
	uint16_t watchdog_time;
};

/** Runtime state of one SEI channel. */
struct hpm_sei_status {
	uint32_t irq_status;
	uint32_t sample_valid;
	uint32_t update_status;
	bool sample_occurred;
	bool update_error;
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_sei_driver_api {
	int (*configure_transceiver)(const struct device *dev, enum hpm_sei_channel channel,
				     const struct hpm_sei_transceiver_config *config);
	int (*configure_data)(const struct device *dev, enum hpm_sei_data_register register_index,
			      const struct hpm_sei_data_format *format);
	int (*set_instruction)(const struct device *dev, uint8_t index,
			       const struct hpm_sei_instruction *instruction);
	int (*configure_latch)(const struct device *dev, enum hpm_sei_channel channel,
			       uint8_t latch, uint8_t transition,
			       const struct hpm_sei_latch_transition *config);
	int (*configure_sample)(const struct device *dev, enum hpm_sei_channel channel,
				const struct hpm_sei_sample_config *config);
	int (*configure_update)(const struct device *dev, enum hpm_sei_channel channel,
				const struct hpm_sei_update_config *config);
	int (*configure_engine)(const struct device *dev, enum hpm_sei_channel channel,
				const struct hpm_sei_engine_config *config);
	int (*enable)(const struct device *dev, enum hpm_sei_channel channel, bool enable);
	int (*software_trigger)(const struct device *dev, enum hpm_sei_channel channel);
	int (*get_data)(const struct device *dev, enum hpm_sei_data_register register_index,
			uint32_t *value);
	int (*set_data)(const struct device *dev, enum hpm_sei_data_register register_index,
			uint32_t value);
	int (*get_status)(const struct device *dev, enum hpm_sei_channel channel,
			  struct hpm_sei_status *status);
};
/** @endcond */

static inline int hpm_sei_configure_transceiver(const struct device *dev,
						enum hpm_sei_channel channel,
						const struct hpm_sei_transceiver_config *config)
{
	return DEVICE_API_GET(hpm_sei, dev)->configure_transceiver(dev, channel, config);
}

static inline int hpm_sei_configure_data(const struct device *dev,
					 enum hpm_sei_data_register register_index,
					 const struct hpm_sei_data_format *format)
{
	return DEVICE_API_GET(hpm_sei, dev)->configure_data(dev, register_index, format);
}

static inline int hpm_sei_set_instruction(const struct device *dev, uint8_t index,
					  const struct hpm_sei_instruction *instruction)
{
	return DEVICE_API_GET(hpm_sei, dev)->set_instruction(dev, index, instruction);
}

static inline int hpm_sei_configure_latch(const struct device *dev, enum hpm_sei_channel channel,
					  uint8_t latch, uint8_t transition,
					  const struct hpm_sei_latch_transition *config)
{
	return DEVICE_API_GET(hpm_sei, dev)
		->configure_latch(dev, channel, latch, transition, config);
}

static inline int hpm_sei_configure_sample(const struct device *dev, enum hpm_sei_channel channel,
					   const struct hpm_sei_sample_config *config)
{
	return DEVICE_API_GET(hpm_sei, dev)->configure_sample(dev, channel, config);
}

static inline int hpm_sei_configure_update(const struct device *dev, enum hpm_sei_channel channel,
					   const struct hpm_sei_update_config *config)
{
	return DEVICE_API_GET(hpm_sei, dev)->configure_update(dev, channel, config);
}

static inline int hpm_sei_configure_engine(const struct device *dev, enum hpm_sei_channel channel,
					   const struct hpm_sei_engine_config *config)
{
	return DEVICE_API_GET(hpm_sei, dev)->configure_engine(dev, channel, config);
}

static inline int hpm_sei_enable(const struct device *dev, enum hpm_sei_channel channel,
				 bool enable)
{
	return DEVICE_API_GET(hpm_sei, dev)->enable(dev, channel, enable);
}

static inline int hpm_sei_software_trigger(const struct device *dev, enum hpm_sei_channel channel)
{
	return DEVICE_API_GET(hpm_sei, dev)->software_trigger(dev, channel);
}

static inline int hpm_sei_get_data(const struct device *dev,
				   enum hpm_sei_data_register register_index, uint32_t *value)
{
	return DEVICE_API_GET(hpm_sei, dev)->get_data(dev, register_index, value);
}

static inline int hpm_sei_set_data(const struct device *dev,
				   enum hpm_sei_data_register register_index, uint32_t value)
{
	return DEVICE_API_GET(hpm_sei, dev)->set_data(dev, register_index, value);
}

static inline int hpm_sei_get_status(const struct device *dev, enum hpm_sei_channel channel,
				     struct hpm_sei_status *status)
{
	return DEVICE_API_GET(hpm_sei, dev)->get_status(dev, channel, status);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_H_ */
