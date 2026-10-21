# Copyright (c) 2024-2026 HPMicro
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_SOC_FAMILY_HPM)

if(CONFIG_XIP)
  set(INCLUDE_BOOTHEADER 1)
endif()

set(HPM_SDK_BINARY_DIR ${CMAKE_BINARY_DIR}/hpm_sdk)
set(HPM_BOARD_DIR ${CONFIG_BOARD})
string(TOUPPER ${CONFIG_SOC} HPM_SOC_TOUPPER)
set(HPM_SOC ${HPM_SOC_TOUPPER})
string(TOUPPER ${CONFIG_SOC_SERIES} HPM_SOC_SERIES_TOUPPER)
set(HPM_SOC_SERIES ${HPM_SOC_SERIES_TOUPPER})
set(TOOLCHAIN_VARIANT "zephyr")
set(ENV{GNURISCV_TOOLCHAIN_PATH} ${SYSROOT_DIR})
set(HPM_SDK_LIB_ITF zephyr_interface)
set(HPM_SDK_GCC_LIB_ITF zephyr_interface)
set(HPM_SDK_NDSGCC_LIB_ITF hpm_sdk_ndsgcc_lib_itf)
add_library(${HPM_SDK_NDSGCC_LIB_ITF} INTERFACE)
set(HPM_SDK_ZCC_LIB_ITF hpm_sdk_zcc_lib_itf)
add_library(${HPM_SDK_ZCC_LIB_ITF} INTERFACE)
set(USE_CUSTOM_STARTUP 1)
set(USE_CUSTOM_LINKER 1)
set(HPM_SOC_DISABLE_B_EXT 1)

zephyr_library_named(hpm_sdk)
set(HPM_SDK_LIB hpm_sdk)
set(HPM_SDK_GCC_LIB hpm_sdk)
set(HPM_SDK_GCC_STARTUP_LIB hpm_sdk)

file(STRINGS "${CMAKE_BINARY_DIR}/zephyr/.config" CONFIG_LINES
     REGEX "^CONFIG_HAS_HPMSDK_[A-Za-z0-9_]+=")
foreach(line IN LISTS CONFIG_LINES)
  string(REGEX REPLACE "(CONFIG_HAS_HPMSDK_[A-Za-z0-9_]+)=y" "\\1" config ${line})
  string(REGEX REPLACE "CONFIG_" "HPMSOC_" HPMSOC_ENABLE ${config})
  set(${HPMSOC_ENABLE} y)
endforeach()

zephyr_include_directories(${HPM_SDK_DIR}/drivers/inc)
zephyr_include_directories(${HPM_SDK_DIR}/arch)
zephyr_include_directories(${HPM_SDK_DIR}/soc/HPM5300/HPM5361)
zephyr_include_directories(${HPM_SDK_DIR}/soc/HPM5300/ip)

add_subdirectory(${HPM_SDK_DIR}/arch ${HPM_SDK_BINARY_DIR}/arch)
add_subdirectory(${HPM_SDK_DIR}/soc ${HPM_SDK_BINARY_DIR}/soc)
add_subdirectory(${HPM_SDK_DIR}/drivers ${HPM_SDK_BINARY_DIR}/drivers)

sdk_inc(.)
endif()
