/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_ENDAT_H_
#define ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_ENDAT_H_

#include <errno.h>
#include <stdint.h>

#include <drivers/motor/hpmicro_sei.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro EnDat 2.1/2.2 encoder protocol consumer
 * @defgroup hpm_sei_endat_interface HPMicro EnDat encoder
 * @ingroup io_interfaces
 * @{
 */

/** EnDat command used to read position values. */
#define HPM_SEI_ENDAT_COMMAND_READ_POSITION 0x07U
/** CRC polynomial used by the standard EnDat position response. */
#define HPM_SEI_ENDAT_CRC_POLYNOMIAL        0x0bU
/** CRC initial value used by the standard EnDat position response. */
#define HPM_SEI_ENDAT_CRC_INITIAL_VALUE     0x1fU

/**
 * @brief Check that periodic polling completes before the engine watchdog.
 *
 * A zero watchdog time disables the watchdog and is always accepted.
 *
 * @retval -EINVAL for invalid rates or when the polling period is not shorter
 *         than the watchdog interval.
 */
static inline int hpm_sei_endat_check_timing(uint32_t baudrate, uint32_t poll_period_us,
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

/** Configuration for one EnDat master channel. */
struct hpm_sei_endat_config {
	/** Synchronous clock frequency in Hz. */
	uint32_t baudrate;
	/** Six-bit mode command sent after the start sequence. */
	uint8_t mode_command;
	/** Single-turn position width, from 1 through 32 bits. */
	uint8_t single_turn_bits;
	/** CRC width, from 1 through 32 bits. */
	uint8_t crc_bits;
	/** CRC polynomial used by the hardware. */
	uint32_t crc_polynomial;
	/** CRC initial value. */
	uint32_t crc_initial_value;
	/** Invert the calculated CRC before comparison. */
	bool crc_invert;
	/** Time between transfers in microseconds. */
	uint32_t poll_period_us;
	/** LATCH0 output delay in SEI clock cycles. */
	uint16_t sample_delay_cycles;
	/** Engine watchdog time in bit times. Zero disables the watchdog. */
	uint16_t watchdog_time;
	/** Optional event callback, called in ISR context. */
	hpm_sei_event_callback_t callback;
	/** Context passed to @ref callback. */
	void *user_data;
};

/** Latest decoded EnDat sample. */
struct hpm_sei_endat_result {
	/** Single-turn position, masked to the configured width. */
	uint32_t position;
	/** EnDat error bit. */
	bool error;
	/** CRC value reported by the encoder, masked to the configured width. */
	uint32_t crc;
	/** Timestamp captured by LATCH0 at the start of the transfer. */
	uint32_t sample_time;
	/** Timestamp captured by LATCH1 after the transfer. */
	uint32_t update_time;
	/** Update timestamp minus sample timestamp, in SYNT timestamp ticks. */
	uint32_t delay_ticks;
};

/**
 * @brief Configure a channel as an EnDat master and start polling.
 *
 * The consumer programs the EnDat 2.1/2.2 position-read sequence through the
 * protocol-neutral SEI API. The mode command is loaded through the trigger
 * table, and the hardware calculates and compares the CRC. A mismatch is
 * reported as a transfer error.
 *
 * Receive and transmit sampling both use CK0. The vendor dynamic bus-delay
 * calibration path depends on an RX-edge latch transition that HPM5361 erratum
 * E00047 makes unavailable.
 *
 * @retval -ENODEV for an invalid or unready SEI device.
 * @retval -EINVAL for invalid arguments or watchdog timing.
 * @retval -ERANGE when the polling interval is too long.
 * @retval -EBUSY when the SEI block is owned by another consumer or the
 *         other channel is already active.
 * @retval -EIO when the SEI driver rejects register programming.
 */
int hpm_sei_endat_configure(const struct device *dev, enum hpm_sei_channel channel,
			    const struct hpm_sei_endat_config *config);

/** Stop the EnDat consumer and release its whole-block ownership. */
int hpm_sei_endat_deconfigure(const struct device *dev, enum hpm_sei_channel channel);

/**
 * @brief Read the latest completed EnDat sample.
 *
 * @retval 0 on success.
 * @retval -EAGAIN when no transfer has completed yet.
 * @retval -EIO when the last transfer reported a transfer, update, exception or
 *         watchdog error.
 * @retval -EINVAL for invalid arguments or an unconfigured channel.
 */
int hpm_sei_endat_get_result(const struct device *dev, enum hpm_sei_channel channel,
			     struct hpm_sei_endat_result *result);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_ENDAT_H_ */
