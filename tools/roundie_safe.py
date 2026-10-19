#!/usr/bin/env python3
"""Bounded RS1 recovery diagnostics; explicit mutations, never blind retries."""
import argparse
import json
import re
import secrets
import sys
import time

from roundie_time import DeviceError, TransportError, open_serial, read_serial_reply, write_command


def parse_reply(line, request_id):
    prefix = f"!RS1 {request_id} ".encode()
    if not line.startswith(prefix):
        return None
    try:
        if not line.endswith(b"\n") or len(line) > 1024:
            raise ValueError("length/framing")
        code_text, payload = line[len(prefix):].split(b" ", 1)
        code = int(code_text)
        status = json.loads(payload)
        for key in ("safe", "retained", "persistence", "skip_gui", "trial", "test_build", "restart"):
            if type(status[key]) is not int or status[key] not in (0, 1):
                raise ValueError(f"invalid {key}")
        for key in ("failures", "fault_remaining"):
            if type(status[key]) is not int or not 0 <= status[key] <= 3:
                raise ValueError(f"invalid {key}")
        for key in ("elf", "other_elf"):
            if status[key] and re.fullmatch(r"[0-9a-f]{64}", status[key]) is None:
                raise ValueError(f"invalid {key}")
        if status["running"] not in ("ota_0", "ota_1", ""):
            raise ValueError("invalid running slot")
        if status["reason"] not in ("none", "request", "boot_loop", "storage", "gui", "core", "persistence"):
            raise ValueError("invalid recovery reason")
    except (ValueError, KeyError, TypeError) as error:
        raise TransportError(f"malformed recovery response: {error}") from error
    if code != 0:
        raise DeviceError(f"recovery rejected command ({code}): {status}")
    return status


def request(port, operation, timeout=8.0):
    if len(operation) > 100 or re.fullmatch(r"[A-Za-z0-9_ ]+", operation) is None:
        raise DeviceError("invalid bounded recovery command")
    request_id = secrets.token_hex(4)
    deadline = time.monotonic() + timeout
    write_command(port, f"!RS1 {request_id} {operation}")
    line = read_serial_reply(port, f"!RS1 {request_id} ".encode(), deadline, 1024)
    return parse_reply(line, request_id)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("status", "request", "clear", "reboot", "slots"):
        commands.add_parser(name)
    select = commands.add_parser("select", help="select alternate verified VALID image; data compatibility is operator-owned")
    select.add_argument("slot", choices=("ota_0", "ota_1"))
    select.add_argument("--elf", required=True, help="explicit confirmation of the exact target ELF identity")
    fault = commands.add_parser("fault", help="development image only: arm exactly three incomplete VALID boots")
    fault.add_argument("--elf", required=True)
    args = parser.parse_args(argv)
    operation = {"status": "STATUS", "request": "REQUEST", "clear": "CLEAR", "reboot": "REBOOT", "slots": "SLOT STATUS"}.get(args.command)
    try:
        if args.command in ("select", "fault"):
            if re.fullmatch(r"[0-9a-f]{64}", args.elf) is None:
                raise DeviceError("--elf must be the exact 64-character lowercase identity")
            operation = f"SELECT {args.slot} {args.elf}" if args.command == "select" else f"FAULT {args.elf}"
        with open_serial(args.port) as port:
            print(json.dumps(request(port, operation), indent=2))
        return 0
    except TransportError as error:
        print(f"Transport error: {error}", file=sys.stderr)
        return 2
    except DeviceError as error:
        print(f"Recovery error: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
