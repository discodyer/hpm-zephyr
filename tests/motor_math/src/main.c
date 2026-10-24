/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>

#include <drivers/motor/hpmicro_rdc.h>
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

ZTEST_SUITE(hpm_motor_math, NULL, NULL, NULL, NULL, NULL);
