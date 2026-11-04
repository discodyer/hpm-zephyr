// SPDX-License-Identifier: Apache-2.0

#include <string.h>

#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/class/hid.h>
#include <zephyr/usb/usbh.h>
#include <zephyr/ztest.h>

/* The experimental host stack currently keeps transfer helpers private. */
#include <usbh_ch9.h>
#include <usbh_class.h>
#include <usbh_desc.h>
#include <usbh_device.h>

#define REPORT_DESCRIPTOR_MAX  512U
#define INPUT_REPORT_MAX       64U
#define REPORT_TIMEOUT_SECONDS 60

struct hid_descriptor {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint16_t bcdHID;
	uint8_t bCountryCode;
	uint8_t bNumDescriptors;
	uint8_t bReportDescriptorType;
	uint16_t wDescriptorLength;
} __packed;

USBH_CONTROLLER_DEFINE(hid_host, DEVICE_DT_GET(DT_NODELABEL(usb0)));
static K_SEM_DEFINE(attached, 0, 1);
static K_SEM_DEFINE(report_done, 0, 1);
static K_MUTEX_DEFINE(hid_lock);
static struct usb_device *hid_device;
static uint8_t hid_endpoint;
static uint8_t hid_interface;
static uint16_t hid_endpoint_mps;
static uint16_t report_descriptor_length;
static int transfer_status;
static uint8_t report[INPUT_REPORT_MAX];
static size_t report_length;
static struct uhc_transfer *pending;
static atomic_t pending_completed;
static bool pending_submitted;
static bool hid_locked;
static bool initialized;
static bool enabled;

static int hid_init(struct usbh_class_data *const data)
{
	ARG_UNUSED(data);
	return 0;
}

static int hid_probe(struct usbh_class_data *const data, struct usb_device *const dev,
		     uint8_t iface)
{
	const struct usb_if_descriptor *iface_desc = usbh_desc_get_iface(dev, iface);
	const struct usb_cfg_descriptor *cfg = dev->cfg_desc;
	const uint8_t *cursor;
	const uint8_t *end;
	uint16_t descriptor_length = 0U;
	uint16_t endpoint_mps = 0U;
	uint8_t endpoint = 0U;

	ARG_UNUSED(data);
	if (iface_desc == NULL || dev->state != USB_STATE_CONFIGURED ||
	    iface_desc->bInterfaceClass != USB_BCC_HID || iface_desc->bInterfaceSubClass != 1U ||
	    iface_desc->bInterfaceProtocol != HID_BOOT_IFACE_CODE_KEYBOARD) {
		return -ENOTSUP;
	}

	cursor = (const uint8_t *)iface_desc + iface_desc->bLength;
	end = (const uint8_t *)cfg + sys_le16_to_cpu(cfg->wTotalLength);
	while (cursor + sizeof(struct usb_desc_header) <= end) {
		const struct usb_desc_header *header = (const struct usb_desc_header *)cursor;
		const struct usb_ep_descriptor *ep = (const struct usb_ep_descriptor *)cursor;
		const struct hid_descriptor *hid = (const struct hid_descriptor *)cursor;

		if (header->bLength < sizeof(*header) || cursor + header->bLength > end ||
		    header->bDescriptorType == USB_DESC_INTERFACE) {
			break;
		}
		if (header->bDescriptorType == USB_DESC_HID && header->bLength >= sizeof(*hid)) {
			descriptor_length = sys_get_le16((const uint8_t *)&hid->wDescriptorLength);
		} else if (header->bDescriptorType == USB_DESC_ENDPOINT &&
			   header->bLength >= sizeof(*ep) &&
			   USB_EP_DIR_IS_IN(ep->bEndpointAddress) &&
			   (ep->bmAttributes & USB_EP_TRANSFER_TYPE_MASK) ==
				   USB_EP_TYPE_INTERRUPT) {
			endpoint = ep->bEndpointAddress;
			endpoint_mps = USB_MPS_EP_SIZE(sys_le16_to_cpu(ep->wMaxPacketSize));
		}
		cursor += header->bLength;
	}

	if (descriptor_length == 0U || descriptor_length > REPORT_DESCRIPTOR_MAX ||
	    endpoint == 0U || endpoint_mps == 0U || endpoint_mps > INPUT_REPORT_MAX) {
		return -ENOENT;
	}

	k_mutex_lock(&hid_lock, K_FOREVER);
	hid_device = dev;
	hid_endpoint = endpoint;
	hid_interface = iface;
	hid_endpoint_mps = endpoint_mps;
	report_descriptor_length = descriptor_length;
	k_mutex_unlock(&hid_lock);
	k_sem_give(&attached);
	return 0;
}

static int hid_removed(struct usbh_class_data *const data)
{
	ARG_UNUSED(data);
	k_mutex_lock(&hid_lock, K_FOREVER);
	hid_device = NULL;
	k_mutex_unlock(&hid_lock);
	return 0;
}

static struct usbh_class_api hid_api = {
	.init = hid_init,
	.probe = hid_probe,
	.removed = hid_removed,
};
static const struct usbh_class_filter hid_filter[] = {
	{
		.class = USB_BCC_HID,
		.sub = 1U,
		.proto = HID_BOOT_IFACE_CODE_KEYBOARD,
		.flags = USBH_CLASS_MATCH_CODE_TRIPLE,
	},
	{0},
};
USBH_DEFINE_CLASS(acceptance_hid_report, &hid_api, NULL, hid_filter);

static int hid_report(struct usb_device *const dev, struct uhc_transfer *const xfer)
{
	ARG_UNUSED(dev);
	transfer_status = xfer->err;
	report_length = MIN(sizeof(report), xfer->buf->len);
	memcpy(report, xfer->buf->data, report_length);
	atomic_set(&pending_completed, 1);
	k_sem_give(&report_done);
	return 0;
}

static int fetch_report(uint8_t *data, size_t *length, int64_t deadline)
{
	int64_t remaining = deadline - k_uptime_get();
	int ret;

	if (remaining <= 0) {
		return -ETIMEDOUT;
	}

	pending_submitted = false;
	atomic_clear(&pending_completed);
	k_sem_reset(&report_done);
	pending = usbh_xfer_alloc(hid_device, hid_endpoint, hid_report, NULL);
	if (pending == NULL) {
		return -ENOMEM;
	}
	pending->buf = usbh_xfer_buf_alloc(hid_device, hid_endpoint_mps);
	if (pending->buf == NULL) {
		(void)uhc_xfer_unref(pending);
		pending = NULL;
		return -ENOMEM;
	}
	pending->buf->len = 0U;
	ret = usbh_xfer_enqueue(hid_device, pending);
	if (ret != 0) {
		net_buf_unref(pending->buf);
		(void)uhc_xfer_unref(pending);
		pending = NULL;
		return ret;
	}
	pending_submitted = true;

	ret = k_sem_take(&report_done, K_MSEC(remaining));
	if (ret != 0) {
		ret = uhc_ep_dequeue(hid_host.dev, pending);
		if (ret == 0) {
			ret = k_sem_take(&report_done, K_SECONDS(1));
			if (ret != 0) {
				return -ETIMEDOUT;
			}
		} else {
			return ret;
		}
	}

	if (transfer_status != 0) {
		ret = transfer_status;
	} else {
		*length = report_length;
		memcpy(data, report, report_length);
		ret = 0;
	}

	net_buf_unref(pending->buf);
	(void)uhc_xfer_unref(pending);
	pending = NULL;
	return ret;
}

static bool report_is_zero(const uint8_t *data, size_t length)
{
	for (size_t i = 0U; i < length; ++i) {
		if (data[i] != 0U) {
			return false;
		}
	}
	return true;
}

static void print_report(const char *label, const uint8_t *data, size_t length)
{
	printk("USB host: %s report (%zu bytes):", label, length);
	for (size_t i = 0U; i < length; ++i) {
		printk(" %02x", data[i]);
	}
	printk("\n");
}

static void hid_after(void *fixture)
{
	ARG_UNUSED(fixture);
	if (pending != NULL) {
		if (pending_submitted && !atomic_get(&pending_completed)) {
			(void)uhc_ep_dequeue(hid_host.dev, pending);
			(void)k_sem_take(&report_done, K_SECONDS(1));
		}
		if (!pending_submitted || atomic_get(&pending_completed)) {
			if (pending->buf != NULL) {
				net_buf_unref(pending->buf);
			}
			(void)uhc_xfer_unref(pending);
			pending = NULL;
		}
	}
	if (hid_locked) {
		k_mutex_unlock(&hid_lock);
		hid_locked = false;
	}
	if (enabled) {
		(void)usbh_disable(&hid_host);
		enabled = false;
	}
	if (initialized) {
		(void)usbh_shutdown(&hid_host);
		initialized = false;
	}
}

ZTEST(hpm_usb_host_hid_report, test_report_protocol_and_input_reports)
{
	uint8_t active_report[INPUT_REPORT_MAX];
	uint8_t release_report[INPUT_REPORT_MAX];
	struct net_buf *descriptor;
	struct net_buf *protocol;
	size_t active_length = 0U;
	size_t release_length = 0U;
	int64_t deadline;
	int ret;

	zassert_true(device_is_ready(hid_host.dev));
	zassert_ok(usbh_init(&hid_host));
	initialized = true;
	zassert_ok(usbh_enable(&hid_host));
	enabled = true;
	printk("USB host: attach a wired boot keyboard through the OTG adapter\n");
	zassert_ok(k_sem_take(&attached, K_SECONDS(60)), "HID keyboard enumeration timeout");
	zassert_ok(k_mutex_lock(&hid_lock, K_SECONDS(1)));
	hid_locked = true;
	zassert_not_null(hid_device);
	zassert_equal(hid_device->state, USB_STATE_CONFIGURED);
	printk("USB host: VID:PID %04x:%04x, address %u, speed %u, interface %u, IN %02x, MPS %u\n",
	       sys_le16_to_cpu(hid_device->dev_desc.idVendor),
	       sys_le16_to_cpu(hid_device->dev_desc.idProduct), hid_device->addr, hid_device->speed,
	       hid_interface, hid_endpoint, hid_endpoint_mps);

	descriptor = usbh_xfer_buf_alloc(hid_device, report_descriptor_length);
	zassert_not_null(descriptor);
	ret = usbh_req_desc(hid_device, USB_DESC_HID_REPORT, 0U, hid_interface,
			    report_descriptor_length, descriptor);
	zassert_ok(ret, "HID report descriptor request failed");
	zassert_equal(descriptor->len, report_descriptor_length,
		      "Short HID report descriptor: %zu of %u bytes", descriptor->len,
		      report_descriptor_length);
	printk("USB host: HID report descriptor %zu bytes, first bytes %02x %02x %02x %02x\n",
	       descriptor->len, descriptor->data[0], descriptor->data[1], descriptor->data[2],
	       descriptor->data[3]);
	net_buf_unref(descriptor);

	zassert_ok(usbh_req_setup(hid_device, 0x21U, USB_HID_SET_PROTOCOL, HID_PROTOCOL_REPORT,
				  hid_interface, 0U, NULL),
		   "SET_PROTOCOL(report) failed");
	protocol = usbh_xfer_buf_alloc(hid_device, 1U);
	zassert_not_null(protocol);
	zassert_ok(usbh_req_setup(hid_device, 0xa1U, USB_HID_GET_PROTOCOL, 0U, hid_interface, 1U,
				  protocol),
		   "GET_PROTOCOL failed");
	zassert_equal(protocol->len, 1U);
	zassert_equal(protocol->data[0], HID_PROTOCOL_REPORT,
		      "HID protocol readback is %u, expected report", protocol->data[0]);
	net_buf_unref(protocol);
	printk("USB host: report protocol selected and read back successfully\n");

	printk("USB host: press and release a key within %d s\n", REPORT_TIMEOUT_SECONDS);
	deadline = k_uptime_get() + REPORT_TIMEOUT_SECONDS * 1000;
	do {
		zassert_ok(fetch_report(active_report, &active_length, deadline),
			   "No HID interrupt-IN report");
	} while (report_is_zero(active_report, active_length));
	print_report("key-down", active_report, active_length);

	deadline = k_uptime_get() + REPORT_TIMEOUT_SECONDS * 1000;
	do {
		zassert_ok(fetch_report(release_report, &release_length, deadline),
			   "No HID release report");
	} while (release_length == active_length &&
		 memcmp(release_report, active_report, active_length) == 0);
	print_report("release", release_report, release_length);
	zassert_true(report_is_zero(release_report, release_length),
		     "Release report is not all zero");
	printk("USB host: HID report protocol and input report transitions passed\n");
}

ZTEST_SUITE(hpm_usb_host_hid_report, NULL, NULL, NULL, hid_after, NULL);
