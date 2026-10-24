/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_TAMAGAWA_H_
#define ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_TAMAGAWA_H_

#include <errno.h>
#include <stdint.h>

#include <drivers/motor/hpmicro_sei.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro Tamagawa encoder protocol consumer
 * @defgroup hpm_sei_tamagawa_interface HPMicro Tamagawa encoder
 * @ingroup io_interfaces
 * @{
 */

#define HPM_SEI_TAMAGAWA_ST_BITS   24U
#define HPM_SEI_TAMAGAWA_MT_BITS   24U
#define HPM_SEI_TAMAGAWA_DATA_ID_3 0x1aU

/**
 * @brief Check that periodic polling completes before the engine watchdog.
 *
 * A zero watchdog time disables the watchdog and is always accepted.
 *
 * @retval -EINVAL for invalid rates or when the polling period is not shorter
 *         than the watchdog interval.
 */
static inline int hpm_sei_tamagawa_check_timing(uint32_t baudrate, uint32_t poll_period_us,
						uint16_t watchdog_time)
{
	if ((baudrate == 0U) || (poll_period_us == 0U)) {
		return -EINVAL;
	}
	if ((watchdog_time != 0U) &&
	    ((uint64_t)watchdog_time * 1000000U <= (uint64_t)poll_period_us * baudrate)) {
		return -EINVAL;
	}

	return 0;
}

/** Configuration for one Tamagawa master channel. */
struct hpm_sei_tamagawa_config {
	/** Asynchronous data rate in bit/s, commonly 2.5 Mbit/s or 5 Mbit/s. */
	uint32_t baudrate;
	/** Encoder ID expected in the response; zero disables ID checking. */
	uint8_t encoder_id;
	/** Time between transfers in microseconds. */
	uint32_t poll_period_us;
	/** LATCH0 output delay in SEI clock cycles. */
	uint16_t sample_delay_cycles;
	/** Engine watchdog time in bit times. Zero disables the watchdog. */
	uint16_t watchdog_time;
	/** Optional event callback, called in ISR context after watchdog recovery. */
	hpm_sei_event_callback_t callback;
	/** Context passed to @ref callback. */
	void *user_data;
};

/** Latest decoded Tamagawa sample. */
struct hpm_sei_tamagawa_result {
	/** Absolute position within one revolution (ABS). */
	uint32_t position;
	/** Multi-turn count (ABM). */
	uint32_t revolution;
	/** Control field echoed by the encoder. */
	uint8_t control_field;
	/** Encoder status field. */
	uint8_t status_field;
	/** Encoder ID. */
	uint8_t encoder_id;
	/** Alarm code. */
	uint8_t alarm;
	/** CRC value reconstructed from the CRC-mode data register. */
	uint8_t crc;
	/** Timestamp captured by LATCH0 at the start of the transfer. */
	uint32_t sample_time;
	/** Timestamp captured by LATCH1 after the transfer. */
	uint32_t update_time;
	/** Update timestamp minus sample timestamp, in SYNT timestamp ticks. */
	uint32_t delay_ticks;
};

/**
 * @brief Configure a channel as a Tamagawa master and start polling.
 *
 * The consumer programs the standard Data ID 3 instruction sequence through
 * the protocol-neutral SEI API. Configuration must be serialized by the
 * caller with other operations on the same SEI channel.
 *
 * @retval -ENODEV for an invalid or unready SEI device.
 * @retval -EINVAL for invalid arguments.
 * @retval -ERANGE when the polling interval is too long.
 * @retval -EBUSY when the SEI block is owned by another consumer or the
 *         other channel is already active.
 * @retval -EIO when the SEI driver rejects register programming.
 */
int hpm_sei_tamagawa_configure(const struct device *dev, enum hpm_sei_channel channel,
			       const struct hpm_sei_tamagawa_config *config);

/** Stop the Tamagawa consumer and release its whole-block ownership. */
int hpm_sei_tamagawa_deconfigure(const struct device *dev, enum hpm_sei_channel channel);

/**
 * @brief Read the latest completed Tamagawa sample.
 *
 * @retval 0 on success.
 * @retval -EAGAIN when no transfer has completed yet.
 * @retval -EIO when the last transfer reported a transfer, update or watchdog error.
 * @retval -EINVAL for invalid arguments or an unconfigured channel.
 */
int hpm_sei_tamagawa_get_result(const struct device *dev, enum hpm_sei_channel channel,
				struct hpm_sei_tamagawa_result *result);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_TAMAGAWA_H_ */
