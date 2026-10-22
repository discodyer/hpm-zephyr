#!/usr/bin/env python3
# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

"""Validate the static layout and SDK capability mapping of the HPM module."""

import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

REQUIRED = (
    "zephyr/module.yml",
    "Kconfig.hpm",
    "soc/hpmicro/soc.yml",
    "soc/hpmicro/hpm5300/Kconfig.soc",
    "boards/hpmicro/hpm5300evk/board.yml",
    "boards/hpmicro/hpm5300evk/hpm5300evk.dts",
    "boards/hpmicro/hpm5361icb/board.yml",
    "boards/hpmicro/hpm5361icb/hpm5361icb.dts",
    "drivers/i2c/i2c_hpmicro.c",
    "drivers/spi/spi_hpmicro.c",
    "dts/bindings/hpmicro,hpm5361icb.yaml",
    "dts/bindings/i2c/hpmicro,hpm-i2c.yaml",
    "dts/bindings/spi/hpmicro,hpm-spi.yaml",
    "dts/riscv/hpmicro/hpm5361.dtsi",
)


def _kconfig_symbols(text: str, prefix: str) -> set[str]:
    return set(re.findall(rf"^config {re.escape(prefix)}([A-Za-z0-9_]+)$", text, re.MULTILINE))


def _check_sdk_mapping(root: Path) -> list[str]:
    errors = []
    sdk_dir = os.environ.get("HPM_SDK_DIR")
    kconfig = (root / "modules/hpm_sdk/Kconfig").read_text(encoding="utf-8")
    capabilities = _kconfig_symbols(kconfig, "HAS_HPMSDK_")
    selections = _kconfig_symbols(kconfig, "USE_HPMSDK_")

    missing_dependencies = [
        name
        for name in sorted(selections)
        if not re.search(
            rf"^config USE_HPMSDK_{re.escape(name)}\n(?:.*\n)*?\tdepends on HAS_HPMSDK_{re.escape(name)}$",
            kconfig,
            re.MULTILINE,
        )
    ]
    if missing_dependencies:
        errors.append("USE symbols without HAS dependency: " + ", ".join(missing_dependencies))

    if not sdk_dir:
        return errors

    sdk = Path(sdk_dir)
    module_list = sdk / "soc/HPM5300/HPM5361/soc_modules.list"
    if not module_list.is_file():
        errors.append(f"missing pinned SDK module list: {module_list}")
        return errors

    sdk_capabilities = set(
        re.findall(
            r"^HPMSOC_HAS_HPMSDK_([A-Za-z0-9_]+)=y$",
            module_list.read_text(encoding="utf-8"),
            re.MULTILINE,
        )
    )
    soc_kconfig = (root / "soc/hpmicro/hpm5300/Kconfig.soc").read_text(encoding="utf-8")
    hpm5361 = re.search(r"^config SOC_HPM5361\n(?:.*\n)*?(?=^config |\Z)", soc_kconfig, re.MULTILINE)
    if not hpm5361:
        errors.append("SOC_HPM5361 is not defined in Kconfig.soc")
        return errors

    selected = set(re.findall(r"^\tselect HAS_HPMSDK_([A-Za-z0-9_]+)$", hpm5361.group(0), re.MULTILINE))
    expected = sdk_capabilities | {"XPI"}
    if selected != expected:
        missing = sorted(expected - selected)
        extra = sorted(selected - expected)
        if missing:
            errors.append("HPM5361 does not select SDK capabilities: " + ", ".join(missing))
        if extra:
            errors.append("HPM5361 selects unknown capabilities: " + ", ".join(extra))

    sdk_drivers = sdk / "drivers/CMakeLists.txt"
    if sdk_drivers.is_file():
        driver_capabilities = set(
            re.findall(
                r"sdk_src_ifdef\(HPMSOC_HAS_HPMSDK_([A-Za-z0-9_]+)",
                sdk_drivers.read_text(encoding="utf-8"),
            )
        )
        missing_selections = driver_capabilities - selections
        if missing_selections:
            errors.append(
                "SDK driver capabilities without USE symbols: "
                + ", ".join(sorted(missing_selections))
            )

    return errors


def main() -> int:
    missing = [item for item in REQUIRED if not (ROOT / item).is_file()]
    if missing:
        for item in missing:
            print(f"missing: {item}", file=sys.stderr)
        return 1

    if (ROOT / "soc" / "hpmicro" / "hpm5300" / "soc.yml").exists():
        print("duplicate SoC discovery file: soc/hpmicro/hpm5300/soc.yml",
              file=sys.stderr)
        return 1

    module_yml = (ROOT / "zephyr/module.yml").read_text(encoding="utf-8")
    for setting in ("board_root", "dts_root", "soc_root", "module_ext_root"):
        if setting not in module_yml:
            print(f"module.yml missing setting: {setting}", file=sys.stderr)
            return 1

    errors = _check_sdk_mapping(ROOT)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print("hpm-zephyr module layout and SDK capability mapping: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
