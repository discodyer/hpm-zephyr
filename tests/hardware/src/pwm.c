/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#define OUTPUT_CHANNEL   4U
#define CAPTURE_CHANNEL  4U
#define CAPTURE_WRAP     BIT64(28)
#define QUIET_MS         5U
#define CALLBACK_DEVICE  BIT(0)
#define CALLBACK_CHANNEL BIT(1)
#define CALLBACK_DATA    BIT(2)
#define CALLBACK_CONTEXT BIT(3)
#define CALLBACK_STATUS  BIT(4)
#define CALLBACK_PERIOD  BIT(5)
#define CALLBACK_PULSE   BIT(6)

PINCTRL_DT_DEFINE(DT_NODELABEL(hw_fixture));

static const struct device *const output = DEVICE_DT_GET(DT_NODELABEL(pwm0));
static const struct device *const capture = DEVICE_DT_GET(DT_NODELABEL(pwm1));
static const struct gpio_dt_spec level = GPIO_DT_SPEC_GET(DT_NODELABEL(hw_fixture), in_gpios);
static K_SEM_DEFINE(captured, 0, 1);
static uint64_t capture_rate;
static struct {
	atomic_t count;
	atomic_t errors;
	atomic_t last_period;
	atomic_t last_pulse;
	uint32_t period;
	uint32_t pulse;
	uint32_t period_tolerance;
	uint32_t pulse_tolerance;
	pwm_flags_t flags;
} capture_state;

static bool within_tolerance(uint32_t actual, uint32_t expected, uint32_t tolerance)
{
	uint32_t difference = actual > expected ? actual - expected : expected - actual;

	return difference <= tolerance;
}

static void pwm_captured(const struct device *dev, uint32_t channel, uint32_t period,
			 uint32_t pulse, int status, void *data)
{
	atomic_val_t errors = 0;

	if (dev != capture) {
		errors |= CALLBACK_DEVICE;
	}
	if (channel != CAPTURE_CHANNEL) {
		errors |= CALLBACK_CHANNEL;
	}
	if (data != &capture_state) {
		errors |= CALLBACK_DATA;
	}
	if (!k_is_in_isr()) {
		errors |= CALLBACK_CONTEXT;
	}
	if (status != 0) {
		errors |= CALLBACK_STATUS;
	}
	if ((capture_state.flags & PWM_CAPTURE_TYPE_PERIOD) != 0U &&
	    !within_tolerance(period, capture_state.period, capture_state.period_tolerance)) {
		errors |= CALLBACK_PERIOD;
	}
	if ((capture_state.flags & PWM_CAPTURE_TYPE_PULSE) != 0U &&
	    !within_tolerance(pulse, capture_state.pulse, capture_state.pulse_tolerance)) {
		errors |= CALLBACK_PULSE;
	}
	atomic_set(&capture_state.last_period, period);
	atomic_set(&capture_state.last_pulse, pulse);
	atomic_or(&capture_state.errors, errors);
	atomic_inc(&capture_state.count);
	k_sem_give(&captured);
}

static void prepare_capture(uint32_t period_ns, uint32_t pulse_ns, pwm_flags_t output_polarity,
			    pwm_flags_t capture_flags)
{
	uint32_t active_ns = pulse_ns;

	if ((output_polarity & PWM_POLARITY_MASK) != (capture_flags & PWM_POLARITY_MASK)) {
		active_ns = period_ns - pulse_ns;
	}
	capture_state.period = capture_rate * period_ns / NSEC_PER_SEC;
	capture_state.pulse = capture_rate * active_ns / NSEC_PER_SEC;
	/* Independent of observations: one percent, with a 32-cycle floor. */
	capture_state.period_tolerance = MAX(capture_state.period / 100U, 32U);
	capture_state.pulse_tolerance = MAX(capture_state.pulse / 100U, 32U);
	capture_state.flags = capture_flags;
	atomic_clear(&capture_state.count);
	atomic_clear(&capture_state.errors);
	atomic_clear(&capture_state.last_period);
	atomic_clear(&capture_state.last_pulse);
	k_sem_reset(&captured);
}

static void check_capture_errors(void)
{
	zassert_equal(atomic_get(&capture_state.errors), 0,
		      "PWM callback errors=0x%x last period=%u pulse=%u expected=%u/%u",
		      (unsigned int)atomic_get(&capture_state.errors),
		      (unsigned int)atomic_get(&capture_state.last_period),
		      (unsigned int)atomic_get(&capture_state.last_pulse), capture_state.period,
		      capture_state.pulse);
}

static void check_quiet(uint32_t expected_count)
{
	zassert_equal(k_sem_take(&captured, K_MSEC(QUIET_MS)), -EAGAIN,
		      "Unexpected PWM capture callback");
	zassert_equal(atomic_get(&capture_state.count), expected_count,
		      "Missing or duplicate PWM capture callback");
	check_capture_errors();
}

static void pwm_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_true(device_is_ready(output));
	zassert_true(device_is_ready(capture));
	zassert_true(gpio_is_ready_dt(&level));
	zassert_false(level.dt_flags & GPIO_ACTIVE_LOW);
	zassert_ok(pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(hw_fixture)),
				       PINCTRL_STATE_DEFAULT));
	zassert_ok(gpio_pin_configure_dt(&level, GPIO_INPUT));
	zassert_ok(pwm_get_cycles_per_sec(capture, CAPTURE_CHANNEL, &capture_rate));
	zassert_true(capture_rate > 0U);
	(void)pwm_disable_capture(capture, CAPTURE_CHANNEL);
	zassert_ok(pwm_set(output, OUTPUT_CHANNEL, PWM_MSEC(1), 0, PWM_POLARITY_NORMAL));
	k_msleep(QUIET_MS);
	prepare_capture(PWM_MSEC(1), 0, PWM_POLARITY_NORMAL, PWM_CAPTURE_TYPE_BOTH);
}

static void pwm_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)pwm_disable_capture(capture, CAPTURE_CHANNEL);
	(void)pwm_set(output, OUTPUT_CHANNEL, PWM_MSEC(1), 0, PWM_POLARITY_NORMAL);
	k_sem_reset(&captured);
}

static void single_capture(uint32_t period_ns, uint32_t pulse_ns, pwm_flags_t output_polarity,
			   pwm_flags_t flags)
{
	prepare_capture(period_ns, pulse_ns, output_polarity, flags);
	zassert_ok(pwm_configure_capture(capture, CAPTURE_CHANNEL, flags, pwm_captured,
					 &capture_state));
	zassert_ok(pwm_enable_capture(capture, CAPTURE_CHANNEL));
	zassert_ok(k_sem_take(&captured, K_MSEC(100)), "PWM0.P4/PB04 -> PWM1.P4/PB08 timeout");
	check_quiet(1U);
	zassert_ok(pwm_disable_capture(capture, CAPTURE_CHANNEL));
}

ZTEST(hpm_pwm_loop, test_single_capture_matrix)
{
	const uint32_t periods_us[] = {500U, 1000U, 2000U};
	const uint32_t duties[] = {25U, 50U, 75U};
	const pwm_flags_t polarities[] = {PWM_POLARITY_NORMAL, PWM_POLARITY_INVERTED};
	const pwm_flags_t types[] = {PWM_CAPTURE_TYPE_PERIOD, PWM_CAPTURE_TYPE_PULSE,
				     PWM_CAPTURE_TYPE_BOTH};
	uint32_t cases = 0U;
	uint64_t period = 0U;
	uint64_t pulse = 0U;

	for (size_t p = 0U; p < ARRAY_SIZE(periods_us); p++) {
		uint32_t period_ns = PWM_USEC(periods_us[p]);

		for (size_t d = 0U; d < ARRAY_SIZE(duties); d++) {
			uint32_t pulse_ns = period_ns * duties[d] / 100U;

			for (size_t o = 0U; o < ARRAY_SIZE(polarities); o++) {
				zassert_ok(pwm_set(output, OUTPUT_CHANNEL, period_ns, pulse_ns,
						   polarities[o]));
				k_usleep(periods_us[p] * 2U);
				for (size_t c = 0U; c < ARRAY_SIZE(polarities); c++) {
					for (size_t t = 0U; t < ARRAY_SIZE(types); t++) {
						pwm_flags_t flags = polarities[c] | types[t] |
								    PWM_CAPTURE_MODE_SINGLE;

						single_capture(period_ns, pulse_ns, polarities[o],
							       flags);
						cases++;
					}
				}
			}
		}
	}
	printk("PWM single capture: %u period/duty/polarity/type combinations\n", cases);

	zassert_ok(
		pwm_set(output, OUTPUT_CHANNEL, PWM_MSEC(1), PWM_USEC(250), PWM_POLARITY_NORMAL));
	k_msleep(QUIET_MS);
	zassert_ok(pwm_capture_usec(capture, CAPTURE_CHANNEL, PWM_CAPTURE_TYPE_BOTH, &period,
				    &pulse, K_MSEC(100)));
	zassert_within(period, 1000U, 10U, "PWM microsecond period conversion");
	zassert_within(pulse, 250U, 3U, "PWM microsecond pulse conversion");
	zassert_ok(pwm_capture_nsec(capture, CAPTURE_CHANNEL, PWM_CAPTURE_TYPE_BOTH, &period,
				    &pulse, K_MSEC(100)));
	zassert_within(period, PWM_MSEC(1), PWM_USEC(10), "PWM nanosecond period conversion");
	zassert_within(pulse, PWM_USEC(250), PWM_USEC(3), "PWM nanosecond pulse conversion");
	zassert_ok(pwm_disable_capture(capture, CAPTURE_CHANNEL));
	printk("PWM blocking capture: microsecond and nanosecond conversions passed\n");
}

ZTEST(hpm_pwm_loop, test_continuous_capture_wrap_disable_rearm)
{
	const pwm_flags_t flags = PWM_CAPTURE_TYPE_BOTH | PWM_CAPTURE_MODE_CONTINUOUS;
	uint64_t wrap_ms = DIV_ROUND_UP(CAPTURE_WRAP * MSEC_PER_SEC, capture_rate);
	uint32_t duration_ms;
	uint32_t count;
	uint32_t expected;
	int64_t started;
	int64_t elapsed;

	/* Include one complete free-running 28-bit counter wrap plus 250 periods. */
	zassert_true(wrap_ms <= 30000U, "Capture clock too slow for the bounded wrap test");
	duration_ms = (uint32_t)wrap_ms + 250U;
	zassert_ok(
		pwm_set(output, OUTPUT_CHANNEL, PWM_MSEC(1), PWM_USEC(250), PWM_POLARITY_NORMAL));
	k_msleep(QUIET_MS);
	prepare_capture(PWM_MSEC(1), PWM_USEC(250), PWM_POLARITY_NORMAL, flags);
	zassert_ok(pwm_configure_capture(capture, CAPTURE_CHANNEL, flags, pwm_captured,
					 &capture_state));
	zassert_equal(
		pwm_configure_capture(output, OUTPUT_CHANNEL, flags, pwm_captured, &capture_state),
		-EBUSY);
	started = k_uptime_get();
	zassert_ok(pwm_enable_capture(capture, CAPTURE_CHANNEL));
	zassert_equal(pwm_enable_capture(capture, CAPTURE_CHANNEL), -EBUSY);
	zassert_equal(pwm_configure_capture(capture, CAPTURE_CHANNEL, flags, pwm_captured,
					    &capture_state),
		      -EBUSY);
	/* Channel zero has no fixture pin: an incorrect success cannot drive PB08. */
	zassert_equal(pwm_set_cycles(capture, 0, 1000U, 250U, PWM_POLARITY_NORMAL), -EBUSY);
	k_msleep(duration_ms);
	zassert_ok(pwm_disable_capture(capture, CAPTURE_CHANNEL));
	elapsed = k_uptime_get() - started;
	count = atomic_get(&capture_state.count);
	expected = (uint32_t)elapsed;
	check_capture_errors();
	zassert_within(count, expected, MAX(expected / 100U, 4U),
		       "Continuous PWM callbacks lost or duplicated");
	k_sem_reset(&captured);
	check_quiet(count);
	printk("PWM continuous: rate=%llu Hz wrap=%llu ms ran=%lld ms callbacks=%u\n",
	       (unsigned long long)capture_rate, (unsigned long long)wrap_ms, (long long)elapsed,
	       count);

	/* Re-enable the existing configuration while the source continues running. */
	prepare_capture(PWM_MSEC(1), PWM_USEC(250), PWM_POLARITY_NORMAL, flags);
	zassert_ok(pwm_enable_capture(capture, CAPTURE_CHANNEL));
	zassert_ok(k_sem_take(&captured, K_MSEC(100)), "PWM capture did not rearm");
	k_msleep(10U);
	zassert_ok(pwm_disable_capture(capture, CAPTURE_CHANNEL));
	count = atomic_get(&capture_state.count);
	zassert_true(count >= 5U, "PWM continuous capture did not resume");
	k_sem_reset(&captured);
	check_quiet(count);
}

ZTEST(hpm_pwm_loop, test_zero_and_full_duty_constant_levels)
{
	const pwm_flags_t polarities[] = {PWM_POLARITY_NORMAL, PWM_POLARITY_INVERTED};
	const pwm_flags_t flags = PWM_CAPTURE_TYPE_BOTH | PWM_CAPTURE_MODE_CONTINUOUS;
	uint32_t period_cycles = 0U;
	uint32_t pulse_cycles = 0U;

	for (size_t p = 0U; p < ARRAY_SIZE(polarities); p++) {
		for (uint32_t full = 0U; full <= 1U; full++) {
			uint32_t pulse_ns = full ? PWM_MSEC(1) : 0U;
			int expected_level = full ^ (polarities[p] == PWM_POLARITY_INVERTED);

			zassert_ok(pwm_set(output, OUTPUT_CHANNEL, PWM_MSEC(1), pulse_ns,
					   polarities[p]));
			k_msleep(QUIET_MS);
			prepare_capture(PWM_MSEC(1), pulse_ns, polarities[p], flags);
			zassert_ok(pwm_configure_capture(capture, CAPTURE_CHANNEL, flags,
							 pwm_captured, &capture_state));
			zassert_ok(pwm_enable_capture(capture, CAPTURE_CHANNEL));
			for (uint32_t sample = 0U; sample < 20U; sample++) {
				zassert_equal(gpio_pin_get_dt(&level), expected_level,
					      "PWM 0/100%% physical level differs on PB11");
				k_msleep(1U);
			}
			check_quiet(0U);
			zassert_ok(pwm_disable_capture(capture, CAPTURE_CHANNEL));
		}
	}
	zassert_equal(pwm_capture_cycles(capture, CAPTURE_CHANNEL, PWM_CAPTURE_TYPE_BOTH,
					 &period_cycles, &pulse_cycles, K_MSEC(10)),
		      -EAGAIN);
	zassert_equal(pwm_enable_capture(capture, CAPTURE_CHANNEL), -EINVAL);
	zassert_ok(
		pwm_set(output, OUTPUT_CHANNEL, PWM_MSEC(1), PWM_USEC(250), PWM_POLARITY_NORMAL));
	k_msleep(QUIET_MS);
	single_capture(PWM_MSEC(1), PWM_USEC(250), PWM_POLARITY_NORMAL, PWM_CAPTURE_TYPE_BOTH);
	printk("PWM constant levels: 0/100%% normal/inverted, no capture callbacks\n");
	printk("PWM blocking timeout: callback released and capture recovered\n");
}

ZTEST_SUITE(hpm_pwm_loop, NULL, NULL, pwm_before, pwm_after, NULL);
