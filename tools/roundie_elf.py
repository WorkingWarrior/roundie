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

from roundie_time import DeviceBusy, DeviceError, TransportError, open_serial, read_serial_reply, write_command

ROOT = Path(__file__).resolve().parents[1]
MAX_FILE = 65536
HEX64 = re.compile(r"[0-9a-f]{64}\Z")


TARGETS = ("esp32s3.rep1", "linux-x64.rep1")


def checker_sources(target):
    """Compiler arguments selecting the target's validator (docs/native-apps.md, N2)."""
    if target not in TARGETS:
        raise DeviceError(f"target must be one of {', '.join(TARGETS)}")
    return ((["-DROUNDIE_TARGET_LINUX_X64=1"] if target == "linux-x64.rep1" else []) +
            [str(ROOT / "components/roundie_elf/validate.c"),
             str(ROOT / "components/roundie_elf/validate_x86_64.c")])


def inspect(path, target="esp32s3.rep1"):
    path = Path(path).resolve()
    if not 52 <= path.stat().st_size <= MAX_FILE:
        raise DeviceError("ELF size must be 52..65536 bytes")
    with tempfile.TemporaryDirectory(prefix="rep-elf-check-") as temporary:
        checker = str(Path(temporary) / "validate")
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "include"),
                        "-I" + str(ROOT / "components/roundie_elf/include"),
                        str(ROOT / "tools/validate_elf.c"),
                        *checker_sources(target), "-o", checker], check=True)
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
        if len(line) > 2048 or not line.endswith(b"\n"):
            raise ValueError("length/framing")
        code, raw = line[len(prefix):].split(b" ", 1)
        if re.fullmatch(rb"-?[0-9]{1,10}", code) is None:
            raise ValueError("error code")
        status = json.loads(raw)
        if status == {"format": 1, "error": "busy"}:
            raise DeviceBusy("device busy: io_worker queue full, retry later")
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
        if not 0 <= status["received"] <= status["total"] <= MAX_FILE or status["state"] > 11:
            raise ValueError("progress/state")
        if re.fullmatch(r"[0-9a-f]{8}", status["session"]) is None or status["abi"] not in (0, 1, 2, 3) or status["loader"] != "1.3.5":
            raise ValueError("session/ABI/loader")
        if any(type(status[key]) is not int for key in ("loader_result", "app_result")) or not isinstance(status["error"], str):
            raise ValueError("result/error")
        if "resources" in status:
            resources = status["resources"]
            control = status["control"]
            if (not isinstance(resources, list) or len(resources) != 4 or
                    any(type(v) is not int or not 0 <= v <= 0xffffffff for v in resources) or
                    resources[0] != sum(resources[1:]) or
                    not isinstance(control, list) or len(control) != 3 or
                    any(type(v) is not int for v in control) or not 0 <= control[1] <= control[0] <= 0xffffffff):
                raise ValueError("resource/control accounting")
            for key in ("callbacks", "callback_max_us", "init_us", "suspend_us", "resume_us", "shutdown_us",
                        "events", "ticks", "users", "value", "latency_max_us", "queue", "dropped",
                        "coalesced", "rejected", "timer", "subscriptions"):
                if type(status[key]) is not int or not 0 <= status[key] <= 0xffffffff:
                    raise ValueError(key)
            ui_events = status.get("ui_events", 0)
            if type(ui_events) is not int or not 0 <= ui_events <= 0xffffffff:
                raise ValueError("ui event accounting")
            # events also counts types without their own counter, such as
            # REP3_EVENT_CLOCK, so it bounds the per-type counters from above.
            if status["queue"] > 8 or status["subscriptions"] > 1 or status["events"] < status["ticks"] + status["users"] + ui_events:
                raise ValueError("event accounting")
    except (ValueError, TypeError, KeyError) as error:
        raise TransportError(f"malformed ELF response: {error}") from error
    if int(code) != 0:
        raise DeviceError(f"ELF rejected ({int(code)}): {status}")
    return status


def request(port, operation, timeout=8):
    request_id = secrets.token_hex(4)
    deadline = time.monotonic() + timeout
    write_command(port, f"!RE1 {request_id} {operation}")
    return parse_reply(read_serial_reply(port, f"!RE1 {request_id} ".encode(), deadline, 2048), request_id)


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


def wait_state(port, instance, state, control=None, timeout=4):
    deadline = time.monotonic() + timeout
    while True:
        result = request(port, "STATUS", timeout=1)
        if result["instance"] != instance:
            raise DeviceError("application identity changed")
        complete = control is None or result["control"][1] == control
        if complete and result["state"] == state and (state != 0 or not result["busy"]):
            if result["error"] or result["over_budget"] or (control is not None and result["control"][2] != 0):
                raise DeviceError(f"application failed: {result}")
            return result
        if result["state"] == 0 and state != 0:
            raise DeviceError(f"application unloaded before requested state: {result}")
        if time.monotonic() >= deadline:
            raise DeviceError("lifecycle still pending; no forced unload or mutation retry attempted")
        time.sleep(0.02)


def start(port, digest):
    if HEX64.fullmatch(digest) is None:
        raise DeviceError("provide exact ELF file SHA-256")
    submitted = request(port, f"START {digest}")
    return wait_state(port, submitted["instance"], 3)


def control(port, operation, instance):
    if operation not in ("SUSPEND", "RESUME", "STOP", "UNLOAD") or not 1 <= instance <= 0xffffffff:
        raise DeviceError("invalid lifecycle operation/instance")
    submitted = request(port, f"{operation} {instance}")
    state = dict(SUSPEND=9, RESUME=3, STOP=11, UNLOAD=0)[operation]
    return wait_state(port, instance, state, submitted["control"][0])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status")
    commands.add_parser("resources")
    for name in ("inspect", "upload"):
        commands.add_parser(name).add_argument("file", type=Path)
    commands.add_parser("run").add_argument("--sha256", required=True)
    commands.add_parser("start").add_argument("--sha256", required=True)
    for operation in ("suspend", "resume", "stop", "unload", "event"):
        command = commands.add_parser(operation)
        command.add_argument("--instance", type=int, required=True)
        if operation == "event":
            command.add_argument("--value", type=int, required=True)
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
            elif args.command == "start":
                result = start(port, args.sha256)
            elif args.command in ("suspend", "resume", "stop", "unload"):
                result = control(port, args.command.upper(), args.instance)
            elif args.command == "event":
                if not 1 <= args.instance <= 0xffffffff or not 0 <= args.value <= 0xffffffff:
                    raise DeviceError("invalid event instance/value")
                result = request(port, f"EVENT {args.instance} {args.value}")
            elif args.command == "abort":
                if re.fullmatch(r"[0-9a-f]{8}", args.session) is None or args.session == "00000000":
                    raise DeviceError("invalid session")
                result = request(port, f"ABORT {args.session}")
            else:
                result = request(port, "RESOURCES" if args.command == "resources" else "STATUS")
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
