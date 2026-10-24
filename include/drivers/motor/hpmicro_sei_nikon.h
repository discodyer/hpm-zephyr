/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_NIKON_H_
#define ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_NIKON_H_

#include <errno.h>
#include <stdint.h>

#include <drivers/motor/hpmicro_sei.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro Nikon encoder protocol consumer
 * @defgroup hpm_sei_nikon_interface HPMicro Nikon encoder
 * @ingroup io_interfaces
 * @{
 */

/** Default Sink Code used by the vendor CDF0 reference. */
#define HPM_SEI_NIKON_DEFAULT_SINK_CODE 0x02U
/** Default CDF0 command code used by the vendor reference. */
#define HPM_SEI_NIKON_DEFAULT_CDF0      0x00U
/** Single-turn position width in the CDF0 response. */
#define HPM_SEI_NIKON_SINGLE_TURN_BITS  20U
/** Multi-turn position width in the CDF0 response. */
#define HPM_SEI_NIKON_MULTI_TURN_BITS   20U

/**
 * @brief Check that periodic polling completes before the engine watchdog.
 *
 * A zero watchdog time disables the watchdog and is always accepted.
 *
 * @retval -EINVAL for invalid rates or when the polling period is not shorter
 *         than the watchdog interval.
 */
static inline int hpm_sei_nikon_check_timing(uint32_t baudrate, uint32_t poll_period_us,
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

/** Configuration for one Nikon CDF0 master channel. */
struct hpm_sei_nikon_config {
	/** Asynchronous data rate in bit/s, commonly 2.5 Mbit/s. */
	uint32_t baudrate;
	/** Three-bit Sink Code sent before the command. */
	uint8_t sink_code;
	/** CDF0 command code, from 0 through 31. */
	uint8_t cdf0;
	/** Expected six-bit information-field echo. */
	uint8_t expected_eax;
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

/** Latest decoded Nikon CDF0 sample. */
struct hpm_sei_nikon_result {
	/** Single-turn position. */
	uint32_t position;
	/** Multi-turn revolution count. */
	uint32_t revolution;
	/** Information-field extended address echo. */
	uint8_t extended_address;
	/** Information-field command-code echo. */
	uint8_t command_code;
	/** Information-field encoder status. */
	uint8_t encoder_status;
	/** Data-field CRC reported by the encoder. */
	uint8_t checksum;
	/** Timestamp captured by LATCH0 at the start of the transfer. */
	uint32_t sample_time;
	/** Timestamp captured by LATCH1 after the transfer. */
	uint32_t update_time;
	/** Update timestamp minus sample timestamp, in SYNT timestamp ticks. */
	uint32_t delay_ticks;
};

/**
 * @brief Configure a channel as a Nikon CDF0 master and start polling.
 *
 * The consumer programs the vendor three-bit Sink Code and ten-bit CDF0
 * command, verifies its three-bit command CRC, echoes the information field and
 * validates the CRC-8 data field. The response provides 20-bit single-turn and
 * multi-turn position values plus encoder status.
 *
 * The fixed LATCH0/LATCH1 transitions avoid the E00047-affected receive-edge
 * path and do not perform the vendor dynamic bus-delay calibration.
 *
 * @retval -ENODEV for an invalid or unready SEI device.
 * @retval -EINVAL for invalid arguments or watchdog timing.
 * @retval -ERANGE when the polling interval is too long.
 * @retval -EBUSY when the SEI block is owned by another consumer or the
 *         other channel is already active.
 * @retval -EIO when the SEI driver rejects register programming.
 */
int hpm_sei_nikon_configure(const struct device *dev, enum hpm_sei_channel channel,
			    const struct hpm_sei_nikon_config *config);

/** Stop the Nikon consumer and release its whole-block ownership. */
int hpm_sei_nikon_deconfigure(const struct device *dev, enum hpm_sei_channel channel);

/**
 * @brief Read the latest completed Nikon CDF0 sample.
 *
 * @retval 0 on success.
 * @retval -EAGAIN when no transfer has completed yet.
 * @retval -EIO when the last transfer reported a transfer, update, exception or
 *         watchdog error.
 * @retval -EINVAL for invalid arguments or an unconfigured channel.
 */
int hpm_sei_nikon_get_result(const struct device *dev, enum hpm_sei_channel channel,
			     struct hpm_sei_nikon_result *result);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_NIKON_H_ */
