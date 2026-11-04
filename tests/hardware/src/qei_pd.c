/* SPDX-License-Identifier: Apache-2.0 */

#include <drivers/sensor/hpmicro_qeiv2.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#define COUNTS_PER_REVOLUTION 64U
#define COUNTS_PER_PULSE      2U
#define PULSE_WIDTH_US        500U
#define DIRECTION_SETTLE_US   500U

enum pd_direction {
	PD_DIRECTION_LOW,
	PD_DIRECTION_HIGH,
};

PINCTRL_DT_DEFINE(DT_NODELABEL(hw_fixture));

static const struct device *const qei = DEVICE_DT_GET(DT_NODELABEL(qei1));
static const struct gpio_dt_spec pulse = GPIO_DT_SPEC_GET(DT_NODELABEL(hw_fixture), a_gpios);
static const struct gpio_dt_spec direction = GPIO_DT_SPEC_GET(DT_NODELABEL(hw_fixture), b_gpios);

static void expect_position(int32_t total, uint32_t counts_per_revolution)
{
	struct sensor_value count = {0};
	struct sensor_value revolutions = {0};
	struct sensor_value rotation = {0};
	int32_t expected_revolutions = total / (int32_t)counts_per_revolution;
	int32_t expected_phase = total % (int32_t)counts_per_revolution;
	int64_t expected_rotation;

	if (expected_phase < 0) {
		expected_phase += counts_per_revolution;
		expected_revolutions--;
	}
	expected_rotation = (int64_t)expected_phase * 360000000 / counts_per_revolution;
	zassert_ok(sensor_sample_fetch(qei));
	zassert_ok(sensor_channel_get(qei, SENSOR_CHAN_ENCODER_COUNT, &count));
	zassert_ok(sensor_channel_get(qei, SENSOR_CHAN_ENCODER_REVOLUTIONS, &revolutions));
	zassert_ok(sensor_channel_get(qei, SENSOR_CHAN_ROTATION, &rotation));
	zassert_equal(count.val1, expected_phase, "total %d: phase %d expected %d", total,
		      count.val1, expected_phase);
	zassert_equal(count.val2, 0);
	zassert_equal(revolutions.val1, expected_revolutions,
		      "total %d: revolutions %d expected %d", total, revolutions.val1,
		      expected_revolutions);
	zassert_equal(revolutions.val2, 0);
	zassert_equal(sensor_value_to_micro(&rotation), expected_rotation,
		      "total %d: incorrect angle for CPR %u", total, counts_per_revolution);
}

static void configure_decoder(uint32_t counts_per_revolution)
{
	const struct hpm_qeiv2_mode_config mode = {
		.mode = HPM_QEIV2_MODE_PULSE_DIRECTION,
		.counts_per_revolution = counts_per_revolution,
	};

	zassert_ok(hpm_qeiv2_configure_mode(qei, &mode));
	expect_position(0, counts_per_revolution);
}

static void send_pulses(uint32_t pulses, enum pd_direction direction_state, int32_t *total,
			uint32_t counts_per_revolution)
{
	int32_t delta =
		(direction_state == PD_DIRECTION_HIGH) ? -COUNTS_PER_PULSE : COUNTS_PER_PULSE;

	zassert_ok(gpio_pin_set_dt(&direction, direction_state == PD_DIRECTION_HIGH ? 1 : 0));
	k_busy_wait(DIRECTION_SETTLE_US);

	for (uint32_t index = 0U; index < pulses; ++index) {
		zassert_ok(gpio_pin_set_dt(&pulse, 1));
		k_busy_wait(PULSE_WIDTH_US);
		zassert_ok(gpio_pin_set_dt(&pulse, 0));
		k_busy_wait(PULSE_WIDTH_US);
		*total += delta;
	}
	expect_position(*total, counts_per_revolution);
}

static void qei_pd_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_true(device_is_ready(qei));
	zassert_true(gpio_is_ready_dt(&pulse));
	zassert_true(gpio_is_ready_dt(&direction));
	zassert_ok(pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(hw_fixture)),
				       PINCTRL_STATE_DEFAULT));
	zassert_ok(gpio_pin_configure_dt(&direction, GPIO_OUTPUT_INACTIVE));
	zassert_ok(gpio_pin_configure_dt(&pulse, GPIO_OUTPUT_INACTIVE));
	k_busy_wait(DIRECTION_SETTLE_US);
	zassert_ok(hpm_qeiv2_clear_events(qei, HPM_QEIV2_EVENT_ALL));
	configure_decoder(COUNTS_PER_REVOLUTION);
}

static void qei_pd_after(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)gpio_pin_configure_dt(&pulse, GPIO_INPUT);
	(void)gpio_pin_configure_dt(&direction, GPIO_INPUT);
}

ZTEST(hpm_qei_pd_loop, test_low_direction_advances)
{
	int32_t total = 0;

	/*
	 * The SDK enables both pulse edges, so one complete pulse cycle contributes
	 * two counts. This verified pad path increments phase while direction is low.
	 */
	send_pulses(10U, PD_DIRECTION_LOW, &total, COUNTS_PER_REVOLUTION);
	zassert_equal(total, 20);
	printk("QEI PD: 10 low-direction pulses produced +20 counts\n");
}

ZTEST(hpm_qei_pd_loop, test_high_direction_reverses)
{
	int32_t total = 0;

	send_pulses(10U, PD_DIRECTION_HIGH, &total, COUNTS_PER_REVOLUTION);
	zassert_equal(total, -20);
	printk("QEI PD: 10 high-direction pulses produced -20 counts\n");
}

ZTEST(hpm_qei_pd_loop, test_pulse_counter_wrap)
{
	int32_t total = 0;

	send_pulses(COUNTS_PER_REVOLUTION / COUNTS_PER_PULSE, PD_DIRECTION_LOW, &total,
		    COUNTS_PER_REVOLUTION);
	send_pulses(COUNTS_PER_REVOLUTION / COUNTS_PER_PULSE, PD_DIRECTION_HIGH, &total,
		    COUNTS_PER_REVOLUTION);
	zassert_equal(total, 0);
	printk("QEI PD: CPR 64 wrap passed with 32 pulses per revolution\n");
}

ZTEST(hpm_qei_pd_loop, test_runtime_counts_per_revolution)
{
	int32_t total = 0;

	send_pulses(COUNTS_PER_REVOLUTION / COUNTS_PER_PULSE, PD_DIRECTION_LOW, &total,
		    COUNTS_PER_REVOLUTION);
	configure_decoder(128U);
	total = 0;
	send_pulses(128U / COUNTS_PER_PULSE, PD_DIRECTION_LOW, &total, 128U);
	send_pulses(128U / COUNTS_PER_PULSE, PD_DIRECTION_HIGH, &total, 128U);
	zassert_equal(total, 0);
	printk("QEI PD: runtime reset and CPR 64 -> 128 passed\n");
}

ZTEST_SUITE(hpm_qei_pd_loop, NULL, NULL, qei_pd_before, qei_pd_after, NULL);
