/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_usb

#include <errno.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usb_ch9.h>

#include <hpm_clock_drv.h>
#include <hpm_usb_device.h>
#include <hpm_usb_drv.h>

#include "udc_common.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(udc_hpmicro, CONFIG_UDC_DRIVER_LOG_LEVEL);

#define HPM_USB_EP0_MPS       64U
#define HPM_USB_EP_MAX_MPS    1024U
#define HPM_USB_EP_COUNT      USB_SOC_DCD_MAX_ENDPOINT_COUNT
#define HPM_USB_DCD_ALIGNMENT USB_SOC_DCD_DATA_RAM_ADDRESS_ALIGNMENT
#define HPM_USB_SETUP_SIZE    sizeof(struct usb_setup_packet)

struct udc_hpmicro_config {
	USB_Type *base;
	const struct device *clock;
	clock_name_t clock_name;
	const struct pinctrl_dev_config *pcfg;
	void (*irq_enable_func)(const struct device *dev);
	void (*irq_disable_func)(const struct device *dev);
	struct udc_ep_config *ep_cfg_in;
	struct udc_ep_config *ep_cfg_out;
};

struct udc_hpmicro_data {
	usb_device_handle_t handle;
	struct k_thread thread;
	struct k_event events;
	struct k_spinlock lock;
	atomic_t xfer_finished;
	uint8_t setup[HPM_USB_SETUP_SIZE];
	bool initialized;
};

enum udc_hpmicro_event {
	UDC_HPMICRO_EVT_SETUP = 0,
	UDC_HPMICRO_EVT_XFER,
	UDC_HPMICRO_EVT_RESET,
};

static inline uint8_t udc_hpmicro_ep_bit(uint8_t ep_idx)
{
	return (ep_idx / 2U) + ((ep_idx % 2U) != 0U ? 16U : 0U);
}

static int udc_hpmicro_start_transfer(const struct device *dev, struct udc_ep_config *cfg,
				      struct net_buf *buf)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);
	struct udc_buf_info *bi = udc_get_buf_info(buf);
	uint8_t *buffer;
	uint32_t length;

	if ((cfg->addr == USB_CONTROL_EP_OUT) && bi->setup) {
		return 0;
	}

	if (USB_EP_DIR_IS_OUT(cfg->addr)) {
		length = bi->status ? 0U : net_buf_tailroom(buf);
		buffer = length == 0U ? NULL : net_buf_tail(buf);
		if (length != 0U) {
			(void)sys_cache_data_invd_range(buffer, length);
		}
	} else {
		length = buf->len;
		buffer = length == 0U ? NULL : buf->data;
		if (length != 0U) {
			(void)sys_cache_data_flush_range(buffer, length);
		}
	}

	if (!usb_device_edpt_xfer(&data->handle, cfg->addr, buffer, length)) {
		return -EIO;
	}

	return 0;
}

static void udc_hpmicro_start_pending(const struct device *dev)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);

	for (uint8_t ep_idx = 0U; ep_idx < HPM_USB_EP_COUNT; ep_idx++) {
		struct udc_ep_config *cfg;
		struct net_buf *buf;
		uint8_t ep = (ep_idx / 2U) | ((ep_idx & 1U) != 0U ? USB_EP_DIR_IN : 0U);

		cfg = udc_get_ep_cfg(dev, ep);
		if ((cfg == NULL) || !cfg->stat.enabled || cfg->stat.halted ||
		    udc_ep_is_busy(cfg)) {
			continue;
		}

		buf = udc_buf_peek(cfg);
		if ((buf == NULL) || ((ep == USB_CONTROL_EP_OUT) && udc_get_buf_info(buf)->setup)) {
			continue;
		}

		udc_ep_set_busy(cfg, true);
		if (udc_hpmicro_start_transfer(dev, cfg, buf) < 0) {
			buf = udc_buf_get(cfg);
			udc_ep_set_busy(cfg, false);
			(void)udc_submit_ep_event(dev, buf, -EIO);
			k_event_post(&data->events, BIT(UDC_HPMICRO_EVT_XFER));
			LOG_ERR("Failed to start ep 0x%02x transfer", cfg->addr);
		}
	}
}

static void udc_hpmicro_complete_transfer(const struct device *dev, struct udc_ep_config *cfg,
					  uint32_t transferred)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);
	struct net_buf *buf = udc_buf_peek(cfg);

	if (buf == NULL) {
		LOG_ERR("No buffer for ep 0x%02x", cfg->addr);
		udc_ep_set_busy(cfg, false);
		k_event_post(&data->events, BIT(UDC_HPMICRO_EVT_XFER));
		return;
	}

	if (USB_EP_DIR_IS_OUT(cfg->addr)) {
		uint32_t length = MIN(transferred, net_buf_tailroom(buf));
		size_t cache_length = net_buf_tailroom(buf);

		if (cache_length != 0U) {
			(void)sys_cache_data_invd_range(net_buf_tail(buf), cache_length);
		}
		net_buf_add(buf, length);
	}

	if (USB_EP_DIR_IS_IN(cfg->addr) && udc_ep_buf_has_zlp(buf)) {
		udc_ep_buf_clear_zlp(buf);
		if (!usb_device_edpt_xfer(&data->handle, cfg->addr, NULL, 0U)) {
			buf = udc_buf_get(cfg);
			udc_ep_set_busy(cfg, false);
			(void)udc_submit_ep_event(dev, buf, -EIO);
			k_event_post(&data->events, BIT(UDC_HPMICRO_EVT_XFER));
		}
		return;
	}

	buf = udc_buf_get(cfg);
	udc_ep_set_busy(cfg, false);
	(void)udc_submit_ep_event(dev, buf, 0);
	k_event_post(&data->events, BIT(UDC_HPMICRO_EVT_XFER));
}

static void udc_hpmicro_fail_transfer(const struct device *dev, struct udc_ep_config *cfg, int err)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);
	struct net_buf *buf = udc_buf_get(cfg);

	udc_ep_set_busy(cfg, false);
	if (buf != NULL) {
		(void)udc_submit_ep_event(dev, buf, err);
	}
	k_event_post(&data->events, BIT(UDC_HPMICRO_EVT_XFER));
}

static void udc_hpmicro_handle_endpoint_complete(const struct device *dev, uint8_t ep_idx)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);
	usb_device_handle_t *handle = &data->handle;
	uint8_t ep = (ep_idx / 2U) | ((ep_idx & 1U) != 0U ? USB_EP_DIR_IN : 0U);
	struct udc_ep_config *cfg = udc_get_ep_cfg(dev, ep);
	dcd_qhd_t *qhd = usb_device_qhd_get(handle, ep_idx);
	dcd_qtd_t *qtd = qhd->attached_qtd;
	uint32_t transferred = 0U;
	bool finished = false;

	if (cfg == NULL) {
		return;
	}

	while (qtd != NULL) {
		if (qtd->halted || qtd->xact_err || qtd->buffer_err) {
			udc_hpmicro_fail_transfer(dev, cfg, -EIO);
			return;
		}
		if (qtd->active) {
			return;
		}

		transferred += qtd->expected_bytes - qtd->total_bytes;
		if (qtd->next == USB_SOC_DCD_QTD_NEXT_INVALID) {
			finished = true;
			break;
		}
		qtd = (dcd_qtd_t *)qtd->next;
	}

	if (finished) {
		udc_hpmicro_complete_transfer(dev, cfg, transferred);
	}
}

static void udc_hpmicro_handle_bus_reset(const struct device *dev)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);
	usb_endpoint_config_t ep0 = {
		.xfer = USB_EP_TYPE_CONTROL,
		.max_packet_size = HPM_USB_EP0_MPS,
	};
	k_spinlock_key_t key;

	if (udc_lock_internal(dev, K_FOREVER) != 0) {
		return;
	}

	key = k_spin_lock(&data->lock);
	usb_device_bus_reset(&data->handle, HPM_USB_EP0_MPS);
	ep0.ep_addr = USB_CONTROL_EP_OUT;
	(void)usb_device_edpt_open(&data->handle, &ep0);
	ep0.ep_addr = USB_CONTROL_EP_IN;
	(void)usb_device_edpt_open(&data->handle, &ep0);

	for (uint8_t ep_idx = 0U; ep_idx < HPM_USB_EP_COUNT; ep_idx++) {
		uint8_t ep = (ep_idx / 2U) | ((ep_idx & 1U) != 0U ? USB_EP_DIR_IN : 0U);
		struct udc_ep_config *cfg = udc_get_ep_cfg(dev, ep);

		if (cfg != NULL && cfg->stat.enabled) {
			udc_ep_set_busy(cfg, false);
			cfg->stat.halted = false;
		}
	}

	atomic_clear(&data->xfer_finished);
	k_spin_unlock(&data->lock, key);
	(void)udc_unlock_internal(dev);

	udc_set_suspended(dev, false);
	(void)udc_submit_event(dev, UDC_EVT_RESET, 0);
}

static void udc_hpmicro_thread(void *arg1, void *arg2, void *arg3)
{
	const struct device *dev = arg1;
	struct udc_hpmicro_data *data = udc_get_private(dev);

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		uint32_t events = k_event_wait_safe(&data->events, UINT32_MAX, false, K_FOREVER);

		if ((events & BIT(UDC_HPMICRO_EVT_RESET)) != 0U) {
			udc_hpmicro_handle_bus_reset(dev);
			continue;
		}

		if ((events & BIT(UDC_HPMICRO_EVT_SETUP)) != 0U) {
			uint8_t setup[HPM_USB_SETUP_SIZE];
			k_spinlock_key_t key;

			key = k_spin_lock(&data->lock);
			memcpy(setup, data->setup, sizeof(setup));
			k_spin_unlock(&data->lock, key);
			udc_setup_received(dev, setup);
		}

		if ((events & BIT(UDC_HPMICRO_EVT_XFER)) != 0U) {
			uint32_t complete = atomic_clear(&data->xfer_finished);

			if (udc_lock_internal(dev, K_FOREVER) == 0) {
				for (uint8_t ep_idx = 0U; ep_idx < HPM_USB_EP_COUNT; ep_idx++) {
					if ((complete & BIT(udc_hpmicro_ep_bit(ep_idx))) != 0U) {
						udc_hpmicro_handle_endpoint_complete(dev, ep_idx);
					}
				}

				udc_hpmicro_start_pending(dev);
				(void)udc_unlock_internal(dev);
			}
		}
	}
}

static int udc_hpmicro_ep_enqueue(const struct device *dev, struct udc_ep_config *cfg,
				  struct net_buf *buf)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);
	struct udc_buf_info *bi = udc_get_buf_info(buf);

	udc_buf_put(cfg, buf);

	if (cfg->stat.halted || ((cfg->addr == USB_CONTROL_EP_OUT) && bi->setup)) {
		return 0;
	}

	k_event_post(&data->events, BIT(UDC_HPMICRO_EVT_XFER));
	return 0;
}

static int udc_hpmicro_ep_dequeue(const struct device *dev, struct udc_ep_config *cfg)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);
	uint8_t ep_idx = 2U * USB_EP_GET_IDX(cfg->addr) + (USB_EP_DIR_IS_IN(cfg->addr) ? 1U : 0U);
	bool reenable = cfg->stat.enabled;
	int ret = 0;

	usb_device_edpt_close(&data->handle, cfg->addr);
	if (reenable) {
		usb_endpoint_config_t ep = {
			.xfer = cfg->attributes & USB_EP_TRANSFER_TYPE_MASK,
			.ep_addr = cfg->addr,
			.max_packet_size = cfg->mps,
		};

		if (!usb_device_edpt_open(&data->handle, &ep)) {
			ret = -EIO;
		}
	}

	atomic_and(&data->xfer_finished, ~BIT(udc_hpmicro_ep_bit(ep_idx)));
	udc_ep_cancel_queued(dev, cfg);
	udc_ep_set_busy(cfg, false);

	return ret;
}

static int udc_hpmicro_ep_set_halt(const struct device *dev, struct udc_ep_config *cfg)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);

	usb_device_edpt_stall(&data->handle, cfg->addr);
	if (USB_EP_GET_IDX(cfg->addr) != 0U) {
		cfg->stat.halted = true;
	}

	return 0;
}

static int udc_hpmicro_ep_clear_halt(const struct device *dev, struct udc_ep_config *cfg)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);

	usb_device_edpt_clear_stall(&data->handle, cfg->addr);
	cfg->stat.halted = false;
	k_event_post(&data->events, BIT(UDC_HPMICRO_EVT_XFER));

	return 0;
}

static int udc_hpmicro_ep_enable(const struct device *dev, struct udc_ep_config *cfg)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);
	usb_endpoint_config_t ep = {
		.xfer = cfg->attributes & USB_EP_TRANSFER_TYPE_MASK,
		.ep_addr = cfg->addr,
		.max_packet_size = cfg->mps,
	};

	if (!usb_device_edpt_open(&data->handle, &ep)) {
		return -EIO;
	}

	return 0;
}

static int udc_hpmicro_ep_disable(const struct device *dev, struct udc_ep_config *cfg)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);
	uint8_t ep_idx = 2U * USB_EP_GET_IDX(cfg->addr) + (USB_EP_DIR_IS_IN(cfg->addr) ? 1U : 0U);

	usb_device_edpt_close(&data->handle, cfg->addr);
	atomic_and(&data->xfer_finished, ~BIT(udc_hpmicro_ep_bit(ep_idx)));
	udc_ep_set_busy(cfg, false);

	return 0;
}

static int udc_hpmicro_host_wakeup(const struct device *dev)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);

	if (!udc_is_suspended(dev)) {
		return -EACCES;
	}

	usb_device_remote_wakeup(&data->handle);
	udc_set_suspended(dev, false);
	(void)udc_submit_event(dev, UDC_EVT_RESUME, 0);

	return 0;
}

static int udc_hpmicro_set_address(const struct device *dev, uint8_t addr)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);

	usb_dcd_set_address(data->handle.regs, addr);

	return 0;
}

static int udc_hpmicro_test_mode(const struct device *dev, uint8_t mode, bool dryrun)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);
	usb_test_mode_t hpm_mode;

	switch (mode) {
	case USB_SFS_TEST_MODE_J:
		hpm_mode = usb_test_j_state;
		break;
	case USB_SFS_TEST_MODE_K:
		hpm_mode = usb_test_k_state;
		break;
	case USB_SFS_TEST_MODE_SE0_NAK:
		hpm_mode = usb_test_se0_nak;
		break;
	case USB_SFS_TEST_MODE_PACKET:
		hpm_mode = usb_test_packet;
		break;
	default:
		return -ENOTSUP;
	}

	if (!dryrun) {
		usb_set_port_test_mode(data->handle.regs, hpm_mode);
	}

	return 0;
}

static enum udc_bus_speed udc_hpmicro_device_speed(const struct device *dev)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);

	switch (usb_device_get_port_speed(&data->handle)) {
	case 2U:
		return UDC_BUS_SPEED_HS;
	case 1U:
	case 0U:
	default:
		return UDC_BUS_SPEED_FS;
	}
}

static void udc_hpmicro_isr(const struct device *dev)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);
	usb_device_handle_t *handle = &data->handle;
	uint32_t status = usb_device_status_flags(handle) & usb_device_interrupts(handle);

	if (status == 0U) {
		return;
	}

	usb_device_clear_status_flags(handle, status);

	if ((status & USB_USBINTR_UEE_MASK) != 0U) {
		(void)udc_submit_event(dev, UDC_EVT_ERROR, -EIO);
	}

	if ((status & USB_USBINTR_URE_MASK) != 0U) {
		k_event_post(&data->events, BIT(UDC_HPMICRO_EVT_RESET));
		return;
	}

	if ((status & USB_USBINTR_SLE_MASK) != 0U) {
		if (usb_device_get_suspend_status(handle) != 0U &&
		    usb_device_get_address(handle) != 0U && !udc_is_suspended(dev)) {
			udc_set_suspended(dev, true);
			(void)udc_submit_event(dev, UDC_EVT_SUSPEND, 0);
		}
	}

	if ((status & USB_USBINTR_PCE_MASK) != 0U) {
		if (!usb_device_get_port_ccs(handle)) {
			(void)udc_submit_event(dev, UDC_EVT_VBUS_REMOVED, 0);
		} else {
			bool resumed = udc_is_suspended(dev);

			udc_set_suspended(dev, false);
			if (resumed) {
				(void)udc_submit_event(dev, UDC_EVT_RESUME, 0);
			}
			(void)udc_submit_event(dev, UDC_EVT_VBUS_READY, 0);
		}
	}

	if ((status & USB_USBINTR_UE_MASK) != 0U) {
		uint32_t complete = usb_device_get_edpt_complete_status(handle);
		uint32_t setup = usb_device_get_setup_status(handle);

		if (complete != 0U) {
			usb_device_clear_edpt_complete_status(handle, complete);
			atomic_or(&data->xfer_finished, complete);
			k_event_post(&data->events, BIT(UDC_HPMICRO_EVT_XFER));
		}

		if (setup != 0U) {
			dcd_qhd_t *qhd = usb_device_qhd_get(handle, 0U);
			k_spinlock_key_t key;

			usb_device_clear_setup_status(handle, setup);
			atomic_and(&data->xfer_finished,
				   ~(BIT(udc_hpmicro_ep_bit(0U)) | BIT(udc_hpmicro_ep_bit(1U))));
			key = k_spin_lock(&data->lock);
			do {
				usb_dcd_set_sutw(handle->regs, true);
				memcpy(data->setup, (const void *)&qhd->setup_request,
				       HPM_USB_SETUP_SIZE);
			} while (!usb_dcd_get_sutw(handle->regs));
			usb_dcd_set_sutw(handle->regs, false);
			k_spin_unlock(&data->lock, key);
			k_event_post(&data->events, BIT(UDC_HPMICRO_EVT_SETUP));
		}
	}
}

static void udc_hpmicro_release_endpoints(const struct device *dev)
{
	for (uint8_t i = 0U; i < HPM_USB_EP_COUNT * 2U; i++) {
		uint8_t ep = (i & 1U) != 0U ? (USB_EP_DIR_IN | (i / 2U)) : (i / 2U);
		struct udc_ep_config *cfg = udc_get_ep_cfg(dev, ep);

		if (cfg != NULL) {
			cfg->stat.enabled = false;
			cfg->stat.halted = false;
			cfg->stat.busy = false;
		}
	}
}

static int udc_hpmicro_init(const struct device *dev)
{
	const struct udc_hpmicro_config *config = dev->config;
	struct udc_hpmicro_data *data = udc_get_private(dev);
	uint32_t interrupts = USB_USBINTR_UE_MASK | USB_USBINTR_UEE_MASK | USB_USBINTR_SLE_MASK |
			      USB_USBINTR_PCE_MASK | USB_USBINTR_URE_MASK;
	int ret;

	ret = clock_control_on(config->clock, (clock_control_subsys_t)config->clock_name);
	if (ret < 0) {
		return ret;
	}

	if (!usb_device_init(&data->handle, interrupts)) {
		(void)clock_control_off(config->clock, (clock_control_subsys_t)config->clock_name);
		return -EIO;
	}

	if (IS_ENABLED(CONFIG_UDC_HPMICRO_VBUS_INTERNAL)) {
		usb_phy_using_internal_vbus(config->base);
	} else {
		usb_phy_using_external_vbus(config->base);
	}

	if (!IS_ENABLED(CONFIG_UDC_DRIVER_HIGH_SPEED_SUPPORT_ENABLED)) {
		config->base->PORTSC1 |= USB_PORTSC1_PFSC_MASK;
	}

	/*
	 * usb_device_init() connects the controller. UDC init() only prepares
	 * the controller and must not make it visible to a host, so stop it
	 * without using usb_device_disconnect(), which waits for SOF.
	 */
	config->base->USBCMD &= ~USB_USBCMD_RS_MASK;
	k_event_clear(&data->events, UINT32_MAX);
	atomic_clear(&data->xfer_finished);

	ret = udc_ep_enable_internal(dev, USB_CONTROL_EP_OUT, USB_EP_TYPE_CONTROL, HPM_USB_EP0_MPS,
				     0U);
	if (ret < 0) {
		goto cleanup;
	}
	ret = udc_ep_enable_internal(dev, USB_CONTROL_EP_IN, USB_EP_TYPE_CONTROL, HPM_USB_EP0_MPS,
				     0U);
	if (ret < 0) {
		goto cleanup;
	}

	data->initialized = true;
	config->irq_enable_func(dev);

	return 0;

cleanup:
	usb_device_deinit(&data->handle);
	udc_hpmicro_release_endpoints(dev);
	(void)clock_control_off(config->clock, (clock_control_subsys_t)config->clock_name);

	return ret;
}

static int udc_hpmicro_enable(const struct device *dev)
{
	struct udc_hpmicro_data *data = udc_get_private(dev);

	usb_device_connect(&data->handle);

	return 0;
}

static int udc_hpmicro_disable(const struct device *dev)
{
	const struct udc_hpmicro_config *config = dev->config;

	config->base->USBCMD &= ~USB_USBCMD_RS_MASK;

	return 0;
}

static int udc_hpmicro_shutdown(const struct device *dev)
{
	const struct udc_hpmicro_config *config = dev->config;
	struct udc_hpmicro_data *data = udc_get_private(dev);
	int ret = 0;

	if (!data->initialized) {
		return 0;
	}

	config->irq_disable_func(dev);
	usb_device_deinit(&data->handle);
	udc_hpmicro_release_endpoints(dev);

	if (clock_control_off(config->clock, (clock_control_subsys_t)config->clock_name) < 0) {
		ret = -EIO;
	}

	data->initialized = false;

	return ret;
}

static void udc_hpmicro_lock(const struct device *dev)
{
	(void)udc_lock_internal(dev, K_FOREVER);
}

static void udc_hpmicro_unlock(const struct device *dev)
{
	(void)udc_unlock_internal(dev);
}

static const struct udc_api udc_hpmicro_api = {
	.device_speed = udc_hpmicro_device_speed,
	.ep_enqueue = udc_hpmicro_ep_enqueue,
	.ep_dequeue = udc_hpmicro_ep_dequeue,
	.ep_set_halt = udc_hpmicro_ep_set_halt,
	.ep_clear_halt = udc_hpmicro_ep_clear_halt,
	.ep_try_config = NULL,
	.ep_enable = udc_hpmicro_ep_enable,
	.ep_disable = udc_hpmicro_ep_disable,
	.host_wakeup = udc_hpmicro_host_wakeup,
	.set_address = udc_hpmicro_set_address,
	.test_mode = udc_hpmicro_test_mode,
	.enable = udc_hpmicro_enable,
	.disable = udc_hpmicro_disable,
	.init = udc_hpmicro_init,
	.shutdown = udc_hpmicro_shutdown,
	.lock = udc_hpmicro_lock,
	.unlock = udc_hpmicro_unlock,
};

#define UDC_HPMICRO_PINCTRL_DT_INST_DEFINE(n)                                                      \
	COND_CODE_1(DT_INST_PINCTRL_HAS_NAME(n, default), (PINCTRL_DT_INST_DEFINE(n)), ())

#define UDC_HPMICRO_PINCTRL_DT_INST_DEV_CONFIG_GET(n)                                              \
	COND_CODE_1(DT_INST_PINCTRL_HAS_NAME(n, default),                                          \
		    ((void *)PINCTRL_DT_INST_DEV_CONFIG_GET(n)), (NULL))

#define UDC_HPMICRO_DEFINE(n)                                                                      \
	K_THREAD_STACK_DEFINE(udc_hpmicro_stack_##n, CONFIG_UDC_HPMICRO_STACK_SIZE);               \
	static void udc_hpmicro_irq_enable_##n(const struct device *dev)                           \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), udc_hpmicro_isr,            \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}                                                                                          \
	static void udc_hpmicro_irq_disable_##n(const struct device *dev)                          \
	{                                                                                          \
		irq_disable(DT_INST_IRQN(n));                                                      \
	}                                                                                          \
	UDC_HPMICRO_PINCTRL_DT_INST_DEFINE(n);                                                     \
	static struct udc_ep_config ep_cfg_in_##n[DT_INST_PROP(n, num_bidir_endpoints)];           \
	static struct udc_ep_config ep_cfg_out_##n[DT_INST_PROP(n, num_bidir_endpoints)];          \
	static dcd_data_t dcd_data_##n __nocache __aligned(HPM_USB_DCD_ALIGNMENT);                 \
	static const struct udc_hpmicro_config udc_hpmicro_config_##n = {                          \
		.base = (USB_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                    \
		.clock_name = (clock_name_t)DT_INST_PROP(n, clock_name),                           \
		.pcfg = UDC_HPMICRO_PINCTRL_DT_INST_DEV_CONFIG_GET(n),                             \
		.irq_enable_func = udc_hpmicro_irq_enable_##n,                                     \
		.irq_disable_func = udc_hpmicro_irq_disable_##n,                                   \
		.ep_cfg_in = ep_cfg_in_##n,                                                        \
		.ep_cfg_out = ep_cfg_out_##n,                                                      \
	};                                                                                         \
	static struct udc_hpmicro_data udc_hpmicro_data_##n = {                                    \
		.handle =                                                                          \
			{                                                                          \
				.regs = (USB_Type *)DT_INST_REG_ADDR(n),                           \
				.dcd_data = &dcd_data_##n,                                         \
			},                                                                         \
	};                                                                                         \
	static struct udc_data udc_data_##n = {                                                    \
		.priv = &udc_hpmicro_data_##n,                                                     \
	};                                                                                         \
	static int udc_hpmicro_preinit_##n(const struct device *dev)                               \
	{                                                                                          \
		const struct udc_hpmicro_config *config = dev->config;                             \
		struct udc_data *data = dev->data;                                                 \
		struct udc_hpmicro_data *priv = data->priv;                                        \
		int ret;                                                                           \
		k_mutex_init(&data->mutex);                                                        \
		k_event_init(&priv->events);                                                       \
		data->caps.hs = true;                                                              \
		data->caps.rwup = true;                                                            \
		data->caps.can_detect_vbus = true;                                                 \
		data->caps.mps0 = UDC_MPS0_64;                                                     \
		for (uint8_t i = 0U; i < DT_INST_PROP(n, num_bidir_endpoints); i++) {              \
			config->ep_cfg_out[i].addr = i;                                            \
			config->ep_cfg_out[i].caps.out = 1;                                        \
			config->ep_cfg_out[i].caps.control = (i == 0U);                            \
			config->ep_cfg_out[i].caps.bulk = (i != 0U);                               \
			config->ep_cfg_out[i].caps.interrupt = (i != 0U);                          \
			config->ep_cfg_out[i].caps.iso = (i != 0U);                                \
			config->ep_cfg_out[i].caps.mps =                                           \
				(i == 0U) ? HPM_USB_EP0_MPS : HPM_USB_EP_MAX_MPS;                  \
			ret = udc_register_ep(dev, &config->ep_cfg_out[i]);                        \
			if (ret < 0) {                                                             \
				return ret;                                                        \
			}                                                                          \
			config->ep_cfg_in[i].addr = USB_EP_DIR_IN | i;                             \
			config->ep_cfg_in[i].caps.in = 1;                                          \
			config->ep_cfg_in[i].caps.control = (i == 0U);                             \
			config->ep_cfg_in[i].caps.bulk = (i != 0U);                                \
			config->ep_cfg_in[i].caps.interrupt = (i != 0U);                           \
			config->ep_cfg_in[i].caps.iso = (i != 0U);                                 \
			config->ep_cfg_in[i].caps.mps =                                            \
				(i == 0U) ? HPM_USB_EP0_MPS : HPM_USB_EP_MAX_MPS;                  \
			ret = udc_register_ep(dev, &config->ep_cfg_in[i]);                         \
			if (ret < 0) {                                                             \
				return ret;                                                        \
			}                                                                          \
		}                                                                                  \
		if (config->pcfg != NULL &&                                                        \
		    pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT) < 0) {                \
			return -EIO;                                                               \
		}                                                                                  \
		k_thread_create(&priv->thread, udc_hpmicro_stack_##n,                              \
				K_THREAD_STACK_SIZEOF(udc_hpmicro_stack_##n), udc_hpmicro_thread,  \
				(void *)DEVICE_DT_INST_GET(n), NULL, NULL,                         \
				K_PRIO_COOP(CONFIG_UDC_HPMICRO_THREAD_PRIORITY), K_ESSENTIAL,      \
				K_NO_WAIT);                                                        \
		k_thread_name_set(&priv->thread, dev->name);                                       \
		return 0;                                                                          \
	}                                                                                          \
	DEVICE_DT_INST_DEFINE(n, udc_hpmicro_preinit_##n, NULL, &udc_data_##n,                     \
			      &udc_hpmicro_config_##n, POST_KERNEL,                                \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &udc_hpmicro_api)

DT_INST_FOREACH_STATUS_OKAY(UDC_HPMICRO_DEFINE)
