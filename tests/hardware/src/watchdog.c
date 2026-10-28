/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#define TIMEOUT_MS   500U
#define TOLERANCE_MS 80U

static const struct device *const watchdog = DEVICE_DT_GET(DT_NODELABEL(ewdg0));
static K_SEM_DEFINE(expired, 0, 1);
static int64_t irq_time;
static const struct device *irq_dev;
static int irq_channel;
static bool irq_in_isr;
static atomic_t irq_count;
static atomic_t feed_once;
static int callback_feed_result;

static void watchdog_cb(const struct device *dev, int channel)
{
	irq_time = k_uptime_get();
	irq_dev = dev;
	irq_channel = channel;
	irq_in_isr = k_is_in_isr();
	atomic_inc(&irq_count);
	if (atomic_cas(&feed_once, 1, 0)) {
		callback_feed_result = wdt_feed(dev, channel);
	}
	k_sem_give(&expired);
}

static void watchdog_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_true(device_is_ready(watchdog));
	k_sem_reset(&expired);
	atomic_clear(&irq_count);
	atomic_clear(&feed_once);
	callback_feed_result = -EINPROGRESS;
}

static void watchdog_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)wdt_disable(watchdog);
	k_sem_reset(&expired);
}

static int watchdog_start(uint32_t timeout_ms, int64_t *started)
{
	const struct wdt_timeout_cfg timeout = {
		.window = {.min = 0, .max = timeout_ms},
		.callback = watchdog_cb,
		.flags = WDT_FLAG_RESET_NONE,
	};
	int channel;

	channel = wdt_install_timeout(watchdog, &timeout);
	zassert_true(channel >= 0);
	*started = k_uptime_get();
	zassert_ok(wdt_setup(watchdog, 0));

	return channel;
}

static void watchdog_wait(int channel, int64_t started, uint32_t timeout_ms, uint32_t tolerance_ms)
{
	zassert_ok(k_sem_take(&expired, K_MSEC(timeout_ms + 1000U)), "Watchdog did not expire");
	printk("Watchdog IRQ after start/feed: %lld ms (expected %u +/- %u ms)\n",
	       (long long)(irq_time - started), timeout_ms, tolerance_ms);
	zassert_true(irq_in_isr, "Watchdog callback was not in ISR context");
	zassert_equal_ptr(irq_dev, watchdog, "Incorrect callback device");
	zassert_equal(irq_channel, channel, "Incorrect callback channel");
	zassert_within(irq_time - started, timeout_ms, tolerance_ms,
		       "Watchdog timeout outside tolerance");
}

ZTEST(hpm_watchdog_irq, test_feed_then_repeat_timeout_without_reset)
{
	int64_t fed;
	int channel = watchdog_start(TIMEOUT_MS, &fed);

	for (int i = 0; i < 3; ++i) {
		k_msleep(100);
		zassert_ok(wdt_feed(watchdog, channel));
		zassert_equal(k_sem_take(&expired, K_NO_WAIT), -EBUSY, "Premature watchdog IRQ");
	}
	fed = k_uptime_get();
	watchdog_wait(channel, fed, TIMEOUT_MS, TOLERANCE_MS);
	zassert_equal(atomic_get(&irq_count), 1, "Repeated IRQ before feeding");
	/* Feed after the full undivided counter range, with no reset requested. */
	k_msleep(2200);
	zassert_equal(atomic_get(&irq_count), 1, "Repeated IRQ while waiting to feed");
	zassert_ok(wdt_feed(watchdog, channel));
	fed = k_uptime_get();
	watchdog_wait(channel, fed, TIMEOUT_MS, TOLERANCE_MS);
	zassert_equal(atomic_get(&irq_count), 2);
	zassert_ok(wdt_disable(watchdog));
}

ZTEST(hpm_watchdog_irq, test_feed_from_callback)
{
	int64_t started;
	int channel;

	atomic_set(&feed_once, 1);
	channel = watchdog_start(TIMEOUT_MS, &started);
	watchdog_wait(channel, started, TIMEOUT_MS, TOLERANCE_MS);
	zassert_ok(callback_feed_result, "Watchdog feed failed in callback");
	started = irq_time;
	watchdog_wait(channel, started, TIMEOUT_MS, TOLERANCE_MS);
	zassert_equal(atomic_get(&irq_count), 2);
	zassert_ok(wdt_disable(watchdog));
}

ZTEST(hpm_watchdog_irq, test_disable_and_reinstall)
{
	int64_t started;
	int channel = watchdog_start(TIMEOUT_MS, &started);

	k_msleep(300);
	zassert_ok(wdt_disable(watchdog));
	zassert_equal(k_sem_take(&expired, K_MSEC(TIMEOUT_MS + TOLERANCE_MS)), -EAGAIN,
		      "Watchdog IRQ after disable");
	zassert_equal(atomic_get(&irq_count), 0);
	channel = watchdog_start(TIMEOUT_MS, &started);
	watchdog_wait(channel, started, TIMEOUT_MS, TOLERANCE_MS);
	zassert_equal(atomic_get(&irq_count), 1);
	zassert_ok(wdt_disable(watchdog));
}

ZTEST(hpm_watchdog_irq, test_reconfigure_longer_timeout)
{
	int64_t started;
	int channel = watchdog_start(TIMEOUT_MS, &started);

	watchdog_wait(channel, started, TIMEOUT_MS, TOLERANCE_MS);
	zassert_ok(wdt_disable(watchdog));
	/* 2500 ms exceeds the 16-bit counter range at the 32 kHz source clock. */
	channel = watchdog_start(2500U, &started);
	watchdog_wait(channel, started, 2500U, 160U);
	zassert_equal(atomic_get(&irq_count), 2);
	zassert_ok(wdt_disable(watchdog));
}

ZTEST_SUITE(hpm_watchdog_irq, NULL, NULL, watchdog_before, watchdog_after, NULL);
