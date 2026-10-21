# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

set(HPM_ZEPHYR_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(_hpm_sdk_default
  ${CMAKE_CURRENT_LIST_DIR}/../hal/hpm_sdk ABSOLUTE)
set(HPM_SDK_DIR ${HPM_SDK_DIR} CACHE PATH "HPM SDK root directory")

if(NOT HPM_SDK_DIR AND DEFINED ENV{HPM_SDK_BASE})
  set(HPM_SDK_DIR $ENV{HPM_SDK_BASE})
endif()

if(NOT HPM_SDK_DIR)
  set(HPM_SDK_DIR ${_hpm_sdk_default})
endif()

if(NOT EXISTS ${HPM_SDK_DIR}/cmake/cmake-ext.cmake)
  message(FATAL_ERROR
          "HPM_SDK_DIR does not contain an HPM SDK checkout: ${HPM_SDK_DIR}")
endif()

set(HPM_SDK_BASE ${HPM_SDK_DIR})
include(${HPM_SDK_DIR}/cmake/version.cmake)
include(${HPM_SDK_DIR}/cmake/cmake-ext.cmake)
include(${HPM_SDK_DIR}/cmake/ide/segger.cmake)
include(${HPM_SDK_DIR}/cmake/ide/iar.cmake)

zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/include)
zephyr_include_directories(${HPM_SDK_DIR}/drivers/inc)
zephyr_include_directories(${HPM_SDK_DIR}/arch)
zephyr_include_directories(${HPM_SDK_DIR}/soc/HPM5300/HPM5361)
zephyr_include_directories(${HPM_SDK_DIR}/soc/HPM5300/ip)
include(${CMAKE_CURRENT_LIST_DIR}/modules/hpm_sdk/CMakeLists.txt)
add_subdirectory(drivers)
