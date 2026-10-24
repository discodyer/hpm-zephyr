# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_UDC_HPMICRO
	${HPM_SDK_DIR}/components/usb/device/hpm_usb_device.c
	udc_hpmicro.c)

zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/usb/udc)

zephyr_include_directories_ifdef(CONFIG_UDC_HPMICRO
	${HPM_SDK_DIR}/components/usb/device)
