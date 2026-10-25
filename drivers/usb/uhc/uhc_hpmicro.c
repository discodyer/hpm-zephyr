/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_usb

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usb_ch9.h>

#include <hpm_clock_drv.h>
#include <hpm_usb_drv.h>

#include "uhc_common.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(uhc_hpmicro, CONFIG_UHC_DRIVER_LOG_LEVEL);

#define HPM_USB_FRAME_LIST_SIZE 1024U
#define HPM_USB_QTD_MAX_BYTES   4096U
#define HPM_USB_ITD_MAX_BYTES   3072U
#define HPM_USB_QTD_MASK        0xFFFFFFE0U
#define HPM_USB_QTD_END         0x00000001U
#define HPM_USB_QH_END          0x00000001U
#define HPM_USB_QH_LINK_MASK    0xFFFFFFE0U
#define HPM_USB_QH_LINK_QH      0x00000002U
#define HPM_USB_CERR            3U

#define HPM_USB_ITD_STATUS_MASK    0xF0000000U
#define HPM_USB_ITD_STATUS_ACTIVE  BIT(31)
#define HPM_USB_ITD_STATUS_DBERR   BIT(30)
#define HPM_USB_ITD_STATUS_BABBLE  BIT(29)
#define HPM_USB_ITD_STATUS_XACTERR BIT(28)
#define HPM_USB_ITD_LENGTH_SHIFT   16U
#define HPM_USB_ITD_LENGTH_MASK    (0xFFFU << HPM_USB_ITD_LENGTH_SHIFT)
#define HPM_USB_ITD_IOC            BIT(15)
#define HPM_USB_ITD_PG_SHIFT       12U
#define HPM_USB_ITD_OFFSET_MASK    BIT_MASK(12)
#define HPM_USB_ITD_BP_MASK        0xFFFFF000U
#define HPM_USB_ITD_DIRECTION_IN   BIT(11)
#define HPM_USB_ITD_MPS_MASK       BIT_MASK(11)

#define HPM_USB_QTD_STATUS_ACTIVE  BIT(7)
#define HPM_USB_QTD_STATUS_HALTED  BIT(6)
#define HPM_USB_QTD_STATUS_DBERR   BIT(5)
#define HPM_USB_QTD_STATUS_BABBLE  BIT(4)
#define HPM_USB_QTD_STATUS_XACTERR BIT(3)
#define HPM_USB_QTD_STATUS_ERRORS                                                                  \
	(HPM_USB_QTD_STATUS_DBERR | HPM_USB_QTD_STATUS_BABBLE | HPM_USB_QTD_STATUS_XACTERR)
#define HPM_USB_QTD_PID_SHIFT    8U
#define HPM_USB_QTD_PID_OUT      (0U << HPM_USB_QTD_PID_SHIFT)
#define HPM_USB_QTD_PID_IN       (1U << HPM_USB_QTD_PID_SHIFT)
#define HPM_USB_QTD_PID_SETUP    (2U << HPM_USB_QTD_PID_SHIFT)
#define HPM_USB_QTD_CERR_SHIFT   10U
#define HPM_USB_QTD_IOC          BIT(15)
#define HPM_USB_QTD_NBYTES_SHIFT 16U
#define HPM_USB_QTD_NBYTES_MASK  (0x7FFFU << HPM_USB_QTD_NBYTES_SHIFT)
#define HPM_USB_QTD_TOGGLE       BIT(31)

#define HPM_USB_QH_EPCHAR_DEVADDR_SHIFT 0U
#define HPM_USB_QH_EPCHAR_ENDPT_SHIFT   8U
#define HPM_USB_QH_EPCHAR_EPS_SHIFT     12U
#define HPM_USB_QH_EPCHAR_DTC           BIT(14)
#define HPM_USB_QH_EPCHAR_H             BIT(15)
#define HPM_USB_QH_EPCHAR_MAXPKT_SHIFT  16U
#define HPM_USB_QH_EPCHAR_C             BIT(27)
#define HPM_USB_QH_EPCHAR_RL_SHIFT      28U
#define HPM_USB_QH_EPCAPS_SMASK_SHIFT   0U

#define HPM_USB_PORTSC_W1C (USB_PORTSC1_CSC_MASK | USB_PORTSC1_PEC_MASK | USB_PORTSC1_OCC_MASK)
#define HPM_USB_USBSTS_W1C                                                                         \
	(USB_USBSTS_UI_MASK | USB_USBSTS_UEI_MASK | USB_USBSTS_PCI_MASK | USB_USBSTS_SRI_MASK |    \
	 USB_USBSTS_SLI_MASK | USB_USBSTS_URI_MASK | USB_USBSTS_AAI_MASK | USB_USBSTS_UAI_MASK)
#define HPM_USB_HOST_INTRS                                                                         \
	(USB_USBINTR_UE_MASK | USB_USBINTR_UEE_MASK | USB_USBINTR_PCE_MASK | USB_USBINTR_SRE_MASK)

struct hpm_ehci_qtd {
	uint32_t next_qtd;
	uint32_t alt_next_qtd;
	uint32_t token;
	uint32_t bpl[5];
} __aligned(32);

struct hpm_ehci_qh {
	uint32_t hlp;
	uint32_t epchar;
	uint32_t epcap;
	uint32_t curr_qtd;
	struct hpm_ehci_qtd overlay;
} __aligned(32);

struct hpm_ehci_itd {
	uint32_t next_itd;
	uint32_t control[8];
	uint32_t bpl[7];
} __aligned(64);

struct uhc_hpmicro_qtd {
	struct hpm_ehci_qtd hw;
	bool in_use;
	bool count_bytes;
	size_t length;
	struct uhc_transfer *xfer;
} __aligned(32);

struct uhc_hpmicro_qh {
	struct hpm_ehci_qh hw;
	bool in_use;
	bool periodic;
	bool data_toggle;
	struct uhc_transfer *xfer;
	struct uhc_hpmicro_qtd *first_qtd;
} __aligned(32);

struct uhc_hpmicro_itd {
	struct hpm_ehci_itd hw;
	struct uhc_transfer *xfer;
	uint16_t length;
	uint8_t microframe;
	uint16_t frame_slot;
	bool in_use;
	bool count_bytes;
} __aligned(64);

struct hpm_usb_toggle {
	const struct usb_device *udev;
	uint8_t ep;
	bool data1;
};

struct uhc_hpmicro_config {
	USB_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	clock_name_t clock_name;
	clock_source_t clock_src;
	uint32_t clock_div;
	bool power_ctrl_active_high;
	const struct pinctrl_dev_config *pincfg;
	void (*irq_enable_func)(const struct device *dev);
	void (*irq_disable_func)(const struct device *dev);
};

struct uhc_hpmicro_data {
	struct k_thread thread;
	struct k_event events;
	struct k_spinlock lock;
	struct uhc_hpmicro_qh *async_head;
	struct uhc_hpmicro_qh *periodic_head;
	struct uhc_hpmicro_qh *qh_pool;
	struct uhc_hpmicro_qtd *qtd_pool;
	struct uhc_hpmicro_itd *itd_pool;
	uint32_t *frame_list;
	struct hpm_usb_toggle toggles[CONFIG_UHC_HPMICRO_QH_COUNT];
	bool port_connected;
};

enum uhc_hpmicro_event {
	UHC_HPMICRO_EVENT_PORT_CHANGE = 0,
	UHC_HPMICRO_EVENT_IRQ,
};

static inline bool qtd_is_active(const struct hpm_ehci_qtd *qtd)
{
	return (qtd->token & HPM_USB_QTD_STATUS_ACTIVE) != 0U;
}

static inline bool qtd_has_error(const struct hpm_ehci_qtd *qtd)
{
	return ((qtd->token & (HPM_USB_QTD_STATUS_HALTED | HPM_USB_QTD_STATUS_ERRORS)) != 0U) &&
	       !qtd_is_active(qtd);
}

static inline uint32_t qtd_remaining(const struct hpm_ehci_qtd *qtd)
{
	return (qtd->token & HPM_USB_QTD_NBYTES_MASK) >> HPM_USB_QTD_NBYTES_SHIFT;
}

static struct uhc_hpmicro_qtd *qtd_next(struct uhc_hpmicro_qtd *qtd)
{
	uint32_t next = qtd->hw.next_qtd & HPM_USB_QTD_MASK;

	if (next == 0U) {
		return NULL;
	}

	return CONTAINER_OF((void *)(uintptr_t)next, struct uhc_hpmicro_qtd, hw);
}

static struct uhc_hpmicro_qh *qh_next(struct uhc_hpmicro_qh *qh)
{
	uint32_t next = qh->hw.hlp & HPM_USB_QH_LINK_MASK;

	if (next == 0U) {
		return NULL;
	}

	return CONTAINER_OF((void *)(uintptr_t)next, struct uhc_hpmicro_qh, hw);
}

static void qtd_bpl_fill(struct hpm_ehci_qtd *qtd, uintptr_t address, size_t length)
{
	uintptr_t page = address & ~BIT_MASK(12);

	qtd->bpl[0] = (uint32_t)address;
	for (uint8_t i = 1U; i < ARRAY_SIZE(qtd->bpl); i++) {
		if (length <= (BIT(12) - (address & BIT_MASK(12)))) {
			qtd->bpl[i] = 0U;
			break;
		}
		page += BIT(12);
		qtd->bpl[i] = (uint32_t)page;
	}
}

static struct uhc_hpmicro_qtd *qtd_alloc(struct uhc_hpmicro_data *data)
{
	for (uint32_t i = 0U; i < CONFIG_UHC_HPMICRO_QTD_COUNT; i++) {
		if (!data->qtd_pool[i].in_use) {
			data->qtd_pool[i].in_use = true;
			return &data->qtd_pool[i];
		}
	}

	return NULL;
}

static void qtd_free_chain(struct uhc_hpmicro_data *data, struct uhc_hpmicro_qtd *qtd)
{
	while (qtd != NULL) {
		struct uhc_hpmicro_qtd *next = qtd_next(qtd);

		qtd->in_use = false;
		qtd->xfer = NULL;
		qtd = next;
	}
}

static struct uhc_hpmicro_qh *qh_alloc(struct uhc_hpmicro_data *data, bool periodic)
{
	for (uint32_t i = 0U; i < CONFIG_UHC_HPMICRO_QH_COUNT; i++) {
		if (!data->qh_pool[i].in_use) {
			memset(&data->qh_pool[i], 0, sizeof(data->qh_pool[i]));
			data->qh_pool[i].in_use = true;
			data->qh_pool[i].periodic = periodic;
			return &data->qh_pool[i];
		}
	}

	return NULL;
}

static void qh_free(struct uhc_hpmicro_data *data, struct uhc_hpmicro_qh *qh)
{
	qtd_free_chain(data, qh->first_qtd);
	qh->first_qtd = NULL;
	qh->xfer = NULL;
	qh->in_use = false;
}

static void qh_link_insert(struct uhc_hpmicro_qh *head, struct uhc_hpmicro_qh *qh)
{
	qh->hw.hlp = head->hw.hlp;
	head->hw.hlp = ((uint32_t)qh & HPM_USB_QH_LINK_MASK) | HPM_USB_QH_LINK_QH;
}

static void qh_link_remove(struct uhc_hpmicro_qh *head, struct uhc_hpmicro_qh *remove)
{
	struct uhc_hpmicro_qh *qh = head;

	while (qh_next(qh) != remove) {
		qh = qh_next(qh);
		if ((qh == NULL) || (qh == head)) {
			return;
		}
	}

	qh->hw.hlp = remove->hw.hlp;
}

static void qh_schedule_disable(const struct device *dev)
{
	const struct uhc_hpmicro_config *config = dev->config;

	k_timepoint_t timeout = sys_timepoint_calc(K_MSEC(100));

	config->base->USBCMD &= ~(USB_USBCMD_PSE_MASK | USB_USBCMD_ASE_MASK);
	while ((config->base->USBCMD & (USB_USBCMD_PSE_MASK | USB_USBCMD_ASE_MASK)) != 0U) {
		if (sys_timepoint_expired(timeout)) {
			break;
		}
	}
	while ((config->base->USBSTS & (USB_USBSTS_AS_MASK | USB_USBSTS_PS_MASK)) != 0U) {
		if (sys_timepoint_expired(timeout)) {
			break;
		}
	}
}

static void qh_schedule_enable(const struct device *dev)
{
	const struct uhc_hpmicro_config *config = dev->config;

	config->base->USBCMD |= USB_USBCMD_PSE_MASK | USB_USBCMD_ASE_MASK;
}

static bool endpoint_toggle_get(struct uhc_hpmicro_data *data, const struct usb_device *udev,
				uint8_t ep)
{
	for (uint32_t i = 0U; i < ARRAY_SIZE(data->toggles); i++) {
		if ((data->toggles[i].udev == udev) && (data->toggles[i].ep == ep)) {
			return data->toggles[i].data1;
		}
	}

	return false;
}

static void endpoint_toggle_set(struct uhc_hpmicro_data *data, const struct usb_device *udev,
				uint8_t ep, bool data1)
{
	struct hpm_usb_toggle *free_entry = NULL;

	for (uint32_t i = 0U; i < ARRAY_SIZE(data->toggles); i++) {
		if ((data->toggles[i].udev == udev) && (data->toggles[i].ep == ep)) {
			data->toggles[i].data1 = data1;
			return;
		}

		if ((free_entry == NULL) && (data->toggles[i].udev == NULL)) {
			free_entry = &data->toggles[i];
		}
	}

	if (free_entry != NULL) {
		free_entry->udev = udev;
		free_entry->ep = ep;
		free_entry->data1 = data1;
	}
}

static uint8_t qh_speed(const struct usb_device *udev)
{
	switch (udev->speed) {
	case USB_SPEED_SPEED_LS:
		return 1U;
	case USB_SPEED_SPEED_FS:
		return 0U;
	case USB_SPEED_SPEED_HS:
		return 2U;
	default:
		return 2U;
	}
}

static uint8_t periodic_smask(uint16_t interval)
{
	uint16_t value = 1U;
	uint8_t order = 0U;

	while (interval > 1U) {
		value <<= 1U;
		interval--;
	}

	if (value < 2U) {
		return 0xFFU;
	}
	if (value < 4U) {
		return 0x55U;
	}
	if (value < 8U) {
		return 0x22U;
	}

	for (order = 0U; value > 1U; order++) {
		value >>= 1U;
	}
	return BIT(order % 8U);
}

static void qh_fill(struct uhc_hpmicro_qh *qh, const struct uhc_transfer *xfer, bool periodic)
{
	const uint8_t ep = USB_EP_GET_IDX(xfer->ep);
	uint32_t epchar = 0U;
	uint32_t epcap = 0U;

	epchar |= FIELD_PREP(BIT_MASK(7), xfer->udev->addr);
	epchar |= FIELD_PREP(BIT_MASK(4) << HPM_USB_QH_EPCHAR_ENDPT_SHIFT, ep);
	epchar |= FIELD_PREP(BIT_MASK(2) << HPM_USB_QH_EPCHAR_EPS_SHIFT, qh_speed(xfer->udev));
	epchar |= FIELD_PREP(BIT_MASK(11) << HPM_USB_QH_EPCHAR_MAXPKT_SHIFT,
			     USB_MPS_EP_SIZE(xfer->mps));

	if (xfer->type == USB_EP_TYPE_CONTROL) {
		epchar |= HPM_USB_QH_EPCHAR_DTC;
		if (xfer->udev->speed != USB_SPEED_SPEED_HS) {
			epchar |= HPM_USB_QH_EPCHAR_C;
		}
		epchar |= FIELD_PREP(BIT_MASK(4) << HPM_USB_QH_EPCHAR_RL_SHIFT,
				     xfer->udev->speed == USB_SPEED_SPEED_HS ? 4U : 0U);
	} else {
		epchar |= HPM_USB_QH_EPCHAR_DTC;
	}

	if (xfer->udev->speed != USB_SPEED_SPEED_HS) {
		uint8_t hub_addr = xfer->udev->hub == NULL ? 0U : xfer->udev->hub->addr;
		uint8_t hub_port = xfer->udev->hub == NULL ? 1U : xfer->udev->hub_port;

		epcap |= FIELD_PREP(BIT_MASK(7) << 16, hub_addr);
		epcap |= FIELD_PREP(BIT_MASK(7) << 23, hub_port);
	}

	if (periodic) {
		epcap |= FIELD_PREP(BIT_MASK(8) << HPM_USB_QH_EPCAPS_SMASK_SHIFT,
				    periodic_smask(xfer->interval));
		if (xfer->udev->speed != USB_SPEED_SPEED_HS) {
			epcap |= FIELD_PREP(BIT_MASK(8) << 8, 0x78U);
		}
	}

	if (xfer->udev->speed == USB_SPEED_SPEED_HS) {
		uint8_t mult = periodic ? USB_MPS_ADDITIONAL_TRANSACTIONS(xfer->mps) + 1U : 1U;

		epcap |= FIELD_PREP(BIT_MASK(2) << 30, MIN(mult, 3U));
	}

	qh->hw.hlp = HPM_USB_QH_END;
	qh->hw.epchar = epchar;
	qh->hw.epcap = epcap;
	qh->hw.curr_qtd = 0U;
	qh->hw.overlay.next_qtd = HPM_USB_QTD_END;
	qh->hw.overlay.alt_next_qtd = HPM_USB_QTD_END;
	qh->hw.overlay.token = HPM_USB_QTD_STATUS_HALTED;
	qh->xfer = NULL;
}

static int qtd_prepare(struct uhc_hpmicro_data *data, struct uhc_hpmicro_qtd **first,
		       struct uhc_hpmicro_qtd **last, struct uhc_transfer *xfer, uint8_t *buffer,
		       size_t length, uint32_t pid, bool toggle, bool count_bytes)
{
	size_t offset = 0U;
	struct uhc_hpmicro_qtd *prev = NULL;

	*first = NULL;
	*last = NULL;

	do {
		struct uhc_hpmicro_qtd *qtd = qtd_alloc(data);
		size_t chunk = MIN(length - offset, HPM_USB_QTD_MAX_BYTES);
		bool last_chunk = (offset + chunk) == length;
		uint32_t token;

		if (qtd == NULL) {
			if (*first != NULL) {
				qtd_free_chain(data, *first);
			}
			return -ENOMEM;
		}

		memset(&qtd->hw, 0, sizeof(qtd->hw));
		qtd->xfer = xfer;
		qtd->length = chunk;
		qtd->count_bytes = count_bytes;

		token = HPM_USB_QTD_STATUS_ACTIVE | pid | (HPM_USB_CERR << HPM_USB_QTD_CERR_SHIFT) |
			(chunk << HPM_USB_QTD_NBYTES_SHIFT);
		if (toggle) {
			token |= HPM_USB_QTD_TOGGLE;
		}
		if (last_chunk) {
			token |= HPM_USB_QTD_IOC;
		}

		qtd->hw.token = token;
		qtd->hw.next_qtd = HPM_USB_QTD_END;
		qtd->hw.alt_next_qtd = HPM_USB_QTD_END;
		if (chunk != 0U) {
			qtd_bpl_fill(&qtd->hw, (uintptr_t)buffer + offset, chunk);
		}

		if (prev != NULL) {
			prev->hw.next_qtd = (uint32_t)qtd & HPM_USB_QTD_MASK;
		} else {
			*first = qtd;
		}
		prev = qtd;
		*last = qtd;
		offset += chunk;

		if ((chunk != 0U) && ((DIV_ROUND_UP(chunk, xfer->mps) & 1U) != 0U)) {
			toggle = !toggle;
		}
	} while (offset < length);

	return 0;
}

static void qh_start(struct uhc_hpmicro_qh *qh, struct uhc_hpmicro_qtd *first)
{
	qh->first_qtd = first;
	qh->hw.curr_qtd = (uint32_t)first & HPM_USB_QTD_MASK;
	qh->hw.overlay.next_qtd = (uint32_t)first & HPM_USB_QTD_MASK;
	qh->hw.overlay.alt_next_qtd = HPM_USB_QTD_END;
}

static struct uhc_hpmicro_itd *itd_alloc(struct uhc_hpmicro_data *data)
{
	for (uint32_t i = 0U; i < CONFIG_UHC_HPMICRO_ITD_COUNT; i++) {
		if (!data->itd_pool[i].in_use) {
			data->itd_pool[i].in_use = true;
			return &data->itd_pool[i];
		}
	}

	return NULL;
}

static void itd_free(struct uhc_hpmicro_itd *itd)
{
	if (itd == NULL) {
		return;
	}

	itd->in_use = false;
	itd->xfer = NULL;
	itd->length = 0U;
}

static inline bool itd_is_active(const struct uhc_hpmicro_itd *itd)
{
	return (itd->hw.control[itd->microframe] & HPM_USB_ITD_STATUS_ACTIVE) != 0U;
}

static int itd_completion_status(const struct uhc_hpmicro_itd *itd)
{
	uint32_t control = itd->hw.control[itd->microframe];

	if ((control & HPM_USB_ITD_STATUS_ACTIVE) != 0U) {
		return -EINPROGRESS;
	}

	return (control & (HPM_USB_ITD_STATUS_DBERR | HPM_USB_ITD_STATUS_BABBLE |
			   HPM_USB_ITD_STATUS_XACTERR)) != 0U
		       ? -EIO
		       : 0;
}

static uint8_t itd_interval_shift(uint16_t interval)
{
	if (interval == 0U) {
		return 0U;
	}

	return (uint8_t)MIN(interval - 1U, 15U);
}

static int itd_schedule_point(const struct device *dev, uint16_t interval, uint16_t *frame_slot,
			      uint8_t *microframe)
{
	const struct uhc_hpmicro_config *config = dev->config;
	uint8_t shift = itd_interval_shift(interval);
	uint32_t interval_microframes = BIT(shift);
	uint32_t target = usb_hcd_get_frame_index(config->base) + 8U;

	target = (target + interval_microframes - 1U) & ~(interval_microframes - 1U);
	*frame_slot = (uint16_t)((target >> 3U) & BIT_MASK(10));
	*microframe = (uint8_t)(target & BIT_MASK(3));
	return 0;
}

static void itd_link_insert(struct uhc_hpmicro_data *data, struct uhc_hpmicro_itd *itd)
{
	uint32_t *entry = &data->frame_list[itd->frame_slot];

	itd->hw.next_itd = *entry;
	*entry = (uint32_t)(uintptr_t)&itd->hw;
}

static void itd_link_remove(struct uhc_hpmicro_data *data, struct uhc_hpmicro_itd *itd)
{
	uint32_t *link = &data->frame_list[itd->frame_slot];
	uint32_t target = (uint32_t)(uintptr_t)&itd->hw;
	uint32_t node = *link;

	for (uint32_t i = 0U; i < CONFIG_UHC_HPMICRO_ITD_COUNT; i++) {
		if ((node & HPM_USB_QTD_MASK) == target) {
			*link = itd->hw.next_itd;
			return;
		}

		if ((node == 0U) || ((node & BIT_MASK(2)) != 0U)) {
			return;
		}

		link = (uint32_t *)(uintptr_t)(node & HPM_USB_QTD_MASK);
		node = *link;
	}
}

static int submit_isochronous(const struct device *dev, struct uhc_transfer *xfer)
{
	struct uhc_hpmicro_data *data = uhc_get_private(dev);
	struct uhc_hpmicro_itd *itd;
	uint8_t *buffer;
	size_t length;
	uint32_t control;
	uint32_t page;
	uint32_t offset;
	uint32_t slot;
	bool in = USB_EP_DIR_IS_IN(xfer->ep);
	uint32_t mps = USB_MPS_EP_SIZE(xfer->mps);
	uint32_t tpl = USB_MPS_TO_TPL(xfer->mps);
	uint32_t mult = USB_MPS_ADDITIONAL_TRANSACTIONS(xfer->mps);
	uint16_t frame_slot;
	uint8_t microframe;
	int ret;

	if ((xfer->buf == NULL) || (xfer->udev->speed != USB_SPEED_SPEED_HS) || (mps == 0U) ||
	    (mps > HPM_USB_ITD_MPS_MASK)) {
		return -ENOTSUP;
	}

	if (in) {
		buffer = net_buf_tail(xfer->buf);
		length = MIN(net_buf_tailroom(xfer->buf), tpl);
	} else {
		buffer = xfer->buf->data;
		length = MIN(xfer->buf->len, tpl);
	}
	if ((length == 0U) || (length > HPM_USB_ITD_MAX_BYTES)) {
		return -EINVAL;
	}

	ret = itd_schedule_point(dev, xfer->interval, &frame_slot, &microframe);
	if (ret < 0) {
		return ret;
	}

	itd = itd_alloc(data);
	if (itd == NULL) {
		return -ENOMEM;
	}

	itd->frame_slot = frame_slot;
	itd->microframe = microframe;
	memset(&itd->hw, 0, sizeof(itd->hw));
	itd->xfer = xfer;
	itd->length = length;

	itd->hw.bpl[0] = FIELD_PREP(BIT_MASK(7), xfer->udev->addr) |
			 FIELD_PREP(BIT_MASK(4) << 8U, USB_EP_GET_IDX(xfer->ep));
	itd->hw.bpl[1] =
		(in ? HPM_USB_ITD_DIRECTION_IN : 0U) | FIELD_PREP(HPM_USB_ITD_MPS_MASK, mps);
	itd->hw.bpl[2] = MIN(mult + 1U, 3U);

	page = (uint32_t)(uintptr_t)buffer & HPM_USB_ITD_BP_MASK;
	offset = (uint32_t)(uintptr_t)buffer & HPM_USB_ITD_OFFSET_MASK;
	slot = (itd->microframe & 1U) ? 5U : 3U;
	itd->hw.bpl[slot] = page;
	if ((offset + length) > 4096U) {
		itd->hw.bpl[slot + 1U] = page + 4096U;
	}
	control = FIELD_PREP(HPM_USB_ITD_LENGTH_MASK, length) |
		  FIELD_PREP(BIT_MASK(3) << HPM_USB_ITD_PG_SHIFT, slot) | offset | HPM_USB_ITD_IOC |
		  HPM_USB_ITD_STATUS_ACTIVE;
	if (in) {
		(void)sys_cache_data_invd_range(buffer, length);
	} else {
		(void)sys_cache_data_flush_range(buffer, length);
	}
	compiler_barrier();
	itd->hw.control[itd->microframe] = control;
	itd_link_insert(data, itd);

	return 0;
}

static int submit_control(const struct device *dev, struct uhc_transfer *xfer)
{
	struct uhc_hpmicro_data *data = uhc_get_private(dev);
	const struct usb_setup_packet *setup = (const struct usb_setup_packet *)xfer->setup_pkt;
	uint16_t setup_length = sys_le16_to_cpu(setup->wLength);
	struct uhc_hpmicro_qtd *setup_qtd;
	struct uhc_hpmicro_qtd *data_qtd = NULL;
	struct uhc_hpmicro_qtd *data_last = NULL;
	struct uhc_hpmicro_qtd *status_qtd;
	struct uhc_hpmicro_qtd *last = NULL;
	struct uhc_hpmicro_qh *qh;
	uint8_t *data_buffer = NULL;
	size_t data_length = 0U;
	bool in = usb_reqtype_is_to_host(setup);
	int ret;

	qh = qh_alloc(data, false);
	if (qh == NULL) {
		return -ENOMEM;
	}

	qh_fill(qh, xfer, false);
	qh->xfer = xfer;

	ret = qtd_prepare(data, &setup_qtd, &last, xfer, xfer->setup_pkt, 8U, HPM_USB_QTD_PID_SETUP,
			  false, false);
	if (ret < 0) {
		qh_free(data, qh);
		return ret;
	}

	if (setup_length != 0U) {
		if (xfer->buf == NULL) {
			ret = -EINVAL;
			goto error;
		}

		if (in) {
			data_buffer = net_buf_tail(xfer->buf);
			data_length = MIN(net_buf_tailroom(xfer->buf), setup_length);
		} else {
			data_buffer = xfer->buf->data;
			data_length = MIN(xfer->buf->len, setup_length);
		}

		ret = qtd_prepare(data, &data_qtd, &data_last, xfer, data_buffer, data_length,
				  in ? HPM_USB_QTD_PID_IN : HPM_USB_QTD_PID_OUT, true, true);
		if (ret < 0) {
			goto error;
		}
	}

	if (!xfer->no_status) {
		ret = qtd_prepare(data, &status_qtd, &last, xfer, NULL, 0U,
				  in ? HPM_USB_QTD_PID_OUT : HPM_USB_QTD_PID_IN, true, false);
		if (ret < 0) {
			goto error;
		}

		if (data_last != NULL) {
			/* IOC belongs only to the final qTD in the control chain. */
			data_last->hw.token &= ~HPM_USB_QTD_IOC;
			data_last->hw.next_qtd = (uint32_t)status_qtd & HPM_USB_QTD_MASK;
		} else {
			setup_qtd->hw.next_qtd = (uint32_t)status_qtd & HPM_USB_QTD_MASK;
		}
	}

	if (data_qtd != NULL) {
		setup_qtd->hw.next_qtd = (uint32_t)data_qtd & HPM_USB_QTD_MASK;
	}

	if (!xfer->no_status || (data_qtd != NULL)) {
		setup_qtd->hw.token &= ~HPM_USB_QTD_IOC;
	}
	last->hw.next_qtd = HPM_USB_QTD_END;

	sys_cache_data_flush_range(xfer->setup_pkt, sizeof(xfer->setup_pkt));
	xfer->stage = UHC_CONTROL_STAGE_SETUP;
	qh_start(qh, setup_qtd);
	qh_link_insert(data->async_head, qh);

	return 0;

error:
	qh_free(data, qh);
	return ret;
}

static int submit_data(const struct device *dev, struct uhc_transfer *xfer)
{
	struct uhc_hpmicro_data *data = uhc_get_private(dev);
	struct uhc_hpmicro_qtd *first;
	struct uhc_hpmicro_qtd *last;
	struct uhc_hpmicro_qh *qh;
	uint8_t *buffer;
	size_t length;
	bool in = USB_EP_DIR_IS_IN(xfer->ep);
	bool periodic = xfer->type == USB_EP_TYPE_INTERRUPT;
	bool toggle;
	int ret;

	if ((xfer->buf == NULL) || (xfer->type == USB_EP_TYPE_ISO)) {
		return -EINVAL;
	}

	if (in) {
		buffer = net_buf_tail(xfer->buf);
		length = net_buf_tailroom(xfer->buf);
	} else {
		buffer = xfer->buf->data;
		length = xfer->buf->len;
	}

	if (length == 0U) {
		return -EINVAL;
	}

	qh = qh_alloc(data, periodic);
	if (qh == NULL) {
		return -ENOMEM;
	}
	qh_fill(qh, xfer, periodic);
	qh->xfer = xfer;

	toggle = endpoint_toggle_get(data, xfer->udev, xfer->ep);
	qh->data_toggle = toggle;
	ret = qtd_prepare(data, &first, &last, xfer, buffer, length,
			  in ? HPM_USB_QTD_PID_IN : HPM_USB_QTD_PID_OUT, toggle, true);
	if (ret < 0) {
		qh_free(data, qh);
		return ret;
	}

	qh_start(qh, first);
	if (periodic) {
		qh_link_insert(data->periodic_head, qh);
	} else {
		qh_link_insert(data->async_head, qh);
	}

	return 0;
}

static int uhc_hpmicro_enqueue(const struct device *dev, struct uhc_transfer *xfer)
{
	struct uhc_hpmicro_data *data = uhc_get_private(dev);
	k_spinlock_key_t key;
	int ret;

	if ((xfer->udev == NULL) || (xfer->mps == 0U)) {
		return -EINVAL;
	}

	(void)uhc_xfer_append(dev, xfer);

	key = k_spin_lock(&data->lock);
	switch (xfer->type) {
	case USB_EP_TYPE_CONTROL:
		ret = submit_control(dev, xfer);
		break;
	case USB_EP_TYPE_BULK:
	case USB_EP_TYPE_INTERRUPT:
		ret = submit_data(dev, xfer);
		break;
	case USB_EP_TYPE_ISO:
		qh_schedule_disable(dev);
		ret = submit_isochronous(dev, xfer);
		qh_schedule_enable(dev);
		break;
	default:
		ret = -ENOTSUP;
		break;
	}
	k_spin_unlock(&data->lock, key);

	if (ret < 0) {
		sys_dlist_remove(&xfer->node);
	}

	return ret;
}

static int uhc_hpmicro_dequeue(const struct device *dev, struct uhc_transfer *xfer)
{
	struct uhc_hpmicro_data *data = uhc_get_private(dev);
	struct uhc_hpmicro_qh *head;
	k_spinlock_key_t key;
	bool found = false;

	qh_schedule_disable(dev);
	key = k_spin_lock(&data->lock);

	head = data->async_head;
	for (struct uhc_hpmicro_qh *qh = qh_next(head); (qh != NULL) && (qh != head);
	     qh = qh_next(qh)) {
		if (qh->xfer == xfer) {
			qh_link_remove(head, qh);
			qh_free(data, qh);
			found = true;
			break;
		}
	}

	if (!found) {
		head = data->periodic_head;
		for (struct uhc_hpmicro_qh *qh = qh_next(head); (qh != NULL) && (qh != head);
		     qh = qh_next(qh)) {
			if (qh->xfer == xfer) {
				qh_link_remove(head, qh);
				qh_free(data, qh);
				found = true;
				break;
			}
		}
	}

	if (!found) {
		for (uint32_t i = 0U; i < CONFIG_UHC_HPMICRO_ITD_COUNT; i++) {
			struct uhc_hpmicro_itd *itd = &data->itd_pool[i];

			if (itd->in_use && (itd->xfer == xfer)) {
				itd->hw.control[itd->microframe] &= ~HPM_USB_ITD_STATUS_ACTIVE;
				itd_link_remove(data, itd);
				itd_free(itd);
				found = true;
				break;
			}
		}
	}

	k_spin_unlock(&data->lock, key);
	qh_schedule_enable(dev);

	if (found) {
		uhc_xfer_return(dev, xfer, -ECONNRESET);
	}

	return 0;
}

static size_t qh_transferred_bytes(const struct uhc_hpmicro_qh *qh)
{
	size_t transferred = 0U;

	for (struct uhc_hpmicro_qtd *qtd = qh->first_qtd; qtd != NULL; qtd = qtd_next(qtd)) {
		if (qtd->count_bytes) {
			transferred += qtd->length - qtd_remaining(&qtd->hw);
		}
	}

	return transferred;
}

static int qh_completion_status(const struct uhc_hpmicro_qh *qh)
{
	for (struct uhc_hpmicro_qtd *qtd = qh->first_qtd; qtd != NULL; qtd = qtd_next(qtd)) {
		if (qtd_is_active(&qtd->hw)) {
			return -EINPROGRESS;
		}
		if (qtd_has_error(&qtd->hw)) {
			if ((qtd->hw.token & HPM_USB_QTD_STATUS_HALTED) != 0U) {
				return -EPIPE;
			}
			return -EIO;
		}
	}

	return 0;
}

static struct uhc_transfer *qh_complete(const struct device *dev, struct uhc_hpmicro_qh *qh,
					int *err_out)
{
	struct uhc_hpmicro_data *data = uhc_get_private(dev);
	struct uhc_transfer *xfer = qh->xfer;
	struct uhc_hpmicro_qh *head = qh->periodic ? data->periodic_head : data->async_head;
	int err = qh_completion_status(qh);
	size_t transferred = qh_transferred_bytes(qh);
	bool data1 = qh->data_toggle;

	*err_out = err;
	if (err == -EINPROGRESS) {
		return NULL;
	}

	if ((xfer->type != USB_EP_TYPE_CONTROL) && (err == 0) && (transferred != 0U)) {
		if ((DIV_ROUND_UP(transferred, xfer->mps) & 1U) != 0U) {
			data1 = !data1;
		}
	}

	if ((xfer->type == USB_EP_TYPE_CONTROL) &&
	    (xfer->no_status || (xfer->stage != UHC_CONTROL_STAGE_STATUS))) {
		xfer->stage = UHC_CONTROL_STAGE_STATUS;
	}

	qh_link_remove(head, qh);

	if ((err == 0) && USB_EP_DIR_IS_IN(xfer->ep) && (transferred != 0U) &&
	    (xfer->buf != NULL)) {
		net_buf_add(xfer->buf, MIN(transferred, net_buf_tailroom(xfer->buf)));
	}

	if ((xfer->type != USB_EP_TYPE_CONTROL) && (err == 0)) {
		endpoint_toggle_set(data, xfer->udev, xfer->ep, data1);
	}

	qh_free(data, qh);

	return xfer;
}

static struct uhc_transfer *itd_complete(const struct device *dev, struct uhc_hpmicro_itd *itd,
					 int *err_out)
{
	struct uhc_hpmicro_data *data = uhc_get_private(dev);
	struct uhc_transfer *xfer = itd->xfer;
	uint32_t control = itd->hw.control[itd->microframe];
	size_t transferred;

	*err_out = itd_completion_status(itd);
	if (*err_out == -EINPROGRESS) {
		return NULL;
	}

	if ((*err_out == 0) && USB_EP_DIR_IS_IN(xfer->ep)) {
		transferred = MIN(
			(size_t)((control & HPM_USB_ITD_LENGTH_MASK) >> HPM_USB_ITD_LENGTH_SHIFT),
			(size_t)itd->length);
		(void)sys_cache_data_invd_range(net_buf_tail(xfer->buf), transferred);
		net_buf_add(xfer->buf, MIN(transferred, net_buf_tailroom(xfer->buf)));
	}

	itd_link_remove(data, itd);
	itd_free(itd);

	return xfer;
}

static void uhc_hpmicro_scan_itd(const struct device *dev)
{
	struct uhc_hpmicro_data *data = uhc_get_private(dev);

	while (true) {
		struct uhc_transfer *completed = NULL;
		k_spinlock_key_t key;
		int err = 0;

		key = k_spin_lock(&data->lock);
		for (uint32_t i = 0U; i < CONFIG_UHC_HPMICRO_ITD_COUNT; i++) {
			struct uhc_hpmicro_itd *itd = &data->itd_pool[i];

			if (itd->in_use && !itd_is_active(itd)) {
				completed = itd_complete(dev, itd, &err);
				break;
			}
		}
		k_spin_unlock(&data->lock, key);

		if (completed == NULL) {
			break;
		}

		uhc_xfer_return(dev, completed, err);
	}
}

static bool uhc_hpmicro_has_completed_qh(const struct device *dev)
{
	struct uhc_hpmicro_data *data = uhc_get_private(dev);
	k_spinlock_key_t key;
	bool completed = false;

	key = k_spin_lock(&data->lock);
	for (struct uhc_hpmicro_qh *qh = qh_next(data->async_head);
	     (qh != NULL) && (qh != data->async_head); qh = qh_next(qh)) {
		if (qh_completion_status(qh) != -EINPROGRESS) {
			completed = true;
			break;
		}
	}

	if (!completed) {
		for (struct uhc_hpmicro_qh *qh = qh_next(data->periodic_head);
		     (qh != NULL) && (qh != data->periodic_head); qh = qh_next(qh)) {
			if (qh_completion_status(qh) != -EINPROGRESS) {
				completed = true;
				break;
			}
		}
	}
	k_spin_unlock(&data->lock, key);

	return completed;
}

static void uhc_hpmicro_scan_qh(const struct device *dev)
{
	struct uhc_hpmicro_data *data = uhc_get_private(dev);

	if (!uhc_hpmicro_has_completed_qh(dev)) {
		return;
	}

	qh_schedule_disable(dev);

	while (true) {
		struct uhc_transfer *completed = NULL;
		k_spinlock_key_t key;
		int err = 0;

		key = k_spin_lock(&data->lock);

		for (struct uhc_hpmicro_qh *qh = qh_next(data->async_head);
		     (qh != NULL) && (qh != data->async_head); qh = qh_next(qh)) {
			if (qh_completion_status(qh) != -EINPROGRESS) {
				completed = qh_complete(dev, qh, &err);
				break;
			}
		}

		if (completed == NULL) {
			for (struct uhc_hpmicro_qh *qh = qh_next(data->periodic_head);
			     (qh != NULL) && (qh != data->periodic_head); qh = qh_next(qh)) {
				if (qh_completion_status(qh) != -EINPROGRESS) {
					completed = qh_complete(dev, qh, &err);
					break;
				}
			}
		}

		k_spin_unlock(&data->lock, key);

		if (completed == NULL) {
			break;
		}

		uhc_xfer_return(dev, completed, err);
	}

	qh_schedule_enable(dev);
}

static void uhc_hpmicro_scan(const struct device *dev)
{
	uhc_hpmicro_scan_itd(dev);
	uhc_hpmicro_scan_qh(dev);
}

static void uhc_hpmicro_flush_xfers(const struct device *dev, int err)
{
	struct uhc_hpmicro_data *data = uhc_get_private(dev);

	qh_schedule_disable(dev);

	while (true) {
		struct uhc_transfer *xfer = NULL;
		k_spinlock_key_t key;

		key = k_spin_lock(&data->lock);
		for (struct uhc_hpmicro_qh *qh = qh_next(data->async_head);
		     (qh != NULL) && (qh != data->async_head); qh = qh_next(qh)) {
			xfer = qh->xfer;
			qh_link_remove(data->async_head, qh);
			qh_free(data, qh);
			break;
		}

		if (xfer == NULL) {
			for (struct uhc_hpmicro_qh *qh = qh_next(data->periodic_head);
			     (qh != NULL) && (qh != data->periodic_head); qh = qh_next(qh)) {
				xfer = qh->xfer;
				qh_link_remove(data->periodic_head, qh);
				qh_free(data, qh);
				break;
			}
		}

		if (xfer == NULL) {
			for (uint32_t i = 0U; i < CONFIG_UHC_HPMICRO_ITD_COUNT; i++) {
				struct uhc_hpmicro_itd *itd = &data->itd_pool[i];

				if (itd->in_use) {
					xfer = itd->xfer;
					itd->hw.control[itd->microframe] &=
						~HPM_USB_ITD_STATUS_ACTIVE;
					itd_link_remove(data, itd);
					itd_free(itd);
					break;
				}
			}
		}
		k_spin_unlock(&data->lock, key);

		if (xfer == NULL) {
			break;
		}
		uhc_xfer_return(dev, xfer, err);
	}

	qh_schedule_enable(dev);
}

static void uhc_hpmicro_port_event(const struct device *dev)
{
	const struct uhc_hpmicro_config *config = dev->config;
	struct uhc_hpmicro_data *data = uhc_get_private(dev);
	uint32_t portsc = config->base->PORTSC1;
	bool connected = (portsc & USB_PORTSC1_CCS_MASK) != 0U;

	if (connected == data->port_connected) {
		return;
	}

	if (!connected) {
		uhc_hpmicro_flush_xfers(dev, -ENODEV);
		memset(data->toggles, 0, sizeof(data->toggles));
	}

	data->port_connected = connected;
	if (connected) {
		uint8_t speed = usb_get_port_speed(config->base);

		if (speed == 0U) {
			(void)uhc_submit_event(dev, UHC_EVT_DEV_CONNECTED_FS, 0);
		} else if (speed == 1U) {
			(void)uhc_submit_event(dev, UHC_EVT_DEV_CONNECTED_LS, 0);
		} else {
			(void)uhc_submit_event(dev, UHC_EVT_DEV_CONNECTED_HS, 0);
		}
	} else {
		(void)uhc_submit_event(dev, UHC_EVT_DEV_REMOVED, 0);
	}
}

static void uhc_hpmicro_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	struct uhc_hpmicro_data *data = uhc_get_private(dev);

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		uint32_t events = k_event_wait(&data->events,
					       BIT(UHC_HPMICRO_EVENT_PORT_CHANGE) |
						       BIT(UHC_HPMICRO_EVENT_IRQ),
					       false, K_FOREVER);

		k_event_clear(&data->events, events);

		if ((events & BIT(UHC_HPMICRO_EVENT_IRQ)) != 0U) {
			uhc_hpmicro_scan(dev);
		}

		if ((events & BIT(UHC_HPMICRO_EVENT_PORT_CHANGE)) != 0U) {
			k_sleep(K_MSEC(100));
			uhc_hpmicro_port_event(dev);
		}
	}
}

static void uhc_hpmicro_isr(const struct device *dev)
{
	const struct uhc_hpmicro_config *config = dev->config;
	struct uhc_hpmicro_data *data = uhc_get_private(dev);
	uint32_t status = config->base->USBSTS & config->base->USBINTR;
	uint32_t portsc;

	config->base->USBSTS = status & HPM_USB_USBSTS_W1C;

	if ((status & USB_USBSTS_PCI_MASK) != 0U) {
		portsc = config->base->PORTSC1;
		config->base->PORTSC1 = (portsc & ~HPM_USB_PORTSC_W1C) | HPM_USB_PORTSC_W1C;
		k_event_post(&data->events, BIT(UHC_HPMICRO_EVENT_PORT_CHANGE));
	}

	if ((status & (USB_USBSTS_UI_MASK | USB_USBSTS_UEI_MASK)) != 0U) {
		k_event_post(&data->events, BIT(UHC_HPMICRO_EVENT_IRQ));
	}
}

static int uhc_hpmicro_lock(const struct device *dev)
{
	struct uhc_data *data = dev->data;

	return k_mutex_lock(&data->mutex, K_FOREVER);
}

static int uhc_hpmicro_unlock(const struct device *dev)
{
	struct uhc_data *data = dev->data;

	return k_mutex_unlock(&data->mutex);
}

static int uhc_hpmicro_init(const struct device *dev)
{
	const struct uhc_hpmicro_config *config = dev->config;
	struct uhc_hpmicro_data *data = uhc_get_private(dev);
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	if (config->pincfg != NULL) {
		ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
		if (ret < 0) {
			return ret;
		}
	}

	if (clock_set_source_divider(config->clock_name, config->clock_src, config->clock_div) !=
	    status_success) {
		return -EINVAL;
	}

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		return ret;
	}

	memset(data->qh_pool, 0, sizeof(*data->qh_pool) * CONFIG_UHC_HPMICRO_QH_COUNT);
	memset(data->qtd_pool, 0, sizeof(*data->qtd_pool) * CONFIG_UHC_HPMICRO_QTD_COUNT);
	memset(data->itd_pool, 0, sizeof(*data->itd_pool) * CONFIG_UHC_HPMICRO_ITD_COUNT);
	memset(data->toggles, 0, sizeof(data->toggles));
	data->port_connected = false;

	data->async_head->hw.hlp =
		((uint32_t)data->async_head & HPM_USB_QH_LINK_MASK) | HPM_USB_QH_LINK_QH;
	data->async_head->hw.epchar = HPM_USB_QH_EPCHAR_H;
	data->async_head->hw.curr_qtd = 0U;
	data->async_head->hw.overlay.next_qtd = HPM_USB_QTD_END;
	data->async_head->hw.overlay.alt_next_qtd = HPM_USB_QTD_END;
	data->async_head->hw.overlay.token = HPM_USB_QTD_STATUS_HALTED;

	data->periodic_head->hw.hlp = HPM_USB_QH_END;
	data->periodic_head->hw.epchar = 0U;
	data->periodic_head->hw.epcap = 1U;
	data->periodic_head->hw.curr_qtd = 0U;
	data->periodic_head->hw.overlay.next_qtd = HPM_USB_QTD_END;
	data->periodic_head->hw.overlay.alt_next_qtd = HPM_USB_QTD_END;
	data->periodic_head->hw.overlay.token = HPM_USB_QTD_STATUS_HALTED;

	for (uint32_t i = 0U; i < HPM_USB_FRAME_LIST_SIZE; i++) {
		data->frame_list[i] =
			((uint32_t)data->periodic_head & HPM_USB_QH_LINK_MASK) | HPM_USB_QH_LINK_QH;
	}

	if (!usb_hcd_init(config->base, HPM_USB_HOST_INTRS, HPM_USB_FRAME_LIST_SIZE)) {
		return -EIO;
	}

	usb_hcd_set_async_list_addr(config->base, (uint32_t)data->async_head);
	usb_hcd_set_periodic_list_addr(config->base, (uint32_t)data->frame_list);
	config->base->USBSTS = HPM_USB_USBSTS_W1C;
	config->base->USBINTR = HPM_USB_HOST_INTRS;

	return 0;
}

static int uhc_hpmicro_enable(const struct device *dev)
{
	const struct uhc_hpmicro_config *config = dev->config;
	struct uhc_hpmicro_data *data = uhc_get_private(dev);

	config->irq_enable_func(dev);
	usb_hcd_set_power_ctrl_polarity(config->base, config->power_ctrl_active_high);
	usb_hcd_enable_port_power(config->base);
	k_sleep(K_MSEC(100));
	config->base->USBINTR = HPM_USB_HOST_INTRS;
	usb_hcd_run(config->base);
	k_event_post(&data->events, BIT(UHC_HPMICRO_EVENT_PORT_CHANGE));

	return 0;
}

static int uhc_hpmicro_disable(const struct device *dev)
{
	const struct uhc_hpmicro_config *config = dev->config;
	struct uhc_hpmicro_data *data = uhc_get_private(dev);

	if (data->port_connected) {
		data->port_connected = false;
		(void)uhc_submit_event(dev, UHC_EVT_DEV_REMOVED, 0);
	}

	uhc_hpmicro_flush_xfers(dev, -ENODEV);
	config->base->USBINTR = 0U;
	usb_hcd_disable_ase_pse(config->base);
	config->irq_disable_func(dev);
	usb_hcd_stop(config->base);
	config->base->PORTSC1 &= ~USB_PORTSC1_PP_MASK;

	return 0;
}

static int uhc_hpmicro_shutdown(const struct device *dev)
{
	const struct uhc_hpmicro_config *config = dev->config;

	config->base->USBINTR = 0U;
	usb_hcd_stop(config->base);
	usb_phy_deinit(config->base);

	return 0;
}

static int uhc_hpmicro_sof_enable(const struct device *dev)
{
	const struct uhc_hpmicro_config *config = dev->config;

	config->base->USBCMD |= USB_USBCMD_PSE_MASK;
	return 0;
}

static int uhc_hpmicro_bus_suspend(const struct device *dev)
{
	const struct uhc_hpmicro_config *config = dev->config;

	if ((config->base->PORTSC1 & USB_PORTSC1_SUSP_MASK) != 0U) {
		return -EALREADY;
	}

	config->base->PORTSC1 |= USB_PORTSC1_SUSP_MASK;
	(void)uhc_submit_event(dev, UHC_EVT_SUSPENDED, 0);
	return 0;
}

static int uhc_hpmicro_bus_resume(const struct device *dev)
{
	const struct uhc_hpmicro_config *config = dev->config;

	config->base->PORTSC1 |= USB_PORTSC1_FPR_MASK;
	k_sleep(K_MSEC(20));
	config->base->PORTSC1 &= ~USB_PORTSC1_FPR_MASK;
	(void)uhc_submit_event(dev, UHC_EVT_RESUMED, 0);
	return 0;
}

static int uhc_hpmicro_bus_reset(const struct device *dev)
{
	const struct uhc_hpmicro_config *config = dev->config;
	struct uhc_hpmicro_data *data = uhc_get_private(dev);

	memset(data->toggles, 0, sizeof(data->toggles));
	usb_hcd_port_reset(config->base);
	(void)uhc_submit_event(dev, UHC_EVT_RESETED, 0);
	return 0;
}

static DEVICE_API(uhc, uhc_hpmicro_api) = {
	.lock = uhc_hpmicro_lock,
	.unlock = uhc_hpmicro_unlock,
	.init = uhc_hpmicro_init,
	.enable = uhc_hpmicro_enable,
	.disable = uhc_hpmicro_disable,
	.shutdown = uhc_hpmicro_shutdown,
	.bus_reset = uhc_hpmicro_bus_reset,
	.sof_enable = uhc_hpmicro_sof_enable,
	.bus_suspend = uhc_hpmicro_bus_suspend,
	.bus_resume = uhc_hpmicro_bus_resume,
	.ep_enqueue = uhc_hpmicro_enqueue,
	.ep_dequeue = uhc_hpmicro_dequeue,
};

#define UHC_HPMICRO_IRQ_CONFIG(n)                                                                  \
	static void uhc_hpmicro_irq_enable_##n(const struct device *dev)                           \
	{                                                                                          \
		ARG_UNUSED(dev);                                                                   \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}                                                                                          \
	static void uhc_hpmicro_irq_disable_##n(const struct device *dev)                          \
	{                                                                                          \
		ARG_UNUSED(dev);                                                                   \
		irq_disable(DT_INST_IRQN(n));                                                      \
	}                                                                                          \
	static struct uhc_hpmicro_qh uhc_hpmicro_async_head_##n __nocache __aligned(32);           \
	static struct uhc_hpmicro_qh uhc_hpmicro_periodic_head_##n __nocache __aligned(32);        \
	static struct uhc_hpmicro_qh                                                               \
		uhc_hpmicro_qh_pool_##n[CONFIG_UHC_HPMICRO_QH_COUNT] __nocache __aligned(32);      \
	static struct uhc_hpmicro_qtd                                                              \
		uhc_hpmicro_qtd_pool_##n[CONFIG_UHC_HPMICRO_QTD_COUNT] __nocache __aligned(32);    \
	static struct uhc_hpmicro_itd                                                              \
		uhc_hpmicro_itd_pool_##n[CONFIG_UHC_HPMICRO_ITD_COUNT] __nocache __aligned(64);    \
	static uint32_t uhc_hpmicro_frame_list_##n[HPM_USB_FRAME_LIST_SIZE] __nocache              \
		__aligned(4096);

#define UHC_HPMICRO_PINCTRL_DT_INST_DEFINE(n)                                                      \
	COND_CODE_1(DT_INST_PINCTRL_HAS_NAME(n, default), (PINCTRL_DT_INST_DEFINE(n)), ())

#define UHC_HPMICRO_PINCTRL_DT_INST_DEV_CONFIG_GET(n)                                              \
	COND_CODE_1(DT_INST_PINCTRL_HAS_NAME(n, default),                                          \
		    ((void *)PINCTRL_DT_INST_DEV_CONFIG_GET(n)), (NULL))

#define UHC_HPMICRO_INIT(n)                                                                        \
	UHC_HPMICRO_PINCTRL_DT_INST_DEFINE(n);                                                     \
	UHC_HPMICRO_IRQ_CONFIG(n)                                                                  \
                                                                                                   \
	K_THREAD_STACK_DEFINE(uhc_hpmicro_stack_##n, CONFIG_UHC_HPMICRO_THREAD_STACK_SIZE);        \
                                                                                                   \
	static struct uhc_hpmicro_data uhc_hpmicro_priv_##n = {                                    \
		.events = Z_EVENT_INITIALIZER(uhc_hpmicro_priv_##n.events),                        \
		.async_head = &uhc_hpmicro_async_head_##n,                                         \
		.periodic_head = &uhc_hpmicro_periodic_head_##n,                                   \
		.qh_pool = uhc_hpmicro_qh_pool_##n,                                                \
		.qtd_pool = uhc_hpmicro_qtd_pool_##n,                                              \
		.itd_pool = uhc_hpmicro_itd_pool_##n,                                              \
		.frame_list = uhc_hpmicro_frame_list_##n,                                          \
	};                                                                                         \
                                                                                                   \
	static struct uhc_data uhc_hpmicro_data_##n = {                                            \
		.mutex = Z_MUTEX_INITIALIZER(uhc_hpmicro_data_##n.mutex),                          \
		.priv = &uhc_hpmicro_priv_##n,                                                     \
		.caps = {.hs = 1},                                                                 \
	};                                                                                         \
                                                                                                   \
	static const struct uhc_hpmicro_config uhc_hpmicro_config_##n = {                          \
		.base = (USB_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.clock_name = DT_INST_PROP(n, clock_name),                                         \
		.clock_src = DT_INST_CLOCKS_CELL_BY_IDX(n, 0, source),                             \
		.clock_div = DT_INST_CLOCKS_CELL_BY_IDX(n, 0, divider),                            \
		.power_ctrl_active_high = DT_INST_PROP_OR(n, power_control_active_high, false),    \
		.pincfg = UHC_HPMICRO_PINCTRL_DT_INST_DEV_CONFIG_GET(n),                           \
		.irq_enable_func = uhc_hpmicro_irq_enable_##n,                                     \
		.irq_disable_func = uhc_hpmicro_irq_disable_##n,                                   \
	};                                                                                         \
                                                                                                   \
	static int uhc_hpmicro_preinit_##n(const struct device *dev)                               \
	{                                                                                          \
		struct uhc_hpmicro_data *data = uhc_get_private(dev);                              \
                                                                                                   \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), uhc_hpmicro_isr,            \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		k_thread_create(&data->thread, uhc_hpmicro_stack_##n,                              \
				K_THREAD_STACK_SIZEOF(uhc_hpmicro_stack_##n), uhc_hpmicro_thread,  \
				(void *)dev, NULL, NULL,                                           \
				K_PRIO_COOP(CONFIG_UHC_HPMICRO_THREAD_PRIORITY), K_ESSENTIAL,      \
				K_NO_WAIT);                                                        \
		k_thread_name_set(&data->thread, dev->name);                                       \
		return 0;                                                                          \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, uhc_hpmicro_preinit_##n, NULL, &uhc_hpmicro_data_##n,             \
			      &uhc_hpmicro_config_##n, POST_KERNEL,                                \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &uhc_hpmicro_api);

DT_INST_FOREACH_STATUS_OKAY(UHC_HPMICRO_INIT)
