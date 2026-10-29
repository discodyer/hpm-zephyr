/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/drivers/usb/udc.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/ztest.h>

#define ENDPOINT_PAIRS  15U
#define TRANSFER_MAX    1025U
#define TRANSFER_ROUNDS 3U
#define FRAME_ACK       0x5aU
#define TEST_TIMEOUT_MS 240000

struct endpoint_descriptors {
	struct usb_if_descriptor interface;
	struct usb_ep_descriptor out[ENDPOINT_PAIRS];
	struct usb_ep_descriptor in[ENDPOINT_PAIRS];
	struct usb_desc_header nil;
	const struct usb_desc_header *headers[2U * ENDPOINT_PAIRS + 2U];
};

USBD_DEVICE_DEFINE(endpoint_usbd, DEVICE_DT_GET(DT_NODELABEL(usb0)), 0x1209, 0x0002);
USBD_DESC_LANG_DEFINE(endpoint_lang);
USBD_DESC_MANUFACTURER_DEFINE(endpoint_manufacturer, "HPM Zephyr tests");
USBD_DESC_PRODUCT_DEFINE(endpoint_product, "HPM5361 USB endpoint acceptance");
USBD_DESC_STRING_DEFINE(endpoint_serial, "HPM5361-USB-ENDPOINT", USBD_DUT_STRING_SERIAL_NUMBER);
USBD_CONFIGURATION_DEFINE(endpoint_fs_config, USB_SCD_SELF_POWERED, 0, NULL);
#if USBD_SUPPORTS_HIGH_SPEED
USBD_CONFIGURATION_DEFINE(endpoint_hs_config, USB_SCD_SELF_POWERED, 0, NULL);
#endif

static struct endpoint_descriptors fs_descriptors;
#if USBD_SUPPORTS_HIGH_SPEED
static struct endpoint_descriptors hs_descriptors;
#endif
static K_SEM_DEFINE(configured, 0, 1);
static K_SEM_DEFINE(transfer_complete, 0, 1);
static const uint16_t transfer_sizes[] = {1, 63, 64, 65, 511, 512, 513, 1024, 1025};
static uint8_t received[TRANSFER_MAX];
static uint16_t completions[2][ENDPOINT_PAIRS + 1U];
static uint8_t expected_ep;
static uint8_t completed_ep;
static size_t completed_length;
static int completed_status;
static atomic_t usb_errors;
static bool initialized;
static bool enabled;

static void prepare_descriptors(struct endpoint_descriptors *descriptors, uint16_t mps)
{
	descriptors->interface = (struct usb_if_descriptor){
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bNumEndpoints = 2U * ENDPOINT_PAIRS,
		.bInterfaceClass = USB_BCC_VENDOR,
	};
	descriptors->headers[0] = (struct usb_desc_header *)&descriptors->interface;
	for (size_t pair = 0U; pair < ENDPOINT_PAIRS; ++pair) {
		descriptors->out[pair] = (struct usb_ep_descriptor){
			.bLength = sizeof(struct usb_ep_descriptor),
			.bDescriptorType = USB_DESC_ENDPOINT,
			.bEndpointAddress = 0x01,
			.bmAttributes = USB_EP_TYPE_BULK,
			.wMaxPacketSize = sys_cpu_to_le16(mps),
		};
		descriptors->in[pair] = descriptors->out[pair];
		descriptors->in[pair].bEndpointAddress = 0x81;
		descriptors->headers[1U + 2U * pair] =
			(struct usb_desc_header *)&descriptors->out[pair];
		descriptors->headers[2U + 2U * pair] =
			(struct usb_desc_header *)&descriptors->in[pair];
	}
	descriptors->headers[1U + 2U * ENDPOINT_PAIRS] = &descriptors->nil;
}

static const void *endpoint_get_descriptors(struct usbd_class_data *const c_data,
					    const enum usbd_speed speed)
{
	ARG_UNUSED(c_data);
#if USBD_SUPPORTS_HIGH_SPEED
	if (speed == USBD_SPEED_HS) {
		return hs_descriptors.headers;
	}
#else
	ARG_UNUSED(speed);
#endif
	return fs_descriptors.headers;
}

static int endpoint_request(struct usbd_class_data *const c_data, struct net_buf *buf, int err)
{
	uint8_t ep = udc_get_buf_info(buf)->ep;
	uint8_t number = USB_EP_GET_IDX(ep);

	ARG_UNUSED(c_data);
	completed_ep = ep;
	completed_status = err;
	completed_length = buf->len;
	if ((ep != expected_ep) || (number == 0U) || (number > ENDPOINT_PAIRS)) {
		atomic_or(&usb_errors, BIT(2));
	} else if (err == 0) {
		completions[USB_EP_DIR_IS_IN(ep) ? 1 : 0][number]++;
	}
	if (USB_EP_DIR_IS_OUT(ep)) {
		if (buf->len <= sizeof(received)) {
			memcpy(received, buf->data, buf->len);
		} else {
			atomic_or(&usb_errors, BIT(3));
		}
	}
	net_buf_unref(buf);
	k_sem_give(&transfer_complete);
	return 0;
}

static int endpoint_init(struct usbd_class_data *const c_data)
{
	ARG_UNUSED(c_data);
	return 0;
}

static const struct usbd_class_api endpoint_api = {
	.init = endpoint_init,
	.request = endpoint_request,
	.get_desc = endpoint_get_descriptors,
};

USBD_DEFINE_CLASS(endpoint_fixture, &endpoint_api, NULL, NULL);

static void usb_message(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	if (ctx != &endpoint_usbd) {
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

static void transfer(uint8_t ep, size_t length, int64_t deadline)
{
	int64_t remaining = MIN(deadline - k_uptime_get(), 10000);
	struct net_buf *buf;
	int ret;

	zassert_true(remaining > 0, "USB test deadline expired");
	buf = usbd_ep_buf_alloc(&endpoint_fixture, ep, length);
	zassert_not_null(buf, "No buffer for endpoint 0x%02x", ep);
	expected_ep = ep;
	if (USB_EP_DIR_IS_IN(ep)) {
		for (size_t offset = 0U; offset < length; ++offset) {
			net_buf_add_u8(buf, received[offset] ^ 0xa5U);
		}
	}
	ret = usbd_ep_enqueue(&endpoint_fixture, buf);
	if (ret != 0) {
		net_buf_unref(buf);
	}
	zassert_ok(ret, "Enqueue endpoint 0x%02x", ep);
	zassert_ok(k_sem_take(&transfer_complete, K_MSEC(remaining)), "Endpoint 0x%02x timeout",
		   ep);
	zassert_ok(completed_status, "Endpoint 0x%02x status", ep);
	zassert_equal(completed_ep, ep);
	zassert_equal(completed_length, length, "Endpoint 0x%02x length", ep);
	zassert_equal(atomic_get(&usb_errors), 0, "USB stack/endpoint error");
}

static void usb_after(void *fixture)
{
	ARG_UNUSED(fixture);
	if (enabled) {
		(void)usbd_disable(&endpoint_usbd);
		enabled = false;
	}
	if (initialized) {
		(void)usbd_shutdown(&endpoint_usbd);
		initialized = false;
	}
}

ZTEST(hpm_usb_endpoints, test_all_noncontrol_endpoint_pairs)
{
	struct endpoint_descriptors *descriptors = &fs_descriptors;
	int64_t deadline = k_uptime_get() + TEST_TIMEOUT_MS;
	uint32_t total = 0U;
	uint32_t in_bitmap = 0U;
	uint32_t out_bitmap = 0U;
	enum usbd_speed speed;

	prepare_descriptors(&fs_descriptors, 64U);
	zassert_ok(usbd_add_descriptor(&endpoint_usbd, &endpoint_lang));
	zassert_ok(usbd_add_descriptor(&endpoint_usbd, &endpoint_manufacturer));
	zassert_ok(usbd_add_descriptor(&endpoint_usbd, &endpoint_product));
	zassert_ok(usbd_add_descriptor(&endpoint_usbd, &endpoint_serial));
	zassert_ok(usbd_add_configuration(&endpoint_usbd, USBD_SPEED_FS, &endpoint_fs_config));
	zassert_ok(usbd_register_class(&endpoint_usbd, "endpoint_fixture", USBD_SPEED_FS, 1));
	usbd_device_set_code_triple(&endpoint_usbd, USBD_SPEED_FS, 0, 0, 0);
#if USBD_SUPPORTS_HIGH_SPEED
	prepare_descriptors(&hs_descriptors, 512U);
	zassert_ok(usbd_add_configuration(&endpoint_usbd, USBD_SPEED_HS, &endpoint_hs_config));
	zassert_ok(usbd_register_class(&endpoint_usbd, "endpoint_fixture", USBD_SPEED_HS, 1));
	usbd_device_set_code_triple(&endpoint_usbd, USBD_SPEED_HS, 0, 0, 0);
#endif
	zassert_ok(usbd_msg_register_cb(&endpoint_usbd, usb_message));
	zassert_ok(usbd_init(&endpoint_usbd));
	initialized = true;
	zassert_ok(usbd_enable(&endpoint_usbd));
	enabled = true;
	printk("USB endpoints ready: run scripts/usb_endpoint_peer.py on the USB host\n");
	zassert_ok(k_sem_take(&configured, K_SECONDS(90)), "PC did not configure USB device");
	speed = usbd_bus_speed(&endpoint_usbd);
#if USBD_SUPPORTS_HIGH_SPEED
	if (speed == USBD_SPEED_HS) {
		descriptors = &hs_descriptors;
	}
#else
	zassert_equal(speed, USBD_SPEED_FS, "FS-only firmware negotiated another speed");
#endif
	printk("USB: configured at %s speed\n", speed == USBD_SPEED_HS ? "high" : "full");
	for (size_t pair = 0U; pair < ENDPOINT_PAIRS; ++pair) {
		uint8_t out = descriptors->out[pair].bEndpointAddress;
		uint8_t in = descriptors->in[pair].bEndpointAddress;

		zassert_equal(out, pair + 1U, "OUT assignment");
		zassert_equal(in, USB_EP_DIR_IN | (pair + 1U), "IN assignment");
		out_bitmap |= BIT(USB_EP_GET_IDX(out));
		in_bitmap |= BIT(USB_EP_GET_IDX(in));
		printk("USB: pair %u OUT 0x%02x IN 0x%02x\n", (unsigned int)pair, out, in);
	}
	zassert_equal(out_bitmap, 0xfffeU);
	zassert_equal(in_bitmap, 0xfffeU);
	for (uint32_t round = 0U; round < TRANSFER_ROUNDS; ++round) {
		for (size_t pair = 0U; pair < ENDPOINT_PAIRS; ++pair) {
			for (size_t frame = 0U; frame < ARRAY_SIZE(transfer_sizes); ++frame) {
				uint16_t length = transfer_sizes[frame];

				transfer(descriptors->out[pair].bEndpointAddress, length, deadline);
				for (size_t offset = 0U; offset < length; ++offset) {
					uint8_t expected = (offset * 37U + round * 13U +
							    pair * 11U + frame * 7U) &
							   0xffU;

					zassert_equal(received[offset], expected,
						      "round %u pair %u frame %u offset %u", round,
						      (unsigned int)pair, (unsigned int)frame,
						      (unsigned int)offset);
				}
				transfer(descriptors->in[pair].bEndpointAddress, length, deadline);
				total += length;
			}
		}
	}
	for (size_t ep = 1U; ep <= ENDPOINT_PAIRS; ++ep) {
		zassert_equal(completions[0][ep], TRANSFER_ROUNDS * ARRAY_SIZE(transfer_sizes));
		zassert_equal(completions[1][ep], TRANSFER_ROUNDS * ARRAY_SIZE(transfer_sizes));
	}
	/* The host sends this only after checking the final EP15 IN reply. */
	transfer(descriptors->out[ENDPOINT_PAIRS - 1U].bEndpointAddress, 1U, deadline);
	zassert_equal(received[0], FRAME_ACK, "PC rejected final reply");
	printk("USB: all 15 IN/OUT pairs, %u exchanges, %u checked bytes each direction; ACK\n",
	       TRANSFER_ROUNDS * ENDPOINT_PAIRS * (unsigned int)ARRAY_SIZE(transfer_sizes), total);
}

ZTEST_SUITE(hpm_usb_endpoints, NULL, NULL, NULL, usb_after, NULL);
