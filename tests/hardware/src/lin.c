/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/lin.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#define LIN_COMMANDER DT_ALIAS(lin_commander)
#define LIN_RESPONDER DT_ALIAS(lin_responder)

#define LIN_WRITE_ID         0x01U
#define LIN_READ_ID          0x02U
#define LIN_FILTER_ID        0x21U
#define LIN_FILTER_REJECT_ID 0x22U

static const struct device *const commander = DEVICE_DT_GET(LIN_COMMANDER);
static const struct device *const responder = DEVICE_DT_GET(LIN_RESPONDER);

static const struct lin_config commander_cfg = {
	.mode = LIN_MODE_COMMANDER,
	.baudrate = 19200U,
	.break_len = 13U,
	.break_delimiter_len = 1U,
};

static const struct lin_config responder_cfg = {
	.mode = LIN_MODE_RESPONDER,
	.baudrate = 19200U,
	.break_len = 13U,
	.break_delimiter_len = 1U,
};

static const uint8_t write_data[LIN_MAX_DLEN] = {
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
};

static const uint8_t read_data[LIN_MAX_DLEN] = {
	0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
};

static struct k_sem commander_done;
static struct k_sem commander_header;
static struct k_sem responder_done;
static struct k_sem responder_header;

static struct lin_msg commander_msg;
static struct lin_msg responder_msg;
static uint32_t commander_events;
static uint32_t responder_events;
static uint32_t responder_header_count;
static uint32_t responder_error_flags;
static int commander_status;
static int responder_status;
static bool responder_auto_respond;

static void commander_callback(const struct device *dev, const struct lin_event *event,
			       void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	commander_events++;
	commander_status = event->status;

	switch (event->type) {
	case LIN_EVT_TX_HEADER:
		k_sem_give(&commander_header);
		break;
	case LIN_EVT_TX_DATA:
	case LIN_EVT_RX_DATA:
	case LIN_EVT_ERR:
		k_sem_give(&commander_done);
		break;
	default:
		break;
	}
}

static int responder_schedule_action(uint8_t id)
{
	int ret;

	memset(&responder_msg, 0, sizeof(responder_msg));
	responder_msg.id = id;
	responder_msg.data_len = LIN_MAX_DLEN;
	responder_msg.checksum_type = LIN_CHECKSUM_ENHANCED;

	if (id == LIN_WRITE_ID) {
		responder_msg.checksum_type = LIN_CHECKSUM_CLASSIC;
		ret = lin_read(responder, &responder_msg, K_MSEC(500));
	} else {
		memcpy(responder_msg.data, read_data, sizeof(read_data));
		ret = lin_response(responder, &responder_msg, K_MSEC(500));
	}

	responder_status = ret;
	return ret;
}

static void responder_callback(const struct device *dev, const struct lin_event *event,
			       void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	responder_events++;

	switch (event->type) {
	case LIN_EVT_RX_HEADER:
		responder_header_count++;
		k_sem_give(&responder_header);
		if (responder_auto_respond) {
			(void)responder_schedule_action(lin_get_frame_id(event->header.pid));
		}
		break;
	case LIN_EVT_TX_DATA:
	case LIN_EVT_RX_DATA:
		responder_status = event->status;
		k_sem_give(&responder_done);
		break;
	case LIN_EVT_ERR:
		responder_status = event->status;
		responder_error_flags = event->error_flags;
		k_sem_give(&responder_done);
		break;
	default:
		break;
	}
}

static void lin_fixture_before(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)lin_stop(commander);
	(void)lin_stop(responder);
	zassert_ok(lin_configure(commander, &commander_cfg));
	zassert_ok(lin_configure(responder, &responder_cfg));
	zassert_ok(lin_set_event_callback(commander, commander_callback, NULL));
	zassert_ok(lin_set_event_callback(responder, responder_callback, NULL));
	zassert_ok(lin_start(responder));
	zassert_ok(lin_start(commander));

	k_sem_reset(&commander_done);
	k_sem_reset(&commander_header);
	k_sem_reset(&responder_done);
	k_sem_reset(&responder_header);
	commander_events = 0U;
	responder_events = 0U;
	responder_header_count = 0U;
	responder_error_flags = 0U;
	commander_status = 0;
	responder_status = 0;
	responder_auto_respond = true;
	memset(&commander_msg, 0, sizeof(commander_msg));
	memset(&responder_msg, 0, sizeof(responder_msg));
}

static void lin_fixture_after(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)lin_stop(commander);
	(void)lin_stop(responder);
}

ZTEST(lin_hardware, test_configure_and_mode_errors)
{
	struct lin_config config;
	struct lin_msg msg = {
		.id = LIN_WRITE_ID,
		.data_len = 1U,
		.checksum_type = LIN_CHECKSUM_CLASSIC,
	};
	struct lin_config unsupported = commander_cfg;

	zassert_ok(lin_get_config(commander, &config));
	zassert_equal(config.mode, LIN_MODE_COMMANDER);
	zassert_equal(config.baudrate, commander_cfg.baudrate);

	unsupported.flags = LIN_BUS_AUTO_SYNC;
	zassert_equal(lin_configure(commander, &unsupported), -ENOTSUP);
	unsupported = commander_cfg;
	unsupported.break_delimiter_len = 3U;
	zassert_equal(lin_configure(commander, &unsupported), -EINVAL);

	zassert_equal(lin_start(commander), -EALREADY);
	zassert_equal(lin_response(commander, &msg, K_NO_WAIT), -EPERM);
	zassert_equal(lin_read(commander, &msg, K_NO_WAIT), -EPERM);

	zassert_equal(lin_send(responder, &msg, K_NO_WAIT), -EPERM);
	zassert_equal(lin_receive(responder, &msg, K_NO_WAIT), -EPERM);
}

ZTEST(lin_hardware, test_commander_write)
{
	int ret;

	memcpy(commander_msg.data, write_data, sizeof(write_data));
	commander_msg.id = LIN_WRITE_ID;
	commander_msg.data_len = LIN_MAX_DLEN;
	commander_msg.checksum_type = LIN_CHECKSUM_CLASSIC;

	ret = lin_send(commander, &commander_msg, K_MSEC(500));
	zassert_ok(ret);
	zassert_ok(k_sem_take(&commander_done, K_MSEC(1000)));
	zassert_ok(k_sem_take(&responder_done, K_MSEC(1000)));
	zassert_equal(commander_status, 0);
	zassert_equal(responder_status, 0);
	zassert_equal(responder_msg.id, LIN_WRITE_ID);
	zassert_mem_equal(responder_msg.data, write_data, sizeof(write_data));
}

ZTEST(lin_hardware, test_commander_read)
{
	int ret;

	commander_msg.id = LIN_READ_ID;
	commander_msg.data_len = LIN_MAX_DLEN;
	commander_msg.checksum_type = LIN_CHECKSUM_ENHANCED;

	ret = lin_receive(commander, &commander_msg, K_MSEC(500));
	zassert_ok(ret);
	zassert_ok(k_sem_take(&responder_done, K_MSEC(1000)));
	zassert_ok(k_sem_take(&commander_done, K_MSEC(1000)));
	zassert_equal(responder_status, 0);
	zassert_equal(commander_status, 0);
	zassert_equal(commander_msg.id, LIN_READ_ID);
	zassert_mem_equal(commander_msg.data, read_data, sizeof(read_data));
}

ZTEST(lin_hardware, test_responder_filter)
{
	const struct lin_filter filter = {
		.primary_pid = 0U,
		.secondary_pid = 0U,
		.mask = 0xFFU,
	};
	struct lin_filter configured = filter;

	configured.primary_pid = lin_compute_pid(LIN_FILTER_ID);
	responder_auto_respond = false;
	zassert_ok(lin_set_rx_filter(responder, &configured));

	commander_msg.id = LIN_FILTER_ID;
	commander_msg.data_len = 0U;
	commander_msg.checksum_type = LIN_CHECKSUM_CLASSIC;
	zassert_ok(lin_send(commander, &commander_msg, K_MSEC(500)));
	zassert_ok(k_sem_take(&commander_header, K_MSEC(1000)));
	zassert_ok(k_sem_take(&responder_header, K_MSEC(1000)));
	zassert_equal(responder_header_count, 1U);

	commander_msg.id = LIN_FILTER_REJECT_ID;
	zassert_ok(lin_send(commander, &commander_msg, K_MSEC(500)));
	zassert_ok(k_sem_take(&commander_header, K_MSEC(1000)));
	k_msleep(20);
	zassert_equal(responder_header_count, 1U);
}

ZTEST(lin_hardware, test_receive_timeout)
{
	struct lin_msg msg = {
		.id = 0x30U,
		.data_len = 1U,
		.checksum_type = LIN_CHECKSUM_CLASSIC,
	};

	zassert_ok(lin_stop(responder));
	zassert_ok(lin_receive(commander, &msg, K_MSEC(100)));
	zassert_ok(k_sem_take(&commander_done, K_MSEC(1000)));
	zassert_equal(commander_status, -ETIMEDOUT);
}

ZTEST(lin_hardware, test_wakeup_send)
{
	zassert_ok(lin_wakeup_send(commander));
	zassert_true(commander_events > 0U);
}

static void *lin_fixture_suite_setup(void)
{
	k_sem_init(&commander_done, 0, 1);
	k_sem_init(&commander_header, 0, 1);
	k_sem_init(&responder_done, 0, 1);
	k_sem_init(&responder_header, 0, 1);
	return NULL;
}

ZTEST_SUITE(lin_hardware, NULL, lin_fixture_suite_setup, lin_fixture_before, lin_fixture_after,
	    NULL);
