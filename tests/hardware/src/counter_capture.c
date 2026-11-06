/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * Copyright (c) 2026 Cody Gu <gujiaqi@iscas.ac.cn>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#define CAPTURE_CHANNEL    0U
#define CAPTURE_TIMEOUT_MS 100
#define CAPTURE_SETTLE_MS  2

#define CAPTURE_EDGE_FLAGS (COUNTER_CAPTURE_RISING_EDGE | COUNTER_CAPTURE_FALLING_EDGE)

PINCTRL_DT_DEFINE(DT_NODELABEL(hw_fixture));

static const struct device *const timer = DEVICE_DT_GET(DT_NODELABEL(gptmr0));
static const struct gpio_dt_spec trigger = GPIO_DT_SPEC_GET(DT_NODELABEL(hw_fixture), out_gpios);

K_SEM_DEFINE(capture_sem, 0, 1);
static atomic_t capture_count;

static struct {
	const struct device *dev;
	uint8_t channel;
	counter_capture_flags_t flags;
	uint32_t ticks;
} last_capture;

static void capture_cb(const struct device *dev, uint8_t channel, counter_capture_flags_t flags,
		       uint32_t ticks, void *user_data)
{
	ARG_UNUSED(user_data);

	last_capture.dev = dev;
	last_capture.channel = channel;
	last_capture.flags = flags;
	last_capture.ticks = ticks;
	atomic_inc(&capture_count);
	k_sem_give(&capture_sem);
}

static void settle_trigger(int level)
{
	zassert_ok(gpio_pin_set_dt(&trigger, level));
	k_msleep(CAPTURE_SETTLE_MS);
}

static void expect_capture(int level, counter_capture_flags_t flags)
{
	uint32_t before;
	uint32_t after;
	int ret;

	zassert_ok(counter_get_value(timer, &before));
	zassert_ok(gpio_pin_set_dt(&trigger, level));
	ret = k_sem_take(&capture_sem, K_MSEC(CAPTURE_TIMEOUT_MS));
	zassert_ok(ret, "capture callback timeout for level %d", level);
	zassert_ok(counter_get_value(timer, &after));

	zassert_equal(last_capture.dev, timer);
	zassert_equal(last_capture.channel, CAPTURE_CHANNEL);
	zassert_equal(last_capture.flags, flags, "flags 0x%x, expected 0x%x", last_capture.flags,
		      flags);
	zassert_true((last_capture.ticks >= before) && (last_capture.ticks <= after),
		     "capture tick %u outside [%u, %u]", last_capture.ticks, before, after);
}

static void expect_no_capture(void)
{
	zassert_equal(k_sem_take(&capture_sem, K_MSEC(10)), -EAGAIN, "unexpected capture callback");
}

static void start_capture(counter_capture_flags_t flags)
{
	zassert_ok(counter_capture_configure(timer, CAPTURE_CHANNEL, flags, capture_cb, NULL));
	zassert_ok(counter_enable_capture(timer, CAPTURE_CHANNEL));
}

static void stop_capture(void)
{
	zassert_ok(counter_disable_capture(timer, CAPTURE_CHANNEL));
	expect_no_capture();
}

static void capture_before(void *fixture)
{
	ARG_UNUSED(fixture);
	const struct counter_top_cfg top_cfg = {
		.ticks = counter_get_max_top_value(timer),
	};

	zassert_ok(gpio_pin_set_dt(&trigger, 0));
	k_msleep(CAPTURE_SETTLE_MS);
	zassert_ok(counter_stop(timer));
	zassert_ok(counter_disable_capture(timer, CAPTURE_CHANNEL));
	zassert_ok(counter_set_top_value(timer, &top_cfg));
	zassert_ok(counter_reset(timer));
	zassert_ok(counter_start(timer));
	k_sem_reset(&capture_sem);
	atomic_clear(&capture_count);
	memset(&last_capture, 0, sizeof(last_capture));
}

static void capture_after(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)counter_disable_capture(timer, CAPTURE_CHANNEL);
	(void)counter_stop(timer);
	(void)gpio_pin_set_dt(&trigger, 0);
}

static void *capture_setup(void)
{
	zassert_true(device_is_ready(timer));
	zassert_true(gpio_is_ready_dt(&trigger));
	zassert_ok(pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(hw_fixture)),
				       PINCTRL_STATE_DEFAULT));
	zassert_ok(gpio_pin_configure_dt(&trigger, GPIO_OUTPUT_INACTIVE));

	return NULL;
}

ZTEST(hpm_counter_capture, test_capture_api_validation)
{
	zassert_equal(counter_capture_configure(timer, CAPTURE_CHANNEL, 0U, capture_cb, NULL),
		      -EINVAL);
	zassert_equal(counter_capture_configure(timer, CAPTURE_CHANNEL, COUNTER_CAPTURE_RISING_EDGE,
						NULL, NULL),
		      -EINVAL);
	zassert_equal(counter_capture_configure(timer, CAPTURE_CHANNEL,
						COUNTER_CAPTURE_RISING_EDGE | BIT(3), capture_cb,
						NULL),
		      -EINVAL);
	zassert_equal(counter_enable_capture(timer, CAPTURE_CHANNEL), -EINVAL);
	zassert_equal(
		counter_capture_configure(timer, 4U, COUNTER_CAPTURE_RISING_EDGE, capture_cb, NULL),
		-ENOTSUP);

	zassert_ok(counter_capture_configure(
		timer, CAPTURE_CHANNEL, COUNTER_CAPTURE_RISING_EDGE | COUNTER_CAPTURE_CONTINUOUS,
		capture_cb, NULL));
	zassert_equal(
		counter_capture_configure(timer, CAPTURE_CHANNEL,
					  COUNTER_CAPTURE_FALLING_EDGE | COUNTER_CAPTURE_CONTINUOUS,
					  capture_cb, NULL),
		-EBUSY);
	zassert_ok(counter_disable_capture(timer, CAPTURE_CHANNEL));
}

ZTEST(hpm_counter_capture, test_rising_edge_continuous)
{
	const counter_capture_flags_t flags =
		COUNTER_CAPTURE_RISING_EDGE | COUNTER_CAPTURE_CONTINUOUS;

	settle_trigger(0);
	start_capture(flags);
	for (uint8_t i = 0U; i < 3U; i++) {
		expect_capture(1, flags);
		settle_trigger(0);
		expect_no_capture();
	}

	stop_capture();
	settle_trigger(1);
	settle_trigger(0);
	expect_no_capture();
	zassert_equal(atomic_get(&capture_count), 3);
}

ZTEST(hpm_counter_capture, test_falling_edge_continuous)
{
	const counter_capture_flags_t flags =
		COUNTER_CAPTURE_FALLING_EDGE | COUNTER_CAPTURE_CONTINUOUS;

	settle_trigger(1);
	start_capture(flags);
	for (uint8_t i = 0U; i < 3U; i++) {
		expect_capture(0, flags);
		settle_trigger(1);
		expect_no_capture();
	}

	stop_capture();
	settle_trigger(0);
	settle_trigger(1);
	expect_no_capture();
	zassert_equal(atomic_get(&capture_count), 3);
}

ZTEST(hpm_counter_capture, test_both_edges_continuous)
{
	const counter_capture_flags_t flags =
		COUNTER_CAPTURE_BOTH_EDGES | COUNTER_CAPTURE_CONTINUOUS;

	settle_trigger(0);
	start_capture(flags);
	for (uint8_t i = 0U; i < 2U; i++) {
		expect_capture(1, flags);
		expect_capture(0, flags);
	}

	stop_capture();
	settle_trigger(1);
	settle_trigger(0);
	expect_no_capture();
	zassert_equal(atomic_get(&capture_count), 4);
}

ZTEST(hpm_counter_capture, test_rising_edge_single_shot)
{
	const counter_capture_flags_t flags =
		COUNTER_CAPTURE_RISING_EDGE | COUNTER_CAPTURE_SINGLE_SHOT;

	settle_trigger(0);
	start_capture(flags);
	expect_capture(1, flags);
	settle_trigger(0);
	settle_trigger(1);
	expect_no_capture();
	zassert_equal(atomic_get(&capture_count), 1);
	zassert_ok(counter_disable_capture(timer, CAPTURE_CHANNEL));
}

ZTEST(hpm_counter_capture, test_falling_edge_single_shot)
{
	const counter_capture_flags_t flags =
		COUNTER_CAPTURE_FALLING_EDGE | COUNTER_CAPTURE_SINGLE_SHOT;

	settle_trigger(1);
	start_capture(flags);
	expect_capture(0, flags);
	settle_trigger(1);
	settle_trigger(0);
	expect_no_capture();
	zassert_equal(atomic_get(&capture_count), 1);
	zassert_ok(counter_disable_capture(timer, CAPTURE_CHANNEL));
}

ZTEST(hpm_counter_capture, test_both_edges_single_shot)
{
	const counter_capture_flags_t flags =
		COUNTER_CAPTURE_BOTH_EDGES | COUNTER_CAPTURE_SINGLE_SHOT;

	settle_trigger(0);
	start_capture(flags);
	expect_capture(1, flags);
	settle_trigger(0);
	settle_trigger(1);
	expect_no_capture();
	zassert_equal(atomic_get(&capture_count), 1);
	zassert_ok(counter_disable_capture(timer, CAPTURE_CHANNEL));
}

ZTEST_SUITE(hpm_counter_capture, NULL, capture_setup, capture_before, capture_after, NULL);
