/* SPDX-License-Identifier: Apache-2.0 */

#include <drivers/sensor/hpmicro_qeiv2.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#define COUNTS_PER_REVOLUTION 64U
#define COMPARE0_EVENT        HPM_QEIV2_EVENT_POSITION_COMPARE
#define COMPARE1_EVENT        HPM_QEIV2_EVENT_POSITION2_COMPARE
#define COMPARE_EVENTS        (COMPARE0_EVENT | COMPARE1_EVENT)

PINCTRL_DT_DEFINE(DT_NODELABEL(hw_fixture));

static const struct device *const qei = DEVICE_DT_GET(DT_NODELABEL(qei1));
static const struct gpio_dt_spec a = GPIO_DT_SPEC_GET(DT_NODELABEL(hw_fixture), a_gpios);
static const struct gpio_dt_spec b = GPIO_DT_SPEC_GET(DT_NODELABEL(hw_fixture), b_gpios);
static const uint8_t gray_states[] = {0, 1, 3, 2};
static uint8_t gray_index;
static int callback_cookie;
static atomic_t callback_calls;
static atomic_t callback_errors;
static atomic_t callback_events;
static atomic_t compare0_calls;
static atomic_t compare1_calls;

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
		.mode = HPM_QEIV2_MODE_ABZ,
		.counts_per_revolution = counts_per_revolution,
	};

	zassert_ok(hpm_qeiv2_configure_mode(qei, &mode));
	expect_position(0, counts_per_revolution);
}

static void advance_edges(uint32_t edges, bool forward, int32_t *total,
			  uint32_t counts_per_revolution)
{
	for (uint32_t edge = 0; edge < edges; ++edge) {
		uint8_t previous = gray_states[gray_index];
		uint8_t state;

		gray_index = (gray_index + (forward ? 1U : 3U)) % ARRAY_SIZE(gray_states);
		state = gray_states[gray_index];
		if (((previous ^ state) & BIT(0)) != 0U) {
			zassert_ok(gpio_pin_set_dt(&a, state & BIT(0)));
		} else {
			zassert_ok(gpio_pin_set_dt(&b, (state >> 1) & BIT(0)));
		}
		k_busy_wait(100);
		*total += forward ? 1 : -1;
		expect_position(*total, counts_per_revolution);
	}
}

static void event_callback(const struct device *dev, uint32_t events, void *user_data)
{
	if ((dev != qei) || (user_data != &callback_cookie) || !k_is_in_isr()) {
		atomic_or(&callback_errors, BIT(0));
	}
	if ((events == 0U) || ((events & ~COMPARE_EVENTS) != 0U)) {
		atomic_or(&callback_errors, BIT(1));
	}
	atomic_or(&callback_events, events);
	if ((events & COMPARE0_EVENT) != 0U) {
		atomic_inc(&compare0_calls);
	}
	if ((events & COMPARE1_EVENT) != 0U) {
		atomic_inc(&compare1_calls);
	}
	atomic_inc(&callback_calls);
}

static void expect_callbacks(int count, int compare0, int compare1)
{
	zassert_true(WAIT_FOR(atomic_get(&callback_calls) == count, 50000, k_busy_wait(10)),
		     "callback count %ld expected %d", (long)atomic_get(&callback_calls), count);
	zassert_equal(atomic_get(&compare0_calls), compare0);
	zassert_equal(atomic_get(&compare1_calls), compare1);
	zassert_equal(atomic_get(&callback_errors), 0, "invalid callback metadata or event mask");
}

static void expect_events(uint32_t expected)
{
	uint32_t events = 0U;

	zassert_ok(hpm_qeiv2_get_events(qei, &events));
	zassert_equal(events & COMPARE_EVENTS, expected, "compare events 0x%08x expected 0x%08x",
		      events & COMPARE_EVENTS, expected);
}

static void configure_phase_comparators(void)
{
	struct hpm_qeiv2_compare_config compare = {
		.source = HPM_QEIV2_COMPARE_PHASE,
		.value = 8U,
		.ignore_direction = true,
		.ignore_z = true,
	};

	zassert_ok(hpm_qeiv2_configure_compare(qei, 0U, &compare));
	compare.value = 16U;
	zassert_ok(hpm_qeiv2_configure_compare(qei, 1U, &compare));
	zassert_ok(hpm_qeiv2_clear_events(qei, HPM_QEIV2_EVENT_ALL));
}

static void qei_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_true(device_is_ready(qei));
	zassert_true(gpio_is_ready_dt(&a));
	zassert_true(gpio_is_ready_dt(&b));
	zassert_ok(pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(hw_fixture)),
				       PINCTRL_STATE_DEFAULT));
	zassert_ok(hpm_qeiv2_set_event_callback(qei, 0U, NULL, NULL));
	zassert_ok(hpm_qeiv2_set_event_route(qei, HPM_QEIV2_EVENT_ROUTE_INTERRUPT,
					     HPM_QEIV2_EVENT_ALL, false));
	zassert_ok(gpio_pin_configure_dt(&a, GPIO_OUTPUT_INACTIVE));
	zassert_ok(gpio_pin_configure_dt(&b, GPIO_OUTPUT_INACTIVE));
	k_busy_wait(100);
	gray_index = 0U;
	configure_decoder(COUNTS_PER_REVOLUTION);
	zassert_ok(hpm_qeiv2_clear_events(qei, HPM_QEIV2_EVENT_ALL));
	atomic_clear(&callback_calls);
	atomic_clear(&callback_errors);
	atomic_clear(&callback_events);
	atomic_clear(&compare0_calls);
	atomic_clear(&compare1_calls);
}

static void qei_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)hpm_qeiv2_set_event_callback(qei, 0U, NULL, NULL);
	(void)hpm_qeiv2_set_event_route(qei, HPM_QEIV2_EVENT_ROUTE_INTERRUPT, HPM_QEIV2_EVENT_ALL,
					false);
	(void)hpm_qeiv2_clear_events(qei, HPM_QEIV2_EVENT_ALL);
	(void)gpio_pin_configure_dt(&a, GPIO_INPUT);
	(void)gpio_pin_configure_dt(&b, GPIO_INPUT);
}

ZTEST(hpm_qei_loop, test_quadrature_forward_reverse_and_wrap)
{
	int32_t total = 0;

	/* A leads B for forward motion: 00 -> 01 -> 11 -> 10 -> 00. */
	advance_edges(128U, true, &total, COUNTS_PER_REVOLUTION);
	advance_edges(256U, false, &total, COUNTS_PER_REVOLUTION);
	advance_edges(128U, true, &total, COUNTS_PER_REVOLUTION);
	zassert_equal(total, 0);
	printk("QEI: 512 edges checked, CPR 64, +2/-2 revolutions and return to zero\n");
}

ZTEST(hpm_qei_loop, test_compare_interrupt_mask_poll_and_recovery)
{
	int32_t total = 0;

	configure_phase_comparators();
	zassert_ok(hpm_qeiv2_set_event_callback(qei, COMPARE_EVENTS, event_callback,
						&callback_cookie));
	advance_edges(7U, true, &total, COUNTS_PER_REVOLUTION);
	expect_callbacks(0, 0, 0);
	advance_edges(1U, true, &total, COUNTS_PER_REVOLUTION);
	expect_callbacks(1, 1, 0);
	advance_edges(7U, true, &total, COUNTS_PER_REVOLUTION);
	expect_callbacks(1, 1, 0);
	advance_edges(1U, true, &total, COUNTS_PER_REVOLUTION);
	expect_callbacks(2, 1, 1);
	zassert_equal((uint32_t)atomic_get(&callback_events), COMPARE_EVENTS);
	expect_events(COMPARE_EVENTS);
	zassert_ok(hpm_qeiv2_clear_events(qei, COMPARE_EVENTS));
	expect_events(0U);

	/* Comparator 1 remains observable while only comparator 0 requests IRQs. */
	zassert_ok(hpm_qeiv2_set_event_callback(qei, COMPARE0_EVENT, event_callback,
						&callback_cookie));
	advance_edges(COUNTS_PER_REVOLUTION, true, &total, COUNTS_PER_REVOLUTION);
	expect_callbacks(3, 2, 1);
	/* Keep both the ISR latch and masked hardware flag across unbind. */
	zassert_ok(hpm_qeiv2_set_event_callback(qei, 0U, NULL, NULL));
	expect_events(COMPARE_EVENTS);
	zassert_ok(hpm_qeiv2_clear_events(qei, COMPARE_EVENTS));
	expect_events(0U);

	/* With the callback removed, both hardware flags remain available. */
	advance_edges(COUNTS_PER_REVOLUTION, true, &total, COUNTS_PER_REVOLUTION);
	expect_callbacks(3, 2, 1);
	expect_events(COMPARE_EVENTS);
	zassert_ok(hpm_qeiv2_clear_events(qei, COMPARE0_EVENT));
	expect_events(COMPARE1_EVENT);
	zassert_ok(hpm_qeiv2_clear_events(qei, COMPARE1_EVENT));
	expect_events(0U);

	zassert_ok(hpm_qeiv2_set_event_callback(qei, COMPARE_EVENTS, event_callback,
						&callback_cookie));
	advance_edges(COUNTS_PER_REVOLUTION, true, &total, COUNTS_PER_REVOLUTION);
	expect_callbacks(5, 3, 2);
	expect_events(COMPARE_EVENTS);
	printk("QEI: phase compares 8/16, IRQs 3/2, mask/poll/clear/rebind passed\n");
}

ZTEST(hpm_qei_loop, test_runtime_reset_resolution_and_compare_source)
{
	struct hpm_qeiv2_compare_config compare = {
		.source = HPM_QEIV2_COMPARE_PHASE,
		.value = 8U,
		.ignore_direction = true,
		.ignore_z = true,
	};
	int32_t total = 0;

	advance_edges(100U, true, &total, COUNTS_PER_REVOLUTION);
	zassert_ok(hpm_qeiv2_configure_compare(qei, 0U, &compare));
	compare.source = HPM_QEIV2_COMPARE_POSITION;
	zassert_equal(hpm_qeiv2_configure_compare(qei, 1U, &compare), -ENOTSUP);

	/* Reset a nonzero revolution and phase, and select a different source. */
	configure_decoder(128U);
	total = 0;
	zassert_ok(hpm_qeiv2_configure_compare(qei, 0U, &compare));
	compare.value = 16U;
	zassert_ok(hpm_qeiv2_configure_compare(qei, 1U, &compare));
	advance_edges(24U, true, &total, 128U);
	compare.source = HPM_QEIV2_COMPARE_PHASE;
	zassert_equal(hpm_qeiv2_configure_compare(qei, 0U, &compare), -ENOTSUP);

	configure_decoder(128U);
	total = 0;
	configure_phase_comparators();
	advance_edges(256U, true, &total, 128U);
	advance_edges(256U, false, &total, 128U);
	zassert_equal(total, 0);
	printk("QEI: runtime reset and CPR 64 -> 128, compare source reset passed\n");
}

ZTEST_SUITE(hpm_qei_loop, NULL, NULL, qei_before, qei_after, NULL);
