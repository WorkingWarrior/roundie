#!/usr/bin/env python3
"""Check the inspected 1.85B layout, then optionally update its factory app only."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BACKUP = Path(
    "/mnt/Dane/Development/roundie-backups/"
    "preflash-2026-10-17-pMVcHN/flash-16MiB.bin"
)
BACKUP_SHA256 = "46d2d36244a9e303cc61a65fb29d9a71b9708d41d181e9e5b5befb9261039de7"
INSTALLED_BOOTLOADER_SHA256 = "c7a70b9af9d599095d524cac280838544d074bbd088f9a2e88d483aed95df26d"
FACTORY_OFFSET = 0x20000
FACTORY_SIZE = 0x692000
SECTOR_SIZE = 0x1000


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def read_device(port, offset, length, destination):
    subprocess.run(
        [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
         "read-flash", hex(offset), hex(length), str(destination)],
        check=True,
    )
    data = destination.read_bytes()
    if len(data) != length:
        raise ValueError(f"short read at {offset:#x}: {len(data)} of {length} bytes")
    return data


def preflight(backup, table, otadata, live_bootloader, built_bootloader, app,
              flash_files):
    if len(backup) != 0x1000000 or sha256(backup) != BACKUP_SHA256:
        raise ValueError("original 16 MiB factory backup is missing or has changed")
    if table != backup[0x8000:0x9000]:
        raise ValueError("live partition table differs from the inspected 1.85B backup")
    if otadata != b"\xff" * 0x2000:
        raise ValueError("OTA metadata is not erased; factory boot is not established")
    installed_image = live_bootloader[:len(built_bootloader)]
    if sha256(installed_image) != INSTALLED_BOOTLOADER_SHA256:
        raise ValueError("installed bootloader differs from the last verified device readback")
    # The rebuilt image changes header fields and its 1-byte digest marker + 32-byte digest.
    if installed_image[0x100:-33] != built_bootloader[0x100:-33]:
        raise ValueError("built bootloader code differs from the installed bootloader")
    if flash_files != {
        "0x0": "bootloader/bootloader.bin",
        "0x8000": "partition_table/partition-table.bin",
        "0x10000": "roundie.bin",
    }:
        raise ValueError("build flash manifest no longer describes the diagnostic layout")
    if not app or app[0] != 0xE9:
        raise ValueError("built application image is missing or invalid")
    if (len(app) + SECTOR_SIZE - 1) // SECTOR_SIZE * SECTOR_SIZE > FACTORY_SIZE:
        raise ValueError("application erase range exceeds the installed factory slot")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="ESP32-S3 ROM serial port")
    parser.add_argument("--write", action="store_true",
                        help="after preflight, write only the factory app at 0x20000")
    args = parser.parse_args()

    backup = BACKUP.read_bytes()
    app = (ROOT / "build/roundie.bin").read_bytes()
    built_bootloader = (ROOT / "build/bootloader/bootloader.bin").read_bytes()
    flash_files = json.loads((ROOT / "build/flasher_args.json").read_text())["flash_files"]
    bootloader_read_size = (len(built_bootloader) + SECTOR_SIZE - 1) // SECTOR_SIZE * SECTOR_SIZE
    if bootloader_read_size > 0x8000:
        raise ValueError("built bootloader overlaps the installed partition table")

    with tempfile.TemporaryDirectory(prefix="roundie-preflight-") as directory:
        temporary = Path(directory)
        table = read_device(args.port, 0x8000, 0x1000, temporary / "table.bin")
        otadata = read_device(args.port, 0xD000, 0x2000, temporary / "otadata.bin")
        live_bootloader = read_device(args.port, 0x0, bootloader_read_size,
                                      temporary / "bootloader.bin")
        preflight(backup, table, otadata, live_bootloader, built_bootloader,
                  app, flash_files)

    print(f"Preflight passed: factory app at {FACTORY_OFFSET:#x}, {len(app)} bytes")
    if not args.write:
        print("Check only; no Flash write requested")
        return

    subprocess.run(
        [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", args.port,
         "--after", "hard-reset", "write-flash", hex(FACTORY_OFFSET),
         str(ROOT / "build/roundie.bin")],
        check=True,
    )


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Deployment stopped: {error}", file=sys.stderr)
        sys.exit(1)
