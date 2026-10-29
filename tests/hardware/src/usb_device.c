/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/drivers/uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/ztest.h>

#define CDC_NODE        DT_NODELABEL(cdc_test)
#define TRANSFER_MAX    1025U
#define TRANSFER_ROUNDS 3U
#define FRAME_ACK       0x5aU
#define TEST_TIMEOUT_MS 150000

USBD_DEVICE_DEFINE(test_usbd, DEVICE_DT_GET(DT_NODELABEL(usb0)), 0x1209, 0x0001);
USBD_DESC_LANG_DEFINE(test_lang);
USBD_DESC_MANUFACTURER_DEFINE(test_manufacturer, "HPM Zephyr tests");
USBD_DESC_PRODUCT_DEFINE(test_product, "HPM5361 hardware acceptance");
USBD_DESC_STRING_DEFINE(test_serial, "HPM5361-HW-ACCEPT", USBD_DUT_STRING_SERIAL_NUMBER);
USBD_DESC_CONFIG_DEFINE(fs_desc, "CDC FS");
USBD_DESC_CONFIG_DEFINE(hs_desc, "CDC HS");
USBD_CONFIGURATION_DEFINE(fs_config, USB_SCD_SELF_POWERED, 0, &fs_desc);
USBD_CONFIGURATION_DEFINE(hs_config, USB_SCD_SELF_POWERED, 0, &hs_desc);

static K_SEM_DEFINE(configured, 0, 1);
static K_SEM_DEFINE(tx_queued, 0, 1);
static K_SEM_DEFINE(rx_ready, 0, 1);
RING_BUF_DECLARE(rx_buffer, 2U * TRANSFER_MAX);
static const struct device *const cdc = DEVICE_DT_GET(CDC_NODE);
static const uint16_t transfer_sizes[] = {1, 63, 64, 65, 511, 512, 513, 1024, 1025};
static uint8_t received[TRANSFER_MAX];
static uint8_t reply[TRANSFER_MAX];
static size_t tx_length;
static size_t tx_offset;
static atomic_t usb_errors;
static bool initialized;
static bool enabled;

BUILD_ASSERT(DT_PROP(CDC_NODE, tx_fifo_size) > TRANSFER_MAX);
BUILD_ASSERT(DT_PROP(CDC_NODE, rx_fifo_size) > TRANSFER_MAX);

static void usb_message(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	if (ctx != &test_usbd) {
		atomic_or(&usb_errors, BIT(0));
	}
	if ((msg->type == USBD_MSG_UDC_ERROR) || (msg->type == USBD_MSG_STACK_ERROR)) {
		atomic_or(&usb_errors, BIT(1));
		printk("USB: %s status %d\n", usbd_msg_type_string(msg->type), msg->status);
	}
	if ((msg->type == USBD_MSG_CONFIGURATION) && (msg->status != 0)) {
		k_sem_give(&configured);
	}
}

static void receive_bytes(uint8_t *buffer, size_t length, int64_t deadline)
{
	size_t offset = 0U;

	while (offset < length) {
		int64_t remaining;

		offset += ring_buf_get(&rx_buffer, &buffer[offset], length - offset);
		if (offset == length) {
			break;
		}
		remaining = deadline - k_uptime_get();
		zassert_true(remaining > 0, "CDC RX deadline expired");
		zassert_ok(k_sem_take(&rx_ready, K_MSEC(remaining)), "CDC RX timeout at %u/%u",
			   (unsigned int)offset, (unsigned int)length);
	}
}

static void cdc_irq(const struct device *dev, void *user_data)
{
	int written;

	ARG_UNUSED(user_data);
	uart_irq_update(dev);
	if (uart_irq_rx_ready(dev)) {
		uint8_t buffer[64];
		int length;

		while ((length = uart_fifo_read(dev, buffer, sizeof(buffer))) > 0) {
			if (ring_buf_put(&rx_buffer, buffer, length) != length) {
				atomic_or(&usb_errors, BIT(3));
			}
			k_sem_give(&rx_ready);
		}
		if (length < 0) {
			atomic_or(&usb_errors, BIT(3));
		}
	}
	if (!uart_irq_tx_ready(dev)) {
		return;
	}

	written = uart_fifo_fill(dev, &reply[tx_offset], tx_length - tx_offset);
	if (written < 0) {
		atomic_or(&usb_errors, BIT(2));
	} else {
		tx_offset += written;
	}
	if ((written < 0) || (tx_offset == tx_length)) {
		uart_irq_tx_disable(dev);
		k_sem_give(&tx_queued);
	}
}

static void send_reply(size_t length, int64_t deadline)
{
	int64_t remaining = deadline - k_uptime_get();

	zassert_true(remaining > 0, "CDC TX deadline expired");
	tx_offset = 0U;
	tx_length = length;
	uart_irq_tx_enable(cdc);
	zassert_ok(k_sem_take(&tx_queued, K_MSEC(remaining)), "CDC TX timeout");
	zassert_equal(atomic_get(&usb_errors), 0, "USB/CDC reported an error");
}

static void usb_after(void *fixture)
{
	ARG_UNUSED(fixture);
	uart_irq_tx_disable(cdc);
	uart_irq_rx_disable(cdc);
	if (enabled) {
		(void)usbd_disable(&test_usbd);
		enabled = false;
	}
	if (initialized) {
		(void)usbd_shutdown(&test_usbd);
		initialized = false;
	}
}

ZTEST(hpm_usb_device, test_enumeration_and_cdc_transfers)
{
	int64_t deadline = k_uptime_get() + TEST_TIMEOUT_MS;
	uint32_t dtr = 0U;
	uint32_t total = 0U;
	enum usbd_speed speed;

	zassert_true(device_is_ready(cdc));
	zassert_ok(uart_irq_callback_user_data_set(cdc, cdc_irq, NULL));
	zassert_ok(usbd_add_descriptor(&test_usbd, &test_lang));
	zassert_ok(usbd_add_descriptor(&test_usbd, &test_manufacturer));
	zassert_ok(usbd_add_descriptor(&test_usbd, &test_product));
	zassert_ok(usbd_add_descriptor(&test_usbd, &test_serial));
	zassert_ok(usbd_add_configuration(&test_usbd, USBD_SPEED_FS, &fs_config));
	zassert_ok(usbd_register_class(&test_usbd, "cdc_acm_0", USBD_SPEED_FS, 1));
	zassert_ok(usbd_add_configuration(&test_usbd, USBD_SPEED_HS, &hs_config));
	zassert_ok(usbd_register_class(&test_usbd, "cdc_acm_0", USBD_SPEED_HS, 1));
	usbd_device_set_code_triple(&test_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 2, 1);
	usbd_device_set_code_triple(&test_usbd, USBD_SPEED_HS, USB_BCC_MISCELLANEOUS, 2, 1);
	zassert_ok(usbd_msg_register_cb(&test_usbd, usb_message));
	zassert_ok(usbd_init(&test_usbd));
	initialized = true;
	uart_irq_rx_enable(cdc);
	zassert_ok(usbd_enable(&test_usbd));
	enabled = true;
	printk("USB CDC ready: run scripts/usb_cdc_peer.py on the USB host\n");
	zassert_ok(k_sem_take(&configured, K_SECONDS(60)), "PC did not configure USB device");
	speed = usbd_bus_speed(&test_usbd);
	zassert_true((speed == USBD_SPEED_FS) || (speed == USBD_SPEED_HS));
	printk("USB: configured at %s speed\n", speed == USBD_SPEED_HS ? "high" : "full");

	zassert_true(WAIT_FOR((uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &dtr) == 0) && dtr,
			      30000000, k_msleep(1)),
		     "PC peer did not open CDC port");
	for (uint32_t round = 0U; round < TRANSFER_ROUNDS; ++round) {
		for (size_t frame = 0U; frame < ARRAY_SIZE(transfer_sizes); ++frame) {
			uint16_t length = transfer_sizes[frame];
			uint8_t ack;

			receive_bytes(received, length, MIN(deadline, k_uptime_get() + 10000));
			for (size_t offset = 0U; offset < length; ++offset) {
				uint8_t expected =
					(offset * 37U + round * 13U + frame * 7U) & 0xffU;

				zassert_equal(received[offset], expected,
					      "round %u frame %u offset %u", round,
					      (unsigned int)frame, (unsigned int)offset);
				reply[offset] = received[offset] ^ 0xa5U;
			}
			/* Retry partial FIFO writes from the CDC callback without dropping bytes.
			 */
			send_reply(length, MIN(deadline, k_uptime_get() + 10000));
			/* The peer acknowledges only after checking the complete reply. */
			receive_bytes(&ack, 1U, MIN(deadline, k_uptime_get() + 10000));
			zassert_equal(ack, FRAME_ACK, "PC rejected reply");
			total += length;
			zassert_equal(atomic_get(&usb_errors), 0,
				      "USB stack/UDC reported an error");
		}
	}
	printk("USB: %u frames, %u checked bytes each direction; host ACK received\n",
	       TRANSFER_ROUNDS * (unsigned int)ARRAY_SIZE(transfer_sizes), total);
}

ZTEST_SUITE(hpm_usb_device, NULL, NULL, NULL, usb_after, NULL);
