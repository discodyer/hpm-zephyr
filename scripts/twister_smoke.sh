#!/usr/bin/env sh
# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

set -eu

: "${ZEPHYR_BASE:?set ZEPHYR_BASE to the Zephyr checkout}"
: "${HPM_SDK_DIR:?set HPM_SDK_DIR to the HPM SDK checkout}"
MODULE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR=${TWISTER_OUT_DIR:-"$MODULE_DIR/twister-out"}

ZEPHYR_EXTRA_MODULES=$MODULE_DIR HPM_SDK_DIR=$HPM_SDK_DIR west twister \
	-T "$MODULE_DIR/tests" \
	-p hpm5300evk/hpm5361 \
	-p hpm5361icb/hpm5361 \
	--build-only \
	-O "$OUT_DIR" \
	"$@"

printf 'HPM5300 Twister build-only tests passed\n'
