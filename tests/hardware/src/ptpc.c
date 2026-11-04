// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <stdint.h>

#include <zephyr/drivers/ptp_clock.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include <drivers/ptp_clock/hpmicro_ptpc.h>
#include <hpm_ptpc_drv.h>

#define PPS_SAMPLE_MS  1100
#define RATE_SAMPLE_MS 500

static const struct device *const ptpc0 = DEVICE_DT_GET(DT_NODELABEL(ptpc0));
static const struct device *const ptpc1 = DEVICE_DT_GET(DT_NODELABEL(ptpc1));
static PTPC_Type *const ptpc_base = (PTPC_Type *)DT_REG_ADDR(DT_PARENT(DT_NODELABEL(ptpc0)));

static atomic_t pps_event_count;
static uint32_t pps_event_flags;
static uint32_t compare0_events;
static uint32_t compare1_events;
static K_SEM_DEFINE(pps_event_seen, 0, 1);
static K_SEM_DEFINE(compare0_seen, 0, 1);
static K_SEM_DEFINE(compare1_seen, 0, 1);

static int64_t ptpc_time_to_ns(const struct net_ptp_time *time)
{
	return (int64_t)time->second * NSEC_PER_SEC + time->nanosecond;
}

static void ptpc_time_add_ns(struct net_ptp_time *time, uint32_t nanoseconds)
{
	uint64_t total = (uint64_t)time->nanosecond + nanoseconds;

	time->second += total / NSEC_PER_SEC;
	time->nanosecond = total % NSEC_PER_SEC;
}

static void pps_callback(const struct device *dev, uint32_t events, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	pps_event_flags |= events;
	atomic_inc(&pps_event_count);
	k_sem_give(&pps_event_seen);
}

static void compare_callback(const struct device *dev, uint32_t events, void *user_data)
{
	ARG_UNUSED(user_data);

	if (dev == ptpc0) {
		compare0_events |= events;
		k_sem_give(&compare0_seen);
	} else if (dev == ptpc1) {
		compare1_events |= events;
		k_sem_give(&compare1_seen);
	}
}

static void *ptpc_setup(void)
{
	struct net_ptp_time before0;
	struct net_ptp_time after0;
	struct net_ptp_time before1;
	struct net_ptp_time after1;

	zassert_true(device_is_ready(ptpc0));
	zassert_true(device_is_ready(ptpc1));

	/* Both counters must progress after driver initialization without an explicit set. */
	zassert_ok(ptp_clock_get(ptpc0, &before0));
	zassert_ok(ptp_clock_get(ptpc1, &before1));
	k_msleep(20);
	zassert_ok(ptp_clock_get(ptpc0, &after0));
	zassert_ok(ptp_clock_get(ptpc1, &after1));
	zassert_true(ptpc_time_to_ns(&after0) > ptpc_time_to_ns(&before0));
	zassert_true(ptpc_time_to_ns(&after1) > ptpc_time_to_ns(&before1));

	return NULL;
}

static void ptpc_after(void *fixture)
{
	const struct hpm_ptpc_compare_config disabled = {.enable = false};

	ARG_UNUSED(fixture);

	(void)hpm_ptpc_set_event_callback(ptpc0, 0, NULL, NULL);
	(void)hpm_ptpc_set_event_callback(ptpc1, 0, NULL, NULL);
	(void)hpm_ptpc_configure_compare(ptpc0, &disabled);
	(void)hpm_ptpc_configure_compare(ptpc1, &disabled);
	(void)hpm_ptpc_clear_events(ptpc0, HPM_PTPC_EVENT_ALL);
	(void)hpm_ptpc_clear_events(ptpc1, HPM_PTPC_EVENT_ALL);
	(void)ptp_clock_rate_adjust(ptpc0, 1.0);
	(void)ptp_clock_rate_adjust(ptpc1, 1.0);
	atomic_clear(&pps_event_count);
	pps_event_flags = 0U;
	compare0_events = 0U;
	compare1_events = 0U;
	k_sem_reset(&pps_event_seen);
	k_sem_reset(&compare0_seen);
	k_sem_reset(&compare1_seen);
}

static uint32_t measure_pps_edges(const struct device *dev, uint8_t control,
				  uint32_t edges_per_second)
{
	struct net_ptp_time now;
	uint32_t count_start;
	uint32_t count_end;
	uint32_t events;
	uint32_t elapsed_ms;
	uint32_t edges;
	uint32_t expected_edges;
	uint32_t measured_edges_per_second;
	int64_t time_start;
	int64_t time_end;
	int ret;

	zassert_ok(hpm_ptpc_clear_events(dev, HPM_PTPC_EVENT_ALL));
	zassert_ok(hpm_ptpc_set_event_callback(dev, HPM_PTPC_EVENT_PPS, pps_callback, NULL));
	atomic_clear(&pps_event_count);
	pps_event_flags = 0U;
	k_sem_reset(&pps_event_seen);
	zassert_ok(hpm_ptpc_configure_pps(dev, control));

	ret = k_sem_take(&pps_event_seen, K_SECONDS(3));
	zassert_ok(ret, "PPS control %u produced no interrupt", control);
	count_start = (uint32_t)atomic_get(&pps_event_count);
	time_start = k_uptime_get();
	k_msleep(PPS_SAMPLE_MS);
	time_end = k_uptime_get();
	count_end = (uint32_t)atomic_get(&pps_event_count);
	elapsed_ms = (uint32_t)(time_end - time_start);
	edges = count_end - count_start;

	zassert_ok(hpm_ptpc_get_events(dev, &events));
	zassert_true((events & HPM_PTPC_EVENT_PPS) != 0U, "PPS event was not latched");
	zassert_true((pps_event_flags & HPM_PTPC_EVENT_PPS) != 0U,
		     "PPS callback reported the wrong event flags");
	zassert_ok(hpm_ptpc_clear_events(dev, HPM_PTPC_EVENT_ALL));
	zassert_ok(hpm_ptpc_set_event_callback(dev, 0, NULL, NULL));

	expected_edges = (edges_per_second * elapsed_ms + 500U) / 1000U;
	zassert_true(edges + 3U >= expected_edges && edges <= expected_edges + 3U,
		     "PPS control %u: got %u edges, expected about %u in %u ms", control, edges,
		     expected_edges, elapsed_ms);
	measured_edges_per_second = (edges * 1000U + elapsed_ms / 2U) / elapsed_ms;
	printk("PTPC PPS control %u: %u edges in %u ms (%u edges/s, expected %u)\n", control, edges,
	       elapsed_ms, measured_edges_per_second, edges_per_second);

	zassert_ok(ptp_clock_get(dev, &now));
	zassert_true(now.second > 0U, "PTPC timer did not advance while PPS was active");

	return edges;
}

ZTEST(hpm_ptpc, test_time_set_and_adjust)
{
	struct net_ptp_time after0;
	struct net_ptp_time after1;
	struct net_ptp_time invalid;
	struct net_ptp_time initial = {.second = 10, .nanosecond = 0};
	int64_t elapsed;

	zassert_ok(ptp_clock_set(ptpc0, &initial));
	zassert_ok(ptp_clock_adjust(ptpc0, 200000000));
	zassert_ok(ptp_clock_get(ptpc0, &after0));
	elapsed = ptpc_time_to_ns(&after0) - 10LL * NSEC_PER_SEC;
	zassert_true(elapsed >= 190000000 && elapsed < 280000000,
		     "positive time adjustment produced %lld ns", elapsed);

	zassert_ok(ptp_clock_adjust(ptpc0, -200000000));
	zassert_ok(ptp_clock_get(ptpc0, &after0));
	elapsed = ptpc_time_to_ns(&after0) - 10LL * NSEC_PER_SEC;
	zassert_true(elapsed >= 0 && elapsed < 100000000,
		     "negative time adjustment produced %lld ns", elapsed);

	initial.second = 20U;
	zassert_ok(ptp_clock_set(ptpc1, &initial));
	zassert_ok(ptp_clock_get(ptpc1, &after1));
	elapsed = ptpc_time_to_ns(&after1) - 20LL * NSEC_PER_SEC;
	zassert_true(elapsed >= 0 && elapsed < 100000000);
	zassert_ok(ptp_clock_get(ptpc0, &after0));
	zassert_true(after0.second >= 10U && after0.second < 11U, "setting PTPC1 changed PTPC0");

	invalid = initial;
	invalid.second = (uint64_t)UINT32_MAX + 1U;
	zassert_equal(ptp_clock_set(ptpc0, &invalid), -EINVAL);
	invalid = initial;
	invalid.nanosecond = NSEC_PER_SEC;
	zassert_equal(ptp_clock_set(ptpc0, &invalid), -EINVAL);
	zassert_equal(ptp_clock_get(ptpc0, NULL), -EINVAL);
}

static int32_t measure_rate_ppm(const struct device *dev, double ratio)
{
	struct net_ptp_time start;
	struct net_ptp_time end;
	int64_t ptp_start;
	int64_t ptp_end;
	int64_t uptime_start;
	int64_t uptime_end;
	double measured;

	zassert_ok(ptp_clock_rate_adjust(dev, ratio));
	k_msleep(20);
	zassert_ok(ptp_clock_get(dev, &start));
	uptime_start = k_uptime_get();
	k_msleep(RATE_SAMPLE_MS);
	uptime_end = k_uptime_get();
	zassert_ok(ptp_clock_get(dev, &end));

	ptp_start = ptpc_time_to_ns(&start);
	ptp_end = ptpc_time_to_ns(&end);
	measured = (double)(ptp_end - ptp_start) /
		   ((double)(uptime_end - uptime_start) * (double)NSEC_PER_MSEC);

	return (int32_t)((measured - 1.0) * 1000000.0);
}

ZTEST(hpm_ptpc, test_rate_adjust)
{
	struct net_ptp_time initial = {.second = 30, .nanosecond = 0};
	int32_t ppm;

	zassert_ok(ptp_clock_set(ptpc0, &initial));
	ppm = measure_rate_ppm(ptpc0, 1.05);
	printk("PTPC rate 1.05 measured %d ppm\n", ppm);
	zassert_true(ppm >= 30000 && ppm <= 70000, "1.05 rate measured %d ppm", ppm);

	ppm = measure_rate_ppm(ptpc0, 0.95);
	printk("PTPC rate 0.95 measured %d ppm\n", ppm);
	zassert_true(ppm >= -70000 && ppm <= -30000, "0.95 rate measured %d ppm", ppm);

	zassert_equal(ptp_clock_rate_adjust(ptpc0, 0.0), -ERANGE);
	zassert_equal(ptp_clock_rate_adjust(ptpc0, -1.0), -ERANGE);
	zassert_ok(ptp_clock_rate_adjust(ptpc0, 1.0));
}

ZTEST(hpm_ptpc, test_pps_modes_and_events)
{
	struct net_ptp_time initial = {.second = 40, .nanosecond = 0};

	zassert_ok(ptp_clock_set(ptpc0, &initial));
	(void)measure_pps_edges(ptpc0, 0U, 1U);
	(void)measure_pps_edges(ptpc0, 1U, 2U);
	(void)measure_pps_edges(ptpc0, 2U, 4U);
	(void)measure_pps_edges(ptpc0, 4U, 16U);

	zassert_ok(hpm_ptpc_configure_pps(ptpc0, 15U));
	zassert_equal(PTPC_PTPC_PPS_CTRL_PPS_CTRL_GET(ptpc_base->PTPC[0].PPS_CTRL), 15U);
	zassert_equal(hpm_ptpc_configure_pps(ptpc0, 16U), -EINVAL);
	zassert_ok(hpm_ptpc_configure_pps(ptpc0, 0U));
}

ZTEST(hpm_ptpc, test_shared_irq_both_channels)
{
	struct net_ptp_time target0 = {.second = 100, .nanosecond = 0};
	struct net_ptp_time target1 = {.second = 200, .nanosecond = 0};
	struct hpm_ptpc_compare_config compare0 = {.target = target0, .enable = true};
	struct hpm_ptpc_compare_config compare1 = {.target = target1, .enable = true};
	uint32_t events;

	zassert_ok(ptp_clock_set(ptpc0, &target0));
	zassert_ok(ptp_clock_set(ptpc1, &target1));
	ptpc_time_add_ns(&compare0.target, 100000000U);
	ptpc_time_add_ns(&compare1.target, 150000000U);

	compare0_events = 0U;
	compare1_events = 0U;
	k_sem_reset(&compare0_seen);
	k_sem_reset(&compare1_seen);
	zassert_ok(
		hpm_ptpc_set_event_callback(ptpc0, HPM_PTPC_EVENT_COMPARE, compare_callback, NULL));
	zassert_ok(
		hpm_ptpc_set_event_callback(ptpc1, HPM_PTPC_EVENT_COMPARE, compare_callback, NULL));
	zassert_ok(hpm_ptpc_configure_compare(ptpc0, &compare0));
	zassert_ok(hpm_ptpc_configure_compare(ptpc1, &compare1));

	zassert_ok(k_sem_take(&compare0_seen, K_SECONDS(2)), "PTPC0 compare IRQ timeout");
	zassert_ok(k_sem_take(&compare1_seen, K_SECONDS(2)), "PTPC1 compare IRQ timeout");
	zassert_true((compare0_events & HPM_PTPC_EVENT_COMPARE) != 0U);
	zassert_true((compare1_events & HPM_PTPC_EVENT_COMPARE) != 0U);

	zassert_ok(hpm_ptpc_get_events(ptpc0, &events));
	zassert_true((events & HPM_PTPC_EVENT_COMPARE) != 0U);
	zassert_ok(hpm_ptpc_get_events(ptpc1, &events));
	zassert_true((events & HPM_PTPC_EVENT_COMPARE) != 0U);
}

ZTEST_SUITE(hpm_ptpc, NULL, ptpc_setup, NULL, ptpc_after, NULL);
