/*
 * Copyright (c) 2022-2026 HPMicro
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HPM_ZEPHYR_SOC_HPM5300_PINCTRL_SOC_H_
#define HPM_ZEPHYR_SOC_HPM5300_PINCTRL_SOC_H_

#include <zephyr/types.h>
#include <dt-bindings/pinctrl/hpmicro-pinctrl-common.h>

typedef struct {
	uint32_t pinmux;
	uint32_t pincfg;
} pinctrl_soc_pin_t;

#define Z_PINCTRL_STATE_PIN_INIT(node_id, prop, idx)			\
	{								\
		.pinmux = DT_PROP_BY_IDX(node_id, prop, idx),		\
		.pincfg = 0,						\
	},

#define Z_PINCTRL_STATE_PINS_INIT(node_id, prop)			\
	{								\
		DT_FOREACH_PROP_ELEM(node_id, prop, Z_PINCTRL_STATE_PIN_INIT) \
	}

#endif /* HPM_ZEPHYR_SOC_HPM5300_PINCTRL_SOC_H_ */
