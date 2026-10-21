#!/usr/bin/env sh
# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

set -eu

: "${ZEPHYR_BASE:?set ZEPHYR_BASE to the Zephyr checkout}"
: "${HPM_SDK_DIR:?set HPM_SDK_DIR to the HPM SDK checkout}"
MODULE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
APP_DIR=$(mktemp -d)
trap 'find "$APP_DIR" -depth -delete' EXIT INT TERM

mkdir -p "$APP_DIR/src"
cat > "$APP_DIR/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hpm_smoke)
target_sources(app PRIVATE src/main.c)
CMAKE
cat > "$APP_DIR/prj.conf" <<'CONF'
CONFIG_CONSOLE=y
CONFIG_SERIAL=y
CONFIG_UART_CONSOLE=y
CONFIG_GPIO=y
CONFIG_PINCTRL=y
CONFIG_XIP=n
CONF
cat > "$APP_DIR/src/main.c" <<'C'
int main(void)
{
	return 0;
}
C

cmake -S "$APP_DIR" -B "$APP_DIR/build" -GNinja \
	-DBOARD=hpm5300evk/hpm5361 \
	-DZEPHYR_MODULES="$MODULE_DIR" \
	-DHPM_SDK_DIR="$HPM_SDK_DIR"
cmake --build "$APP_DIR/build"
