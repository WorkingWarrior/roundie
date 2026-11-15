#!/usr/bin/env python3
"""Offline-only REP artifact/migration checks. No device or write operations."""

import argparse
import json
import os
from pathlib import Path
import sys

from flash_bringup import BACKUP_SHA256, check_legacy_build, sha256

ROOT = Path(__file__).resolve().parents[1]
FLASH_SIZE = 0x1000000
BOARD_FLASH_SIZES = ("16MB", "32MB")
EXPECTED = [
    ("nvs", 1, 2, 0x9000, 0x4000),
    ("otadata", 1, 0, 0xD000, 0x2000),
    ("phy_init", 1, 1, 0xF000, 0x1000),
    ("ota_0", 0, 0x10, 0x10000, 0x380000),
    ("ota_1", 0, 0x11, 0x390000, 0x380000),
    ("storage", 1, 0x83, 0x710000, 0x8D0000),
    ("coredump", 1, 3, 0xFE0000, 0x20000),
]
LEGACY_DEVICE = [
    ("nvs", 1, 2, 0x9000, 0x4000),
    ("otadata", 1, 0, 0xD000, 0x2000),
    ("phy_init", 1, 1, 0xF000, 0x1000),
    ("factory", 0, 0, 0x20000, 0x692000),
    ("ota_0", 0, 0x10, 0x6C0000, 0x400000),
    ("assets", 1, 0x82, 0xAC0000, 0x400000),
]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def entries(table):
    return [(p.name, p.type, p.subtype, p.offset, p.size) for p in table]


def validate_table(table):
    table.verify()
    require(entries(table) == EXPECTED, "partition layout is not REP v1")
    end = 0x9000  # ROM/bootloader area and partition-table sector
    for partition in table:
        require(partition.offset >= end, "partition overlap or unordered table")
        require(partition.size > 0 and partition.size % 0x1000 == 0,
                "invalid partition size")
        require(partition.offset % (0x10000 if partition.type == 0 else 0x1000) == 0,
                "invalid partition alignment")
        require(not partition.encrypted and not partition.readonly,
                "unexpected partition flags")
        end = partition.offset + partition.size
        require(end <= FLASH_SIZE, "partition exceeds 16 MiB")
    require(end == FLASH_SIZE, "unexpected unallocated tail")


def validate_app(app):
    require(len(app) >= 288 and app[0] == 0xE9 and
            app[32:36] == b"\x32\x54\xcd\xab" and
            app[80:112].split(b"\0", 1)[0] == b"roundie_rep",
            "image is not an identified REP app")
    rounded = (len(app) + 0xFFF) & ~0xFFF
    for name, kind, _, _, size in EXPECTED:
        if kind == 0:
            require(rounded <= size, f"app erase range exceeds {name}")


def validate_build(build, generator):
    require((build / "roundie-profile.txt").read_text().strip() == "rep",
            "build profile is not REP")
    table = generator.PartitionTable.from_binary(
        (build / "partition_table/partition-table.bin").read_bytes())
    validate_table(table)
    csv = generator.PartitionTable.from_csv((ROOT / "partitions/rep.csv").read_text())
    require(csv.to_binary() == table.to_binary(), "CSV/binary partition mismatch")
    config = json.loads((build / "config/sdkconfig.json").read_text())
    # REP v1 occupies the first 16 MiB. A larger chip (the AMOLED 1.75C has
    # 32 MiB) keeps the same layout; the rest stays unallocated.
    require(config.get("ESPTOOLPY_FLASHSIZE") in BOARD_FLASH_SIZES,
            "config mismatch: ESPTOOLPY_FLASHSIZE")
    for key, value in {
        "IDF_TARGET": "esp32s3",
        "PARTITION_TABLE_OFFSET": 0x8000,
        "PARTITION_TABLE_CUSTOM": True,
        "PARTITION_TABLE_CUSTOM_FILENAME": "partitions/rep.csv",
        "BOOTLOADER_APP_ROLLBACK_ENABLE": True,
        "SPIRAM_XIP_FROM_PSRAM": True,
        "ESP_PHY_INIT_DATA_IN_PARTITION": False,
        "ESP_COREDUMP_ENABLE_TO_NONE": True,
        "SECURE_BOOT": False, "SECURE_FLASH_ENC_ENABLED": False,
        "BOOTLOADER_APP_ANTI_ROLLBACK": False,
    }.items():
        require(config.get(key, False) == value, f"config mismatch: {key}")
    manifest = json.loads((build / "flasher_args.json").read_text())
    require(manifest["flash_files"] == {
        "0x0": "bootloader/bootloader.bin",
        "0x8000": "partition_table/partition-table.bin",
        "0xd000": "ota_data_initial.bin", "0x10000": "roundie_rep.bin",
    }, "unexpected flash manifest; review migration")
    bootloader = (build / "bootloader/bootloader.bin").read_bytes()
    require(0 < len(bootloader) <= 0x8000 and bootloader[0] == 0xE9,
            "bootloader absent or overlaps table")
    require((build / "ota_data_initial.bin").read_bytes() == b"\xff" * 0x2000,
            "initial OTA metadata must contain exactly two erased sectors")
    app = (build / "roundie_rep.bin").read_bytes()
    validate_app(app)
    print(f"REP v1: 16 MiB, no overlap, no unused tail in the layout; "
          f"chip {config['ESPTOOLPY_FLASHSIZE']}; app={len(app)} bytes")
    for name, kind, subtype, offset, size in EXPECTED:
        print(f"  {name:10s} type={kind:#x}/{subtype:#x} offset={offset:#08x} size={size:#08x}")
        if kind == 0:
            print(f"    image headroom={size - len(app)} bytes ({100 * (size - len(app)) / size:.1f}%)")
    for relative in ("bootloader/bootloader.bin", "partition_table/partition-table.bin",
                     "ota_data_initial.bin", "roundie_rep.bin"):
        data = (build / relative).read_bytes()
        print(f"  artifact {relative}: {len(data)} bytes SHA256={sha256(data)}")
    return table


def validate_backup(path, table, generator):
    backup = path.read_bytes()
    require(len(backup) == FLASH_SIZE and sha256(backup) == BACKUP_SHA256,
            "original backup size/hash mismatch")
    original = generator.PartitionTable.from_binary(backup[0x8000:0x9000])
    require(entries(original) == LEGACY_DEVICE, "unrecognized original partition table")
    require(backup[0xD000:0xF000] == b"\xff" * 0x2000,
            "original backup OTA data differs from evidence")
    print("Original backup size/hash/table: PASS (offline evidence, not live device state)")
    for new in table:
        overlaps = [old.name for old in original
                    if max(old.offset, new.offset) < min(old.offset + old.size,
                                                         new.offset + new.size)]
        print(f"  target {new.name} overlaps original: {', '.join(overlaps) or 'unused'}")
    print("Fresh plan: separately authorize destructive layout replacement; erase target Flash,")
    print("then use exactly bootloader@0, table@0x8000, erased otadata@0xd000, REP app@0x10000.")
    print("Leave ota_1/storage/coredump blank; no filesystem provisioning is implied.")
    print("Data preservation: NOT established; export/inspect/convert first. No device access performed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build-rep")
    parser.add_argument("--backup", type=Path, help="verify preserved backup offline")
    parser.add_argument("--legacy-build", type=Path, help="check old updater eligibility offline")
    args = parser.parse_args()
    idf = Path(os.environ["IDF_PATH"])
    sys.path.insert(0, str(idf / "components/partition_table"))
    import gen_esp32part as generator
    table = validate_build(args.build.resolve(), generator)
    if args.backup:
        validate_backup(args.backup, table, generator)
    if args.legacy_build:
        app = check_legacy_build(args.legacy_build)
        print(f"Legacy build profile/table/embedded identity: PASS ({len(app)} bytes)")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        print(f"Offline validation failed: {error}", file=sys.stderr)
        sys.exit(1)
