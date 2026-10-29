#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check every non-control endpoint pair of the HPM USB Device test fixture."""

import argparse
import time
from contextlib import contextmanager

import usb.core
import usb.util

TEST_VID = 0x1209
TEST_PID = 0x0002
TEST_SERIAL = "HPM5361-USB-ENDPOINT"
TRANSFER_SIZES = (1, 63, 64, 65, 511, 512, 513, 1024, 1025)
TRANSFER_ROUNDS = 3
ENDPOINT_PAIRS = 15


def usb_gone(error):
    return error.errno == 19 or error.backend_error_code == -4


@contextmanager
def endpoint_device(args):
    deadline = time.monotonic() + args.wait
    while True:
        devices = [
            item
            for item in usb.core.find(
                find_all=True, idVendor=TEST_VID, idProduct=TEST_PID
            )
            if (args.bus is None or item.bus == args.bus)
            and (args.address is None or item.address == args.address)
        ]
        if len(devices) > 1:
            raise RuntimeError("Multiple fixtures found; specify --bus and --address")
        if devices:
            device = devices[0]
            break
        if time.monotonic() >= deadline:
            raise RuntimeError("USB endpoint test fixture 1209:0002 not found")
        time.sleep(0.1)

    detached = []
    claimed = []
    try:
        if device.serial_number != TEST_SERIAL:
            raise RuntimeError(
                "Device serial does not identify the endpoint test fixture"
            )
        configuration = device.get_active_configuration()
        interfaces = [item for item in configuration if item.bAlternateSetting == 0]
        if len(interfaces) != 1 or interfaces[0].bInterfaceClass != 0xFF:
            raise RuntimeError("Expected one vendor interface")
        interface = interfaces[0]
        endpoints = {endpoint.bEndpointAddress: endpoint for endpoint in interface}
        expected = set(range(1, 16)) | set(range(0x81, 0x90))
        if len(list(interface)) != 30 or set(endpoints) != expected:
            raise RuntimeError(f"Expected EP1..15 IN/OUT, received {sorted(endpoints)}")
        if device.speed not in (2, 3):
            raise RuntimeError(f"Unexpected USB speed {device.speed}")
        speed = "high" if device.speed == 3 else "full"
        if args.speed and speed != args.speed:
            raise RuntimeError(f"Expected {args.speed} speed; negotiated {speed}")
        mps = 512 if speed == "high" else 64
        for address, endpoint in endpoints.items():
            if (
                usb.util.endpoint_type(endpoint.bmAttributes)
                != usb.util.ENDPOINT_TYPE_BULK
            ):
                raise RuntimeError(f"Endpoint 0x{address:02x} is not bulk")
            if endpoint.wMaxPacketSize != mps:
                raise RuntimeError(
                    f"Endpoint 0x{address:02x} has MPS {endpoint.wMaxPacketSize}"
                )
        number = interface.bInterfaceNumber
        if device.is_kernel_driver_active(number):
            device.detach_kernel_driver(number)
            detached.append(number)
        usb.util.claim_interface(device, number)
        claimed.append(number)
        print(
            f"Using USB {device.bus}:{device.address}, {speed} speed; "
            f"15 bulk endpoint pairs, MPS {mps}",
            flush=True,
        )
        for pair in range(1, 16):
            print(f"Pair {pair}: OUT 0x{pair:02x}, IN 0x{pair | 0x80:02x}", flush=True)
        yield device
    finally:
        cleanup_errors = []
        for number in reversed(claimed):
            try:
                usb.util.release_interface(device, number)
            except usb.core.USBError as error:
                if not usb_gone(error):
                    cleanup_errors.append(error)
        for number in detached:
            try:
                if not device.is_kernel_driver_active(number):
                    device.attach_kernel_driver(number)
            except usb.core.USBError as error:
                if not usb_gone(error):
                    cleanup_errors.append(error)
        usb.util.dispose_resources(device)
        if cleanup_errors:
            raise RuntimeError(
                "Could not release/restore fixture interface"
            ) from cleanup_errors[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--wait", type=float, default=75, help="Seconds to wait for enumeration"
    )
    parser.add_argument("--bus", type=int, help="Select the USB bus")
    parser.add_argument("--address", type=int, help="Select the USB device address")
    parser.add_argument(
        "--speed", choices=("full", "high"), help="Require this negotiated speed"
    )
    args = parser.parse_args()
    if args.wait < 0:
        parser.error("--wait must be nonnegative")
    total = 0
    with endpoint_device(args) as device:
        for round_index in range(TRANSFER_ROUNDS):
            for pair_index in range(ENDPOINT_PAIRS):
                endpoint_out = pair_index + 1
                endpoint_in = endpoint_out | 0x80
                for frame_index, size in enumerate(TRANSFER_SIZES):
                    payload = bytes(
                        (
                            offset * 37
                            + round_index * 13
                            + pair_index * 11
                            + frame_index * 7
                        )
                        & 0xFF
                        for offset in range(size)
                    )
                    if device.write(endpoint_out, payload, timeout=10000) != size:
                        raise RuntimeError(f"Short write on EP{endpoint_out}")
                    result = bytes(device.read(endpoint_in, size, timeout=10000))
                    if result != bytes(value ^ 0xA5 for value in payload):
                        raise RuntimeError(
                            f"Round {round_index}, EP{endpoint_out}, length {size}: "
                            f"short/corrupt reply ({len(result)} bytes)"
                        )
                    total += size
        if device.write(15, b"\x5a", timeout=10000) != 1:
            raise RuntimeError("Short final acknowledgment on EP15 OUT")
    print(
        f"PASS: all 15 IN/OUT pairs, {TRANSFER_ROUNDS * ENDPOINT_PAIRS * len(TRANSFER_SIZES)} "
        f"exchanges, {total} checked bytes each direction; final ACK sent",
        flush=True,
    )


if __name__ == "__main__":
    main()
