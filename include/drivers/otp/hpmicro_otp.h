/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_OTP_HPMICRO_OTP_H_
#define ZEPHYR_INCLUDE_DRIVERS_OTP_HPMICRO_OTP_H_

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/otp.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro OTP extensions
 * @defgroup hpm_otp_interface HPMicro OTP
 * @ingroup otp_interface
 * @{
 */

#define HPM_OTP_WORD_COUNT 128U
#define HPM_OTP_WORD_SIZE  4U

/** OTP regions supported by the HPM5361 ROM interface. */
enum hpm_otp_region {
	HPM_OTP_REGION_0 = 0,
	HPM_OTP_REGION_1,
	HPM_OTP_REGION_2,
	HPM_OTP_REGION_3,
};

/** Irreversible OTP lock policy. */
enum hpm_otp_lock_option {
	HPM_OTP_NO_LOCK = 0,
	HPM_OTP_READ_ONLY,
	HPM_OTP_PERMANENT_NO_LOCK,
	HPM_OTP_DISABLE_ACCESS,
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_otp_driver_api {
	/** Must remain first so the device also implements the OTP API. */
	struct otp_driver_api otp_api;
	int (*reload)(const struct device *dev, enum hpm_otp_region region);
	int (*lock_otp)(const struct device *dev, uint32_t word_index,
			enum hpm_otp_lock_option option);
	int (*lock_shadow)(const struct device *dev, uint32_t word_index,
			   enum hpm_otp_lock_option option);
	int (*set_configurable_region)(const struct device *dev, uint32_t start,
				       uint32_t word_count);
};

DEVICE_API_EXTENDS(hpm_otp, otp, otp_api);
/** @endcond */

/**
 * @brief Reload an OTP region from fuses into its shadow registers.
 *
 * @param dev OTP device.
 * @param region OTP region to reload.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid region.
 * @retval -EIO if the ROM interface rejects the reload.
 */
static inline int hpm_otp_reload(const struct device *dev, enum hpm_otp_region region)
{
	return DEVICE_API_GET(hpm_otp, dev)->reload(dev, region);
}

/**
 * @brief Set the software lock policy for an OTP word.
 *
 * This operation is irreversible in hardware and is only available when
 * CONFIG_OTP_HPMICRO_ALLOW_LOCK is enabled.
 */
static inline int hpm_otp_lock(const struct device *dev, uint32_t word_index,
			       enum hpm_otp_lock_option option)
{
	const struct hpm_otp_driver_api *api = DEVICE_API_GET(hpm_otp, dev);

	if (api->lock_otp == NULL) {
		return -ENOTSUP;
	}

	return api->lock_otp(dev, word_index, option);
}

/**
 * @brief Set the software lock policy for an OTP shadow word.
 *
 * This operation is irreversible in hardware and is only available when
 * CONFIG_OTP_HPMICRO_ALLOW_LOCK is enabled.
 */
static inline int hpm_otp_lock_shadow(const struct device *dev, uint32_t word_index,
				      enum hpm_otp_lock_option option)
{
	const struct hpm_otp_driver_api *api = DEVICE_API_GET(hpm_otp, dev);

	if (api->lock_shadow == NULL) {
		return -ENOTSUP;
	}

	return api->lock_shadow(dev, word_index, option);
}

/**
 * @brief Define the configurable OTP region.
 *
 * This changes the range controlled by region 3 and is only available when
 * CONFIG_OTP_HPMICRO_ALLOW_REGION_CONFIG is enabled.
 */
static inline int hpm_otp_set_configurable_region(const struct device *dev, uint32_t start,
						  uint32_t word_count)
{
	const struct hpm_otp_driver_api *api = DEVICE_API_GET(hpm_otp, dev);

	if (api->set_configurable_region == NULL) {
		return -ENOTSUP;
	}

	return api->set_configurable_region(dev, start, word_count);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_OTP_HPMICRO_OTP_H_ */
