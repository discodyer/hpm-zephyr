#!/usr/bin/env python3
# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

"""Validate the static layout of the HPM Zephyr module."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]

REQUIRED = (
    "zephyr/module.yml",
    "Kconfig.hpm",
    "soc/hpmicro/soc.yml",
    "soc/hpmicro/HPM5300/Kconfig.soc",
    "boards/hpmicro/hpm5300evk/board.yml",
    "boards/hpmicro/hpm5300evk/hpm5300evk.dts",
    "dts/riscv/hpmicro/hpm5361.dtsi",
)


def main() -> int:
    missing = [item for item in REQUIRED if not (ROOT / item).is_file()]
    if missing:
        for item in missing:
            print(f"missing: {item}", file=sys.stderr)
        return 1

    module_yml = (ROOT / "zephyr/module.yml").read_text(encoding="utf-8")
    for setting in ("board_root", "dts_root", "soc_root", "module_ext_root"):
        if setting not in module_yml:
            print(f"module.yml missing setting: {setting}", file=sys.stderr)
            return 1

    print("hpm-zephyr module layout: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
