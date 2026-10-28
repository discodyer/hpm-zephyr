# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/hpm_sdk.cmake)

if(CONFIG_XIP)
  board_runner_args(openocd "--file-type=hex")
else()
  board_runner_args(openocd "--file-type=elf")
endif()

board_runner_args(openocd
  "--openocd-search=${HPM_SDK_DIR}/boards/openocd"
  "--cmd-pre-init=adapter speed 500"
  "--target-handle=_TARGET0")

include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
