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
          "foreground admitting pool_before pool_free realized_objects values").split()
# Refusal causes (bridge diagnostics, firmware after 0.3.86); they sum to rejected.
REJECT_FIELDS = "rejected_full rejected_admission rejected_request reject_reason reject_depth reject_pump_age_us".split()
REJECT_REASONS = {0: "none", 1: "full", 2: "closed", 3: "transition", 4: "error", 5: "request"}
OPERATION = re.compile(r"PRESS [1-9][0-9]{0,9} [1-9][0-9]{0,9}|VALUE [1-9][0-9]{0,9} [1-9][0-9]{0,9} -?[0-9]{1,7}")


def request(port, operation="STATUS", timeout=4):
    if operation != "STATUS" and OPERATION.fullmatch(operation) is None:
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
        # Firmware before Round UI reports every field except the last; firmware
        # up to 0.3.86 has no refusal causes.
        if len(values) not in (len(FIELDS) - 1, len(FIELDS), len(FIELDS) + len(REJECT_FIELDS)) or any(type(v) is not int or not 0 <= v <= 0xffffffff for v in values):
            raise ValueError("counters")
        result = dict(zip(FIELDS + REJECT_FIELDS, values), error=result["error"])
        if "reject_reason" in result:
            if (result["reject_reason"] not in REJECT_REASONS or result["reject_depth"] > 12 or
                    result["rejected_full"] + result["rejected_admission"] + result["rejected_request"] !=
                    result["rejected"]):
                raise ValueError("refusals")
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
    value = sub.add_parser("value", help="development injection of a round widget value, applied by the GUI")
    value.add_argument("generation", type=int)
    value.add_argument("widget", type=int)
    value.add_argument("value", type=int)
    args = parser.parse_args()
    if args.command == "status":
        operation = "STATUS"
    elif args.command == "press":
        operation = f"PRESS {args.generation} {args.widget}"
    else:
        operation = f"VALUE {args.generation} {args.widget} {args.value}"
    with open_serial(args.port) as port:
        print(json.dumps(request(port, operation), indent=2))


if __name__ == "__main__":
    main()
