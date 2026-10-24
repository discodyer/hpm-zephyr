/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_HIPERFACE_H_
#define ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_HIPERFACE_H_

#include <errno.h>
#include <stdint.h>

#include <drivers/motor/hpmicro_sei.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro HIPERFACE encoder protocol consumer
 * @defgroup hpm_sei_hiperface_interface HPMicro HIPERFACE encoder
 * @ingroup io_interfaces
 * @{
 */

/** Default HIPERFACE encoder address used by the vendor reference. */
#define HPM_SEI_HIPERFACE_DEFAULT_ADDRESS  0x40U
/** Default HIPERFACE position command used by the vendor reference. */
#define HPM_SEI_HIPERFACE_DEFAULT_COMMAND  0x42U
/** Single-turn width in the standard 32-bit position response. */
#define HPM_SEI_HIPERFACE_SINGLE_TURN_BITS 12U
/** Multi-turn width in the standard 32-bit position response. */
#define HPM_SEI_HIPERFACE_MULTI_TURN_BITS  20U

/**
 * @brief Check that periodic polling completes before the engine watchdog.
 *
 * A zero watchdog time disables the watchdog and is always accepted.
 *
 * @retval -EINVAL for invalid rates or when the polling period is not shorter
 *         than the watchdog interval.
 */
static inline int hpm_sei_hiperface_check_timing(uint32_t baudrate, uint32_t poll_period_us,
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

/** Configuration for one HIPERFACE master channel. */
struct hpm_sei_hiperface_config {
	/** Asynchronous data rate in bit/s, commonly 9600. */
	uint32_t baudrate;
	/** Encoder address sent in the request. */
	uint8_t address;
	/** Position command sent in the request. */
	uint8_t command;
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

/** Latest decoded HIPERFACE sample. */
struct hpm_sei_hiperface_result {
	/** Single-turn position. */
	uint32_t position;
	/** Multi-turn revolution count. */
	uint32_t revolution;
	/** Address echoed by the encoder. */
	uint8_t address;
	/** Command echoed by the encoder. */
	uint8_t command;
	/** Checksum reported by the encoder. */
	uint8_t checksum;
	/** Timestamp captured by LATCH0 at the start of the transfer. */
	uint32_t sample_time;
	/** Timestamp captured by LATCH1 after the transfer. */
	uint32_t update_time;
	/** Update timestamp minus sample timestamp, in SYNT timestamp ticks. */
	uint32_t delay_ticks;
};

/**
 * @brief Configure a channel as a HIPERFACE master and start polling.
 *
 * The consumer programs the 8E1 request, software checksum, encoder echo
 * validation and 32-bit position response through the protocol-neutral SEI
 * API. The standard response is split into 12 single-turn bits and 20
 * multi-turn bits.
 *
 * The LATCH0 transition uses the HPM5361 E00047-safe instruction-pointer path;
 * the vendor dynamic bus-delay calibration is not performed.
 *
 * HPM5361 E00025 makes the SEI hardware parity function unreliable. This
 * consumer retains the vendor 8E1 programming sequence for build-only
 * validation; hardware use requires the CRC-based parity workaround or a
 * validated external UART path.
 *
 * @retval -ENODEV for an invalid or unready SEI device.
 * @retval -EINVAL for invalid arguments or watchdog timing.
 * @retval -ERANGE when the polling interval is too long.
 * @retval -EBUSY when the SEI block is owned by another consumer or the
 *         other channel is already active.
 * @retval -EIO when the SEI driver rejects register programming.
 */
int hpm_sei_hiperface_configure(const struct device *dev, enum hpm_sei_channel channel,
				const struct hpm_sei_hiperface_config *config);

/** Stop the HIPERFACE consumer and release its whole-block ownership. */
int hpm_sei_hiperface_deconfigure(const struct device *dev, enum hpm_sei_channel channel);

/**
 * @brief Read the latest completed HIPERFACE sample.
 *
 * @retval 0 on success.
 * @retval -EAGAIN when no transfer has completed yet.
 * @retval -EIO when the last transfer reported a transfer, update, exception or
 *         watchdog error.
 * @retval -EINVAL for invalid arguments or an unconfigured channel.
 */
int hpm_sei_hiperface_get_result(const struct device *dev, enum hpm_sei_channel channel,
				 struct hpm_sei_hiperface_result *result);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_HIPERFACE_H_ */
