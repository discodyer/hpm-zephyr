/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ztest.h>

PINCTRL_DT_DEFINE(DT_NODELABEL(hw_fixture));

static const struct gpio_dt_spec output = GPIO_DT_SPEC_GET(DT_NODELABEL(hw_fixture), out_gpios);
static const struct gpio_dt_spec input = GPIO_DT_SPEC_GET(DT_NODELABEL(hw_fixture), in_gpios);
static struct gpio_callback callback;
static K_SEM_DEFINE(edge, 0, 1);
static atomic_t edges;
static atomic_t callback_errors;
static bool callback_added;
static bool output_configured;

static void gpio_edge(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
	if (!k_is_in_isr() || (port != input.port) || (cb != &callback) ||
	    (pins != BIT(input.pin))) {
		atomic_inc(&callback_errors);
	}
	atomic_inc(&edges);
	k_sem_give(&edge);
}

static void gpio_check_level(int level)
{
	gpio_port_value_t value;

	/* Allow pad propagation before sampling the separate input through the jumper. */
	k_busy_wait(10);
	zassert_equal(gpio_pin_get_dt(&output), level, "Output pad readback differs");
	zassert_equal(gpio_pin_get_dt(&input), level, "Check the output-to-input jumper");
	zassert_ok(gpio_port_get_raw(input.port, &value));
	zassert_equal(!!(value & BIT(input.pin)), level, "Raw input differs from pin API");
}

static void gpio_check_quiet(atomic_val_t expected)
{
	zassert_equal(k_sem_take(&edge, K_MSEC(2)), -EAGAIN, "Unexpected GPIO callback");
	zassert_equal(atomic_get(&edges), expected, "Lost or duplicate GPIO IRQ");
	zassert_equal(atomic_get(&callback_errors), 0, "Invalid GPIO callback context or source");
}

static void gpio_drive_and_check(int level, bool expect_irq)
{
	atomic_val_t previous = atomic_get(&edges);

	zassert_ok(gpio_pin_set_dt(&output, level));
	if (expect_irq) {
		zassert_ok(k_sem_take(&edge, K_MSEC(100)), "No GPIO IRQ; check the jumper");
	}
	gpio_check_level(level);
	gpio_check_quiet(previous + (expect_irq ? 1 : 0));
}

static void gpio_masked_write_and_check(int level, bool expect_irq)
{
	const gpio_port_pins_t mask = BIT(output.pin);
	atomic_val_t previous = atomic_get(&edges);

	/* Bits outside the mask must be ignored, including when rewriting a level. */
	zassert_ok(gpio_port_set_masked_raw(output.port, mask, level ? UINT32_MAX : ~mask));
	if (expect_irq) {
		zassert_ok(k_sem_take(&edge, K_MSEC(100)), "No GPIO IRQ after masked write");
	}
	gpio_check_level(level);
	gpio_check_quiet(previous + (expect_irq ? 1 : 0));
}

static void gpio_start_interrupt(gpio_flags_t flags, int initial_level)
{
	zassert_ok(gpio_pin_interrupt_configure_dt(&input, GPIO_INT_DISABLE));
	zassert_ok(gpio_pin_set_dt(&output, initial_level));
	gpio_check_level(initial_level);
	atomic_clear(&edges);
	atomic_clear(&callback_errors);
	k_sem_reset(&edge);
	zassert_ok(gpio_pin_interrupt_configure_dt(&input, flags));
	gpio_check_quiet(0);
}

static void gpio_before(void *fixture)
{
	ARG_UNUSED(fixture);
	callback_added = false;
	output_configured = false;
	zassert_true(gpio_is_ready_dt(&output));
	zassert_true(gpio_is_ready_dt(&input));
	/* Raw-port assertions below use physical high/low values. */
	zassert_false(output.dt_flags & GPIO_ACTIVE_LOW);
	zassert_false(input.dt_flags & GPIO_ACTIVE_LOW);
	zassert_ok(pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(hw_fixture)),
				       PINCTRL_STATE_DEFAULT));
	zassert_ok(gpio_pin_interrupt_configure_dt(&input, GPIO_INT_DISABLE));
	zassert_ok(gpio_pin_configure_dt(&output, GPIO_OUTPUT_INACTIVE));
	output_configured = true;
	zassert_ok(gpio_pin_configure_dt(&input, GPIO_INPUT));
	atomic_clear(&edges);
	atomic_clear(&callback_errors);
	k_sem_reset(&edge);
	gpio_init_callback(&callback, gpio_edge, BIT(input.pin));
	zassert_ok(gpio_add_callback(input.port, &callback));
	callback_added = true;
	gpio_check_level(0);
}

static void gpio_after(void *fixture)
{
	ARG_UNUSED(fixture);
	if (gpio_is_ready_dt(&input)) {
		(void)gpio_pin_interrupt_configure_dt(&input, GPIO_INT_DISABLE);
	}
	if (callback_added) {
		(void)gpio_remove_callback(input.port, &callback);
		callback_added = false;
	}
	if (output_configured) {
		(void)gpio_pin_configure_dt(&output, GPIO_INPUT);
		output_configured = false;
	}
	k_sem_reset(&edge);
}

ZTEST(hpm_gpio_loop, test_levels_and_raw_port_operations)
{
	const gpio_port_pins_t mask = BIT(output.pin);

	for (int i = 0; i < 20; ++i) {
		gpio_drive_and_check((i + 1) & 1, false);
	}
	zassert_ok(gpio_port_set_bits_raw(output.port, mask));
	gpio_check_level(1);
	zassert_ok(gpio_port_clear_bits_raw(output.port, mask));
	gpio_check_level(0);
	zassert_ok(gpio_port_set_masked_raw(output.port, mask, mask));
	gpio_check_level(1);
	zassert_ok(gpio_port_set_masked_raw(output.port, mask, 0));
	gpio_check_level(0);
	zassert_ok(gpio_port_toggle_bits(output.port, mask));
	gpio_check_level(1);
	zassert_ok(gpio_port_toggle_bits(output.port, mask));
	gpio_check_level(0);
	gpio_check_quiet(0);
	printk("GPIO output pin %u -> input pin %u: levels and raw set/clear/mask/toggle OK\n",
	       output.pin, input.pin);
}

ZTEST(hpm_gpio_loop, test_rising_and_falling_interrupts)
{
	gpio_start_interrupt(GPIO_INT_EDGE_RISING, 0);
	for (int i = 0; i < 20; ++i) {
		gpio_drive_and_check(1, true);
		gpio_drive_and_check(0, false);
	}
	zassert_equal(atomic_get(&edges), 20);
	gpio_start_interrupt(GPIO_INT_EDGE_FALLING, 1);
	for (int i = 0; i < 20; ++i) {
		gpio_drive_and_check(0, true);
		gpio_drive_and_check(1, false);
	}
	zassert_equal(atomic_get(&edges), 20);
	printk("GPIO single-edge IRQs: 20 rising and 20 falling, opposite edges ignored\n");
}

ZTEST(hpm_gpio_loop, test_both_edge_interrupts)
{
	gpio_start_interrupt(GPIO_INT_EDGE_BOTH, 0);
	for (int i = 0; i < 20; ++i) {
		gpio_drive_and_check((i + 1) & 1, true);
	}
	zassert_equal(atomic_get(&edges), 20);
	gpio_masked_write_and_check(0, false);
	gpio_masked_write_and_check(1, true);
	gpio_masked_write_and_check(1, false);
	gpio_masked_write_and_check(0, true);
	gpio_masked_write_and_check(0, false);
	zassert_equal(atomic_get(&edges), 22);
	printk("GPIO both-edge IRQs: 20 pin writes and 2 masked transitions; same-level masked "
	       "writes were quiet\n");
}

ZTEST(hpm_gpio_loop, test_interrupt_disable_and_reenable)
{
	gpio_start_interrupt(GPIO_INT_EDGE_BOTH, 0);
	gpio_drive_and_check(1, true);
	zassert_ok(gpio_pin_interrupt_configure_dt(&input, GPIO_INT_DISABLE));
	for (int i = 0; i < 20; ++i) {
		gpio_drive_and_check(i & 1, false);
	}
	zassert_equal(atomic_get(&edges), 1);
	/* Pending disabled edges must not be delivered when the mode is rearmed. */
	gpio_start_interrupt(GPIO_INT_EDGE_BOTH, 1);
	gpio_drive_and_check(0, true);
	gpio_drive_and_check(1, true);
	zassert_equal(atomic_get(&edges), 2);
	printk("GPIO IRQ disable: 20 transitions without callbacks; reenable restored both "
	       "edges\n");
}

ZTEST_SUITE(hpm_gpio_loop, NULL, NULL, gpio_before, gpio_after, NULL);
