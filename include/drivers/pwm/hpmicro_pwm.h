/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_PWM_HPMICRO_PWM_H_
#define ZEPHYR_INCLUDE_DRIVERS_PWM_HPMICRO_PWM_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro PWM extensions
 * @defgroup hpm_pwm_interface HPMicro PWM
 * @ingroup pwm_interface
 *
 * Standard capture uses an independent PWM block with a free-running 28-bit
 * counter. It cannot be enabled on a block that has configured any output,
 * reference output or fault output during the current boot, even if its pulse
 * width is zero. Output configuration and raw capture configuration return
 * -EBUSY while standard capture is active. Capture starts its own counter;
 * no dummy output is required. Captured periods must be shorter than
 * 2^28 / rate seconds, where rate is returned by pwm_get_cycles_per_sec().
 * Multiple counter wraps cannot be distinguished and are not supported.
 *
 * A NULL standard capture callback clears the binding, as required by the
 * blocking capture helpers' timeout cleanup. Enabling capture without a
 * callback returns -EINVAL.
 * @{
 */

/** Number of comparators in one HPM5361 PWM block. */
#define HPM_PWM_COMPARATOR_COUNT       24U
/** Maximum dead-zone value in PWM clock half-cycles. */
#define HPM_PWM_DEAD_ZONE_MAX          0xFFFFFU
/** First general-purpose reference output used by TRGM. */
#define HPM_PWM_REFERENCE_OUTPUT_FIRST 8U
/** Last general-purpose reference output exposed by HPM5361 TRGM. */
#define HPM_PWM_REFERENCE_OUTPUT_LAST  15U

/** Fault output behavior. */
enum hpm_pwm_fault_mode {
	HPM_PWM_FAULT_FORCE_LOW = 0,
	HPM_PWM_FAULT_FORCE_HIGH,
	HPM_PWM_FAULT_FORCE_HIGH_Z,
};

/** Fault recovery condition. */
enum hpm_pwm_fault_recovery {
	HPM_PWM_FAULT_RECOVER_IMMEDIATELY = 0,
	HPM_PWM_FAULT_RECOVER_ON_RELOAD,
	HPM_PWM_FAULT_RECOVER_ON_HW_EVENT,
	HPM_PWM_FAULT_RECOVER_ON_CLEAR,
};

/** Fault sources, ORed into hpm_pwm_fault_config::source_mask. */
#define HPM_PWM_FAULT_SOURCE_INTERNAL0 BIT(0)
#define HPM_PWM_FAULT_SOURCE_INTERNAL1 BIT(1)
#define HPM_PWM_FAULT_SOURCE_INTERNAL2 BIT(2)
#define HPM_PWM_FAULT_SOURCE_INTERNAL3 BIT(3)
#define HPM_PWM_FAULT_SOURCE_EXTERNAL0 BIT(4)
#define HPM_PWM_FAULT_SOURCE_EXTERNAL1 BIT(5)
#define HPM_PWM_FAULT_SOURCE_DEBUG     BIT(6)

/** Capture edge selection. */
enum hpm_pwm_capture_edge {
	HPM_PWM_CAPTURE_RISING = 0,
	HPM_PWM_CAPTURE_FALLING,
};

struct hpm_pwm_fault_config {
	uint32_t source_mask;
	enum hpm_pwm_fault_mode output_mode;
	enum hpm_pwm_fault_recovery recovery;
	bool external0_active_low;
	bool external1_active_low;
	bool recover_at_rising_edge;
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_pwm_driver_api {
	/** Must remain first so the device also implements the PWM API. */
	struct pwm_driver_api pwm_api;
	int (*set_complementary)(const struct device *dev, uint32_t channel, uint32_t period_cycles,
				 uint32_t pulse_cycles, pwm_flags_t flags,
				 uint32_t dead_zone_half_cycles);
	int (*configure_fault)(const struct device *dev, const struct hpm_pwm_fault_config *config);
	int (*clear_fault)(const struct device *dev);
	int (*configure_capture)(const struct device *dev, uint8_t comparator,
				 enum hpm_pwm_capture_edge edge);
	int (*get_capture)(const struct device *dev, uint8_t comparator, uint32_t *capture);
	int (*set_reference_cycles)(const struct device *dev, uint8_t comparator, uint8_t reference,
				    uint32_t period_cycles, uint32_t pulse_cycles,
				    pwm_flags_t flags);
};

DEVICE_API_EXTENDS(hpm_pwm, pwm, pwm_api);
/** @endcond */

/**
 * @brief Configure a complementary output pair with dead-time insertion.
 *
 * Channels must be an even/odd pair. The primary waveform is generated on
 * @p channel and the complementary waveform on @p channel + 1. Dead time is
 * expressed in PWM clock half-cycles and applies to both pair members.
 *
 * @param dev PWM device.
 * @param channel Even PWM channel number.
 * @param period_cycles PWM period in cycles.
 * @param pulse_cycles Active pulse width in cycles.
 * @param flags Polarity flags for the primary output.
 * @param dead_zone_half_cycles Dead time, up to HPM_PWM_DEAD_ZONE_MAX.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid pair, period, pulse or dead time.
 * @retval -ENOTSUP for unsupported flags.
 * @retval -EBUSY while standard capture is active on this block.
 */
static inline int hpm_pwm_set_complementary(const struct device *dev, uint32_t channel,
					    uint32_t period_cycles, uint32_t pulse_cycles,
					    pwm_flags_t flags, uint32_t dead_zone_half_cycles)
{
	return DEVICE_API_GET(hpm_pwm, dev)
		->set_complementary(dev, channel, period_cycles, pulse_cycles, flags,
				    dead_zone_half_cycles);
}

/**
 * @brief Configure and enable a PWM fault source.
 *
 * The configuration applies to all PWM outputs in the block. Call
 * hpm_pwm_clear_fault() to release a latched fault when recovery is
 * HPM_PWM_FAULT_RECOVER_ON_CLEAR.
 *
 * @retval 0 on success.
 * @retval -EINVAL for null or invalid configuration.
 * @retval -EBUSY while standard capture is active on this block.
 */
static inline int hpm_pwm_configure_fault(const struct device *dev,
					  const struct hpm_pwm_fault_config *config)
{
	return DEVICE_API_GET(hpm_pwm, dev)->configure_fault(dev, config);
}

/** @brief Clear a latched PWM fault. */
static inline int hpm_pwm_clear_fault(const struct device *dev)
{
	return DEVICE_API_GET(hpm_pwm, dev)->clear_fault(dev);
}

/**
 * @brief Configure one comparator as an input-capture channel.
 *
 * @param dev PWM device.
 * @param comparator Comparator index from 0 through HPM_PWM_COMPARATOR_COUNT - 1.
 * @param edge Capture edge.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid comparator or edge.
 * @retval -EBUSY while standard capture is active on this block.
 */
static inline int hpm_pwm_configure_capture(const struct device *dev, uint8_t comparator,
					    enum hpm_pwm_capture_edge edge)
{
	return DEVICE_API_GET(hpm_pwm, dev)->configure_capture(dev, comparator, edge);
}

/**
 * @brief Read the latest raw rising-edge timestamp for a comparator.
 *
 * The value is the CAPPOS register encoding: the 28-bit counter occupies
 * bits 31 through 4. This raw accessor always reads the rising-edge register,
 * independently of the interrupt edge selected by hpm_pwm_configure_capture().
 * Standard PWM capture callbacks instead report elapsed PWM clock cycles.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid comparator or null output.
 */
static inline int hpm_pwm_get_capture(const struct device *dev, uint8_t comparator,
				      uint32_t *capture)
{
	return DEVICE_API_GET(hpm_pwm, dev)->get_capture(dev, comparator, capture);
}

/**
 * @brief Configure a general-purpose PWM reference output for TRGM.
 *
 * HPM5361 channels 8 through 15 expose CHxREF without passing through the
 * complementary, dead-time, force and fault output stage. A single comparator
 * is assigned to @p reference. The resulting reference output can be selected
 * as `HPM_TRGM_INPUT_PWM0_CHnREF` by the TRGM mux driver and used as an ADC,
 * QEIV2 or other peripheral trigger.
 *
 * All PWM channels in one block share the counter period. This function resets
 * and restarts that counter, as the standard set_cycles() API does.
 *
 * @param dev PWM device.
 * @param comparator Comparator index from 0 through HPM_PWM_COMPARATOR_COUNT - 1.
 * @param reference Reference output from HPM_PWM_REFERENCE_OUTPUT_FIRST through
 *                  HPM_PWM_REFERENCE_OUTPUT_LAST.
 * @param period_cycles PWM period in cycles.
 * @param pulse_cycles High pulse width in cycles.
 * @param flags PWM_POLARITY_NORMAL or PWM_POLARITY_INVERTED.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid comparator, reference, period or pulse.
 * @retval -ENOTSUP for unsupported flags or an unrepresentable period.
 * @retval -EBUSY while standard capture is active on this block.
 */
static inline int hpm_pwm_set_reference_cycles(const struct device *dev, uint8_t comparator,
					       uint8_t reference, uint32_t period_cycles,
					       uint32_t pulse_cycles, pwm_flags_t flags)
{
	return DEVICE_API_GET(hpm_pwm, dev)
		->set_reference_cycles(dev, comparator, reference, period_cycles, pulse_cycles,
				       flags);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_PWM_HPMICRO_PWM_H_ */
