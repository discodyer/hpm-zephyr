/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * Copyright (c) 2026 Cody Gu <gujiaqi@iscas.ac.cn>
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_power_domain

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

#include <hpm_sysctl_drv.h>

LOG_MODULE_REGISTER(power_domain_hpmicro, CONFIG_POWER_DOMAIN_LOG_LEVEL);

#define HPM_PD_WAIT_STEP_US 10U

struct hpm_pd_config {
	SYSCTL_Type *base;
	sysctl_resource_t resource;
};

static bool hpm_pd_resource_is_allowed(sysctl_resource_t resource)
{
	/*
	 * HPM5300 manual chapter 13: functional-module resources may be forced
	 * off and re-enabled. Keep boot, reset, clock, memory, watchdog, key
	 * manager and XPI nodes out of the runtime device-domain model.
	 */
	if ((resource < sysctl_resource_can0) || (resource > sysctl_resource_usb0)) {
		return false;
	}

	switch (resource) {
	case sysctl_resource_wdg0:
	case sysctl_resource_wdg1:
	case sysctl_resource_kman:
	case sysctl_resource_xpi0:
		return false;
	default:
		return true;
	}
}

static int hpm_pd_wait_ready(const struct hpm_pd_config *config)
{
	uint32_t waited_us = 0U;

	while (sysctl_resource_target_is_busy(config->base, config->resource)) {
		if (waited_us >= CONFIG_POWER_DOMAIN_HPMICRO_TIMEOUT_US) {
			return -ETIMEDOUT;
		}

		k_busy_wait(HPM_PD_WAIT_STEP_US);
		waited_us += HPM_PD_WAIT_STEP_US;
	}

	return 0;
}

static int hpm_pd_set_resource_mode(const struct hpm_pd_config *config, sysctl_resource_mode_t mode)
{
	int ret;

	sysctl_resource_target_set_mode(config->base, config->resource, mode);
	ret = hpm_pd_wait_ready(config);
	if (ret < 0) {
		LOG_ERR("resource %u did not finish mode %u transition (%d)",
			(unsigned int)config->resource, (unsigned int)mode, ret);
		return ret;
	}

	if (sysctl_resource_target_get_mode(config->base, config->resource) != mode) {
		LOG_ERR("resource %u mode readback mismatch", (unsigned int)config->resource);
		return -EIO;
	}

	return 0;
}

static int hpm_pd_pm_action(const struct device *dev, enum pm_device_action action)
{
	const struct hpm_pd_config *config = dev->config;
	int ret;

	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		ret = hpm_pd_set_resource_mode(config, sysctl_resource_mode_force_on);
		if (ret < 0) {
			return ret;
		}

		pm_device_children_action_run(dev, PM_DEVICE_ACTION_TURN_ON, NULL);
		return 0;
	case PM_DEVICE_ACTION_SUSPEND:
		pm_device_children_action_run(dev, PM_DEVICE_ACTION_TURN_OFF, NULL);
		return hpm_pd_set_resource_mode(config, sysctl_resource_mode_force_off);
	case PM_DEVICE_ACTION_TURN_ON:
	case PM_DEVICE_ACTION_TURN_OFF:
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int hpm_pd_init(const struct device *dev)
{
	const struct hpm_pd_config *config = dev->config;

	if (!hpm_pd_resource_is_allowed(config->resource)) {
		LOG_ERR("resource %u is not safe for runtime power gating",
			(unsigned int)config->resource);
		return -EINVAL;
	}

	return pm_device_driver_init(dev, hpm_pd_pm_action);
}

#define HPM_PD_INIT(n)                                                                             \
	static const struct hpm_pd_config hpm_pd_config_##n = {                                    \
		.base = (SYSCTL_Type *)DT_REG_ADDR(DT_INST_PHANDLE(n, hpmicro_controller)),        \
		.resource = (sysctl_resource_t)DT_INST_PROP(n, hpmicro_resource_id),               \
	};                                                                                         \
                                                                                                   \
	PM_DEVICE_DT_INST_DEFINE(n, hpm_pd_pm_action);                                             \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_pd_init, PM_DEVICE_DT_INST_GET(n), NULL, &hpm_pd_config_##n,  \
			      POST_KERNEL, CONFIG_POWER_DOMAIN_HPMICRO_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(HPM_PD_INIT)
