#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the HPM USB Device profile's CDC data using pyserial or PyUSB on the host."""

import argparse
import struct
import time
from contextlib import contextmanager
from pathlib import Path

TRANSFER_SIZES = (1, 63, 64, 65, 511, 512, 513, 1024, 1025)
TRANSFER_ROUNDS = 3
FRAME_ACK = b"\x5a"
TEST_VID = 0x1209
TEST_PID = 0x0001
TEST_SERIAL = "HPM5361-HW-ACCEPT"


def find_port(port, wait_seconds):
    """Wait for an explicit path or the test device; never select the debug UART."""
    from serial.tools import list_ports

    deadline = time.monotonic() + wait_seconds
    while True:
        if port:
            if Path(port).exists():
                return port
        else:
            matches = [
                item.device
                for item in list_ports.comports()
                if (item.vid, item.pid, item.serial_number) == (TEST_VID, TEST_PID, TEST_SERIAL)
            ]
            if len(matches) > 1:
                raise RuntimeError("Multiple test devices found; specify the CDC port")
            if matches:
                return matches[0]
        if time.monotonic() >= deadline:
            raise RuntimeError(
                "USB CDC port not found. Run this script on the computer connected to "
                "USB OTG, expose the new ttyACM device to this container, or use "
                "--backend usb with /dev/bus/usb access. Do not use /dev/ttyHPM."
            )
        time.sleep(0.1)


@contextmanager
def serial_peer(args):
    import serial

    port = find_port(args.port, args.wait)
    if Path(port).name == "ttyHPM":
        raise ValueError("/dev/ttyHPM is the FT2232 console, not the USB CDC device")
    print(f"Using CDC serial port {port}", flush=True)
    with serial.Serial(port, 115200, timeout=0.1, write_timeout=5, exclusive=True) as peer:
        peer.reset_input_buffer()
        peer.dtr = True
        yield peer


def usb_gone(error):
    return error.errno == 19 or error.backend_error_code == -4


class UsbPeer:
    """Provide the same bounded read/write operations over CDC's bulk endpoints."""

    def __init__(self, device, endpoint_in, endpoint_out):
        import usb.core

        self.device = device
        self.endpoint_in = endpoint_in
        self.endpoint_out = endpoint_out
        self.timeout_error = usb.core.USBTimeoutError

    def write(self, payload):
        return self.device.write(self.endpoint_out.bEndpointAddress, payload, timeout=5000)

    def read(self, length):
        try:
            return self.device.read(self.endpoint_in.bEndpointAddress, length, timeout=100)
        except self.timeout_error:
            return b""


@contextmanager
def usb_peer(args):
    """Claim only this fixture's CDC interfaces and restore any kernel bindings."""
    import usb.core
    import usb.util

    deadline = time.monotonic() + args.wait
    while True:
        devices = list(usb.core.find(find_all=True, idVendor=TEST_VID, idProduct=TEST_PID))
        devices = [
            item
            for item in devices
            if (args.bus is None or item.bus == args.bus)
            and (args.address is None or item.address == args.address)
        ]
        if len(devices) > 1:
            raise RuntimeError("Multiple matching USB devices; select --bus and --address")
        if devices:
            device = devices[0]
            break
        if time.monotonic() >= deadline:
            raise RuntimeError("USB test device 1209:0001 not found on accessible USB buses")
        time.sleep(0.1)

    detached = []
    claimed = []
    try:
        if device.serial_number != TEST_SERIAL:
            raise RuntimeError("USB device serial does not identify the acceptance fixture")
        configuration = device.get_active_configuration()
        interfaces = [item for item in configuration if item.bAlternateSetting == 0]
        control = [item for item in interfaces if item.bInterfaceClass == 0x02]
        data = [item for item in interfaces if item.bInterfaceClass == 0x0A]
        if len(control) != 1 or len(data) != 1:
            raise RuntimeError("Expected exactly one CDC control and one data interface")
        endpoint_in = []
        endpoint_out = []
        for endpoint in data[0]:
            if usb.util.endpoint_type(endpoint.bmAttributes) != usb.util.ENDPOINT_TYPE_BULK:
                continue
            if usb.util.endpoint_direction(endpoint.bEndpointAddress) == usb.util.ENDPOINT_IN:
                endpoint_in.append(endpoint)
            else:
                endpoint_out.append(endpoint)
        if len(endpoint_in) != 1 or len(endpoint_out) != 1:
            raise RuntimeError("Expected one bulk IN and one bulk OUT endpoint")
        for interface in (control[0], data[0]):
            number = interface.bInterfaceNumber
            if device.is_kernel_driver_active(number):
                device.detach_kernel_driver(number)
                detached.append(number)
            usb.util.claim_interface(device, number)
            claimed.append(number)

        speed = {1: "low", 2: "full", 3: "high", 4: "super", 5: "super-plus"}
        print(
            f"Using USB {device.bus}:{device.address}, {speed.get(device.speed, 'unknown')} "
            f"speed; IN 0x{endpoint_in[0].bEndpointAddress:02x} "
            f"MPS {endpoint_in[0].wMaxPacketSize}, "
            f"OUT 0x{endpoint_out[0].bEndpointAddress:02x} "
            f"MPS {endpoint_out[0].wMaxPacketSize}",
            flush=True,
        )
        line_coding = struct.pack("<IBBB", 115200, 0, 0, 8)
        control_number = control[0].bInterfaceNumber
        if device.ctrl_transfer(0x21, 0x20, 0, control_number, line_coding) != 7:
            raise RuntimeError("Short SET_LINE_CODING transfer")
        result = bytes(device.ctrl_transfer(0xA1, 0x21, 0, control_number, 7))
        if result != line_coding:
            raise RuntimeError("GET_LINE_CODING did not match 115200 8N1")
        device.ctrl_transfer(0x21, 0x22, 3, control_number, None)
        yield UsbPeer(device, endpoint_in[0], endpoint_out[0])
    finally:
        # A successful firmware run intentionally disconnects after its final ACK.
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
            raise RuntimeError("Could not release/restore CDC interface") from cleanup_errors[0]


def read_exact(peer, size):
    deadline = time.monotonic() + 10
    result = bytearray()
    while len(result) < size and time.monotonic() < deadline:
        result.extend(peer.read(size - len(result)))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", nargs="?", help="CDC port; default: discover the test USB device")
    parser.add_argument("--backend", choices=("serial", "usb"), default="serial")
    parser.add_argument("--wait", type=float, default=60, help="Seconds to wait for enumeration")
    parser.add_argument("--bus", type=int, help="USB bus number for --backend usb")
    parser.add_argument("--address", type=int, help="USB device address for --backend usb")
    args = parser.parse_args()
    if args.wait < 0:
        parser.error("--wait must be nonnegative")
    if args.backend == "usb" and args.port:
        parser.error("A serial port cannot be used with --backend usb")
    if args.backend == "serial" and (args.bus is not None or args.address is not None):
        parser.error("--bus and --address require --backend usb")
    total = 0
    backend = usb_peer if args.backend == "usb" else serial_peer
    with backend(args) as peer:
        for round_index in range(TRANSFER_ROUNDS):
            for frame_index, size in enumerate(TRANSFER_SIZES):
                payload = bytes(
                    (offset * 37 + round_index * 13 + frame_index * 7) & 0xFF
                    for offset in range(size)
                )
                if peer.write(payload) != size:
                    raise RuntimeError("Short USB CDC write")
                result = read_exact(peer, size)
                expected = bytes(value ^ 0xA5 for value in payload)
                if result != expected:
                    raise RuntimeError(
                        f"Round {round_index}, length {size}: "
                        f"short/corrupt reply ({len(result)} bytes)"
                    )
                if peer.write(FRAME_ACK) != len(FRAME_ACK):
                    raise RuntimeError("Short acknowledgment write")
                total += size
    print(
        f"PASS: {TRANSFER_ROUNDS * len(TRANSFER_SIZES)} CDC exchanges, "
        f"{total} checked bytes each direction; lengths {TRANSFER_SIZES}",
        flush=True,
    )


if __name__ == "__main__":
    main()
