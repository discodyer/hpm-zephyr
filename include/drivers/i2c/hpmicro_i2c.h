/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_I2C_HPMICRO_I2C_H_
#define ZEPHYR_INCLUDE_DRIVERS_I2C_HPMICRO_I2C_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro I2C extensions used by the SMBus transport
 * @defgroup hpm_i2c_interface HPMicro I2C
 * @ingroup i2c_interface
 * @{
 */

/** Maximum data bytes in an SMBus block transfer. */
#define HPM_I2C_SMBUS_BLOCK_BYTES_MAX 32U

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_i2c_driver_api {
	/** Must remain first so the device also implements the I2C API. */
	struct i2c_driver_api i2c_api;
	int (*smbus_quick)(const struct device *dev, uint16_t addr, bool read);
	int (*smbus_block_transfer)(const struct device *dev, uint16_t addr, const uint8_t *tx_buf,
				    uint8_t tx_len, uint8_t *count, uint8_t *rx_buf, uint8_t *pec);
};

DEVICE_API_EXTENDS(hpm_i2c, i2c, i2c_api);
/** @endcond */

/**
 * @brief Issue an SMBus Quick command.
 *
 * The transaction contains START, the target address with R/W, and STOP but
 * no data phase.
 *
 * @param dev HPMicro I2C controller.
 * @param addr 7-bit target address.
 * @param read True for a Quick Read, false for a Quick Write.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid address.
 * @retval -EBUSY if the controller is operating as an I2C target.
 */
static inline int hpm_i2c_smbus_quick(const struct device *dev, uint16_t addr, bool read)
{
	return DEVICE_API_GET(hpm_i2c, dev)->smbus_quick(dev, addr, read);
}

/**
 * @brief Execute an SMBus block read with a dynamically sized data phase.
 *
 * Sends @p tx_buf with START and no STOP, then issues a repeated START and
 * reads the block count. The data and optional PEC byte are read in the same
 * transaction. This API exists because the data length is only known after the
 * count byte has been received.
 *
 * @param dev HPMicro I2C controller.
 * @param addr 7-bit target address.
 * @param tx_buf Command and optional write payload.
 * @param tx_len Number of bytes in @p tx_buf.
 * @param count Output block count.
 * @param rx_buf Buffer for up to HPM_I2C_SMBUS_BLOCK_BYTES_MAX data bytes.
 * @param pec Optional output PEC byte; pass NULL when PEC is disabled.
 *
 * @retval 0 on success.
 * @retval -EINVAL for invalid arguments.
 * @retval -ENODATA if the target reports more than 32 data bytes.
 * @retval -EBUSY if the controller is operating as an I2C target.
 */
static inline int hpm_i2c_smbus_block_transfer(const struct device *dev, uint16_t addr,
					       const uint8_t *tx_buf, uint8_t tx_len,
					       uint8_t *count, uint8_t *rx_buf, uint8_t *pec)
{
	return DEVICE_API_GET(hpm_i2c, dev)
		->smbus_block_transfer(dev, addr, tx_buf, tx_len, count, rx_buf, pec);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_I2C_HPMICRO_I2C_H_ */
