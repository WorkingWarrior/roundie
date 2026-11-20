#!/usr/bin/env python3
"""Bounded RW1 power diagnostics: status, screen off/on, timed light sleep and,
since M8-B, diagnostics inhibitors and the (refused) deep-sleep request."""
import argparse
import json
import re
import secrets
import sys
import time

from roundie_time import DeviceError, TransportError, open_serial, read_serial_reply, write_command

STATES = ("active", "screen_off", "light_sleep")
PHASES = ("idle", "requested", "park", "parked", "resumed")
REFUSALS = ("none", "safe_mode", "not_stable", "ota", "app", "wake_source", "inhibited", "busy", "unsupported")
PM_STATES = ("ACTIVE", "SCREEN_OFF", "LIGHT_SLEEP_PENDING", "LIGHT_SLEEP", "RESUMING", "DEEP_SLEEP_PENDING",
             "DEEP_SLEEP")
GUARDS = ("pending", "armed", "failed")
FRAME = 1024
# M8-B fields; a reply without them comes from older firmware.
COUNTERS = ("pm_age_ms", "transitions", "rejected", "attempts", "refused", "light_sleeps", "user_wakes",
            "timer_wakes", "sleep_errors", "t_active_ms", "t_screen_off_ms", "t_light_sleep_ms", "t_pending_ms",
            "t_resuming_ms", "suspend_ms", "suspend_max_ms", "resume_ms", "resume_max_ms", "inhibitors",
            "inhibit_kinds", "inhibit_owners", "inhibit_stale", "inhibit_expired", "inhibit_full", "guard_pending",
            "guard_checks", "guard_failures", "guard_lost")


def parse_reply(line, request_id):
    prefix = f"!RW1 {request_id} ".encode()
    if not line.startswith(prefix):
        return None
    try:
        if not line.endswith(b"\n") or len(line) > FRAME:
            raise ValueError("length/framing")
        code_text, payload = line[len(prefix):].split(b" ", 1)
        code = int(code_text)
        status = json.loads(payload)
        if status["state"] not in STATES or status["phase"] not in PHASES:
            raise ValueError("invalid state")
        if status["refusal"] not in REFUSALS or status["bound"] not in (0, 1):
            raise ValueError("invalid refusal")
        for key in ("sleeps", "last_sleep_ms", "wake_causes", "screen_offs", "timeout_s", "safety_s"):
            if type(status[key]) is not int or status[key] < 0:
                raise ValueError(f"invalid {key}")
        if "pm" in status:
            if status["pm"] not in PM_STATES or status["pm_from"] not in PM_STATES:
                raise ValueError("invalid pm state")
            if status["last_refusal"] not in REFUSALS or status["admission"] not in REFUSALS:
                raise ValueError("invalid pm refusal")
            if status["guard"] not in GUARDS or re.fullmatch(r"[0-9a-f]{8}", status["handle"]) is None:
                raise ValueError("invalid guard or handle")
            for key in COUNTERS:
                if type(status[key]) is not int or status[key] < 0:
                    raise ValueError(f"invalid {key}")
    except (ValueError, KeyError, TypeError) as error:
        raise TransportError(f"malformed power response: {error}") from error
    if code != 0:
        raise DeviceError(f"power command rejected ({code}): {status}")
    return status


def request(port, operation, timeout=4.0):
    if len(operation) > 40 or re.fullmatch(r"[A-Z0-9 ]+", operation) is None:
        raise DeviceError("invalid bounded power command")
    request_id = secrets.token_hex(4)
    deadline = time.monotonic() + timeout
    write_command(port, f"!RW1 {request_id} {operation}")
    line = read_serial_reply(port, f"!RW1 {request_id} ".encode(), deadline, FRAME)
    return parse_reply(line, request_id)


def request_any(port, operation, timeout=4.0):
    """As request(), but returns (code, status) for an expected refusal."""
    try:
        return 0, request(port, operation, timeout)
    except DeviceError as error:
        match = re.match(r"power command rejected \((-?\d+)\): (.*)$", str(error))
        if match is None:
            raise
        return int(match.group(1)), None


def status_after_wake(port_name, timeout):
    """USB Serial/JTAG disappears during light sleep; reconnect until it answers."""
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            with open_serial(port_name) as port:
                status = request(port, "STATUS")
                if status["phase"] == "idle" and status["state"] != "light_sleep":
                    return status
        except (OSError, TransportError) as error:
            last_error = error
        time.sleep(0.5)
    raise TransportError(f"no idle power status before deadline: {last_error}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status")
    commands.add_parser("screen-off")
    commands.add_parser("screen-on")
    commands.add_parser("storage", help="restart into USB storage mode, as the power menu item does")
    commands.add_parser("deep-sleep", help="M8-B: recorded and refused (unsupported), never executed")
    inhibit = commands.add_parser("inhibit", help="diagnostics inhibitor: KINDS 1..7, SECONDS 1..600")
    inhibit.add_argument("kinds", type=int)
    inhibit.add_argument("seconds", type=int)
    release = commands.add_parser("release", help="release a diagnostics inhibitor by its handle")
    release.add_argument("handle")
    sleep = commands.add_parser("sleep", help="timed light sleep; the board returns over USB after wake")
    sleep.add_argument("seconds", type=int)
    args = parser.parse_args(argv)
    try:
        if args.command == "sleep":
            if not 1 <= args.seconds <= 3600:
                raise DeviceError("seconds must be 1..3600")
            with open_serial(args.port) as port:
                before = request(port, f"SLEEP {args.seconds}")
            started = time.monotonic()
            time.sleep(1.0)
            after = status_after_wake(args.port, args.seconds + 30)
            print(json.dumps({"before": before, "after": after,
                              "host_elapsed_s": round(time.monotonic() - started, 2)}, indent=2))
            if after["sleeps"] != before["sleeps"] + 1:
                print(f"Sleep was not entered: refusal={after['refusal']}", file=sys.stderr)
                return 1
            return 0
        if args.command == "inhibit":
            operation = f"INHIBIT {args.kinds} {args.seconds}"
        elif args.command == "release":
            if re.fullmatch(r"[0-9a-f]{8}", args.handle) is None:
                raise DeviceError("handle: 8 lowercase hex digits")
            operation = f"RELEASE {args.handle.upper()}"
        else:
            operation = {"status": "STATUS", "screen-off": "SCREEN OFF", "screen-on": "SCREEN ON",
                         "storage": "STORAGE", "deep-sleep": "DEEPSLEEP"}[args.command]
        with open_serial(args.port) as port:
            print(json.dumps(request(port, operation), indent=2))
        return 0
    except TransportError as error:
        print(f"Transport error: {error}", file=sys.stderr)
        return 2
    except DeviceError as error:
        print(f"Device error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
