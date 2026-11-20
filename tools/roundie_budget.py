#!/usr/bin/env python3
"""RE1 BUDGET: read-only callback budget and mailbox counters (M9).

The reply holds the counters of the current application instance and the
totals since boot. Firmware before M9 answers the unknown command with a
STATUS frame and no "budget" key; request() then raises Unsupported.
"""
import argparse
import json
import re
import secrets
import time
from roundie_time import DeviceError, TransportError, open_serial, read_serial_reply, write_command

FIELDS = ("instance callbacks under_1ms under_10ms under_80ms near over callback_max_us "
          "latency_max_us queue_peak dropped coalesced rejected discarded").split()
BUCKETS = ("under_1ms", "under_10ms", "under_80ms", "near", "over")
QUEUE_CAPACITY = 8  # ROUNDIE_ELF_EVENT_CAPACITY


class Unsupported(DeviceError):
    """The firmware has no BUDGET command."""


def parse(values):
    if len(values) != len(FIELDS) or any(type(v) is not int or not 0 <= v <= 0xffffffff for v in values):
        raise ValueError("counters")
    counters = dict(zip(FIELDS, values))
    if counters["queue_peak"] > QUEUE_CAPACITY:
        raise ValueError("queue peak")
    return counters


def request(port, operation="STATUS", timeout=4):
    if operation != "STATUS":
        raise DeviceError("BUDGET is read-only")
    identity = secrets.token_hex(4)
    prefix = f"!RE1 {identity} ".encode()
    write_command(port, f"!RE1 {identity} BUDGET")
    # 2048: firmware before M9 answers with a full STATUS frame.
    line = read_serial_reply(port, prefix, time.monotonic() + timeout, 2048)
    try:
        code, payload = line[len(prefix):].split(b" ", 1)
        if re.fullmatch(rb"-?[0-9]{1,10}", code) is None:
            raise ValueError("code")
        result = json.loads(payload)
        if "budget" not in result:
            raise Unsupported("firmware has no RE1 BUDGET")
        counters = {"instance": parse(result["budget"]), "boot": parse(result["boot"])}
    except (ValueError, KeyError, TypeError) as error:
        raise TransportError(f"malformed budget diagnostics: {error}") from error
    if int(code) != 0:
        raise DeviceError(f"BUDGET rejected ({int(code)})")
    return counters


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    args = parser.parse_args()
    with open_serial(args.port) as port:
        print(json.dumps(request(port), indent=2))


if __name__ == "__main__":
    main()
