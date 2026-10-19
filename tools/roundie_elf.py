#!/usr/bin/env python3
"""RE1: trusted development ELF, fixed LittleFS file, explicit execution only."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets
import subprocess
import sys
import tempfile
import time

from roundie_time import DeviceError, TransportError, open_serial, read_serial_reply, write_command

ROOT = Path(__file__).resolve().parents[1]
MAX_FILE = 65536
HEX64 = re.compile(r"[0-9a-f]{64}\Z")


def inspect(path):
    path = Path(path).resolve()
    if not 52 <= path.stat().st_size <= MAX_FILE:
        raise DeviceError("ELF size must be 52..65536 bytes")
    with tempfile.TemporaryDirectory(prefix="rep-elf-check-") as temporary:
        checker = str(Path(temporary) / "validate")
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "include"),
                        "-I" + str(ROOT / "components/roundie_elf/include"),
                        str(ROOT / "tools/validate_elf.c"),
                        str(ROOT / "components/roundie_elf/validate.c"), "-o", checker], check=True)
        checked = subprocess.run([checker, str(path)], capture_output=True, text=True)
        if checked.returncode:
            raise DeviceError(checked.stderr.strip())
        info = json.loads(checked.stdout)
    info["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    return info


def parse_reply(line, request_id):
    prefix = f"!RE1 {request_id} ".encode()
    if not line.startswith(prefix):
        return None
    try:
        if len(line) > 1400 or not line.endswith(b"\n"):
            raise ValueError("length/framing")
        code, raw = line[len(prefix):].split(b" ", 1)
        if re.fullmatch(rb"-?[0-9]{1,10}", code) is None:
            raise ValueError("error code")
        status = json.loads(raw)
        for key in ("allowed", "busy", "executed", "over_budget"):
            if type(status[key]) is not int or status[key] not in (0, 1):
                raise ValueError(key)
        for key in ("before", "relocated", "during", "after"):
            if not isinstance(status[key], list) or len(status[key]) != 5 or any(
                    type(value) is not int or not 0 <= value <= 0xffffffff for value in status[key]):
                raise ValueError(key)
        for key in ("received", "total", "instance", "completed", "state", "code", "data", "alloc",
                    "alloc_peak", "calls", "load_us", "execute_us", "unload_us"):
            if type(status[key]) is not int or not 0 <= status[key] <= 0xffffffff:
                raise ValueError(key)
        if not 0 <= status["received"] <= status["total"] <= MAX_FILE or status["state"] > 5:
            raise ValueError("progress/state")
        if re.fullmatch(r"[0-9a-f]{8}", status["session"]) is None or status["abi"] != 1 or status["loader"] != "1.3.5":
            raise ValueError("session/ABI/loader")
        if any(type(status[key]) is not int for key in ("loader_result", "app_result")) or not isinstance(status["error"], str):
            raise ValueError("result/error")
    except (ValueError, TypeError, KeyError) as error:
        raise TransportError(f"malformed ELF response: {error}") from error
    if int(code) != 0:
        raise DeviceError(f"ELF rejected ({int(code)}): {status}")
    return status


def request(port, operation, timeout=8):
    request_id = secrets.token_hex(4)
    deadline = time.monotonic() + timeout
    write_command(port, f"!RE1 {request_id} {operation}")
    return parse_reply(read_serial_reply(port, f"!RE1 {request_id} ".encode(), deadline, 1400), request_id)


def upload(port, path, info):
    before = request(port, "STATUS")
    if not before["allowed"] or before["busy"]:
        raise DeviceError("ELF upload requires normal/STABLE, confirmed idle OTA, mounted storage and idle ELF")
    session = secrets.token_hex(4)
    while session == "00000000":
        session = secrets.token_hex(4)
    begun = False
    try:
        begun = True
        request(port, f"BEGIN {session} {info['size']} {info['sha256']}")
        with Path(path).open("rb") as stream:
            offset = 0
            while offset < info["size"]:
                data = stream.read(min(512, info["size"] - offset))
                if not data:
                    raise DeviceError("ELF changed during transfer")
                request(port, f"DATA {session} {offset} {data.hex()}")
                offset += len(data)
            if stream.read(1):
                raise DeviceError("ELF changed during transfer")
        result = request(port, f"END {session}")
        begun = False
        return result
    finally:
        if begun:
            try:
                request(port, f"ABORT {session}", timeout=1)
            except (DeviceError, TransportError):
                pass


def run(port, digest):
    if HEX64.fullmatch(digest) is None:
        raise DeviceError("provide exact ELF file SHA-256")
    submitted = request(port, f"RUN {digest}")
    instance = submitted["instance"]
    deadline = time.monotonic() + 4
    while True:
        result = request(port, "STATUS", timeout=1)
        if result["instance"] != instance:
            raise DeviceError("application identity changed")
        if result["state"] == 0:
            if not result["executed"] or result["error"] or result["over_budget"]:
                raise DeviceError(f"application failed: {result}")
            return result
        if time.monotonic() >= deadline:
            raise DeviceError("application still active; no forced unload attempted")
        time.sleep(0.05)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status")
    for name in ("inspect", "upload"):
        commands.add_parser(name).add_argument("file", type=Path)
    commands.add_parser("run").add_argument("--sha256", required=True)
    commands.add_parser("abort").add_argument("--session", required=True)
    args = parser.parse_args(argv)
    try:
        info = inspect(args.file) if args.command in ("inspect", "upload") else None
        if args.command == "inspect":
            print(json.dumps(info, indent=2))
            return 0
        if args.port is None:
            parser.error("--port is required for device commands")
        with open_serial(args.port) as port:
            if args.command == "upload":
                result = upload(port, args.file, info)
                result["artifact"] = info
            elif args.command == "run":
                result = run(port, args.sha256)
            elif args.command == "abort":
                if re.fullmatch(r"[0-9a-f]{8}", args.session) is None or args.session == "00000000":
                    raise DeviceError("invalid session")
                result = request(port, f"ABORT {args.session}")
            else:
                result = request(port, "STATUS")
        print(json.dumps(result, indent=2))
        return 0
    except TransportError as error:
        print(f"Transport error: {error}", file=sys.stderr)
        return 2
    except (DeviceError, OSError, subprocess.CalledProcessError) as error:
        print(f"ELF error: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
