/* SPDX-License-Identifier: Apache-2.0 */

#include <drivers/sensor/hpmicro_qeiv2.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <hpm_qeiv2_drv.h>

#define COUNTS_PER_REVOLUTION 64U
#define UVW_SETTLE_US         1000U

#define UVW_U_STATE  (BIT(0))
#define UVW_V_STATE  (BIT(1))
#define UVW_W_STATE  (BIT(2))
#define UVW_AB_STATE (UVW_U_STATE | UVW_V_STATE)

struct uvw_sector {
	uint8_t state;
	uint32_t position;
	int32_t count;
	int32_t rotation_micro_degrees;
};

/* Forward electrical sequence: 101, 100, 110, 010, 011, 001. */
static const struct uvw_sector uvw_sectors[] = {
	{UVW_U_STATE | UVW_W_STATE, 0x15555555U, 357913941, 29999999},
	{UVW_U_STATE, 0x3fffffffU, 1073741823, 89999999},
	{UVW_U_STATE | UVW_V_STATE, 0x6aaaaaaaU, 1789569706, 149999999},
	{UVW_V_STATE, 0x95555554U, -1789569708, 209999999},
	{UVW_V_STATE | UVW_W_STATE, 0xbfffffffU, -1073741825, 269999999},
	{UVW_W_STATE, 0xeaaaaaa9U, -357913943, 329999999},
};

PINCTRL_DT_DEFINE(DT_NODELABEL(hw_fixture));

static const struct device *const qei = DEVICE_DT_GET(DT_NODELABEL(qei1));
static const struct gpio_dt_spec u = GPIO_DT_SPEC_GET(DT_NODELABEL(hw_fixture), a_gpios);
static const struct gpio_dt_spec v = GPIO_DT_SPEC_GET(DT_NODELABEL(hw_fixture), b_gpios);
static const struct gpio_dt_spec w = GPIO_DT_SPEC_GET(DT_NODELABEL(hw_fixture), z_gpios);
static QEIV2_Type *const qei_regs = (QEIV2_Type *)DT_REG_ADDR(DT_NODELABEL(qei1));

static uint8_t fixture_state(void)
{
	gpio_port_value_t value;

	zassert_ok(gpio_port_get_raw(u.port, &value));
	return (uint8_t)(((value >> u.pin) & BIT(0)) | (((value >> v.pin) & BIT(0)) << 1) |
			 (((value >> w.pin) & BIT(0)) << 2));
}

static uint8_t qei_ab_state(void)
{
	uint32_t phase = qei_regs->COUNT[QEIV2_COUNT_CURRENT].PH;

	return (uint8_t)(QEIV2_COUNT_PH_ASTAT_GET(phase) | (QEIV2_COUNT_PH_BSTAT_GET(phase) << 1));
}

static void set_uvw_state(uint8_t state)
{
	const gpio_port_pins_t mask = BIT(u.pin) | BIT(v.pin) | BIT(w.pin);
	const gpio_port_pins_t value = ((state & UVW_U_STATE) ? BIT(u.pin) : 0U) |
				       ((state & UVW_V_STATE) ? BIT(v.pin) : 0U) |
				       ((state & UVW_W_STATE) ? BIT(w.pin) : 0U);

	zassert_ok(gpio_port_set_masked_raw(u.port, mask, value));
	k_busy_wait(UVW_SETTLE_US);
}

static void expect_sector(uint32_t index, const char *tag)
{
	const struct uvw_sector *expected = &uvw_sectors[index];
	struct sensor_value count = {0};
	struct sensor_value rotation = {0};
	uint8_t fixture;
	uint8_t ab_state;

	zassert_ok(sensor_sample_fetch(qei));
	zassert_ok(sensor_channel_get(qei, SENSOR_CHAN_ENCODER_COUNT, &count));
	zassert_ok(sensor_channel_get(qei, SENSOR_CHAN_ROTATION, &rotation));
	fixture = fixture_state();
	ab_state = qei_ab_state();

	printk("QEI UVW[%s]: req=%c%c%c gpio=%c%c%c qei=%c%c? "
	       "POSITION=%08x count=%d angle=%lld\n",
	       tag, (expected->state & UVW_U_STATE) ? '1' : '0',
	       (expected->state & UVW_V_STATE) ? '1' : '0',
	       (expected->state & UVW_W_STATE) ? '1' : '0', (fixture & UVW_U_STATE) ? '1' : '0',
	       (fixture & UVW_V_STATE) ? '1' : '0', (fixture & UVW_W_STATE) ? '1' : '0',
	       (ab_state & UVW_U_STATE) ? '1' : '0', (ab_state & UVW_V_STATE) ? '1' : '0',
	       qei_regs->POSITION, count.val1, (long long)sensor_value_to_micro(&rotation));

	zassert_equal(fixture, expected->state, "%s: fixture readback 0x%x expected 0x%x", tag,
		      fixture, expected->state);
	zassert_equal(ab_state, expected->state & UVW_AB_STATE,
		      "%s: QEI A/B state 0x%x expected 0x%x", tag, ab_state,
		      (uint32_t)(expected->state & UVW_AB_STATE));
	zassert_equal(qei_regs->POSITION, expected->position, "%s: position 0x%08x expected 0x%08x",
		      tag, qei_regs->POSITION, expected->position);
	zassert_equal(count.val1, expected->count, "%s: count %d expected %d", tag, count.val1,
		      expected->count);
	zassert_equal(count.val2, 0);
	zassert_equal(sensor_value_to_micro(&rotation), expected->rotation_micro_degrees,
		      "%s: angle %lld expected %d", tag,
		      (long long)sensor_value_to_micro(&rotation),
		      expected->rotation_micro_degrees);
}

static void expect_phase_step(uint32_t *previous, bool forward)
{
	uint32_t current = qei_regs->PHASE_CNT;
	uint32_t expected = forward ? *previous + 1U : *previous - 1U;

	zassert_equal(current, expected, "PHASE_CNT %u expected %u after %s edge", current,
		      expected, forward ? "forward" : "reverse");
	*previous = current;
}

static void configure_uvw(uint32_t counts_per_revolution)
{
	const struct hpm_qeiv2_mode_config mode = {
		.mode = HPM_QEIV2_MODE_UVW,
		.counts_per_revolution = counts_per_revolution,
	};

	zassert_ok(hpm_qeiv2_configure_mode(qei, &mode));
}

static void qei_uvw_before(void *fixture)
{
	ARG_UNUSED(fixture);

	zassert_true(device_is_ready(qei));
	zassert_true(gpio_is_ready_dt(&u));
	zassert_true(gpio_is_ready_dt(&v));
	zassert_true(gpio_is_ready_dt(&w));
	zassert_true(u.port == v.port);
	zassert_true(u.port == w.port);
	zassert_ok(pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(hw_fixture)),
				       PINCTRL_STATE_DEFAULT));
	zassert_ok(gpio_pin_configure_dt(&u, GPIO_OUTPUT_ACTIVE));
	zassert_ok(gpio_pin_configure_dt(&v, GPIO_OUTPUT_INACTIVE));
	zassert_ok(gpio_pin_configure_dt(&w, GPIO_OUTPUT_ACTIVE));
	k_busy_wait(UVW_SETTLE_US);
	configure_uvw(COUNTS_PER_REVOLUTION);
}

static void qei_uvw_after(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)gpio_pin_configure_dt(&u, GPIO_INPUT);
	(void)gpio_pin_configure_dt(&v, GPIO_INPUT);
	(void)gpio_pin_configure_dt(&w, GPIO_INPUT);
}

ZTEST(hpm_qei_uvw_loop, test_sector_positions)
{
	for (uint32_t index = 0U; index < ARRAY_SIZE(uvw_sectors); ++index) {
		set_uvw_state(uvw_sectors[index].state);
		expect_sector(index, "sector");
	}
}

ZTEST(hpm_qei_uvw_loop, test_forward_reverse_phase_count)
{
	uint32_t initial;
	uint32_t previous;

	set_uvw_state(uvw_sectors[0].state);
	initial = qei_regs->PHASE_CNT;
	previous = initial;

	for (uint32_t cycle = 0U; cycle < 2U; ++cycle) {
		for (uint32_t index = 1U; index < ARRAY_SIZE(uvw_sectors); ++index) {
			set_uvw_state(uvw_sectors[index].state);
			expect_phase_step(&previous, true);
		}
		set_uvw_state(uvw_sectors[0].state);
		expect_phase_step(&previous, true);
	}

	for (uint32_t cycle = 0U; cycle < 2U; ++cycle) {
		for (uint32_t index = ARRAY_SIZE(uvw_sectors) - 1U; index > 0U; --index) {
			set_uvw_state(uvw_sectors[index].state);
			expect_phase_step(&previous, false);
		}
		set_uvw_state(uvw_sectors[0].state);
		expect_phase_step(&previous, false);
	}

	zassert_equal(previous, initial,
		      "forward/reverse sequence did not return to initial phase");
	printk("QEI UVW: forward/reverse phase count passed, final PHASE_CNT=%u\n", previous);
}

ZTEST(hpm_qei_uvw_loop, test_runtime_reconfiguration)
{
	set_uvw_state(uvw_sectors[3].state);
	expect_sector(3U, "before-reconfigure");

	configure_uvw(COUNTS_PER_REVOLUTION * 2U);
	set_uvw_state(uvw_sectors[0].state);
	expect_sector(0U, "after-reconfigure");
	printk("QEI UVW: runtime CPR 64 -> 128 reconfiguration passed\n");
}

ZTEST_SUITE(hpm_qei_uvw_loop, NULL, NULL, qei_uvw_before, qei_uvw_after, NULL);
