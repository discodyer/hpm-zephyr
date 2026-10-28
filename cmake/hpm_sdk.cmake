# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

# Board runners are configured before the module CMakeLists.txt is processed.
# Resolve the SDK here so both use the same path, including environment fallbacks.
include_guard(GLOBAL)

if(NOT HPM_SDK_DIR AND DEFINED ENV{HPM_SDK_DIR})
  set(HPM_SDK_DIR "$ENV{HPM_SDK_DIR}")
endif()

if(NOT HPM_SDK_DIR AND DEFINED ENV{HPM_SDK_BASE})
  set(HPM_SDK_DIR "$ENV{HPM_SDK_BASE}")
endif()

if(NOT HPM_SDK_DIR)
  set(HPM_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../hal/hpm_sdk")
endif()

get_filename_component(HPM_SDK_DIR "${HPM_SDK_DIR}" ABSOLUTE)
set(HPM_SDK_DIR "${HPM_SDK_DIR}" CACHE PATH "HPM SDK root directory" FORCE)

if(NOT EXISTS "${HPM_SDK_DIR}/cmake/cmake-ext.cmake")
  message(FATAL_ERROR
          "HPM_SDK_DIR does not contain an HPM SDK checkout: ${HPM_SDK_DIR}")
endif()
