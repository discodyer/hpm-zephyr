/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_BISSC_H_
#define ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_BISSC_H_

#include <errno.h>
#include <stdint.h>

#include <drivers/motor/hpmicro_sei.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro BiSS-C encoder protocol consumer
 * @defgroup hpm_sei_bissc_interface HPMicro BiSS-C encoder
 * @ingroup io_interfaces
 * @{
 */

#define HPM_SEI_BISSC_STATUS_BITS 2U

/**
 * @brief Check that periodic polling completes before the engine watchdog.
 *
 * A zero watchdog time disables the watchdog and is always accepted.
 *
 * @retval -EINVAL for invalid rates or when the polling period is not shorter
 *         than the watchdog interval.
 */
static inline int hpm_sei_bissc_check_timing(uint32_t baudrate, uint32_t poll_period_us,
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

/** Configuration for one BiSS-C master channel. */
struct hpm_sei_bissc_config {
	/** Synchronous clock frequency in Hz. */
	uint32_t baudrate;
	/** Multi-turn data width, from 1 through 32 bits. */
	uint8_t multi_turn_bits;
	/** Single-turn data width, from 1 through 32 bits. */
	uint8_t single_turn_bits;
	/** CRC width, from 1 through 32 bits. */
	uint8_t crc_bits;
	/** CRC polynomial used by the hardware. BiSS-C commonly uses 0x03. */
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

/** Latest decoded BiSS-C sample. */
struct hpm_sei_bissc_result {
	/** Multi-turn position, masked to the configured width. */
	uint32_t multi_turn;
	/** Single-turn position, masked to the configured width. */
	uint32_t single_turn;
	/** BiSS-C error status bit. */
	bool error;
	/** BiSS-C warning status bit. */
	bool warning;
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
 * @brief Configure a channel as a BiSS-C master and start polling.
 *
 * The consumer programs the standard BiSS-C response sequence through the
 * protocol-neutral SEI API. The SEI hardware calculates and compares the CRC;
 * a mismatch is reported as a transfer error.
 *
 * The receive and transmit sampling points are set to CK0 and CK1. The dynamic
 * bus-delay calibration used by the vendor example is not performed because
 * HPM5361 erratum E00047 prevents the required RX-edge latch configuration.
 *
 * @retval -ENODEV for an invalid or unready SEI device.
 * @retval -EINVAL for invalid arguments or watchdog timing.
 * @retval -ERANGE when the polling interval is too long.
 * @retval -EBUSY when the SEI block is owned by another consumer or the
 *         other channel is already active.
 * @retval -EIO when the SEI driver rejects register programming.
 */
int hpm_sei_bissc_configure(const struct device *dev, enum hpm_sei_channel channel,
			    const struct hpm_sei_bissc_config *config);

/** Stop the BiSS-C consumer and release its whole-block ownership. */
int hpm_sei_bissc_deconfigure(const struct device *dev, enum hpm_sei_channel channel);

/**
 * @brief Read the latest completed BiSS-C sample.
 *
 * @retval 0 on success.
 * @retval -EAGAIN when no transfer has completed yet.
 * @retval -EIO when the last transfer reported a transfer, update, exception or
 *         watchdog error.
 * @retval -EINVAL for invalid arguments or an unconfigured channel.
 */
int hpm_sei_bissc_get_result(const struct device *dev, enum hpm_sei_channel channel,
			     struct hpm_sei_bissc_result *result);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_BISSC_H_ */
