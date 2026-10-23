/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_GPIOM_HPMICRO_GPIOM_H_
#define ZEPHYR_INCLUDE_DRIVERS_GPIOM_HPMICRO_GPIOM_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HPMicro GPIO manager interface
 * @defgroup hpm_gpiom_interface HPMicro GPIO manager
 * @ingroup io_interfaces
 * @{
 */

/** GPIO port indices used by the HPM5361 GPIO manager. */
enum hpm_gpiom_port {
	HPM_GPIOM_PORT_GPIOA = 0,
	HPM_GPIOM_PORT_GPIOB = 1,
	HPM_GPIOM_PORT_GPIOX = 13,
	HPM_GPIOM_PORT_GPIOY = 14,
};

/** GPIO controllers that can own an IO. */
enum hpm_gpiom_controller {
	HPM_GPIOM_CONTROLLER_GPIO0 = 0,
	HPM_GPIOM_CONTROLLER_FGPIO0 = 2,
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct hpm_gpiom_driver_api {
	int (*set_controller)(const struct device *dev, enum hpm_gpiom_port port, uint8_t pin,
			      enum hpm_gpiom_controller controller);
	int (*get_controller)(const struct device *dev, enum hpm_gpiom_port port, uint8_t pin,
			      enum hpm_gpiom_controller *controller);
	int (*set_visibility)(const struct device *dev, enum hpm_gpiom_port port, uint8_t pin,
			      enum hpm_gpiom_controller controller, bool visible);
	int (*get_visibility)(const struct device *dev, enum hpm_gpiom_port port, uint8_t pin,
			      enum hpm_gpiom_controller controller, bool *visible);
	int (*lock)(const struct device *dev, enum hpm_gpiom_port port, uint8_t pin);
	int (*is_locked)(const struct device *dev, enum hpm_gpiom_port port, uint8_t pin,
			 bool *locked);
};
/** @endcond */

/**
 * @brief Assign an IO to a GPIO controller.
 *
 * The configuration is shared by all tasks and cores. Once locked with
 * hpm_gpiom_lock(), it cannot be changed until the SoC is reset.
 *
 * @param dev GPIO manager device.
 * @param port GPIO port index.
 * @param pin Pin index within the port.
 * @param controller GPIO controller that owns the pin.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid port, pin or controller.
 * @retval -EACCES if the pin configuration is locked.
 */
static inline int hpm_gpiom_set_controller(const struct device *dev, enum hpm_gpiom_port port,
					   uint8_t pin, enum hpm_gpiom_controller controller)
{
	return DEVICE_API_GET(hpm_gpiom, dev)->set_controller(dev, port, pin, controller);
}

/**
 * @brief Read the GPIO controller assigned to an IO.
 *
 * @param dev GPIO manager device.
 * @param port GPIO port index.
 * @param pin Pin index within the port.
 * @param controller Output for the owning GPIO controller.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid port or pin.
 */
static inline int hpm_gpiom_get_controller(const struct device *dev, enum hpm_gpiom_port port,
					   uint8_t pin, enum hpm_gpiom_controller *controller)
{
	return DEVICE_API_GET(hpm_gpiom, dev)->get_controller(dev, port, pin, controller);
}

/**
 * @brief Control whether an IO input is visible to a GPIO controller.
 *
 * Visibility controls only the input path. Output ownership is selected by
 * hpm_gpiom_set_controller().
 *
 * @param dev GPIO manager device.
 * @param port GPIO port index.
 * @param pin Pin index within the port.
 * @param controller GPIO controller whose input visibility is changed.
 * @param visible True to make the input visible, false to hide it.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid port, pin or controller.
 * @retval -EACCES if the pin configuration is locked.
 */
static inline int hpm_gpiom_set_visibility(const struct device *dev, enum hpm_gpiom_port port,
					   uint8_t pin, enum hpm_gpiom_controller controller,
					   bool visible)
{
	return DEVICE_API_GET(hpm_gpiom, dev)->set_visibility(dev, port, pin, controller, visible);
}

/**
 * @brief Read whether an IO input is visible to a GPIO controller.
 *
 * @param dev GPIO manager device.
 * @param port GPIO port index.
 * @param pin Pin index within the port.
 * @param controller GPIO controller to query.
 * @param visible Output for the input visibility.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid port, pin or controller.
 */
static inline int hpm_gpiom_get_visibility(const struct device *dev, enum hpm_gpiom_port port,
					   uint8_t pin, enum hpm_gpiom_controller controller,
					   bool *visible)
{
	return DEVICE_API_GET(hpm_gpiom, dev)->get_visibility(dev, port, pin, controller, visible);
}

/**
 * @brief Permanently lock one IO assignment until reset.
 *
 * @param dev GPIO manager device.
 * @param port GPIO port index.
 * @param pin Pin index within the port.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid port or pin.
 * @retval -EACCES if the pin configuration was already locked.
 */
static inline int hpm_gpiom_lock(const struct device *dev, enum hpm_gpiom_port port, uint8_t pin)
{
	return DEVICE_API_GET(hpm_gpiom, dev)->lock(dev, port, pin);
}

/**
 * @brief Query whether an IO assignment is locked.
 *
 * @param dev GPIO manager device.
 * @param port GPIO port index.
 * @param pin Pin index within the port.
 * @param locked Output for the lock state.
 *
 * @retval 0 on success.
 * @retval -EINVAL for an invalid port or pin.
 */
static inline int hpm_gpiom_is_locked(const struct device *dev, enum hpm_gpiom_port port,
				      uint8_t pin, bool *locked)
{
	return DEVICE_API_GET(hpm_gpiom, dev)->is_locked(dev, port, pin, locked);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_GPIOM_HPMICRO_GPIOM_H_ */
