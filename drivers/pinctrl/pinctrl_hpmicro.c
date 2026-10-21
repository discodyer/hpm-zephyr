/*
 * Copyright (c) 2022-2026 HPMicro
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_pinctrl

#include <zephyr/drivers/pinctrl.h>
#include <soc.h>
#include <hpm_common.h>
#include <hpm_soc.h>
#include <drivers/pinctrl/pinctrl_hpmicro_common.h>

static int hpmicro_pin_configure(IOC_Type *ioc_base, uint32_t pin_mux)
{
	uint32_t ioc_pad = HPMICRO_PAD_NUM(pin_mux);
	uint32_t is_analog = HPMICRO_FUNC_ANALOG(pin_mux);
	uint32_t alt_select = HPMICRO_FUNC_ALT_SELECT(pin_mux);

	if (ioc_base == NULL) {
		return -ENODEV;
	}

	ioc_base->PAD[ioc_pad].FUNC_CTL =
		IOC_PAD_FUNC_CTL_ANALOG_SET(is_analog) |
		IOC_PAD_FUNC_CTL_ALT_SELECT_SET(alt_select);

	return 0;
}

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt,
			   uintptr_t reg)
{
	ARG_UNUSED(reg);

	for (uint8_t i = 0; i < pin_cnt; i++) {
		uint32_t ioc = HPMICRO_FUNC_IOC_SELECT(pins[i].pinmux);
		IOC_Type *base = NULL;
		int ret;

		switch (ioc) {
		case IOC_TYPE_IOC:
			base = (IOC_Type *)DT_REG_ADDR(DT_NODELABEL(pinctrl));
			break;
#if DT_NODE_HAS_STATUS(DT_NODELABEL(pinctrl_pioc), okay)
		case IOC_TYPE_PIOC:
			base = (IOC_Type *)DT_REG_ADDR(DT_NODELABEL(pinctrl_pioc));
			break;
#endif
		default:
			return -EINVAL;
		}

		ret = hpmicro_pin_configure(base, pins[i].pinmux);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}
