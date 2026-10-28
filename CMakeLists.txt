# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

set(HPM_ZEPHYR_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${CMAKE_CURRENT_LIST_DIR}/cmake/hpm_sdk.cmake)

set(HPM_SDK_BASE ${HPM_SDK_DIR})
include(${HPM_SDK_DIR}/cmake/version.cmake)
include(${HPM_SDK_DIR}/cmake/cmake-ext.cmake)
include(${HPM_SDK_DIR}/cmake/ide/segger.cmake)
include(${HPM_SDK_DIR}/cmake/ide/iar.cmake)

zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/include)
# Zephyr scans this interface for __subsystem and DEVICE_API_EXTENDS when
# generating device API sections. Module headers must be registered here
# as well as globally included, otherwise custom API classes have no
# linker section and DEVICE_API_IS() produces undefined symbols.
zephyr_syscall_include_directories(${CMAKE_CURRENT_LIST_DIR}/include)
include(${CMAKE_CURRENT_LIST_DIR}/modules/hpm_sdk/CMakeLists.txt)
add_subdirectory(drivers)
