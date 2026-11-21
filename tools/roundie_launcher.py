#!/usr/bin/env python3
"""RP1 launcher diagnostics; SELECT/EXIT are INJECTED, never physical touch proof."""
import argparse
import json
import re
import secrets
import time
from roundie_time import DeviceError, TransportError, open_serial, read_serial_reply, write_command
from roundie_package import valid_id

STATES = ("WAITING", "REFRESHING", "LIST", "REQUESTED", "LOADING", "ACTIVE", "EXITING", "ERROR", "RESULT", "HOME")
TIMINGS = ("refresh", "copy", "enqueue_max", "admission", "started", "exit_admission",
           "return", "gui_create", "gui_latency_max")


def request(port, operation="NAV", timeout=4, allow_error=False):
    if operation not in ("NAV", "REFRESH", "OPEN"):
        match = re.fullmatch(r"SELECT ([0-9]{1,10}) (.+)|(?:EXIT|BACK) ([0-9]{1,10})", operation)
        if match is None:
            raise DeviceError("invalid launcher diagnostic")
        if match[1] is not None:
            valid_id(match[2])
        if int(match[1] or match[3]) > 0xffffffff:
            raise DeviceError("identity exceeds uint32")
    identity = secrets.token_hex(4)
    prefix = f"!RP1 {identity} ".encode()
    write_command(port, f"!RP1 {identity} {operation}")
    line = read_serial_reply(port, prefix, time.monotonic() + timeout, 1800)
    try:
        code, payload = line[len(prefix):].split(b" ", 1)
        if re.fullmatch(rb"-?[0-9]{1,10}", code) is None:
            raise ValueError("code")
        result = json.loads(payload)
        for field in ("nav", "revision", "request", "generation", "count", "foreground", "stack_min", "discovery_flushes"):
            if type(result[field]) is not int or not 0 <= result[field] <= 0xffffffff:
                raise ValueError(field)
        if result["nav"] >= len(STATES) or result["count"] > 8 or result["foreground"] > 1:
            raise ValueError("bounds")
        for field, count in (("pending", 2), ("input", 3), ("result", 3), ("us", 9), ("gui", 2)):
            values = result[field]
            if not isinstance(values, list) or len(values) != count or any(type(v) is not int for v in values):
                raise ValueError(field)
            if field != "result" and any(not 0 <= v <= 0xffffffff for v in values):
                raise ValueError(field)
        if any(v not in (0, 1) for v in result["pending"]):
            raise ValueError("pending")
        if "completion" in result:
            completion = result["completion"]
            if (not isinstance(completion, list) or len(completion) != 8 or
                    any(type(v) is not int for v in completion) or
                    any(completion[i] not in (0, 1) for i in (3, 4, 5)) or
                    any(not 0 <= completion[i] <= 0xffffffff for i in (0, 1, 6, 7)) or
                    not -(2 ** 31) <= completion[2] < 2 ** 31 or
                    not isinstance(result["name"], str) or len(result["name"].encode()) > 63):
                raise ValueError("completion")
        if "paint" in result:
            paint = result["paint"]
            if (not isinstance(paint, list) or len(paint) != 5 or
                    any(type(v) is not int or v < 0 for v in paint) or
                    any(v > 0xffffffff for v in paint[:4]) or paint[4] >= 2 ** 63):
                raise ValueError("paint")
        if result["selected"]:
            valid_id(result["selected"])
        elif not isinstance(result["selected"], str):
            raise ValueError("selected")
        result["state"] = STATES[result["nav"]]
        result["timings_us"] = dict(zip(TIMINGS, result["us"]))
        result["code"] = int(code)
    except (ValueError, TypeError, KeyError, DeviceError) as error:
        raise TransportError(f"malformed launcher diagnostics: {error}") from error
    if int(code) != 0 and not allow_error:
        raise DeviceError(f"launcher rejected ({int(code)}): {result}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    sub.add_parser("refresh")
    sub.add_parser("open", help="INJECTED Home to list (M10), as the swipe up")
    select = sub.add_parser("select", help="INJECTED semantic launcher selection")
    select.add_argument("revision", type=int)
    select.add_argument("app_id")
    exit_command = sub.add_parser("exit", help="INJECTED system Exit request")
    exit_command.add_argument("generation", type=int)
    back = sub.add_parser("back", help="INJECTED dismissal of a copied completion")
    back.add_argument("request", type=int)
    args = parser.parse_args()
    operation = {"status": "NAV", "refresh": "REFRESH", "open": "OPEN"}.get(args.command)
    if args.command == "select":
        operation = f"SELECT {args.revision} {args.app_id}"
    elif args.command == "exit":
        operation = f"EXIT {args.generation}"
    elif args.command == "back":
        operation = f"BACK {args.request}"
    with open_serial(args.port) as port:
        print(json.dumps(request(port, operation), indent=2))


if __name__ == "__main__":
    main()
