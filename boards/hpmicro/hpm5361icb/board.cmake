# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

if(NOT CONFIG_XIP)
  board_runner_args(openocd "--use-elf")
endif()

board_runner_args(openocd
  "--config=${HPM_SDK_DIR}/boards/openocd/probes/cmsis_dap.cfg"
  "--config=${HPM_SDK_DIR}/boards/openocd/soc/hpm5300.cfg"
  "--config=${HPM_SDK_DIR}/boards/openocd/boards/hpm5300evk.cfg"
  "--openocd-search=${HPM_SDK_DIR}/boards/openocd"
  "--cmd-pre-init=adapter speed 500"
  "--target-handle=_CHIPNAME.cpu0")

include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
