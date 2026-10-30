/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usbh.h>
#include <zephyr/ztest.h>

/* The experimental host stack currently keeps transfer helpers private. */
#include <usbh_ch9.h>
#include <usbh_class.h>
#include <usbh_desc.h>
#include <usbh_device.h>

USBH_CONTROLLER_DEFINE(host, DEVICE_DT_GET(DT_NODELABEL(usb0)));
static K_SEM_DEFINE(attached, 0, 1);
static K_SEM_DEFINE(report_done, 0, 1);
static K_MUTEX_DEFINE(keyboard_lock);
static struct usb_device *keyboard;
static uint8_t endpoint;
static uint8_t interface;
static int transfer_status;
static uint8_t report[8];
static size_t report_len;
static struct uhc_transfer *pending;
static atomic_t pending_completed;
static bool pending_submitted;
static bool keyboard_locked;
static bool initialized;
static bool enabled;

static int keyboard_init(struct usbh_class_data *const data)
{
	ARG_UNUSED(data);
	return 0;
}

static int keyboard_probe(struct usbh_class_data *const data, struct usb_device *const dev,
			  uint8_t iface)
{
	const struct usb_if_descriptor *desc = usbh_desc_get_iface(dev, iface);
	const struct usb_cfg_descriptor *cfg = dev->cfg_desc;
	const uint8_t *cursor;
	const uint8_t *end;

	ARG_UNUSED(data);
	if (desc == NULL || dev->state != USB_STATE_CONFIGURED ||
	    desc->bInterfaceClass != USB_BCC_HID || desc->bInterfaceSubClass != 1 ||
	    desc->bInterfaceProtocol != 1) {
		return -ENOTSUP;
	}
	cursor = (const uint8_t *)desc + desc->bLength;
	end = (const uint8_t *)cfg + sys_le16_to_cpu(cfg->wTotalLength);

	while (cursor + sizeof(struct usb_desc_header) <= end) {
		const struct usb_desc_header *header = (const struct usb_desc_header *)cursor;
		const struct usb_ep_descriptor *ep = (const struct usb_ep_descriptor *)cursor;

		if (header->bLength < sizeof(*header) || cursor + header->bLength > end ||
		    header->bDescriptorType == USB_DESC_INTERFACE) {
			break;
		}
		if (header->bDescriptorType == USB_DESC_ENDPOINT &&
		    header->bLength >= sizeof(*ep) && USB_EP_DIR_IS_IN(ep->bEndpointAddress) &&
		    (ep->bmAttributes & USB_EP_TRANSFER_TYPE_MASK) == USB_EP_TYPE_INTERRUPT &&
		    USB_MPS_EP_SIZE(sys_le16_to_cpu(ep->wMaxPacketSize)) >= sizeof(report)) {
			k_mutex_lock(&keyboard_lock, K_FOREVER);
			keyboard = dev;
			endpoint = ep->bEndpointAddress;
			interface = iface;
			k_mutex_unlock(&keyboard_lock);
			k_sem_give(&attached);
			return 0;
		}
		cursor += header->bLength;
	}
	return -ENOENT;
}

static int keyboard_removed(struct usbh_class_data *const data)
{
	ARG_UNUSED(data);
	/* Keep the device alive while the test thread still uses it. */
	k_mutex_lock(&keyboard_lock, K_FOREVER);
	keyboard = NULL;
	k_mutex_unlock(&keyboard_lock);
	return 0;
}

static struct usbh_class_api keyboard_api = {
	.init = keyboard_init,
	.probe = keyboard_probe,
	.removed = keyboard_removed,
};
static const struct usbh_class_filter keyboard_filter[] = {
	{.class = USB_BCC_HID, .sub = 1, .proto = 1, .flags = USBH_CLASS_MATCH_CODE_TRIPLE},
	{0},
};
USBH_DEFINE_CLASS(acceptance_keyboard, &keyboard_api, NULL, keyboard_filter);

static int keyboard_report(struct usb_device *const dev, struct uhc_transfer *const xfer)
{
	ARG_UNUSED(dev);
	transfer_status = xfer->err;
	report_len = xfer->buf->len;
	memcpy(report, xfer->buf->data, MIN(sizeof(report), report_len));
	/* The test thread owns the transfer and frees it after this callback. */
	atomic_set(&pending_completed, 1);
	k_sem_give(&report_done);
	return 0;
}

static void host_after(void *fixture)
{
	ARG_UNUSED(fixture);
	if (pending != NULL) {
		if (pending_submitted && !atomic_get(&pending_completed)) {
			(void)uhc_ep_dequeue(host.dev, pending);
			(void)k_sem_take(&report_done, K_SECONDS(1));
		}
		/* queued clears before the host completion callback is dispatched. */
		if (!pending_submitted || atomic_get(&pending_completed)) {
			if (pending->buf != NULL) {
				net_buf_unref(pending->buf);
			}
			(void)uhc_xfer_unref(pending);
			pending = NULL;
		}
	}
	if (keyboard_locked) {
		k_mutex_unlock(&keyboard_lock);
		keyboard_locked = false;
	}
	if (enabled) {
		(void)usbh_disable(&host);
		enabled = false;
	}
	if (initialized) {
		(void)usbh_shutdown(&host);
		initialized = false;
	}
}

ZTEST(hpm_usb_host, test_enumeration_hid_control_and_interrupt_in)
{
	struct net_buf *protocol;
	int64_t deadline;
	bool saw_a = false;
	bool saw_release = false;
	int ret;

	zassert_true(device_is_ready(host.dev));
	zassert_ok(usbh_init(&host));
	initialized = true;
	zassert_ok(usbh_enable(&host));
	enabled = true;
	printk("USB host: attach a boot keyboard, then press 'a' when prompted\n");
	zassert_ok(k_sem_take(&attached, K_SECONDS(60)), "Boot keyboard enumeration timeout");
	zassert_ok(k_mutex_lock(&keyboard_lock, K_SECONDS(1)));
	keyboard_locked = true;
	zassert_not_null(keyboard);
	zassert_equal(keyboard->state, USB_STATE_CONFIGURED);
	printk("USB host: VID:PID %04x:%04x, address %u, speed %u, interface %u, IN %02x\n",
	       sys_le16_to_cpu(keyboard->dev_desc.idVendor),
	       sys_le16_to_cpu(keyboard->dev_desc.idProduct), keyboard->addr, keyboard->speed,
	       interface, endpoint);
	/* HID SET_PROTOCOL(boot) and SET_IDLE: class-specific EP0 OUT requests. */
	zassert_ok(usbh_req_setup(keyboard, 0x21, 0x0b, 0, interface, 0, NULL));
	zassert_ok(usbh_req_setup(keyboard, 0x21, 0x0a, 0, interface, 0, NULL));
	protocol = usbh_xfer_buf_alloc(keyboard, 1U);
	zassert_not_null(protocol);
	ret = usbh_req_setup(keyboard, 0xa1, 0x03, 0, interface, 1, protocol);
	if (ret == 0 && (protocol->len != 1U || protocol->data[0] != 0U)) {
		ret = -EBADMSG;
	}
	net_buf_unref(protocol);
	zassert_ok(ret, "HID GET_PROTOCOL did not confirm boot protocol");
	printk("USB host: press and release 'a' now (60 s)\n");
	deadline = k_uptime_get() + 60000;

	while (!saw_release && k_uptime_get() < deadline) {
		bool has_a = false;
		bool all_released = true;

		pending_submitted = false;
		atomic_clear(&pending_completed);
		k_sem_reset(&report_done);
		pending = usbh_xfer_alloc(keyboard, endpoint, keyboard_report, NULL);
		zassert_not_null(pending);
		pending->buf = usbh_xfer_buf_alloc(keyboard, sizeof(report));
		zassert_not_null(pending->buf);
		pending->buf->len = 0;
		zassert_ok(usbh_xfer_enqueue(keyboard, pending));
		pending_submitted = true;
		zassert_ok(k_sem_take(&report_done, K_MSEC(MAX(deadline - k_uptime_get(), 1))),
			   "No HID interrupt-IN report");
		zassert_ok(transfer_status);
		zassert_equal(report_len, sizeof(report), "Expected an 8-byte boot report");
		zassert_equal(report[1], 0U, "Boot report reserved byte is nonzero");
		for (size_t i = 2; i < sizeof(report); ++i) {
			has_a |= report[i] == 0x04;
			all_released &= report[i] == 0U;
		}
		if (has_a) {
			saw_a = true;
			printk("USB host: received 'a' key-down\n");
		}
		saw_release = saw_a && all_released;
		net_buf_unref(pending->buf);
		(void)uhc_xfer_unref(pending);
		pending = NULL;
	}
	zassert_true(saw_a, "No HID usage 0x04 ('a') received");
	zassert_true(saw_release, "No all-keys-released report after 'a'");
	printk("USB host: boot protocol readback, key-down and key-up reports passed\n");
}

ZTEST_SUITE(hpm_usb_host, NULL, NULL, NULL, host_after, NULL);
