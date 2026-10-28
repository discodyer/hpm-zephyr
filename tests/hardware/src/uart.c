/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#define UART_TEST_LENGTH      257U
#define UART_RX_LENGTH        320U
#define UART_IDLE_TIMEOUT_US  50000
#define UART_WAIT_TIMEOUT_MS  3000
#define UART_EVENT_LOG_LENGTH 32U

static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(uart1));
static uint8_t tx[UART_TEST_LENGTH] __aligned(32);
static uint8_t rx[UART_RX_LENGTH] __aligned(32);
static uint8_t received[UART_RX_LENGTH] __aligned(32);

enum uart_test_event {
	UART_TEST_TX_DONE = 1,
	UART_TEST_TX_ABORTED,
	UART_TEST_RX_RDY,
	UART_TEST_RX_REQUEST,
	UART_TEST_RX_RELEASED,
	UART_TEST_RX_DISABLED,
	UART_TEST_RX_STOPPED,
};

struct uart_test_state {
	struct k_spinlock lock;
	const struct device *expected_dev;
	void *expected_user_data;
	const uint8_t *tx_buf;
	size_t tx_len;
	uint8_t *rx_buf;
	size_t rx_capacity;
	size_t received_len;
	size_t tx_done_count;
	size_t tx_aborted_count;
	size_t rx_ready_count;
	size_t rx_request_count;
	size_t rx_released_count;
	size_t rx_disabled_count;
	size_t rx_stopped_count;
	size_t event_log_len;
	uint8_t event_log[UART_EVENT_LOG_LENGTH];
	int callback_error;
	int64_t tx_done_time;
	int64_t first_rx_ready_time;
};

static struct uart_test_state state;
static struct uart_config saved_config;
static bool have_saved_config;
static K_SEM_DEFINE(callback_wake, 0, 1);

static void uart_set_callback_error_locked(int error)
{
	if (state.callback_error == 0) {
		state.callback_error = error;
	}
}

static void uart_log_event_locked(enum uart_test_event event)
{
	if (state.event_log_len < ARRAY_SIZE(state.event_log)) {
		state.event_log[state.event_log_len++] = event;
	} else {
		uart_set_callback_error_locked(-EOVERFLOW);
	}
}

static void uart_event(const struct device *dev, struct uart_event *event, void *user_data)
{
	k_spinlock_key_t key = k_spin_lock(&state.lock);

	if ((dev != state.expected_dev) || (user_data != state.expected_user_data)) {
		uart_set_callback_error_locked(-EXDEV);
	}

	switch (event->type) {
	case UART_TX_DONE:
		uart_log_event_locked(UART_TEST_TX_DONE);
		if ((event->data.tx.buf != state.tx_buf) || (event->data.tx.len != state.tx_len)) {
			uart_set_callback_error_locked(-EPROTO);
		}
		state.tx_done_count++;
		state.tx_done_time = k_uptime_get();
		break;
	case UART_TX_ABORTED:
		uart_log_event_locked(UART_TEST_TX_ABORTED);
		state.tx_aborted_count++;
		uart_set_callback_error_locked(-ECANCELED);
		break;
	case UART_RX_RDY: {
		size_t end = event->data.rx.offset + event->data.rx.len;

		uart_log_event_locked(UART_TEST_RX_RDY);
		if ((event->data.rx.buf != state.rx_buf) ||
		    (event->data.rx.offset != state.received_len) ||
		    (end < event->data.rx.offset) || (end > state.rx_capacity) ||
		    (event->data.rx.len > sizeof(received) - state.received_len)) {
			uart_set_callback_error_locked(-EPROTO);
		} else {
			memcpy(&received[state.received_len],
			       &event->data.rx.buf[event->data.rx.offset], event->data.rx.len);
			state.received_len += event->data.rx.len;
		}
		state.rx_ready_count++;
		if (state.first_rx_ready_time == 0) {
			state.first_rx_ready_time = k_uptime_get();
		}
		break;
	}
	case UART_RX_BUF_REQUEST:
		uart_log_event_locked(UART_TEST_RX_REQUEST);
		state.rx_request_count++;
		break;
	case UART_RX_BUF_RELEASED:
		uart_log_event_locked(UART_TEST_RX_RELEASED);
		if ((event->data.rx_buf.buf != state.rx_buf) ||
		    ((state.received_len != 0U) &&
		     ((state.event_log_len < 2U) ||
		      (state.event_log[state.event_log_len - 2U] != UART_TEST_RX_RDY)))) {
			uart_set_callback_error_locked(-EPROTO);
		}
		state.rx_released_count++;
		break;
	case UART_RX_DISABLED:
		uart_log_event_locked(UART_TEST_RX_DISABLED);
		if ((state.rx_released_count < state.rx_disabled_count + 1U) ||
		    (state.event_log_len < 2U) ||
		    (state.event_log[state.event_log_len - 2U] != UART_TEST_RX_RELEASED)) {
			uart_set_callback_error_locked(-EPROTO);
		}
		state.rx_disabled_count++;
		break;
	case UART_RX_STOPPED:
		uart_log_event_locked(UART_TEST_RX_STOPPED);
		state.rx_stopped_count++;
		uart_set_callback_error_locked(-EIO);
		break;
	default:
		uart_set_callback_error_locked(-EINVAL);
		break;
	}

	k_spin_unlock(&state.lock, key);
	k_sem_give(&callback_wake);
}

static void uart_reset_state(void)
{
	k_spinlock_key_t key = k_spin_lock(&state.lock);

	state.expected_dev = uart;
	state.expected_user_data = &state;
	state.tx_buf = NULL;
	state.tx_len = 0U;
	state.rx_buf = rx;
	state.rx_capacity = sizeof(rx);
	state.received_len = 0U;
	state.tx_done_count = 0U;
	state.tx_aborted_count = 0U;
	state.rx_ready_count = 0U;
	state.rx_request_count = 0U;
	state.rx_released_count = 0U;
	state.rx_disabled_count = 0U;
	state.rx_stopped_count = 0U;
	state.event_log_len = 0U;
	state.callback_error = 0;
	state.tx_done_time = 0;
	state.first_rx_ready_time = 0;
	memset(state.event_log, 0, sizeof(state.event_log));
	memset(received, 0, sizeof(received));

	k_spin_unlock(&state.lock, key);
	k_sem_reset(&callback_wake);
}

static int uart_callback_error(void)
{
	k_spinlock_key_t key = k_spin_lock(&state.lock);
	int error = state.callback_error;

	k_spin_unlock(&state.lock, key);
	return error;
}

static size_t uart_tx_done_count(void)
{
	k_spinlock_key_t key = k_spin_lock(&state.lock);
	size_t count = state.tx_done_count;

	k_spin_unlock(&state.lock, key);
	return count;
}

static size_t uart_rx_ready_count(void)
{
	k_spinlock_key_t key = k_spin_lock(&state.lock);
	size_t count = state.rx_ready_count;

	k_spin_unlock(&state.lock, key);
	return count;
}

static size_t uart_received_len(void)
{
	k_spinlock_key_t key = k_spin_lock(&state.lock);
	size_t length = state.received_len;

	k_spin_unlock(&state.lock, key);
	return length;
}

static size_t uart_rx_disabled_count(void)
{
	k_spinlock_key_t key = k_spin_lock(&state.lock);
	size_t count = state.rx_disabled_count;

	k_spin_unlock(&state.lock, key);
	return count;
}

static size_t uart_rx_released_count(void)
{
	k_spinlock_key_t key = k_spin_lock(&state.lock);
	size_t count = state.rx_released_count;

	k_spin_unlock(&state.lock, key);
	return count;
}

static size_t uart_rx_request_count(void)
{
	k_spinlock_key_t key = k_spin_lock(&state.lock);
	size_t count = state.rx_request_count;

	k_spin_unlock(&state.lock, key);
	return count;
}

static int uart_wait_for(bool (*condition)(void), int32_t timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;

	for (;;) {
		int64_t remaining = deadline - k_uptime_get();

		if (condition() || (uart_callback_error() != 0)) {
			return 0;
		}
		if (remaining <= 0) {
			return -ETIMEDOUT;
		}
		(void)k_sem_take(&callback_wake, K_MSEC(MIN(remaining, INT32_MAX)));
	}
}

static size_t wait_tx_target;
static bool uart_tx_target_reached(void)
{
	return uart_tx_done_count() >= wait_tx_target;
}

static size_t wait_rx_target;
static bool uart_rx_target_reached(void)
{
	return uart_rx_ready_count() >= wait_rx_target;
}

static size_t wait_received_target;
static bool uart_received_target_reached(void)
{
	return uart_received_len() >= wait_received_target;
}

static size_t wait_disabled_target;
static bool uart_disabled_target_reached(void)
{
	return uart_rx_disabled_count() >= wait_disabled_target;
}

static int uart_wait_for_tx(size_t target)
{
	wait_tx_target = target;
	return uart_wait_for(uart_tx_target_reached, UART_WAIT_TIMEOUT_MS);
}

static int uart_wait_for_rx_events(size_t target)
{
	wait_rx_target = target;
	return uart_wait_for(uart_rx_target_reached, UART_WAIT_TIMEOUT_MS);
}

static int uart_wait_for_received(size_t target)
{
	wait_received_target = target;
	return uart_wait_for(uart_received_target_reached, UART_WAIT_TIMEOUT_MS);
}

static int uart_wait_for_disabled(size_t target)
{
	wait_disabled_target = target;
	return uart_wait_for(uart_disabled_target_reached, UART_WAIT_TIMEOUT_MS);
}

static void uart_set_tx_expectation(const uint8_t *buffer, size_t length)
{
	k_spinlock_key_t key = k_spin_lock(&state.lock);

	state.tx_buf = buffer;
	state.tx_len = length;

	k_spin_unlock(&state.lock, key);
}

static void uart_fill_tx(uint8_t salt)
{
	for (size_t i = 0; i < sizeof(tx); ++i) {
		tx[i] = (uint8_t)(i * 73U + salt);
	}
}

static void uart_configure_flow(enum uart_config_flow_control flow_ctrl)
{
	struct uart_config config = saved_config;
	struct uart_config readback;

	config.flow_ctrl = flow_ctrl;
	zassert_ok(uart_configure(uart, &config));
	zassert_ok(uart_config_get(uart, &readback));
	zassert_equal(readback.flow_ctrl, flow_ctrl);
}

static void uart_start_rx(void)
{
	memset(rx, 0xa5, sizeof(rx));
	zassert_ok(uart_rx_enable(uart, rx, sizeof(rx), UART_IDLE_TIMEOUT_US));
	zassert_true(uart_rx_request_count() != 0U);
	zassert_equal(uart_callback_error(), 0);
}

static void uart_send(const uint8_t *buffer, size_t length)
{
	uart_set_tx_expectation(buffer, length);
	zassert_ok(uart_tx(uart, buffer, length, 1000000));
}

static void uart_stop_rx(void)
{
	size_t disabled_target = uart_rx_disabled_count() + 1U;
	int ret;

	ret = uart_rx_disable(uart);
	if (ret == 0) {
		zassert_ok(uart_wait_for_disabled(disabled_target));
	}
}

static void uart_before(void *fixture)
{
	ARG_UNUSED(fixture);

	zassert_true(device_is_ready(uart));
	zassert_ok(uart_config_get(uart, &saved_config));
	have_saved_config = true;
	uart_reset_state();
	zassert_ok(uart_callback_set(uart, uart_event, &state));
}

static void uart_after(void *fixture)
{
	ARG_UNUSED(fixture);

	if (device_is_ready(uart)) {
		size_t disabled_target = uart_rx_disabled_count() + 1U;

		if (uart_rx_disable(uart) == 0) {
			(void)uart_wait_for_disabled(disabled_target);
		}
		(void)uart_tx_abort(uart);
		(void)uart_callback_set(uart, NULL, NULL);
		if (have_saved_config) {
			(void)uart_configure(uart, &saved_config);
		}
	}
	k_sem_reset(&callback_wake);
}

ZTEST(hpm_uart_loop, test_async_dma_single_transfer)
{
	uart_configure_flow(UART_CFG_FLOW_CTRL_NONE);
	uart_fill_tx(0U);
	uart_start_rx();
	uart_send(tx, sizeof(tx));
	zassert_ok(uart_wait_for_tx(1U), "UART TX_DONE timeout");
	zassert_ok(uart_wait_for_received(sizeof(tx)), "UART RX_RDY timeout");
	zassert_equal(uart_callback_error(), 0);
	zassert_equal(uart_received_len(), sizeof(tx));
	zassert_mem_equal(received, tx, sizeof(tx));
	zassert_equal(uart_rx_released_count(), 0U);
	uart_stop_rx();
	zassert_equal(uart_rx_released_count(), 1U);
	zassert_equal(uart_rx_disabled_count(), 1U);
	zassert_equal(uart_callback_error(), 0);
}

ZTEST(hpm_uart_loop, test_async_dma_segmented_offsets)
{
	const size_t first_length = 97U;
	const size_t second_length = sizeof(tx) - first_length;

	uart_configure_flow(UART_CFG_FLOW_CTRL_NONE);
	uart_fill_tx(0x31U);
	uart_start_rx();
	uart_send(tx, first_length);
	zassert_ok(uart_wait_for_tx(1U), "First UART TX_DONE timeout");
	zassert_ok(uart_wait_for_received(first_length), "First RX_RDY timeout");
	zassert_ok(uart_wait_for_rx_events(1U));

	uart_set_tx_expectation(&tx[first_length], second_length);
	zassert_ok(uart_tx(uart, &tx[first_length], second_length, 1000000));
	zassert_ok(uart_wait_for_tx(2U), "Second UART TX_DONE timeout");
	zassert_ok(uart_wait_for_received(sizeof(tx)), "Second RX_RDY timeout");
	zassert_ok(uart_wait_for_rx_events(2U));
	zassert_equal(uart_callback_error(), 0);
	zassert_mem_equal(received, tx, sizeof(tx));
	uart_stop_rx();
	zassert_equal(uart_callback_error(), 0);
}

ZTEST(hpm_uart_loop, test_async_dma_rts_cts)
{
	struct uart_config readback;

	uart_configure_flow(UART_CFG_FLOW_CTRL_RTS_CTS);
	zassert_ok(uart_config_get(uart, &readback));
	zassert_equal(readback.flow_ctrl, UART_CFG_FLOW_CTRL_RTS_CTS);
	uart_fill_tx(0x62U);
	uart_start_rx();
	uart_send(tx, sizeof(tx));
	zassert_ok(uart_wait_for_tx(1U), "RTS/CTS UART TX_DONE timeout; check PB05-PB04");
	zassert_ok(uart_wait_for_received(sizeof(tx)),
		   "RTS/CTS UART RX timeout; check PB07-PB06 and PB05-PB04");
	zassert_equal(uart_callback_error(), 0);
	zassert_mem_equal(received, tx, sizeof(tx));
	uart_stop_rx();
	zassert_equal(uart_callback_error(), 0);
}

ZTEST(hpm_uart_loop, test_async_rx_disable_reenable)
{
	uart_configure_flow(UART_CFG_FLOW_CTRL_NONE);
	uart_fill_tx(0x93U);
	uart_start_rx();
	uart_send(tx, 64U);
	zassert_ok(uart_wait_for_tx(1U));
	zassert_ok(uart_wait_for_received(64U));
	uart_stop_rx();
	zassert_equal(uart_rx_released_count(), 1U);
	zassert_equal(uart_rx_disabled_count(), 1U);
	zassert_equal(uart_callback_error(), 0);

	memset(received, 0, sizeof(received));
	{
		k_spinlock_key_t key = k_spin_lock(&state.lock);

		state.received_len = 0U;
		state.first_rx_ready_time = 0;
		k_spin_unlock(&state.lock, key);
	}
	uart_fill_tx(0xa4U);
	uart_start_rx();
	uart_set_tx_expectation(tx, 64U);
	zassert_ok(uart_tx(uart, tx, 64U, 1000000));
	zassert_ok(uart_wait_for_tx(2U));
	zassert_ok(uart_wait_for_received(64U));
	zassert_equal(uart_callback_error(), 0);
	zassert_equal(uart_rx_released_count(), 1U);
	uart_stop_rx();
	zassert_equal(uart_rx_released_count(), 2U);
	zassert_equal(uart_rx_disabled_count(), 2U);
	zassert_equal(uart_callback_error(), 0);
}

ZTEST_SUITE(hpm_uart_loop, NULL, NULL, uart_before, uart_after, NULL);
