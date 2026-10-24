/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <hpm_clock_drv.h>
#include <hpm_l1c_drv.h>
#include <hpm_pcfg_drv.h>
#include <hpm_pllctlv2_drv.h>
#include <hpm_soc.h>
#include <hpm_sysctl_drv.h>

#if defined(CONFIG_RISCV_CUSTOM_CSR_ANDES_PMA)
#include <pma.h>
#endif

void soc_early_init_hook(void)
{
#if defined(CONFIG_RISCV_CUSTOM_CSR_ANDES_PMA)
	pma_init();
#endif
}

void soc_per_core_init_hook(void)
{
#if defined(CONFIG_RISCV_CUSTOM_CSR_ANDES_PMA)
	pma_init_per_core();
#endif
}

static int hpm5300_clock_init(void)
{
	uint32_t cpu0_freq = clock_get_frequency(clock_cpu0);
	hpm_stat_t status;

	if (cpu0_freq == PLLCTL_SOC_PLL_REFCLK_FREQ) {
		pllctlv2_xtal_set_rampup_time(HPM_PLLCTLV2, 32UL * 1000UL * 9U);
		sysctl_clock_set_preset(HPM_SYSCTL, 2);
	}

	clock_add_to_group(clock_cpu0, 0);
	clock_add_to_group(clock_ahb, 0);
	clock_add_to_group(clock_lmm0, 0);
	clock_add_to_group(clock_mchtmr0, 0);
	clock_add_to_group(clock_rom, 0);
	clock_add_to_group(clock_mot0, 0);
	clock_add_to_group(clock_gpio, 0);
	clock_add_to_group(clock_hdma, 0);
	clock_add_to_group(clock_xpi0, 0);
	clock_add_to_group(clock_ptpc, 0);
	clock_connect_group_to_cpu(0, 0);

	status = pcfg_dcdc_set_voltage(HPM_PCFG, 1275);
	if (status != status_success) {
		return -EIO;
	}

	/* Run CPU0 at 480 MHz and AHB at 160 MHz. */
	status = sysctl_config_cpu0_domain_clock(HPM_SYSCTL,
						 clock_source_pll0_clk0, 2, 3);
	if (status != status_success) {
		return -EIO;
	}

	/* PLL0 outputs: 960 MHz, 600 MHz and 400 MHz. */
	pllctlv2_set_postdiv(HPM_PLLCTLV2, pllctlv2_pll0, pllctlv2_clk0,
			     pllctlv2_div_1p0);
	pllctlv2_set_postdiv(HPM_PLLCTLV2, pllctlv2_pll0, pllctlv2_clk1,
			     pllctlv2_div_1p6);
	pllctlv2_set_postdiv(HPM_PLLCTLV2, pllctlv2_pll0, pllctlv2_clk2,
			     pllctlv2_div_2p4);
	status = pllctlv2_init_pll_with_freq(HPM_PLLCTLV2, pllctlv2_pll0,
					     960000000);
	if (status != status_success) {
		return -EIO;
	}

	status = clock_set_source_divider(clock_mchtmr0, clk_src_osc24m, 1);
	if (status != status_success) {
		return -EIO;
	}

	clock_update_core_clock();
	sysctl_set_cpu_lp_mode(HPM_SYSCTL, HPM_CORE0,
			      cpu_lp_mode_ungate_cpu_clock);

	return 0;
}

static void hpm5300_cache_init(void)
{
	if (IS_ENABLED(CONFIG_ICACHE)) {
		l1c_ic_enable();
	}

	if (IS_ENABLED(CONFIG_DCACHE)) {
		l1c_dc_enable();
		l1c_dc_invalidate_all();
	}

	l1c_fence_i();
}

static int hpm5300_soc_init(void)
{
	int ret;

	ret = hpm5300_clock_init();
	if (ret < 0) {
		return ret;
	}

	hpm5300_cache_init();

	return 0;
}

SYS_INIT(hpm5300_soc_init, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
