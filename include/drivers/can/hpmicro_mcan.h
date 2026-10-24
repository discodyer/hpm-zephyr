/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_CAN_HPMICRO_MCAN_H_
#define ZEPHYR_INCLUDE_DRIVERS_CAN_HPMICRO_MCAN_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro MCAN timestamping-unit extensions
 * @defgroup hpm_mcan_interface HPMicro MCAN
 * @ingroup can_interface
 * @{
 */

#define HPM_MCAN_TSU_INPUT_COUNT 4U
#define HPM_MCAN_TSU_SLOT_COUNT  16U

/** MCAN TSU timebase selection. */
enum hpm_mcan_tsu_timebase {
	/** Use the MCAN internal AHB-derived timebase. */
	HPM_MCAN_TSU_TIMEBASE_INTERNAL = 0,
	/** Use the external PTPC timebase selected by GLB_CTL and the TBSEL table. */
	HPM_MCAN_TSU_TIMEBASE_PTPC,
};

/** Timebase selection indices available at the MCAN GLB_CTL interface. */
enum hpm_mcan_tsu_timebase_input {
	HPM_MCAN_TSU_TIMEBASE_INPUT_0 = 0,
	HPM_MCAN_TSU_TIMEBASE_INPUT_1,
	HPM_MCAN_TSU_TIMEBASE_INPUT_2,
	HPM_MCAN_TSU_TIMEBASE_INPUT_3,
};

struct hpm_mcan_tsu_config {
	enum hpm_mcan_tsu_timebase timebase;
	/**
	 * TSU input selected when timebase is HPM_MCAN_TSU_TIMEBASE_PTPC.
	 * Configure the corresponding global PTPC TSU input route separately
	 * with hpm_ptpc_configure_tsu_input().
	 */
	enum hpm_mcan_tsu_timebase_input timebase_input;
	/** Capture at start-of-frame when true, otherwise at end-of-frame. */
	bool capture_on_sof;
	/** Use 64-bit TSU timestamps instead of 32-bit timestamps. */
	bool use_64bit;
};

struct hpm_mcan_tsu_snapshot {
	/** Slot index read from the TSU array. */
	uint8_t slot;
	bool timestamp_valid;
	bool timestamp_lost;
	/** True when the snapshot contains a 64-bit value. */
	bool is_64bit;
	union {
		uint32_t value32;
		uint64_t value64;
	};
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_mcan_driver_api {
	/** Must remain first so the device also implements the can API. */
	struct can_driver_api can_api;
	int (*configure_tsu)(const struct device *dev, const struct hpm_mcan_tsu_config *config);
	int (*get_tsu_snapshot)(const struct device *dev, uint8_t slot,
				struct hpm_mcan_tsu_snapshot *snapshot);
	int (*decode_rx_timestamp)(const struct device *dev, uint16_t encoded,
				   struct hpm_mcan_tsu_snapshot *snapshot);
	int (*set_rx_filter_timestamp)(const struct device *dev, int filter_id, bool enable);
	int (*get_rx_filter_timestamp)(const struct device *dev, int filter_id, bool *enabled);
};

DEVICE_API_EXTENDS(hpm_mcan, can, can_api);
/** @endcond */

/**
 * @brief Configure the MCAN timestamping unit.
 *
 * The CAN controller must be stopped. This configures the TSU timebase, capture
 * position and timestamp width. It does not alter the generic CAN bit timing or
 * the global PTPC_CAN_TS_SEL routing table. For a PTPC timebase, assign the
 * selected input with hpm_ptpc_configure_tsu_input() first.
 *
 * @param dev MCAN device.
 * @param config TSU configuration.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid configuration.
 * @retval -EBUSY if the CAN controller has been started.
 */
static inline int hpm_mcan_configure_tsu(const struct device *dev,
					 const struct hpm_mcan_tsu_config *config)
{
	return DEVICE_API_GET(hpm_mcan, dev)->configure_tsu(dev, config);
}

/**
 * @brief Read one captured TSU timestamp slot.
 *
 * Reading a slot clears its TSS1.TSN/TSL status bit in hardware. In 64-bit
 * mode, valid logical slots are 0 through 7 and map to two consecutive TS_SEL
 * registers.
 *
 * @param dev MCAN device.
 * @param slot TSU timestamp slot, from 0 through HPM_MCAN_TSU_SLOT_COUNT - 1.
 * @param snapshot Output snapshot.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid slot or null output pointer.
 */
static inline int hpm_mcan_get_tsu_snapshot(const struct device *dev, uint8_t slot,
					    struct hpm_mcan_tsu_snapshot *snapshot)
{
	return DEVICE_API_GET(hpm_mcan, dev)->get_tsu_snapshot(dev, slot, snapshot);
}

/**
 * @brief Decode the TSU pointer captured in a CAN receive timestamp field.
 *
 * The generic CAN frame timestamp field is only 16 bits. In TSU mode it carries
 * the TSC flag in bit 4 and the TSU slot in bits 3:0, not the timestamp value.
 * This helper resolves that slot into a timestamp snapshot.
 *
 * @param dev MCAN device.
 * @param encoded Raw 16-bit timestamp field from the received CAN frame.
 * @param snapshot Output timestamp snapshot.
 *
 * @retval 0 on success.
 * @retval -ENODATA if the frame did not capture a TSU timestamp.
 * @retval -EINVAL for a null output pointer.
 */
static inline int hpm_mcan_decode_rx_timestamp(const struct device *dev, uint16_t encoded,
					       struct hpm_mcan_tsu_snapshot *snapshot)
{
	return DEVICE_API_GET(hpm_mcan, dev)->decode_rx_timestamp(dev, encoded, snapshot);
}

/**
 * @brief Enable or disable TSU capture for an installed receive filter.
 *
 * This modifies the synchronization-message bit (SSYNC/ESYNC) of an RX filter
 * allocated by can_add_rx_filter(). It does not change filter matching.
 *
 * @param dev MCAN device.
 * @param filter_id Filter ID returned by can_add_rx_filter().
 * @param enable True to request a TSU capture on matching frames.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid filter ID.
 * @retval -EIO if the filter element cannot be read or written.
 */
static inline int hpm_mcan_set_rx_filter_timestamp(const struct device *dev, int filter_id,
						   bool enable)
{
	return DEVICE_API_GET(hpm_mcan, dev)->set_rx_filter_timestamp(dev, filter_id, enable);
}

/**
 * @brief Read whether an installed receive filter triggers TSU capture.
 *
 * @param dev MCAN device.
 * @param filter_id Filter ID returned by can_add_rx_filter().
 * @param enabled Output filter synchronization state.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid filter ID or null output pointer.
 * @retval -EIO if the filter element cannot be read.
 */
static inline int hpm_mcan_get_rx_filter_timestamp(const struct device *dev, int filter_id,
						   bool *enabled)
{
	return DEVICE_API_GET(hpm_mcan, dev)->get_rx_filter_timestamp(dev, filter_id, enabled);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_CAN_HPMICRO_MCAN_H_ */
