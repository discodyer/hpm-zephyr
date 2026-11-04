/* SPDX-License-Identifier: Apache-2.0 */

#include <drivers/sensor/hpmicro_qeiv2.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <hpm_qeiv2_drv.h>

#define COUNTS_PER_REVOLUTION 64U
#define COUNTS_PER_PULSE      2U
#define PULSE_WIDTH_US        500U

PINCTRL_DT_DEFINE(DT_NODELABEL(hw_fixture));

static const struct device *const qei = DEVICE_DT_GET(DT_NODELABEL(qei1));
static const struct gpio_dt_spec pulse = GPIO_DT_SPEC_GET(DT_NODELABEL(hw_fixture), a_gpios);
static QEIV2_Type *const qei_regs = (QEIV2_Type *)DT_REG_ADDR(DT_NODELABEL(qei1));

static void expect_position(uint32_t total, uint32_t counts_per_revolution)
{
	struct sensor_value count = {0};
	struct sensor_value revolutions = {0};
	struct sensor_value rotation = {0};
	int32_t expected_revolutions = total / counts_per_revolution;
	uint32_t expected_phase = total % counts_per_revolution;
	uint64_t expected_rotation =
		(uint64_t)expected_phase * 360000000ULL / counts_per_revolution;

	zassert_ok(sensor_sample_fetch(qei));
	zassert_ok(sensor_channel_get(qei, SENSOR_CHAN_ENCODER_COUNT, &count));
	zassert_ok(sensor_channel_get(qei, SENSOR_CHAN_ENCODER_REVOLUTIONS, &revolutions));
	zassert_ok(sensor_channel_get(qei, SENSOR_CHAN_ROTATION, &rotation));
	zassert_equal(count.val1, expected_phase, "total %u: phase %d expected %u", total,
		      count.val1, expected_phase);
	zassert_equal(count.val2, 0);
	zassert_equal(revolutions.val1, expected_revolutions,
		      "total %u: revolutions %d expected %d", total, revolutions.val1,
		      expected_revolutions);
	zassert_equal(revolutions.val2, 0);
	zassert_equal(sensor_value_to_micro(&rotation), expected_rotation,
		      "total %u: incorrect angle for CPR %u", total, counts_per_revolution);
}

static void configure_single(uint32_t counts_per_revolution)
{
	const struct hpm_qeiv2_mode_config mode = {
		.mode = HPM_QEIV2_MODE_SINGLE,
		.counts_per_revolution = counts_per_revolution,
	};

	zassert_ok(hpm_qeiv2_configure_mode(qei, &mode));
	zassert_true((qei_regs->QEI_CFG & QEIV2_QEI_CFG_SIGA_EN_MASK) != 0U);
	zassert_false((qei_regs->QEI_CFG & QEIV2_QEI_CFG_SIGB_EN_MASK) != 0U);
	zassert_false((qei_regs->QEI_CFG & QEIV2_QEI_CFG_SIGZ_EN_MASK) != 0U);
	zassert_true((qei_regs->QEI_CFG & QEIV2_QEI_CFG_POSIDGE_EN_MASK) != 0U);
	zassert_true((qei_regs->QEI_CFG & QEIV2_QEI_CFG_NEGEDGE_EN_MASK) != 0U);
	expect_position(0U, counts_per_revolution);
}

static uint32_t send_pulses(uint32_t pulses)
{
	for (uint32_t index = 0U; index < pulses; ++index) {
		zassert_ok(gpio_pin_set_dt(&pulse, 1));
		k_busy_wait(PULSE_WIDTH_US);
		zassert_ok(gpio_pin_set_dt(&pulse, 0));
		k_busy_wait(PULSE_WIDTH_US);
	}

	return pulses * COUNTS_PER_PULSE;
}

static void qei_single_before(void *fixture)
{
	ARG_UNUSED(fixture);

	zassert_true(device_is_ready(qei));
	zassert_true(gpio_is_ready_dt(&pulse));
	zassert_ok(pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(hw_fixture)),
				       PINCTRL_STATE_DEFAULT));
	zassert_ok(gpio_pin_configure_dt(&pulse, GPIO_OUTPUT_INACTIVE));
	k_busy_wait(PULSE_WIDTH_US);
	zassert_ok(hpm_qeiv2_clear_events(qei, HPM_QEIV2_EVENT_ALL));
	configure_single(COUNTS_PER_REVOLUTION);
}

static void qei_single_after(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)gpio_pin_configure_dt(&pulse, GPIO_INPUT);
}

ZTEST(hpm_qei_single_loop, test_pulses_advance)
{
	uint32_t total = send_pulses(10U);

	zassert_equal(total, 20U);
	expect_position(total, COUNTS_PER_REVOLUTION);
	printk("QEI single: 10 pulses produced +20 counts\n");
}

ZTEST(hpm_qei_single_loop, test_full_revolution)
{
	uint32_t total = send_pulses(COUNTS_PER_REVOLUTION / COUNTS_PER_PULSE);

	zassert_equal(total, COUNTS_PER_REVOLUTION);
	expect_position(total, COUNTS_PER_REVOLUTION);
	printk("QEI single: CPR 64 full revolution passed with 32 pulses\n");
}

ZTEST(hpm_qei_single_loop, test_runtime_counts_per_revolution)
{
	uint32_t total;

	(void)send_pulses(16U);
	configure_single(COUNTS_PER_REVOLUTION * 2U);
	total = send_pulses(COUNTS_PER_REVOLUTION);
	expect_position(total, COUNTS_PER_REVOLUTION * 2U);
	printk("QEI single: runtime reset and CPR 64 -> 128 passed\n");
}

ZTEST_SUITE(hpm_qei_single_loop, NULL, NULL, qei_single_before, qei_single_after, NULL);
