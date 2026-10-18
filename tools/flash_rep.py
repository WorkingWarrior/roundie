#!/usr/bin/env python3
"""Fail-closed REP development app updater; read-only unless --write is given.

Only the initial, confirmed ota_0 selection is supported. No OTA selection,
bootloader, partition table, NVS or filesystem writes are implemented here.
"""

import argparse
import hashlib
import os
from pathlib import Path
import struct
import sys
import zlib

from validate_storage_layout import ROOT, validate_app, validate_build, require

# Proven Milestone 2B rollback-enabled bootloader; rebuilt bootloader descriptors
# contain new build times, so an app-only bootstrap must not require replacing it.
# No caller-selected arbitrary hash or bootloader write is supported.
M2B_BOOTLOADER_SHA256 = "f7c33fe831a564d16b8b00406c20ac2e79b8b7146f75e862263764656f349188"


def bootstrap_bootloader(path):
    image = path.read_bytes()
    require(len(image) == 21200 and hashlib.sha256(image).hexdigest() == M2B_BOOTLOADER_SHA256,
            "bootstrap requires the exact verified Milestone 2B rollback bootloader")
    return image


def check_device(prefix, table, bootloader):
    require(len(prefix) == 0x10000, "short REP preflight read")
    require(prefix[0x8000:0x9000] == table.ljust(0x1000, b"\xff"),
            "live partition table is not the validated REP table")
    require(prefix[:len(bootloader)] == bootloader,
            "live bootloader differs from validated REP build")
    # Pinned IDF's initial USB installation writes seq=1, state=VALID (2),
    # CRC over only the sequence. Reject any later/unknown OTA state.
    sequence = struct.pack("<I", 1)
    record = sequence + b"\xff" * 20 + struct.pack("<II", 2, zlib.crc32(sequence, 0xffffffff))
    require(prefix[0xd000:0xf000] == record.ljust(0x2000, b"\xff"),
            "requires initial VALID ota_0 selection; review changed OTA state")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build-rep")
    parser.add_argument("--port", required=True)
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--installed-m2b-bootloader", type=Path,
                        help="exact read-back 2B bootloader for app-only OTA bootstrap; never written")
    args = parser.parse_args()
    sys.path.insert(0, str(Path(os.environ["IDF_PATH"]) / "components/partition_table"))
    import gen_esp32part as generator

    build = args.build.resolve()
    table = validate_build(build, generator)
    app = (build / "roundie_rep.bin").read_bytes()
    validate_app(app)
    bootloader = (build / "bootloader/bootloader.bin").read_bytes()
    if args.installed_m2b_bootloader is not None:
        bootloader = bootstrap_bootloader(args.installed_m2b_bootloader)
    table_bytes = (build / "partition_table/partition-table.bin").read_bytes()
    require(table_bytes == table.to_binary(), "partition table changed during preflight")
    slot = next(p for p in table if p.name == "ota_0")
    # All artifact bytes are frozen before opening the port.
    import esptool
    from esptool.cmds import read_flash, write_flash, verify_flash
    from serial.tools import list_ports

    port = next((p for p in list_ports.comports()
                 if Path(p.device).resolve() == Path(args.port).resolve()), None)
    require(port is not None and port.vid == 0x303a and port.pid == 0x1001,
            "expected local Espressif USB Serial/JTAG interface")
    require(port.serial_number is not None, "USB serial identity unavailable")
    with esptool.detect_chip(args.port) as device:
        require(device.CHIP_NAME == "ESP32-S3", "target is not ESP32-S3")
        require(bytes(device.read_mac()).hex() == port.serial_number.replace(":", "").lower(),
                "ROM MAC and USB serial identity differ")
        require(not device.get_secure_boot_enabled() and not device.get_flash_encryption_enabled(),
                "secured target is unsupported")
        device = device.run_stub()
        require((device.flash_id() >> 16) & 0xff == 24, "expected 16 MiB Flash")
        prefix = read_flash(device, 0, 0x10000, no_progress=True)
        check_device(prefix, table_bytes, bootloader)
        print(f"REP initial ota_0 preflight PASS; app offset={slot.offset:#x} bytes={len(app)}")
        if args.write:
            write_flash(device, [(slot.offset, app)])
            verify_flash(device, [(slot.offset, app)])
            check_device(read_flash(device, 0, 0x10000, no_progress=True), table_bytes, bootloader)
            print("Application verified; layout/OTA selection unchanged")
        else:
            print("Read-only preflight; no Flash write requested")
        device.hard_reset()
        print("If still in ROM, reconnect USB without BOOT; do not repeat the write.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        print(f"REP deployment stopped: {error}", file=sys.stderr)
        sys.exit(1)
