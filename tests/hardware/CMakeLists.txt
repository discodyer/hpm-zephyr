# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
if(NOT DEFINED EXTRA_CONF_FILE)
  set(EXTRA_CONF_FILE profiles/core.conf CACHE STRING "Hardware acceptance configuration")
endif()
if(NOT DEFINED DTC_OVERLAY_FILE)
  set(DTC_OVERLAY_FILE profiles/core.overlay CACHE STRING "Hardware acceptance overlay")
endif()
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hpm5361_hardware_acceptance)
target_sources(app PRIVATE src/main.c)
foreach(profile core dma flash watchdog can motor gpio uart i2c spi analog front_end pwm qei)
  string(TOUPPER ${profile} symbol)
  target_sources_ifdef(CONFIG_HPM_TEST_${symbol} app PRIVATE src/${profile}.c)
endforeach()

target_sources_ifdef(CONFIG_HPM_TEST_USB_DEVICE app PRIVATE src/usb_device.c)

if(CONFIG_HPM_TEST_CRYPTO)
  target_sources(app PRIVATE ../hpm5300evk/src/crypto.c)
endif()
