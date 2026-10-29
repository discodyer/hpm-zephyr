/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/comparator.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/opamp.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#define DAC_LOW                1024U
#define DAC_HIGH               3072U
#define SETTLE_MS              20U
#define QUIET_MS               50U
#define EDGE_CYCLES            8U
#define CALLBACK_WRONG_DEVICE  BIT(0)
#define CALLBACK_WRONG_DATA    BIT(1)
#define CALLBACK_WRONG_CONTEXT BIT(2)

static const struct device *const adc = DEVICE_DT_GET(DT_NODELABEL(adc0));
static const struct device *const dac = DEVICE_DT_GET(DT_NODELABEL(dac0));
static const struct device *const acmp = DEVICE_DT_GET(DT_NODELABEL(acmp1));
static K_SEM_DEFINE(crossed, 0, 1);
static atomic_t callback_count;
static atomic_t callback_errors;
static atomic_t callback_expect_isr;

static void comparator_cb(const struct device *dev, void *data)
{
	if (dev != acmp) {
		atomic_or(&callback_errors, CALLBACK_WRONG_DEVICE);
	}
	if (data != &crossed) {
		atomic_or(&callback_errors, CALLBACK_WRONG_DATA);
	}
	if (k_is_in_isr() != (atomic_get(&callback_expect_isr) != 0)) {
		atomic_or(&callback_errors, CALLBACK_WRONG_CONTEXT);
	}
	atomic_inc(&callback_count);
	k_sem_give(&crossed);
}

static void set_dac_level(uint16_t level)
{
	zassert_ok(dac_write_value(dac, 0, level));
	k_msleep(SETTLE_MS);
}

static void check_callback_state(uint32_t expected)
{
	zassert_equal(atomic_get(&callback_errors), 0, "ACMP callback errors: 0x%x",
		      (unsigned int)atomic_get(&callback_errors));
	zassert_equal(atomic_get(&callback_count), expected, "ACMP callback count");
}

static void expect_no_callback(uint32_t expected)
{
	zassert_equal(k_sem_take(&crossed, K_MSEC(QUIET_MS)), -EAGAIN, "Unexpected ACMP callback");
	check_callback_state(expected);
}

static void expect_callback(uint32_t expected)
{
	zassert_ok(k_sem_take(&crossed, K_SECONDS(1)), "ACMP edge IRQ timeout");
	check_callback_state(expected);
	expect_no_callback(expected);
}

static void front_before(void *fixture)
{
	const struct dac_channel_cfg config = {.channel_id = 0, .resolution = 12};

	ARG_UNUSED(fixture);
	zassert_true(device_is_ready(dac));
	zassert_true(device_is_ready(acmp));
	zassert_ok(dac_channel_setup(dac, &config));
	zassert_ok(comparator_set_trigger(acmp, COMPARATOR_TRIGGER_NONE));
	zassert_ok(comparator_set_trigger_callback(acmp, NULL, NULL));
	set_dac_level(DAC_LOW);
	zassert_equal(comparator_trigger_is_pending(acmp), 0);
	k_sem_reset(&crossed);
	atomic_clear(&callback_count);
	atomic_clear(&callback_errors);
	atomic_set(&callback_expect_isr, 1);
}

static void front_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)comparator_set_trigger(acmp, COMPARATOR_TRIGGER_NONE);
	(void)comparator_set_trigger_callback(acmp, NULL, NULL);
	(void)dac_write_value(dac, 0, 0);
	(void)comparator_trigger_is_pending(acmp);
	k_sem_reset(&crossed);
}

ZTEST(hpm_analog_front, test_opamp_follower_voltage)
{
	const struct adc_channel_cfg channel = {
		.gain = ADC_GAIN_1,
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		.channel_id = 15,
	};
	uint16_t sample;
	struct adc_sequence sequence = {
		.channels = BIT(15),
		.resolution = 12,
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};
	const uint16_t levels[] = {768, 1536, 2304, 3072};

	zassert_true(device_is_ready(DEVICE_DT_GET(DT_NODELABEL(opamp0))));
	zassert_true(device_is_ready(adc));
	zassert_ok(adc_channel_setup(adc, &channel));
	for (size_t i = 0; i < ARRAY_SIZE(levels); ++i) {
		set_dac_level(levels[i]);
		zassert_ok(adc_read(adc, &sequence));
		printk("OPAMP0 follower: DAC0=%u ADC0.IN15=%u\n", levels[i], sample);
		zassert_within(sample, levels[i], 160, "OPAMP follower: PB08 -> PB04, ADC15=PB00");
	}
}

ZTEST(hpm_analog_front, test_comparator_single_edges_disable_and_rearm)
{
	uint32_t expected = 0U;

	zassert_ok(comparator_set_trigger_callback(acmp, comparator_cb, &crossed));
	/* ACMP positive input is VREF/2; DAC0 drives the negative input PB11. */
	zassert_ok(comparator_set_trigger(acmp, COMPARATOR_TRIGGER_FALLING_EDGE));
	for (uint32_t i = 0U; i < EDGE_CYCLES; ++i) {
		set_dac_level(DAC_HIGH);
		expect_callback(++expected);
		set_dac_level(DAC_LOW);
		expect_no_callback(expected);
	}

	zassert_ok(comparator_set_trigger(acmp, COMPARATOR_TRIGGER_NONE));
	set_dac_level(DAC_HIGH);
	expect_no_callback(expected);
	zassert_ok(comparator_set_trigger(acmp, COMPARATOR_TRIGGER_RISING_EDGE));
	for (uint32_t i = 0U; i < EDGE_CYCLES; ++i) {
		set_dac_level(DAC_LOW);
		expect_callback(++expected);
		set_dac_level(DAC_HIGH);
		expect_no_callback(expected);
	}

	zassert_ok(comparator_set_trigger(acmp, COMPARATOR_TRIGGER_NONE));
	for (uint32_t i = 0U; i < EDGE_CYCLES; ++i) {
		set_dac_level(DAC_LOW);
		set_dac_level(DAC_HIGH);
	}
	expect_no_callback(expected);
	zassert_equal(comparator_trigger_is_pending(acmp), 0);
	zassert_ok(comparator_set_trigger(acmp, COMPARATOR_TRIGGER_RISING_EDGE));
	set_dac_level(DAC_LOW);
	expect_callback(++expected);
	printk("ACMP1 single-edge/disable/rearm: %u ISR callbacks\n", expected);
}

ZTEST(hpm_analog_front, test_comparator_both_edges_and_pending_callbacks)
{
	uint32_t expected = 0U;

	zassert_ok(comparator_set_trigger_callback(acmp, comparator_cb, &crossed));
	zassert_ok(comparator_set_trigger(acmp, COMPARATOR_TRIGGER_BOTH_EDGES));
	for (uint32_t i = 0U; i < EDGE_CYCLES; ++i) {
		set_dac_level(DAC_HIGH);
		expect_callback(++expected);
		set_dac_level(DAC_LOW);
		expect_callback(++expected);
	}

	zassert_ok(comparator_set_trigger_callback(acmp, NULL, NULL));
	set_dac_level(DAC_HIGH);
	expect_no_callback(expected);
	zassert_equal(comparator_trigger_is_pending(acmp), 1, "ACMP falling edge not pending");
	zassert_equal(comparator_trigger_is_pending(acmp), 0, "ACMP pending edge not cleared");
	set_dac_level(DAC_LOW);
	expect_no_callback(expected);
	zassert_equal(comparator_trigger_is_pending(acmp), 1, "ACMP rising edge not pending");
	zassert_equal(comparator_trigger_is_pending(acmp), 0, "ACMP pending edge not cleared");

	set_dac_level(DAC_HIGH);
	expect_no_callback(expected);
	/* The API dispatches an existing pending event immediately in this thread. */
	atomic_clear(&callback_expect_isr);
	zassert_ok(comparator_set_trigger_callback(acmp, comparator_cb, &crossed));
	check_callback_state(expected + 1U);
	atomic_set(&callback_expect_isr, 1);
	expect_callback(++expected);
	zassert_equal(comparator_trigger_is_pending(acmp), 0,
		      "ACMP dispatched event still pending");
	set_dac_level(DAC_LOW);
	expect_callback(++expected);
	printk("ACMP1 both-edge/pending: %u callbacks, one immediate dispatch\n", expected);
}

ZTEST_SUITE(hpm_analog_front, NULL, NULL, front_before, front_after, NULL);
