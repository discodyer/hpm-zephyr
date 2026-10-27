/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/crc.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/drivers/otp.h>
#include <zephyr/drivers/ptp_clock.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/irq_offload.h>
#include <zephyr/ztest.h>
#include <drivers/mux/hpmicro_trgm.h>
#include <drivers/ptp_clock/hpmicro_ptpc.h>

static const struct device *const timer = DEVICE_DT_GET(DT_NODELABEL(gptmr0));
static const struct device *const tsns = DEVICE_DT_GET(DT_NODELABEL(tsns0));
static const struct device *const ptpc = DEVICE_DT_GET(DT_NODELABEL(ptpc0));
static const struct device *const mbx_a = DEVICE_DT_GET(DT_NODELABEL(mbx0a));
static const struct device *const mbx_b = DEVICE_DT_GET(DT_NODELABEL(mbx0b));
static K_SEM_DEFINE(irq_done, 0, 1);
static uint32_t irq_word;
static uint32_t ptpc_events;
static int rng_isr_result;
static uint32_t rng_isr_cycles;
static bool rng_in_isr;

static void alarm_cb(const struct device *dev, uint8_t chan, uint32_t ticks, void *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);
	ARG_UNUSED(ticks);
	k_sem_give(data);
}

static void mailbox_cb(const struct device *dev, uint32_t chan, void *data, struct mbox_msg *msg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);
	if (msg != NULL && msg->size == sizeof(irq_word)) {
		memcpy(&irq_word, msg->data, sizeof(irq_word));
		k_sem_give(data);
	}
}

static struct sensor_trigger temp_trigger = {
	.type = SENSOR_TRIG_THRESHOLD,
	.chan = SENSOR_CHAN_DIE_TEMP,
};

static void temperature_cb(const struct device *dev, const struct sensor_trigger *trigger)
{
	ARG_UNUSED(trigger);
	/* Disable before acknowledging a continuously asserted threshold. */
	(void)sensor_trigger_set(dev, &temp_trigger, NULL);
	k_sem_give(&irq_done);
}

static void ptpc_cb(const struct device *dev, uint32_t events, void *data)
{
	ARG_UNUSED(dev);
	ptpc_events = events;
	k_sem_give(data);
}

static void rng_offload(const void *data)
{
	uint8_t buffer[16];
	uint32_t started = k_cycle_get_32();

	rng_in_isr = k_is_in_isr();
	rng_isr_result = entropy_get_entropy_isr(data, buffer, sizeof(buffer), 0);
	rng_isr_cycles = k_cycle_get_32() - started;
}

static void core_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)counter_cancel_channel_alarm(timer, 0);
	(void)counter_stop(timer);
	(void)mbox_set_enabled(mbx_a, 0, false);
	(void)mbox_set_enabled(mbx_b, 0, false);
	(void)mbox_register_callback(mbx_a, 0, NULL, NULL);
	(void)mbox_register_callback(mbx_b, 0, NULL, NULL);
	(void)sensor_trigger_set(tsns, &temp_trigger, NULL);
	(void)hpm_ptpc_set_event_callback(ptpc, 0, NULL, NULL);
	struct hpm_ptpc_compare_config disabled = {.enable = false};

	(void)hpm_ptpc_configure_compare(ptpc, &disabled);
	(void)hpm_ptpc_configure_pps(ptpc, 0);
	const struct hpm_ptpc_capture_config capture_off = {.edge = HPM_PTPC_CAPTURE_DISABLED};
	const struct hpm_trgm_output_config route_off = {0};

	(void)hpm_ptpc_configure_capture(ptpc, &capture_off);
	(void)hpm_trgm_configure_output(DEVICE_DT_GET(DT_NODELABEL(trgm0)), 46, &route_off);
	k_sem_reset(&irq_done);
}

ZTEST(hpm_core, test_timer_alarm_and_clock_recovery)
{
	const struct device *clk = DEVICE_DT_GET(DT_CLOCKS_CTLR(DT_NODELABEL(gptmr0)));
	clock_control_subsys_t id =
		(clock_control_subsys_t)DT_CLOCKS_CELL(DT_NODELABEL(gptmr0), name);
	struct counter_alarm_cfg alarm = {.callback = alarm_cb, .user_data = &irq_done};
	uint32_t rate;
	uint32_t before;
	uint32_t after;
	int64_t started;
	int wait_ret;

	zassert_true(device_is_ready(timer));
	zassert_ok(counter_stop(timer));
	zassert_ok(clock_control_off(clk, id));
	zassert_ok(clock_control_on(clk, id));
	zassert_ok(clock_control_get_rate(clk, id, &rate));
	zassert_true(rate > 0);
	zassert_ok(counter_start(timer));
	zassert_ok(counter_get_value(timer, &before));
	alarm.ticks = counter_us_to_ticks(timer, 20000);
	started = k_uptime_get();
	zassert_ok(counter_set_channel_alarm(timer, 0, &alarm));
	wait_ret = k_sem_take(&irq_done, K_SECONDS(1));
	if (wait_ret != 0) {
		uint32_t pending = counter_get_pending_int(timer);

		(void)counter_get_value(timer, &after);
		printk("GPTMR alarm diagnostic: before=%u after=%u pending=0x%x\n", before, after,
		       pending);
	}
	zassert_ok(wait_ret, "GPTMR alarm did not interrupt");
	zassert_within(k_uptime_get() - started, 20, 10, "GPTMR/system timer disagreement");
	zassert_ok(counter_get_value(timer, &after));
	zassert_not_equal(before, after);
}

ZTEST(hpm_core, test_rng_selftest_and_data)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(rng));
	uint8_t first[64];
	uint8_t second[64];
	bool varied = false;

	zassert_true(device_is_ready(dev), "RNG initialization/self-test failed");
	zassert_ok(entropy_get_entropy(dev, first, sizeof(first)));
	zassert_ok(entropy_get_entropy(dev, second, sizeof(second)));
	zassert_not_equal(memcmp(first, second, sizeof(first)), 0, "Repeated RNG block");
	for (size_t i = 1; i < sizeof(first); ++i) {
		varied |= first[i] != first[0];
	}
	zassert_true(varied, "Stuck RNG output");
}

ZTEST(hpm_core, test_crc32_streaming_known_answer)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(crc0));
	const uint8_t input[] = "123456789";
	struct crc_ctx ctx = {
		.type = CRC32_IEEE,
		.seed = 0xffffffff,
		.polynomial = 0x04c11db7,
		.reversed = CRC_FLAG_REVERSE_INPUT | CRC_FLAG_REVERSE_OUTPUT,
	};
	int first;
	int second;
	int finish;

	zassert_true(device_is_ready(dev));
	zassert_ok(crc_begin(dev, &ctx));
	first = crc_update(dev, &ctx, input, 4);
	second = crc_update(dev, &ctx, input + 4, 5);
	finish = crc_finish(dev, &ctx);
	zassert_ok(first);
	zassert_ok(second);
	zassert_ok(finish);
	zassert_equal(ctx.result, 0xcbf43926, "CRC32/ISO-HDLC vector mismatch");
}

ZTEST(hpm_core, test_rng_nonblocking_isr_budget)
{
	irq_offload(rng_offload, DEVICE_DT_GET(DT_NODELABEL(rng)));
	zassert_true(rng_in_isr);
	zassert_true(rng_isr_result >= 0 && rng_isr_result <= 16);
	zassert_true(k_cyc_to_us_ceil32(rng_isr_cycles) < 500, "Nonblocking RNG blocked in ISR");
}

ZTEST(hpm_core, test_temperature_and_threshold_interrupt)
{
	struct sensor_value value;
	struct sensor_value lower = {.val1 = -50};
	struct sensor_value upper;

	zassert_true(device_is_ready(tsns));
	zassert_ok(sensor_sample_fetch(tsns));
	zassert_ok(sensor_channel_get(tsns, SENSOR_CHAN_DIE_TEMP, &value));
	zassert_true(value.val1 > -40 && value.val1 < 125, "Implausible die temperature");
	printk("TSNS die temperature: %d.%06d C\n", value.val1, value.val2);
	upper = value;
	upper.val1 -= 5;
	/* Deliberately place the upper threshold below the current temperature. */
	zassert_ok(sensor_attr_set(tsns, SENSOR_CHAN_DIE_TEMP, SENSOR_ATTR_LOWER_THRESH, &lower));
	zassert_ok(sensor_attr_set(tsns, SENSOR_CHAN_DIE_TEMP, SENSOR_ATTR_UPPER_THRESH, &upper));
	zassert_ok(sensor_trigger_set(tsns, &temp_trigger, temperature_cb));
	zassert_ok(k_sem_take(&irq_done, K_SECONDS(2)), "TSNS threshold IRQ did not arrive");
}

ZTEST(hpm_core, test_mailbox_bidirectional_order)
{
	zassert_true(device_is_ready(mbx_a));
	zassert_true(device_is_ready(mbx_b));
	for (unsigned int direction = 0; direction < 2; ++direction) {
		const struct device *tx = direction ? mbx_b : mbx_a;
		const struct device *rx = direction ? mbx_a : mbx_b;

		zassert_ok(mbox_register_callback(rx, 0, mailbox_cb, &irq_done));
		zassert_ok(mbox_set_enabled(rx, 0, true));
		for (uint32_t i = 0; i < 32; ++i) {
			uint32_t word = 0xa5360000 | (direction << 8) | i;
			struct mbox_msg msg = {.data = &word, .size = sizeof(word)};

			zassert_ok(mbox_send(tx, 0, &msg));
			zassert_ok(k_sem_take(&irq_done, K_SECONDS(1)), "MBX receive IRQ timeout");
			zassert_equal(irq_word, word, "MBX corruption or ordering error");
		}
		zassert_ok(mbox_set_enabled(rx, 0, false));
	}
}

ZTEST(hpm_core, test_otp_read_only_repeatability)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(otp));
	/* Public UUID words 88..91; never print identifiers or read key regions. */
	uint32_t first[4];
	uint32_t second[4];

	zassert_true(device_is_ready(dev));
	zassert_ok(otp_read(dev, 88 * sizeof(uint32_t), first, sizeof(first)));
	zassert_ok(otp_read(dev, 88 * sizeof(uint32_t), second, sizeof(second)));
	zassert_mem_equal(first, second, sizeof(first));
}

ZTEST(hpm_core, test_ptpc_progress_and_compare_irq)
{
	struct net_ptp_time initial = {.second = 10, .nanosecond = 0};
	struct net_ptp_time now;
	struct hpm_ptpc_compare_config compare = {
		.target = {.second = 10, .nanosecond = 100000000},
		.enable = true,
	};

	zassert_true(device_is_ready(ptpc));
	zassert_ok(ptp_clock_set(ptpc, &initial));
	const struct hpm_ptpc_capture_config capture = {
		.edge = HPM_PTPC_CAPTURE_RISING,
		.keep = true,
	};
	/* PTPC compare0 -> TRGM -> PTPC capture0, without an external jumper. */
	const struct hpm_trgm_output_config route = {.source = 4};
	int wait_ret;

	zassert_ok(hpm_trgm_configure_output(DEVICE_DT_GET(DT_NODELABEL(trgm0)), 46, &route));
	zassert_ok(hpm_ptpc_configure_capture(ptpc, &capture));
	zassert_ok(hpm_ptpc_clear_events(ptpc, HPM_PTPC_EVENT_ALL));
	zassert_ok(hpm_ptpc_set_event_callback(ptpc, HPM_PTPC_EVENT_COMPARE, ptpc_cb, &irq_done));
	zassert_ok(hpm_ptpc_configure_compare(ptpc, &compare));
	wait_ret = k_sem_take(&irq_done, K_SECONDS(2));
	if (wait_ret != 0) {
		uint32_t events;

		(void)ptp_clock_get(ptpc, &now);
		(void)hpm_ptpc_get_events(ptpc, &events);
		printk("PTPC diagnostic: time=%u.%09u events=0x%x\n", (uint32_t)now.second,
		       now.nanosecond, events);
	}
	zassert_ok(wait_ret, "PTPC compare IRQ timeout");
	zassert_true(ptpc_events & HPM_PTPC_EVENT_COMPARE);
	zassert_ok(ptp_clock_get(ptpc, &now));
	zassert_equal(now.second, 10);
	zassert_true(now.nanosecond >= 100000000 && now.nanosecond < 300000000);
	zassert_ok(hpm_ptpc_read_capture(ptpc, &now));
	zassert_equal(now.second, compare.target.second);
	zassert_within(now.nanosecond, compare.target.nanosecond, 10000,
		       "PTPC internal capture missed the compare edge");
}

ZTEST_SUITE(hpm_core, NULL, NULL, NULL, core_after, NULL);
