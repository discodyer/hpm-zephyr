/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_PTP_CLOCK_HPMICRO_PTPC_H_
#define ZEPHYR_INCLUDE_DRIVERS_PTP_CLOCK_HPMICRO_PTPC_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/ptp_clock.h>
#include <zephyr/net/ptp_time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro PTPC extensions
 * @defgroup hpm_ptpc_interface HPMicro PTPC
 * @ingroup ptp_clock_interface
 * @{
 */

/** Number of CAN timestamp output ports provided by PTPC. */
#define HPM_PTPC_CAN_PORT_COUNT  4U
/** Number of MCAN TSU input selectors controlled by PTPC_CAN_TS_SEL. */
#define HPM_PTPC_TSU_INPUT_COUNT 4U

/** PTPC counter selected for a CAN timestamp output port. */
enum hpm_ptpc_can_time_source {
	HPM_PTPC_CAN_TIME_SOURCE_PTPC0 = 0,
	HPM_PTPC_CAN_TIME_SOURCE_PTPC1,
};

/**
 * MCAN TSU input source encoded in PTPC_CAN_TS_SEL.
 *
 * Values 0x00 through 0x1e select the timestamp output of MCAN0 through MCAN30.
 * PTPC0 and PTPC1 are the only values that this driver exposes because they are
 * the on-chip sources relevant to the PTPC timestamp controller.
 */
enum hpm_ptpc_tsu_input_source {
	HPM_PTPC_TSU_INPUT_PTPC0 = 0x20,
	HPM_PTPC_TSU_INPUT_PTPC1 = 0x21,
};

/** Capture input edge selection. */
enum hpm_ptpc_capture_edge {
	HPM_PTPC_CAPTURE_DISABLED = 0,
	HPM_PTPC_CAPTURE_RISING,
	HPM_PTPC_CAPTURE_FALLING,
	HPM_PTPC_CAPTURE_BOTH,
};

/** PTPC event bits for one timestamp channel. */
#define HPM_PTPC_EVENT_PPS     (1U << 0U)
#define HPM_PTPC_EVENT_CAPTURE (1U << 1U)
#define HPM_PTPC_EVENT_COMPARE (1U << 2U)
#define HPM_PTPC_EVENT_ALL (HPM_PTPC_EVENT_PPS | HPM_PTPC_EVENT_CAPTURE | HPM_PTPC_EVENT_COMPARE)

struct hpm_ptpc_capture_config {
	enum hpm_ptpc_capture_edge edge;
	/** Keep the existing snapshot until capture is disabled or reconfigured. */
	bool keep;
};

struct hpm_ptpc_compare_config {
	/** Compare target time. */
	struct net_ptp_time target;
	/** Enable or disable the compare function. */
	bool enable;
};

/**
 * Callback invoked from the shared PTPC interrupt.
 *
 * @param dev PTPC timestamp channel.
 * @param events Enabled HPM_PTPC_EVENT_* flags that fired.
 * @param user_data Callback context.
 */
typedef void (*hpm_ptpc_event_callback_t)(const struct device *dev, uint32_t events,
					  void *user_data);

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_ptpc_driver_api {
	/** Must remain first so the device also implements the ptp_clock API. */
	struct ptp_clock_driver_api ptp_clock_api;
	int (*configure_capture)(const struct device *dev,
				 const struct hpm_ptpc_capture_config *config);
	int (*read_capture)(const struct device *dev, struct net_ptp_time *capture);
	int (*configure_compare)(const struct device *dev,
				 const struct hpm_ptpc_compare_config *config);
	int (*configure_pps)(const struct device *dev, uint8_t frequency_exponent);
	int (*configure_can_time_source)(const struct device *dev, uint8_t can_port,
					 enum hpm_ptpc_can_time_source source);
	int (*get_can_time_source)(const struct device *dev, uint8_t can_port,
				   enum hpm_ptpc_can_time_source *source);
	int (*configure_tsu_input)(const struct device *dev, uint8_t tsu_input,
				   enum hpm_ptpc_tsu_input_source source);
	int (*get_tsu_input)(const struct device *dev, uint8_t tsu_input,
			     enum hpm_ptpc_tsu_input_source *source);
	int (*get_events)(const struct device *dev, uint32_t *events);
	int (*clear_events)(const struct device *dev, uint32_t events);
	int (*set_event_callback)(const struct device *dev, uint32_t event_mask,
				  hpm_ptpc_event_callback_t callback, void *user_data);
};

DEVICE_API_EXTENDS(hpm_ptpc, ptp_clock, ptp_clock_api);
/** @endcond */

/**
 * @brief Configure PTPC input capture.
 *
 * @param dev PTPC timestamp channel.
 * @param config Capture edge and snapshot-keep configuration.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid configuration.
 */
static inline int hpm_ptpc_configure_capture(const struct device *dev,
					     const struct hpm_ptpc_capture_config *config)
{
	return DEVICE_API_GET(hpm_ptpc, dev)->configure_capture(dev, config);
}

/**
 * @brief Read the latest captured timestamp.
 *
 * @param dev PTPC timestamp channel.
 * @param capture Output captured time.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p capture is NULL.
 */
static inline int hpm_ptpc_read_capture(const struct device *dev, struct net_ptp_time *capture)
{
	return DEVICE_API_GET(hpm_ptpc, dev)->read_capture(dev, capture);
}

/**
 * @brief Configure or disable the timestamp compare output.
 *
 * @param dev PTPC timestamp channel.
 * @param config Compare target and enable state.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid configuration.
 */
static inline int hpm_ptpc_configure_compare(const struct device *dev,
					     const struct hpm_ptpc_compare_config *config)
{
	return DEVICE_API_GET(hpm_ptpc, dev)->configure_compare(dev, config);
}

/**
 * @brief Configure the PPS output frequency.
 *
 * @param dev PTPC timestamp channel.
 * @param frequency_exponent Zero disables PPS, otherwise the output is 2^exponent Hz.
 *
 * @retval 0 on success.
 * @retval -EINVAL if the hardware does not support the requested exponent.
 */
static inline int hpm_ptpc_configure_pps(const struct device *dev, uint8_t frequency_exponent)
{
	return DEVICE_API_GET(hpm_ptpc, dev)->configure_pps(dev, frequency_exponent);
}

/**
 * @brief Select which PTPC counter feeds a CAN timestamp output port.
 *
 * The selection is global to the PTPC block and applies to all MCAN instances
 * using the same CAN timestamp port.
 *
 * @param dev PTPC timestamp channel.
 * @param can_port CAN timestamp output port, from 0 through HPM_PTPC_CAN_PORT_COUNT - 1.
 * @param source PTPC counter to route to the port.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid port or source.
 */
static inline int hpm_ptpc_configure_can_time_source(const struct device *dev, uint8_t can_port,
						     enum hpm_ptpc_can_time_source source)
{
	return DEVICE_API_GET(hpm_ptpc, dev)->configure_can_time_source(dev, can_port, source);
}

/**
 * @brief Read the PTPC counter selected for a CAN timestamp output port.
 *
 * @param dev PTPC timestamp channel.
 * @param can_port CAN timestamp output port.
 * @param source Output selected PTPC counter.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid port or null output pointer.
 */
static inline int hpm_ptpc_get_can_time_source(const struct device *dev, uint8_t can_port,
					       enum hpm_ptpc_can_time_source *source)
{
	return DEVICE_API_GET(hpm_ptpc, dev)->get_can_time_source(dev, can_port, source);
}

/**
 * @brief Assign PTPC0 or PTPC1 to one MCAN TSU input.
 *
 * The same TSU input selector is shared by the four MCAN instances; changing it
 * affects every MCAN that selects the corresponding input.
 *
 * @param dev PTPC timestamp channel.
 * @param tsu_input MCAN TSU input selector, from 0 through HPM_PTPC_TSU_INPUT_COUNT - 1.
 * @param source PTPC counter to expose on the selector.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid selector or source.
 */
static inline int hpm_ptpc_configure_tsu_input(const struct device *dev, uint8_t tsu_input,
					       enum hpm_ptpc_tsu_input_source source)
{
	return DEVICE_API_GET(hpm_ptpc, dev)->configure_tsu_input(dev, tsu_input, source);
}

/**
 * @brief Read the source assigned to one MCAN TSU input selector.
 *
 * @param dev PTPC timestamp channel.
 * @param tsu_input MCAN TSU input selector.
 * @param source Output source selection.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid selector or null output pointer.
 */
static inline int hpm_ptpc_get_tsu_input(const struct device *dev, uint8_t tsu_input,
					 enum hpm_ptpc_tsu_input_source *source)
{
	return DEVICE_API_GET(hpm_ptpc, dev)->get_tsu_input(dev, tsu_input, source);
}

/**
 * @brief Read pending event flags.
 *
 * Interrupt-latched flags are returned once. Raw hardware flags remain visible
 * until cleared with hpm_ptpc_clear_events().
 *
 * @param dev PTPC timestamp channel.
 * @param events Output combination of HPM_PTPC_EVENT_* flags.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p events is NULL.
 */
static inline int hpm_ptpc_get_events(const struct device *dev, uint32_t *events)
{
	return DEVICE_API_GET(hpm_ptpc, dev)->get_events(dev, events);
}

/**
 * @brief Clear event flags.
 *
 * @param dev PTPC timestamp channel.
 * @param events Combination of HPM_PTPC_EVENT_* flags to clear.
 *
 * @retval 0 on success.
 * @retval -EINVAL for unknown event bits.
 */
static inline int hpm_ptpc_clear_events(const struct device *dev, uint32_t events)
{
	return DEVICE_API_GET(hpm_ptpc, dev)->clear_events(dev, events);
}

/**
 * @brief Enable event interrupts and install a callback.
 *
 * Passing a NULL callback or a zero mask disables interrupts for this channel.
 * The callback runs in interrupt context.
 *
 * @param dev PTPC timestamp channel.
 * @param event_mask Combination of HPM_PTPC_EVENT_* flags.
 * @param callback Callback to invoke, or NULL to disable.
 * @param user_data Callback context.
 *
 * @retval 0 on success.
 * @retval -EINVAL for unknown event bits.
 */
static inline int hpm_ptpc_set_event_callback(const struct device *dev, uint32_t event_mask,
					      hpm_ptpc_event_callback_t callback, void *user_data)
{
	return DEVICE_API_GET(hpm_ptpc, dev)
		->set_event_callback(dev, event_mask, callback, user_data);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_PTP_CLOCK_HPMICRO_PTPC_H_ */
