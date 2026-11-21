#!/usr/bin/env python3
"""RG1 gauge capacity diagnostics (1.85B BQ27220, development builds).

status reads only. capacity initializes Design Capacity and Full Charge
Capacity (CEDV profile 1) in the gauge's RAM once: it writes nothing when
Design Capacity already has the value (the learned FCC is kept) and writes only
over the 3000 mAh reset default. A gauge power-on reset (battery disconnected)
restores the defaults. OTP is never programmed; no access keys are sent.

learn reads the capacity learning report (TRM 1.1.3: VDQ, EDV2, FCC update
and the inferred abort cause). learn-hold keeps the system load on (screen
and CPU awake) for one controlled cycle; it ends by itself at the FCC update,
an abort, the voltage floor, SYSDWN/FD, temperature, missing data or the
given time. learn-stop ends it. None of these writes the gauge."""
import argparse
import json
import re
import secrets
import sys
import time

from roundie_time import DeviceError, TransportError, open_serial, read_serial_reply, write_command

UINT16_FIELDS = ("design_mah", "dm_full_mah", "dm_design_mah", "gauging_config", "design_voltage_mv",
                 "charging_voltage_mv", "taper_current_ma", "near_full_mah", "battery_flags",
                 "operation_flags", "voltage_mv", "soc", "remaining_mah", "full_mah")
FORMAT3_FIELDS = ("battery_low_centi", "learning_low_temp", "overload_ma", "reserve_mah", "flag_config_a",
                  "flag_config_b")
STEPS = ("none", "precheck", "read-block", "enter", "write", "verify-cfg", "exit", "verify")
REPLY_MAX = 1024
LEARN_NAMES = {
    "phase": ("idle", "qualified", "edv2"),
    "result": ("none", "updated", "unchanged", "aborted"),
    "cause": ("none", "charge", "low-temp", "low-current", "overload", "gauge-reset", "config-update",
              "not-qualified", "unknown"),
    "hold": ("off", "running", "done", "aborted", "low-voltage", "sysdown", "hot", "no-data", "expired",
             "cancelled"),
}
LEARN_INTS = ("resumed", "old_full", "new_full", "result_s", "qualified", "updated", "unchanged", "aborted",
              "start_rm", "start_full", "min_temp", "edv2", "edv2_qualified", "edv2_mv", "edv2_ma", "edv2_temp",
              "edv2_rm", "dsg_samples", "dsg_avg_ma", "dsg_small_ma", "dsg_large_ma", "need_ma", "hold_s",
              "inhibitor", "stored", "stored_full", "stored_seq", "saves", "load", "low_temp", "overload", "op",
              "flags", "mv", "ma", "temp", "rm", "fcc", "dc")
OPERATIONS = r"STATUS|CAPACITY [0-9]{1,5}|LEARN|LEARN HOLD [1-9][0-9]{0,3}|LEARN STOP"


def parse_reply(line, request_id):
    prefix = f"!RG1 {request_id} ".encode()
    if not line.startswith(prefix):
        return None
    try:
        if not line.endswith(b"\n") or len(line) > REPLY_MAX:
            raise ValueError("length/framing")
        code_text, payload = line[len(prefix):].split(b" ", 1)
        code = int(code_text)
        status = json.loads(payload)
        if "learn" in status:
            parse_learn(status)
            if code != 0:
                raise DeviceError(f"gauge rejected learning command ({code}): {status}")
            return status
        if status["format"] not in (2, 3):
            raise ValueError("invalid format")
        for key in UINT16_FIELDS + (FORMAT3_FIELDS if status["format"] == 3 else ()):
            if type(status[key]) is not int or not 0 <= status[key] <= 0xFFFF:
                raise ValueError(f"invalid {key}")
        if type(status["step"]) is not int or not 0 <= status["step"] < len(STEPS):
            raise ValueError("invalid step")
        if type(status["current_ma"]) is not int or not -32768 <= status["current_ma"] <= 32767:
            raise ValueError("invalid current_ma")
        if type(status["state"]) is not int or not 0 <= status["state"] <= 15:
            raise ValueError("invalid state")
        for key in ("initialized", "written"):
            if type(status[key]) is not int or status[key] not in (0, 1):
                raise ValueError(f"invalid {key}")
        status["step_name"] = STEPS[status["step"]]
    except (ValueError, KeyError, TypeError) as error:
        raise TransportError(f"malformed gauge response: {error}") from error
    if code != 0:
        raise DeviceError(f"gauge rejected command ({code}) at step {status['step_name']}: {status}")
    return status


def parse_learn(status):
    if status["learn"] != 1:
        raise ValueError("invalid learn format")
    for key, names in LEARN_NAMES.items():
        if status[key] not in names:
            raise ValueError(f"invalid {key}")
    for key in LEARN_INTS:
        if type(status[key]) is not int:
            raise ValueError(f"invalid {key}")


def request(port, operation, timeout=15.0):
    if re.fullmatch(OPERATIONS, operation) is None:
        raise DeviceError("invalid gauge command")
    request_id = secrets.token_hex(4)
    deadline = time.monotonic() + timeout
    write_command(port, f"!RG1 {request_id} {operation}")
    line = read_serial_reply(port, f"!RG1 {request_id} ".encode(), deadline, REPLY_MAX)
    return parse_reply(line, request_id)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status")
    capacity = commands.add_parser("capacity", help="one-time RAM initialization, lost at gauge power-on reset")
    capacity.add_argument("mah", type=int)
    commands.add_parser("learn", help="capacity learning report (read-only)")
    hold = commands.add_parser("learn-hold", help="hold the system load for one controlled cycle")
    hold.add_argument("minutes", type=int)
    commands.add_parser("learn-stop", help="end the load hold")
    args = parser.parse_args(argv)
    if args.command == "capacity" and not 100 <= args.mah <= 6000:
        parser.error("capacity must be 100..6000 mAh")
    if args.command == "learn-hold" and not 1 <= args.minutes <= 1440:
        parser.error("minutes must be 1..1440")
    operation = {
        "status": "STATUS",
        "learn": "LEARN",
        "learn-stop": "LEARN STOP",
    }.get(args.command)
    if args.command == "capacity":
        operation = f"CAPACITY {args.mah}"
    elif args.command == "learn-hold":
        operation = f"LEARN HOLD {args.minutes}"
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
