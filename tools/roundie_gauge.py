#!/usr/bin/env python3
"""RG1 gauge capacity diagnostics (1.85B BQ27220, development builds).

status reads only. capacity writes Design Capacity and Full Charge Capacity
(CEDV profile 1) into the gauge's RAM; a gauge power-on reset (battery
disconnected) restores the defaults. OTP is never programmed."""
import argparse
import json
import re
import secrets
import sys
import time

from roundie_time import DeviceError, TransportError, open_serial, read_serial_reply, write_command

UINT16_FIELDS = ("design_mah", "dm_full_mah", "dm_design_mah", "battery_flags", "operation_flags",
                 "voltage_mv", "soc", "remaining_mah", "full_mah")
STEPS = ("none", "precheck", "read-block", "full-access", "enter", "write", "exit", "verify")


def parse_reply(line, request_id):
    prefix = f"!RG1 {request_id} ".encode()
    if not line.startswith(prefix):
        return None
    try:
        if not line.endswith(b"\n") or len(line) > 512:
            raise ValueError("length/framing")
        code_text, payload = line[len(prefix):].split(b" ", 1)
        code = int(code_text)
        status = json.loads(payload)
        if status["format"] != 1:
            raise ValueError("invalid format")
        for key in UINT16_FIELDS:
            if type(status[key]) is not int or not 0 <= status[key] <= 0xFFFF:
                raise ValueError(f"invalid {key}")
        if type(status["step"]) is not int or not 0 <= status["step"] < len(STEPS):
            raise ValueError("invalid step")
        if type(status["current_ma"]) is not int or not -32768 <= status["current_ma"] <= 32767:
            raise ValueError("invalid current_ma")
        if type(status["state"]) is not int or not 0 <= status["state"] <= 15:
            raise ValueError("invalid state")
        if status["initialized"] not in (0, 1) or type(status["initialized"]) is not int:
            raise ValueError("invalid initialized")
        status["step_name"] = STEPS[status["step"]]
    except (ValueError, KeyError, TypeError) as error:
        raise TransportError(f"malformed gauge response: {error}") from error
    if code != 0:
        raise DeviceError(f"gauge rejected command ({code}) at step {status['step_name']}: {status}")
    return status


def request(port, operation, timeout=15.0):
    if re.fullmatch(r"STATUS|CAPACITY [0-9]{1,5}", operation) is None:
        raise DeviceError("invalid gauge command")
    request_id = secrets.token_hex(4)
    deadline = time.monotonic() + timeout
    write_command(port, f"!RG1 {request_id} {operation}")
    line = read_serial_reply(port, f"!RG1 {request_id} ".encode(), deadline, 512)
    return parse_reply(line, request_id)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status")
    capacity = commands.add_parser("capacity", help="RAM only, lost at gauge power-on reset")
    capacity.add_argument("mah", type=int)
    args = parser.parse_args(argv)
    if args.command == "capacity" and not 100 <= args.mah <= 6000:
        parser.error("capacity must be 100..6000 mAh")
    operation = "STATUS" if args.command == "status" else f"CAPACITY {args.mah}"
    try:
        with open_serial(args.port) as port:
            print(json.dumps(request(port, operation), indent=2))
        return 0
    except TransportError as error:
        print(f"Transport error: {error}", file=sys.stderr)
        return 2
    except DeviceError as error:
        print(f"Gauge error: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
