/*
 * Copyright (c) 2022-2026 HPMicro
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HPM_ZEPHYR_SOC_HPM5300_PINCTRL_SOC_H_
#define HPM_ZEPHYR_SOC_HPM5300_PINCTRL_SOC_H_

#include <zephyr/devicetree.h>
#include <zephyr/types.h>
#include <dt-bindings/pinctrl/hpmicro-pinctrl-common.h>

typedef struct {
	uint32_t pinmux;
	uint32_t pincfg;
} pinctrl_soc_pin_t;

#define Z_PINCTRL_HPMICRO_PINCFG_INIT(node_id)                                                     \
	((HPMICRO_OPEN_DRAIN << HPMICRO_OPEN_DRAIN_SHIFT) * DT_PROP(node_id, drive_open_drain) |   \
	 (HPMICRO_NO_PULL << HPMICRO_NO_PULL_SHIFT) * DT_PROP(node_id, bias_disable) |             \
	 (HPMICRO_PULL_DOWN << HPMICRO_PULL_DOWN_SHIFT) * DT_PROP(node_id, bias_pull_down) |       \
	 (HPMICRO_PULL_UP << HPMICRO_PULL_UP_SHIFT) * DT_PROP(node_id, bias_pull_up) |             \
	 (HPMICRO_FORCE_INPUT << HPMICRO_FORCE_INPUT_SHIFT) * DT_PROP(node_id, input_enable) |     \
	 (HPMICRO_DRIVER_STRENGTH << HPMICRO_DRIVER_STRENGTH_SHIFT) *                              \
		 DT_ENUM_IDX(node_id, drive_strength) |                                            \
	 (HPMICRO_SLEW_PD << HPMICRO_SLEW_PD_SHIFT) * DT_ENUM_IDX(node_id, hpmicro_slew_pd) |      \
	 (HPMICRO_PULL_STRENGTH << HPMICRO_PULL_STRENGTH_SHIFT) *                                  \
		 DT_ENUM_IDX(node_id, pull_resistance_strength) |                                  \
	 (HPMICRO_SCHMITT_ENABLE << HPMICRO_SCHMITT_ENABLE_SHIFT) *                                \
		 DT_PROP(node_id, input_schmitt_enable) |                                          \
	 (HPMICRO_POWER << HPMICRO_POWER_SHIFT) * DT_ENUM_IDX(node_id, power_source) |             \
	 (HPMICRO_SLEW_RATE << HPMICRO_SLEW_RATE_SHIFT) *                                          \
		 DT_ENUM_IDX(node_id, hpmicro_slew_rate) |                                         \
	 (HPMICRO_KEEPER_DISABLE << HPMICRO_KEEPER_DISABLE_SHIFT) *                                \
		 DT_PROP(node_id, keeper_capability_disable))

#define Z_PINCTRL_STATE_PIN_INIT(node_id, prop, idx)                                               \
	{                                                                                          \
		.pinmux = DT_PROP_BY_IDX(node_id, prop, idx),                                      \
		.pincfg = Z_PINCTRL_HPMICRO_PINCFG_INIT(node_id),                                  \
	},

#define Z_PINCTRL_STATE_PINS_INIT(node_id, prop)                                                   \
	{DT_FOREACH_CHILD_VARGS(DT_PHANDLE(node_id, prop), DT_FOREACH_PROP_ELEM, pinmux,           \
				Z_PINCTRL_STATE_PIN_INIT)}

#endif /* HPM_ZEPHYR_SOC_HPM5300_PINCTRL_SOC_H_ */
