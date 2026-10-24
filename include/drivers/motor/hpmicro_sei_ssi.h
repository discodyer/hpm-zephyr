/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_SSI_H_
#define ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_SSI_H_

#include <errno.h>
#include <stdint.h>

#include <drivers/motor/hpmicro_sei.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro SSI encoder protocol consumer
 * @defgroup hpm_sei_ssi_interface HPMicro SSI encoder
 * @ingroup io_interfaces
 * @{
 */

/** Configuration for one synchronous SSI master channel. */
struct hpm_sei_ssi_config {
	/** SSI clock frequency in Hz. */
	uint32_t baudrate;
	/** Multi-turn data width, from 1 through 32 bits. */
	uint8_t multi_turn_bits;
	/** Single-turn data width, from 1 through 32 bits. */
	uint8_t single_turn_bits;
	/** Time between SSI transfers in microseconds. */
	uint32_t poll_period_us;
	/** Optional LATCH1/transfer-error callback, invoked in interrupt context. */
	hpm_sei_event_callback_t callback;
	/** Context passed to @ref callback. */
	void *user_data;
};

/** Latest decoded SSI sample. */
struct hpm_sei_ssi_result {
	/** Multi-turn value, masked to the configured width. */
	uint32_t multi_turn;
	/** Single-turn value, masked to the configured width. */
	uint32_t single_turn;
	/** Timestamp captured by LATCH0 at the start of the transfer. */
	uint32_t sample_time;
	/** Timestamp captured by LATCH1 after the transfer. */
	uint32_t update_time;
	/** Update timestamp minus sample timestamp, in SYNT timestamp ticks. */
	uint32_t delay_ticks;
};

/**
 * @brief Convert a microsecond interval to SEI clock cycles.
 *
 * The result is rounded to the nearest cycle and must fit the periodic-trigger
 * register. This helper is available independently of hardware so callers and
 * tests can validate interval calculations.
 *
 * @param clock_hz SEI module clock frequency in Hz.
 * @param period_us Interval in microseconds.
 * @param cycles Converted interval in clock cycles.
 *
 * @retval 0 on success.
 * @retval -EINVAL for a zero input frequency or interval.
 * @retval -ERANGE when the interval exceeds the periodic-trigger range.
 */
static inline int hpm_sei_ssi_period_to_cycles(uint32_t clock_hz, uint32_t period_us,
					       uint32_t *cycles)
{
	uint64_t converted;

	if ((clock_hz == 0U) || (period_us == 0U) || (cycles == NULL)) {
		return -EINVAL;
	}

	converted = ((uint64_t)clock_hz * period_us + 500000U) / 1000000U;
	if ((converted == 0U) || (converted >= UINT32_MAX)) {
		return -ERANGE;
	}

	*cycles = (uint32_t)converted;

	return 0;
}

/**
 * @brief Convert an SSI baud rate to the SEI clock divider.
 *
 * The divider is rounded to the nearest SEI clock cycle. At least four SEI
 * clock cycles per SSI bit are required to produce non-zero CK0/CK1 sampling
 * points. The maximum divider is 65536 because the hardware stores divider
 * minus one in a 16-bit field.
 *
 * @param clock_hz SEI module clock frequency in Hz.
 * @param baudrate SSI clock frequency in Hz.
 * @param divider Converted divider.
 *
 * @retval 0 on success.
 * @retval -EINVAL for a zero input or an unsupported divider.
 */
static inline int hpm_sei_ssi_baud_to_divider(uint32_t clock_hz, uint32_t baudrate,
					      uint32_t *divider)
{
	uint64_t converted;

	if ((clock_hz == 0U) || (baudrate == 0U) || (divider == NULL)) {
		return -EINVAL;
	}

	converted = ((uint64_t)clock_hz + (baudrate / 2U)) / baudrate;
	if ((converted < 4U) || (converted > (UINT16_MAX + 1U))) {
		return -EINVAL;
	}

	*divider = (uint32_t)converted;

	return 0;
}

/**
 * @brief Configure a channel as a synchronous SSI master and start sampling.
 *
 * This programs the fixed SSI instruction program, LATCH0/LATCH1 behavior,
 * SAMPLE/UPDATE mapping and periodic trigger through the protocol-neutral SEI
 * API. Configuration must be serialized by the caller with other operations on
 * the same SEI channel.
 *
 * @param dev SEI device.
 * @param channel SEI channel.
 * @param config SSI configuration.
 *
 * @retval 0 on success.
 * @retval -ENODEV for an invalid or unready SEI device.
 * @retval -EINVAL for invalid arguments or a baud rate that leaves fewer than
 *         four SEI clock cycles per SSI bit.
 * @retval -ERANGE when the polling interval is too long.
 * @retval -EBUSY when the channel is owned by another SEI device.
 * @retval -EIO when the SEI driver rejects register programming.
 */
int hpm_sei_ssi_configure(const struct device *dev, enum hpm_sei_channel channel,
			  const struct hpm_sei_ssi_config *config);

/**
 * @brief Read the latest SSI result.
 *
 * Pending LATCH1 and transfer-error events are consumed. A successful read
 * reports the values captured by the most recent completed transfer.
 *
 * @param dev SEI device.
 * @param channel SEI channel.
 * @param result Destination for the decoded sample.
 *
 * @retval 0 on success.
 * @retval -ENODEV for an invalid or unready SEI device.
 * @retval -EINVAL for invalid arguments or an unconfigured channel.
 * @retval -EAGAIN when no transfer has completed yet.
 * @retval -EIO when the last transfer reported an update or transfer error.
 */
int hpm_sei_ssi_get_result(const struct device *dev, enum hpm_sei_channel channel,
			   struct hpm_sei_ssi_result *result);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MOTOR_HPMICRO_SEI_SSI_H_ */
