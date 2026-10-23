#!/usr/bin/env python3
# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

"""Check HPM5361 package pin availability and pinctrl ownership conflicts."""

import argparse
import os
from pathlib import Path
import sys

# Generated zephyr.dts keeps the encoded HPMICRO_PINMUX cells in low 10 bits.
PORT_BASE = {"PA": 0, "PB": 32, "PX": 416, "PY": 448}


def import_dtlib(zephyr_base: Path):
    sys.path.insert(0, str(zephyr_base / "scripts/dts/python-devicetree/src"))
    from devicetree.dtlib import DT  # pylint: disable=import-outside-toplevel

    return DT


def prop_string(node, name, default=None):
    prop = node.props.get(name)
    return default if prop is None else prop.to_string()


def prop_strings(node, name, default=()):
    prop = node.props.get(name)
    return list(default) if prop is None else prop.to_strings()


def node_okay(node):
    return prop_string(node, "status", "okay") in ("okay", "ok")


def walk(node):
    yield node
    for child in node.nodes.values():
        yield from walk(child)


def pad_name(number):
    for port, base in PORT_BASE.items():
        if base <= number < base + 32:
            return f"{port}{number - base:02d}"
    return f"unknown-{number}"


def pinmux_values(node):
    prop = node.props.get("pinmux")
    return [] if prop is None else prop.to_nums()


def state_pins(group):
    nodes = []

    def collect(node):
        if "pinmux" in node.props:
            nodes.append(node)
        for child in node.nodes.values():
            collect(child)

    collect(group)
    return nodes


def find_package(dt):
    packages = [
        node
        for node in walk(dt.root)
        if "hpmicro,hpm5361-package" in prop_strings(node, "compatible")
    ]
    if len(packages) != 1:
        raise ValueError(f"expected exactly one HPM5361 package node, found {len(packages)}")
    return packages[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dts", type=Path, help="generated zephyr.dts")
    parser.add_argument("--zephyr-base", type=Path,
                        default=Path(os.environ.get("ZEPHYR_BASE", "")))
    args = parser.parse_args()

    if not args.zephyr_base.is_dir():
        parser.error("set ZEPHYR_BASE or pass --zephyr-base")
    if not args.dts.is_file():
        parser.error(f"missing generated devicetree: {args.dts}")

    DT = import_dtlib(args.zephyr_base)
    dt = DT(str(args.dts))
    package = find_package(dt)
    available = set(prop_strings(package, "hpmicro,available-pads"))
    assigned_pads = set()
    errors = []

    for node in walk(dt.root):
        if not node_okay(node):
            continue
        for state in range(5):
            prop = node.props.get(f"pinctrl-{state}")
            if prop is None:
                continue
            owners = {}
            for group in prop.to_nodes():
                for pin_node in state_pins(group):
                    for mux in pinmux_values(pin_node):
                        pad = pad_name(mux & 0x3FF)
                        ioc = (mux >> 30) & 0x3
                        if pad not in available:
                            errors.append(
                                f"{node.path}: state {state}: {pad} is not bonded out "
                                f"by {prop_string(package, 'hpmicro,model')}"
                            )
                        owners.setdefault((ioc, pad), []).append(node.path)
                        assigned_pads.add(pad)

            for (ioc, pad), users in sorted(owners.items(), key=lambda item: item[0]):
                if len(users) > 1:
                    errors.append(
                        f"{node.path}: state {state}: controller {ioc} pad {pad} "
                        f"is assigned more than once ({', '.join(users)})"
                    )

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print(
        f"pinctrl check: {prop_string(package, 'hpmicro,model')} package OK, "
        f"{len(assigned_pads)} pads assigned"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
