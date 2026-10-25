/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_SERIAL_HPMICRO_UART_H_
#define ZEPHYR_INCLUDE_DRIVERS_SERIAL_HPMICRO_UART_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro UART address-match and nine-bit extensions
 * @defgroup hpm_uart_interface HPMicro UART
 * @ingroup uart_interface
 * @{
 */

/** Address-match channel zero. */
#define HPM_UART_ADDRESS_MATCH_0     0U
/** Address-match channel one. */
#define HPM_UART_ADDRESS_MATCH_1     1U
/** Number of hardware address-match channels. */
#define HPM_UART_ADDRESS_MATCH_COUNT 2U

/** Configuration for address-match operation. */
struct hpm_uart_address_match_config {
	/** Enable address-match filtering. When false, all both address fields are ignored. */
	bool enable;
	/** Address zero comparison value. */
	uint8_t address0;
	/** Enable address zero comparison. */
	bool address0_enable;
	/** Address one comparison value. */
	uint8_t address1;
	/** Enable address one comparison. */
	bool address1_enable;
	/** Nine-bit receive mode. Valid only together with address matching. */
	bool nine_bit;
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_uart_driver_api {
	/** Must remain first so the device also implements the UART API. */
	struct uart_driver_api uart_api;
	int (*configure_address_match)(const struct device *dev,
				       const struct hpm_uart_address_match_config *config);
	int (*get_address_match)(const struct device *dev, bool *matched);
	int (*get_address_match_idle)(const struct device *dev, bool *matched);
};

DEVICE_API_EXTENDS(hpm_uart, uart, uart_api);
/** @endcond */

/**
 * @brief Configure HPMicro UART address matching.
 *
 * HPM UART address matching assigns the MSB of the first character as the
 * address/data flag. This is not the same as a nine-bit data frame in the
 * generic Zephyr UART configuration, so this operation is exposed as an
 * HPM-specific extension. The UART must be configured for 8 data bits with no
 * parity before enabling nine-bit mode.
 *
 * @param dev UART device.
 * @param config Address-match configuration. The address fields are seven-bit
 *               values when @ref hpm_uart_address_match_config.nine_bit is
 *               false; the driver supplies the address-flag MSB. In nine-bit
 *               mode the fields are full eight-bit address values.
 *
 * @retval 0 on success.
 * @retval -ENODEV for an invalid device.
 * @retval -ENOTSUP if the SoC or runtime UART configuration does not support it.
 * @retval -EINVAL for invalid arguments.
 */
static inline int
hpm_uart_configure_address_match(const struct device *dev,
				 const struct hpm_uart_address_match_config *config)
{
	return DEVICE_API_GET(hpm_uart, dev)->configure_address_match(dev, config);
}

/**
 * @brief Read and clear the address-match event flag.
 *
 * @param dev UART device.
 * @param matched Set to true when the last address character matched.
 *
 * @retval 0 on success.
 * @retval -ENOTSUP if the SoC does not support address matching.
 */
static inline int hpm_uart_get_address_match(const struct device *dev, bool *matched)
{
	return DEVICE_API_GET(hpm_uart, dev)->get_address_match(dev, matched);
}

/**
 * @brief Read and clear the address-match-and-idle event flag.
 *
 * @param dev UART device.
 * @param matched Set to true when a matching address was followed by an idle
 *                period and the complete data frame is available.
 *
 * @retval 0 on success.
 * @retval -ENOTSUP if the SoC does not support address matching.
 */
static inline int hpm_uart_get_address_match_idle(const struct device *dev, bool *matched)
{
	return DEVICE_API_GET(hpm_uart, dev)->get_address_match_idle(dev, matched);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_SERIAL_HPMICRO_UART_H_ */
