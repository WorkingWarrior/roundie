#!/usr/bin/env python3
"""Collect the whole E0 baseline of one board with one command (docs/baseline/e0-1.75c.md).

    python3 tools/hardware_validation/e0_board.py --board 1.75c

Steps, each one stopping the sequence on BLOCKED:
1. build the CI `rep` image of the board (tools/ci/build_firmware.sh), sourcing
   the ESP-IDF v6.1 environment when idf.py is not on PATH (skip: --no-build);
2. find the board: an explicit --serial/--port, the only Espressif USB
   Serial/JTAG unit, or, with several units, the one whose firmware reports
   this board (RG1 BATTERY board name, or the RTC capability RT1 GET NO_RTC
   only on the 1.75C for firmware without RG1 BATTERY);
3. run_e0.py with --ota (the built image), --elf and --light-sleep;
4. run_e0_followup.py and run_e0_stall.py on the same unit, without a second OTA.

Reports go to build-ci/e0/<board>-<UTC>/{base,followup,stall}. Nothing erases
Flash, NVS or storage; the OTA uses RO1 with the image's own rollback rules.
"""

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_m8 as m8  # noqa: E402
import run_e0  # noqa: E402
from run_m8 import Blocked  # noqa: E402
from roundie_time import DeviceError, TransportError  # noqa: E402

TARGETS = {"1.85b": "waveshare_s3_touch_lcd_1_85b", "1.75c": "waveshare_s3_touch_amoled_1_75c"}
IDF_EXPORT = Path.home() / ".espressif" / "v6.1" / "esp-idf" / "export.sh"


def image_paths(board, root=m8.ROOT):
    build = root / "build-ci" / f"{TARGETS[board]}-rep"
    return build / "roundie_rep.bin", build / "roundie_rep.elf"


def build_command(board, idf_export=None, which=shutil.which):
    """The build as one bash command; sources export.sh only without idf.py."""
    build = f"tools/ci/build_firmware.sh {TARGETS[board]} rep"
    if which("idf.py") is not None:
        return ["bash", "-c", build]
    export = idf_export or (Path(os.environ["IDF_PATH"]) / "export.sh" if "IDF_PATH" in os.environ else IDF_EXPORT)
    return ["bash", "-c", f'source "{export}" >/dev/null && {build}']


def reported_board(device):
    """Board of the firmware on one unit, or None when it cannot tell."""
    try:
        name = device.call("gauge", "BATTERY")["board"]
        for board, expected in run_e0.BOARD_NAMES.items():
            if name == expected:
                return board
        return None
    except (DeviceError, TransportError, OSError):
        pass
    try:
        state = device.call("time", "GET")["state"]
    except (DeviceError, TransportError, OSError):
        return None
    # NO_RTC is the compiled-in capability, not a probe result: only the 1.75C
    # build has no RTC. A 1.85B whose PCF85063 does not answer reports NOT_FOUND.
    return "1.75c" if state == "NO_RTC" else "1.85b"


def find_board(board, port=None, serial=None, by_id=m8.BY_ID, device_factory=run_e0.Device, monitor=None):
    """(path, serial) of the unit running this board's firmware, or raise Blocked."""
    if port is not None or serial is not None:
        return m8.discover(port, serial, by_id)
    candidates = {}
    if by_id.is_dir():
        for entry in sorted(by_id.iterdir()):
            match = m8.ESPRESSIF.fullmatch(entry.name)
            if match is not None:
                candidates[match[1].upper()] = entry
    if not candidates:
        raise Blocked(f"no Espressif USB Serial/JTAG unit; connect the {board} board")
    if len(candidates) == 1:
        found_serial, entry = next(iter(candidates.items()))
        return str(entry), found_serial
    monitor = monitor or m8.Monitor()
    matches = []
    for found_serial, entry in candidates.items():
        if reported_board(device_factory(str(entry), monitor)) == board:
            matches.append((str(entry), found_serial))
    if len(matches) != 1:
        raise Blocked(f"{len(matches)} of {len(candidates)} units report the {board} firmware; pass --serial")
    return matches[0]


def runner_commands(board, serial, out, image, elf, python=sys.executable):
    base = [python, str(HERE / "run_e0.py"), "--board", board, "--serial", serial]
    return [
        ("base", base + ["--out", str(out / "base"), "--ota", str(image), "--elf", str(elf), "--light-sleep"]),
        ("followup", [python, str(HERE / "run_e0_followup.py"), "--board", board, "--serial", serial,
                      "--out", str(out / "followup"), "--elf", str(elf)]),
        ("stall", [python, str(HERE / "run_e0_stall.py"), "--board", board, "--serial", serial,
                   "--out", str(out / "stall"), "--elf", str(elf)]),
    ]


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--board", required=True, choices=m8.BOARDS)
    parser.add_argument("--serial", help="USB serial number (MAC); default: the unit reporting --board")
    parser.add_argument("--port", help="/dev/serial/by-id path of the board")
    parser.add_argument("--no-build", action="store_true", help="use the image already in build-ci/<board>-rep")
    parser.add_argument("--idf-export", type=Path, help="ESP-IDF v6.1 export.sh (default: $IDF_PATH or ~/.espressif)")
    parser.add_argument("--out", type=Path, help="report directory (default build-ci/e0/<board>-<UTC>)")
    parser.add_argument("--only", nargs="*", choices=("base", "followup", "stall"))
    return parser.parse_args(argv)


def main(argv=None, run=subprocess.run, by_id=m8.BY_ID, device_factory=run_e0.Device):
    args = parse_args(argv)
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    out = args.out or m8.ROOT / "build-ci" / "e0" / f"{args.board}-{stamp}"
    image, elf = image_paths(args.board)
    if not args.no_build:
        print(f"build: {TARGETS[args.board]} rep", flush=True)
        if run(build_command(args.board, args.idf_export), cwd=m8.ROOT).returncode != 0:
            print("FAIL: firmware build (see ci-reports/)")
            return 1
    if not image.is_file() or not elf.is_file():
        print(f"BLOCKED: no image in {image.parent}; run without --no-build")
        return 2
    try:
        path, serial = find_board(args.board, args.port, args.serial, by_id, device_factory)
    except Blocked as error:
        print(f"BLOCKED: {error}")
        return 2
    print(f"board {args.board}: {serial} ({path}); reports in {out}", flush=True)
    worst = 0
    for name, command in runner_commands(args.board, serial, out, image, elf):
        if args.only and name not in args.only:
            continue
        print(f"run: {name}", flush=True)
        code = run(command, cwd=m8.ROOT).returncode
        worst = max(worst, code)
        if name == "base" and code != 0:
            print("base run did not pass; follow-up runs skipped")
            break
    print({0: "PASS", 1: "FAIL", 2: "BLOCKED"}.get(worst, f"exit {worst}") + f": {out}")
    return worst


if __name__ == "__main__":
    sys.exit(main())
