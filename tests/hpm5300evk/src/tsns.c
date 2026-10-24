/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/ztest.h>

#if DT_NODE_HAS_STATUS(DT_NODELABEL(tsns0), okay)
static void tsns_trigger_handler(const struct device *dev, const struct sensor_trigger *trigger)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(trigger);
}

static struct sensor_trigger tsns_trigger = {
	.type = SENSOR_TRIG_THRESHOLD,
	.chan = SENSOR_CHAN_DIE_TEMP,
};

ZTEST(hpm_tsns, test_threshold_api)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(tsns0));
	struct sensor_value lower = {.val1 = 20, .val2 = 500000};
	struct sensor_value upper = {.val1 = 80, .val2 = 0};
	struct sensor_value value;

	zassert_ok(sensor_attr_set(dev, SENSOR_CHAN_DIE_TEMP, SENSOR_ATTR_LOWER_THRESH, &lower));
	zassert_ok(sensor_attr_set(dev, SENSOR_CHAN_DIE_TEMP, SENSOR_ATTR_UPPER_THRESH, &upper));
	zassert_ok(sensor_attr_get(dev, SENSOR_CHAN_DIE_TEMP, SENSOR_ATTR_LOWER_THRESH, &value));
	zassert_equal(value.val1, lower.val1);
	zassert_equal(value.val2, lower.val2);
	zassert_ok(sensor_trigger_set(dev, &tsns_trigger, tsns_trigger_handler));
	zassert_ok(sensor_trigger_set(dev, &tsns_trigger, NULL));
}
#endif

ZTEST_SUITE(hpm_tsns, NULL, NULL, NULL, NULL, NULL);
