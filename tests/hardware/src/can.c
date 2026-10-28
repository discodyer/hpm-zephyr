/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <zephyr/drivers/can.h>
#include <zephyr/drivers/ptp_clock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>
#include <drivers/can/hpmicro_mcan.h>
#include <drivers/ptp_clock/hpmicro_ptpc.h>

#define CALLBACK_BAD_CONTEXT BIT(0)
#define CALLBACK_BAD_DEVICE  BIT(1)
#define CALLBACK_BAD_USER    BIT(2)
#define CALLBACK_RX_OVERFLOW BIT(3)
#define TSU_64BIT_SLOTS      (HPM_MCAN_TSU_SLOT_COUNT / 2U)

static const struct device *const can = DEVICE_DT_GET(DT_NODELABEL(mcan0));
CAN_MSGQ_DEFINE(received, 4);
static K_SEM_DEFINE(tx_done, 0, 1);
static atomic_t callback_errors;
static atomic_t rx_count;
static atomic_t tx_count;
static int tx_result;
static int filter_id = -1;

static void check_callback(const struct device *dev, void *user_data, const void *expected)
{
	if (!k_is_in_isr()) {
		atomic_or(&callback_errors, CALLBACK_BAD_CONTEXT);
	}
	if (dev != can) {
		atomic_or(&callback_errors, CALLBACK_BAD_DEVICE);
	}
	if (user_data != expected) {
		atomic_or(&callback_errors, CALLBACK_BAD_USER);
	}
}

static void tx_callback(const struct device *dev, int error, void *user_data)
{
	check_callback(dev, user_data, &tx_done);
	tx_result = error;
	atomic_inc(&tx_count);
	k_sem_give(&tx_done);
}

static void rx_callback(const struct device *dev, struct can_frame *frame, void *user_data)
{
	check_callback(dev, user_data, &received);
	atomic_inc(&rx_count);
	if (k_msgq_put(&received, frame, K_NO_WAIT) != 0) {
		atomic_or(&callback_errors, CALLBACK_RX_OVERFLOW);
	}
}

static void can_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_true(device_is_ready(can));
	atomic_clear(&callback_errors);
	atomic_clear(&rx_count);
	atomic_clear(&tx_count);
	k_sem_reset(&tx_done);
	k_msgq_purge(&received);
}

static void can_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)can_stop(can);
	if (filter_id >= 0) {
		can_remove_rx_filter(can, filter_id);
		filter_id = -1;
	}
	k_msgq_purge(&received);
	k_sem_reset(&tx_done);
}

static void exchange_frame(const struct can_frame *tx, struct can_frame *rx)
{
	tx_result = -EINPROGRESS;
	zassert_ok(can_send(can, tx, K_SECONDS(1), tx_callback, &tx_done));
	zassert_ok(k_sem_take(&tx_done, K_SECONDS(1)), "MCAN TX completion IRQ timeout");
	zassert_ok(tx_result, "MCAN TX failed");
	zassert_ok(k_msgq_get(&received, rx, K_SECONDS(1)), "MCAN RX interrupt timeout");
	zassert_equal(atomic_get(&callback_errors), 0, "Invalid CAN callback or RX queue overflow");
	zassert_equal(rx->id, tx->id);
	zassert_equal(rx->dlc, tx->dlc);
	zassert_equal(rx->flags & (CAN_FRAME_IDE | CAN_FRAME_RTR | CAN_FRAME_FDF | CAN_FRAME_BRS |
				   CAN_FRAME_ESI),
		      tx->flags);
	zassert_mem_equal(rx->data, tx->data, can_dlc_to_bytes(tx->dlc));
}

static void check_completion_count(unsigned int expected)
{
	struct can_frame extra;

	zassert_equal(k_msgq_get(&received, &extra, K_MSEC(2)), -EAGAIN,
		      "Unexpected extra CAN frame");
	zassert_equal(atomic_get(&rx_count), expected, "Incorrect RX callback count");
	zassert_equal(atomic_get(&tx_count), expected, "Incorrect TX callback count");
	zassert_equal(atomic_get(&callback_errors), 0);
	zassert_ok(can_stop(can));
}

ZTEST(hpm_can_loop, test_classic_and_fd_payloads)
{
	const struct can_filter filter = {.id = 0x536, .mask = CAN_STD_ID_MASK};
	struct can_frame tx = {.id = 0x536, .dlc = 8};
	struct can_frame rx;

	zassert_ok(can_set_mode(can, CAN_MODE_LOOPBACK | CAN_MODE_LISTENONLY | CAN_MODE_FD));
	filter_id = can_add_rx_filter(can, rx_callback, &received, &filter);
	zassert_true(filter_id >= 0);
	zassert_ok(can_start(can));
	for (int n = 0; n < 16; ++n) {
		tx.flags = n & 1 ? CAN_FRAME_FDF | CAN_FRAME_BRS : 0;
		tx.dlc = n & 1 ? 15 : 8;
		size_t size = can_dlc_to_bytes(tx.dlc);

		for (size_t i = 0; i < size; ++i) {
			tx.data[i] = n * 17 + i;
		}
		exchange_frame(&tx, &rx);
	}
	check_completion_count(16);
	printk("MCAN silent loopback: 8 classic 8-byte and 8 FD/BRS 64-byte frames, TX/RX ISR "
	       "OK\n");
}

static uint64_t ptpc_time_ns(const struct net_ptp_time *time)
{
	return time->second * NSEC_PER_SEC + time->nanosecond;
}

ZTEST(hpm_can_loop, test_ptpc_timestamp_capture)
{
	const struct device *ptpc = DEVICE_DT_GET(DT_NODELABEL(ptpc0));
	struct net_ptp_time initial = {.second = 10};
	const struct hpm_mcan_tsu_config config = {
		.timebase = HPM_MCAN_TSU_TIMEBASE_PTPC,
		.timebase_input = HPM_MCAN_TSU_TIMEBASE_INPUT_0,
		.capture_on_sof = true,
		.use_64bit = true,
	};
	const struct can_filter filter = {.id = 0x537, .mask = CAN_STD_ID_MASK};
	struct can_frame tx = {.id = 0x537, .dlc = 1};
	struct can_frame rx;
	uint64_t previous = 0;
	uint32_t seen_slots = 0;
	uint8_t previous_slot = 0;

	zassert_true(device_is_ready(ptpc));
	zassert_ok(ptp_clock_set(ptpc, &initial));
	zassert_ok(hpm_ptpc_configure_tsu_input(ptpc, 0, HPM_PTPC_TSU_INPUT_PTPC0));
	zassert_ok(can_set_mode(can, CAN_MODE_LOOPBACK | CAN_MODE_LISTENONLY));
	zassert_ok(hpm_mcan_configure_tsu(can, &config));
	filter_id = can_add_rx_filter(can, rx_callback, &received, &filter);
	zassert_true(filter_id >= 0);
	zassert_ok(hpm_mcan_set_rx_filter_timestamp(can, filter_id, true));
	zassert_ok(can_start(can));
	for (int i = 0; i < 20; ++i) {
		struct hpm_mcan_tsu_snapshot snapshot;
		struct net_ptp_time before;
		struct net_ptp_time after;
		uint64_t captured_ns;
		uint32_t nanoseconds;

		tx.data[0] = i;
		zassert_ok(ptp_clock_get(ptpc, &before));
		exchange_frame(&tx, &rx);
		zassert_ok(ptp_clock_get(ptpc, &after));
		zassert_ok(hpm_mcan_decode_rx_timestamp(can, rx.timestamp, &snapshot));
		zassert_true(snapshot.timestamp_valid);
		zassert_false(snapshot.timestamp_lost);
		zassert_true(snapshot.is_64bit);
		zassert_true(snapshot.value64 > previous, "TSU timestamp did not advance");
		zassert_true(snapshot.slot < TSU_64BIT_SLOTS, "Invalid 64-bit TSU slot");
		if (i != 0) {
			zassert_equal(snapshot.slot, (previous_slot + 1U) % TSU_64BIT_SLOTS,
				      "TSU slot did not advance or wrap");
		}
		/* PTPC outputs TIMEH seconds and TIMEL nanoseconds to the TSU. */
		nanoseconds = (uint32_t)snapshot.value64;
		zassert_true(nanoseconds < NSEC_PER_SEC, "Invalid PTPC nanosecond field");
		captured_ns = (snapshot.value64 >> 32) * NSEC_PER_SEC + nanoseconds;
		zassert_true(captured_ns >= ptpc_time_ns(&before),
			     "TSU captured before transmission");
		zassert_true(captured_ns <= ptpc_time_ns(&after), "TSU captured after reception");
		seen_slots |= BIT(snapshot.slot);
		printk("MCAN TSU frame %d slot %u: %llu s %u ns, within PTPC send/receive "
		       "interval\n",
		       i, snapshot.slot, (unsigned long long)(snapshot.value64 >> 32), nanoseconds);
		previous = snapshot.value64;
		previous_slot = snapshot.slot;
		k_msleep(1);
	}
	zassert_equal(seen_slots, BIT_MASK(TSU_64BIT_SLOTS), "TSU slots did not wrap");
	check_completion_count(20);
	printk("MCAN PTPC timestamps: 20 frames, all 8 64-bit slots and wrap checked\n");
}

ZTEST_SUITE(hpm_can_loop, NULL, NULL, can_before, can_after, NULL);
