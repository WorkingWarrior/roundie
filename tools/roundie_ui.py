#!/usr/bin/env python3
"""RE1 bounded UI diagnostics and explicit semantic injection (not touch proof)."""
import argparse
import json
import re
import secrets
import time
from roundie_time import DeviceError, TransportError, open_serial, read_serial_reply, write_command

FIELDS = ("generation view button widgets pending max_depth submitted applied failed rejected canceled "
          "input_accepted input_dropped input_rejected physical injected delivered latency_max_us "
          "enqueue_max_us create_max_us update_max_us teardown_us input_latency_max_us acquired "
          "released outstanding lvgl_bytes lvgl_peak flushes flush_errors last_widget text_hash "
          "foreground admitting pool_before pool_free realized_objects").split()


def request(port, operation="STATUS", timeout=4):
    if operation != "STATUS" and re.fullmatch(r"PRESS [1-9][0-9]{0,9} [1-9][0-9]{0,9}", operation) is None:
        raise DeviceError("invalid bounded UI diagnostic")
    identity = secrets.token_hex(4)
    prefix = f"!RE1 {identity} ".encode()
    write_command(port, f"!RE1 {identity} UI {operation}")
    line = read_serial_reply(port, prefix, time.monotonic() + timeout, 768)
    try:
        code, payload = line[len(prefix):].split(b" ", 1)
        if re.fullmatch(rb"-?[0-9]{1,10}", code) is None:
            raise ValueError("code")
        result = json.loads(payload)
        values = result["ui"]
        if len(values) != len(FIELDS) or any(type(v) is not int or not 0 <= v <= 0xffffffff for v in values):
            raise ValueError("counters")
        result = dict(zip(FIELDS, values), error=result["error"])
        if (type(result["error"]) is not int or result["widgets"] > 8 or result["pending"] > 12 or
                result["foreground"] > 1 or result["admitting"] > 1 or
                result["acquired"] != result["released"] + result["outstanding"]):
            raise ValueError("ownership")
    except (ValueError, KeyError, TypeError) as error:
        raise TransportError(f"malformed UI diagnostics: {error}") from error
    if int(code) != 0:
        raise DeviceError(f"UI rejected ({int(code)}): {result}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    press = sub.add_parser("press", help="development injection; does not prove CST816S touch")
    press.add_argument("generation", type=int)
    press.add_argument("widget", type=int)
    args = parser.parse_args()
    operation = "STATUS" if args.command == "status" else f"PRESS {args.generation} {args.widget}"
    with open_serial(args.port) as port:
        print(json.dumps(request(port, operation), indent=2))


if __name__ == "__main__":
    main()
