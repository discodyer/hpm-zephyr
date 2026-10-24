/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>

#include <drivers/motor/hpmicro_rdc.h>
#include <drivers/motor/hpmicro_sei_bissc.h>
#include <drivers/motor/hpmicro_sei_endat.h>
#include <drivers/motor/hpmicro_sei_hiperface.h>
#include <drivers/motor/hpmicro_sei_ssi.h>
#include <drivers/motor/hpmicro_sei_tamagawa.h>
#include <zephyr/ztest.h>

#define ANGLE_TOLERANCE 5U

ZTEST(hpm_motor_math, test_rdc_angle_axes)
{
	uint32_t angle;

	zassert_ok(hpm_rdc_angle_from_iq(1, 0, &angle));
	zassert_equal(angle, 32768U);
	zassert_ok(hpm_rdc_angle_from_iq(0, 1, &angle));
	zassert_equal(angle, 16384U);
	zassert_ok(hpm_rdc_angle_from_iq(-1, 0, &angle));
	zassert_equal(angle, 0U);
	zassert_ok(hpm_rdc_angle_from_iq(0, -1, &angle));
	zassert_equal(angle, 49152U);
}

ZTEST(hpm_motor_math, test_rdc_angle_quadrants)
{
	uint32_t angle;

	zassert_ok(hpm_rdc_angle_from_iq(1, 1, &angle));
	zassert_within(angle, 24576U, ANGLE_TOLERANCE);
	zassert_ok(hpm_rdc_angle_from_iq(1, -1, &angle));
	zassert_within(angle, 40960U, ANGLE_TOLERANCE);
	zassert_ok(hpm_rdc_angle_from_iq(-1, -1, &angle));
	zassert_within(angle, 57344U, ANGLE_TOLERANCE);
	zassert_ok(hpm_rdc_angle_from_iq(-1, 1, &angle));
	zassert_within(angle, 8192U, ANGLE_TOLERANCE);
}

ZTEST(hpm_motor_math, test_rdc_angle_limits_and_errors)
{
	uint32_t angle;

	zassert_ok(hpm_rdc_angle_from_iq(INT32_MAX, INT32_MAX, &angle));
	zassert_within(angle, 24576U, ANGLE_TOLERANCE);
	zassert_ok(hpm_rdc_angle_from_iq(INT32_MIN, INT32_MIN, &angle));
	zassert_within(angle, 57344U, ANGLE_TOLERANCE);
	zassert_ok(hpm_rdc_angle_from_iq(INT32_MIN, 0, &angle));
	zassert_equal(angle, 0U);
	zassert_ok(hpm_rdc_angle_from_iq(0, INT32_MIN, &angle));
	zassert_equal(angle, 49152U);
	zassert_equal(hpm_rdc_angle_from_iq(1, 1, NULL), -EINVAL);
	zassert_equal(hpm_rdc_angle_from_iq(0, 0, &angle), -ENODATA);
}

ZTEST(hpm_motor_math, test_ssi_period_conversion)
{
	uint32_t cycles;

	zassert_ok(hpm_sei_ssi_period_to_cycles(120000000U, 200000U, &cycles));
	zassert_equal(cycles, 24000000U);
	zassert_ok(hpm_sei_ssi_period_to_cycles(1000000U, 1U, &cycles));
	zassert_equal(cycles, 1U);
	zassert_ok(hpm_sei_ssi_period_to_cycles(1000001U, 1U, &cycles));
	zassert_equal(cycles, 1U);
	zassert_ok(hpm_sei_ssi_period_to_cycles(1500000U, 1U, &cycles));
	zassert_equal(cycles, 2U);
	zassert_equal(hpm_sei_ssi_period_to_cycles(0U, 1U, &cycles), -EINVAL);
	zassert_equal(hpm_sei_ssi_period_to_cycles(1U, 0U, &cycles), -EINVAL);
	zassert_equal(hpm_sei_ssi_period_to_cycles(1U, 1U, NULL), -EINVAL);
	zassert_equal(hpm_sei_ssi_period_to_cycles(UINT32_MAX, UINT32_MAX, &cycles), -ERANGE);
}

ZTEST(hpm_motor_math, test_ssi_baud_divider)
{
	uint32_t divider;

	zassert_ok(hpm_sei_ssi_baud_to_divider(120000000U, 1000000U, &divider));
	zassert_equal(divider, 120U);
	zassert_ok(hpm_sei_ssi_baud_to_divider(4000000U, 1000000U, &divider));
	zassert_equal(divider, 4U);
	zassert_ok(hpm_sei_ssi_baud_to_divider(65536U, 1U, &divider));
	zassert_equal(divider, 65536U);
	zassert_equal(hpm_sei_ssi_baud_to_divider(1000000U, 1000000U, &divider), -EINVAL);
	zassert_equal(hpm_sei_ssi_baud_to_divider(65537U, 1U, &divider), -EINVAL);
	zassert_equal(hpm_sei_ssi_baud_to_divider(0U, 1U, &divider), -EINVAL);
	zassert_equal(hpm_sei_ssi_baud_to_divider(1U, 0U, &divider), -EINVAL);
	zassert_equal(hpm_sei_ssi_baud_to_divider(1U, 1U, NULL), -EINVAL);
}

ZTEST(hpm_motor_math, test_tamagawa_watchdog_timing)
{
	zassert_ok(hpm_sei_tamagawa_check_timing(2500000U, 20000U, 0xfffeU));
	zassert_ok(hpm_sei_tamagawa_check_timing(2500000U, 30000U, 0U));
	zassert_equal(hpm_sei_tamagawa_check_timing(2500000U, 30000U, 0xfffeU), -EINVAL);
	zassert_equal(hpm_sei_tamagawa_check_timing(0U, 20000U, 0xfffeU), -EINVAL);
	zassert_equal(hpm_sei_tamagawa_check_timing(2500000U, 0U, 0xfffeU), -EINVAL);
}

ZTEST(hpm_motor_math, test_bissc_watchdog_timing)
{
	zassert_ok(hpm_sei_bissc_check_timing(1000000U, 20000U, 0xfffeU));
	zassert_ok(hpm_sei_bissc_check_timing(1000000U, 300000U, 0U));
	zassert_equal(hpm_sei_bissc_check_timing(1000000U, 300000U, 0xfffeU), -EINVAL);
	zassert_equal(hpm_sei_bissc_check_timing(0U, 200000U, 0xfffeU), -EINVAL);
	zassert_equal(hpm_sei_bissc_check_timing(1000000U, 0U, 0xfffeU), -EINVAL);
}

ZTEST(hpm_motor_math, test_endat_watchdog_timing)
{
	zassert_ok(hpm_sei_endat_check_timing(1000000U, 20000U, 0xfffeU));
	zassert_ok(hpm_sei_endat_check_timing(1000000U, 300000U, 0U));
	zassert_equal(hpm_sei_endat_check_timing(1000000U, 300000U, 0xfffeU), -EINVAL);
	zassert_equal(hpm_sei_endat_check_timing(0U, 200000U, 0xfffeU), -EINVAL);
	zassert_equal(hpm_sei_endat_check_timing(1000000U, 0U, 0xfffeU), -EINVAL);
}

ZTEST(hpm_motor_math, test_hiperface_watchdog_timing)
{
	zassert_ok(hpm_sei_hiperface_check_timing(9600U, 20000U, 0xfffeU));
	zassert_ok(hpm_sei_hiperface_check_timing(9600U, 300000U, 0U));
	zassert_equal(hpm_sei_hiperface_check_timing(9600U, 7000000U, 0xfffeU), -EINVAL);
	zassert_equal(hpm_sei_hiperface_check_timing(0U, 200000U, 0xfffeU), -EINVAL);
	zassert_equal(hpm_sei_hiperface_check_timing(9600U, 0U, 0xfffeU), -EINVAL);
}

ZTEST_SUITE(hpm_motor_math, NULL, NULL, NULL, NULL, NULL);
