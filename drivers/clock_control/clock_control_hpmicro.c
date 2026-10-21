/*
 * Copyright (c) 2024-2026 HPMicro
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_pllv2

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <hpm_clock_drv.h>

static int hpm_clock_control_on(const struct device *dev, clock_control_subsys_t sys)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sys);
	return 0;
}

static int hpm_clock_control_off(const struct device *dev, clock_control_subsys_t sys)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sys);
	return 0;
}

static int hpm_clock_control_get_rate(const struct device *dev,
				      clock_control_subsys_t sys,
				      uint32_t *rate)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sys);
	*rate = clock_get_frequency(clock_cpu0);
	return 0;
}

static const struct clock_control_driver_api hpm_clock_control_api = {
	.on = hpm_clock_control_on,
	.off = hpm_clock_control_off,
	.get_rate = hpm_clock_control_get_rate,
};

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, PRE_KERNEL_1,
		      CONFIG_CLOCK_CONTROL_INIT_PRIORITY,
		      &hpm_clock_control_api);
