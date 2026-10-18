#!/usr/bin/env python3
"""Create a new host LittleFS image. Never opens a device or an existing image."""

import argparse
import hashlib
from importlib.metadata import version
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
REQUEST = "Roundie Milestone 2B filesystem check v1\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build-rep")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--diagnostic", action="store_true",
                        help="include the single-use M2B filesystem test request")
    args = parser.parse_args()
    # Use the same pinned host tool as the component's image generator.
    if version("littlefs-python") != "0.15.0":
        raise ValueError("requires littlefs-python==0.15.0")
    sys.path.insert(0, str(Path(os.environ["IDF_PATH"]) / "components/partition_table"))
    import gen_esp32part as generator
    from littlefs import LittleFS
    from littlefs.context import UserContext
    from validate_storage_layout import validate_build

    table = validate_build(args.build.resolve(), generator)
    partition = next(p for p in table if p.name == "storage")
    config = json.loads((args.build / "config/sdkconfig.json").read_text())
    if config["LITTLEFS_PAGE_SIZE"] != 256 or config["LITTLEFS_MULTIVERSION"]:
        raise ValueError("review image compatibility for changed target parameters")
    parameters = {
        "block_size": 4096,
        "block_count": partition.size // 4096,
        "read_size": config["LITTLEFS_READ_SIZE"],
        "prog_size": config["LITTLEFS_WRITE_SIZE"],
        "cache_size": config["LITTLEFS_CACHE_SIZE"],
        "lookahead_size": config["LITTLEFS_LOOKAHEAD_SIZE"],
        "block_cycles": config["LITTLEFS_BLOCK_CYCLES"],
        "name_max": config["LITTLEFS_OBJ_NAME_LEN"],
    }
    context = UserContext(partition.size)
    filesystem = LittleFS(context=context, mount=False, **parameters)
    filesystem.format()  # Explicit host-only initialization of a new buffer.
    filesystem.mount()
    if args.diagnostic:
        with filesystem.open("/.roundie-m2b-request", "xb") as stream:
            stream.write(REQUEST.encode("ascii"))
    filesystem.unmount()
    image = bytes(context.buffer)
    # Verify an independent mount without the Python wrapper's format fallback.
    checked = UserContext(partition.size)
    checked.buffer[:] = image
    filesystem = LittleFS(context=checked, mount=False, **parameters)
    filesystem.mount()
    expected = [".roundie-m2b-request"] if args.diagnostic else []
    if filesystem.listdir("/") != expected:
        raise ValueError("unexpected provisioned directory")
    if args.diagnostic:
        with filesystem.open("/.roundie-m2b-request", "rb") as stream:
            if stream.read() != REQUEST.encode("ascii"):
                raise ValueError("diagnostic request integrity failure")
    print("LittleFS parameters:", json.dumps(parameters, sort_keys=True))
    print("On-disk filesystem:", filesystem.fs_stat())
    filesystem.unmount()
    with args.output.open("xb") as stream:
        stream.write(image)
    print(f"Image: {args.output}; bytes={len(image)} SHA256={hashlib.sha256(image).hexdigest()}")


if __name__ == "__main__":
    main()
