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
given time; at EDV2 it releases the load at once. Every end except a plain
expiry or learn-stop shows an alert on the device and makes the firmware
light-sleep between samples until the battery is seen charging. learn-stop
ends it. learn-end reports that end (EDV2 snapshot, result, protection). An
MCU reset leaves the hold off: nothing resumes. learn-log reads the RAM telemetry ring (one
entry per 5 s gauge sample, including time without USB) after the cable is
reconnected and summarizes the samples taken without a USB host. None of these
writes the gauge."""
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
             "cancelled", "edv2", "cold", "no-result", "test-end", "test-stop"),
}
END_NAMES = {
    "hold": LEARN_NAMES["hold"],
    "alert": ("none", "near-end", "finished", "stopped"),
    "hold_result": LEARN_NAMES["result"],
}
END_INTS = ("learned", "protect", "warned", "warned_s", "ended_s", "sleep_req", "sleep_ok", "snap", "snap_s",
            "snap_mv", "snap_ma", "snap_avg_ma", "snap_temp", "snap_rm", "snap_fcc", "snap_op", "snap_flags",
            "avg_ma", "simulated", "test")
LEARN_INTS = ("resumed", "old_full", "new_full", "result_s", "qualified", "updated", "unchanged", "aborted",
              "start_rm", "start_full", "min_temp", "edv2", "edv2_qualified", "edv2_mv", "edv2_ma", "edv2_temp",
              "edv2_rm", "dsg_samples", "dsg_avg_ma", "dsg_small_ma", "dsg_large_ma", "need_ma", "hold_s",
              "inhibitor", "stored", "stored_full", "stored_seq", "saves", "load", "low_temp", "overload", "op",
              "flags", "mv", "ma", "temp", "rm", "fcc", "dc")
OPERATIONS = (r"STATUS|CAPACITY [0-9]{1,5}|LEARN|LEARN HOLD [1-9][0-9]{0,3}|LEARN STOP|LEARN LOG [0-9]{1,10}"
              r"|LEARN END|LEARN TEST (END|STOP) [0-9]{1,4}")
LOG_FIELDS = ("seq", "time_ms", "mv", "ma", "temp", "rm", "fcc", "op", "flags", "hold", "link", "avg_ma")
LOG_VALID = 0x01
LOG_USB = 0x02
LOG_INHIBITOR = 0x04
LOG_WAKE = 0x08
OP_VDQ = 0x0010
OP_EDV2 = 0x0008
REQUIRED_MA = 70


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
        if "log" in status:
            if code != 0:
                raise DeviceError(f"gauge rejected log command ({code}): {status}")
            return parse_log(status)
        if "end" in status:
            parse_end(status)
            if code != 0:
                raise DeviceError(f"gauge rejected end command ({code}): {status}")
            return status
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


def parse_end(status):
    if status["end"] != 1:
        raise ValueError("invalid end format")
    for key, names in END_NAMES.items():
        if status[key] not in names:
            raise ValueError(f"invalid {key}")
    for key in END_INTS:
        if type(status[key]) is not int:
            raise ValueError(f"invalid {key}")
    if status["learned"] == 1 and (status["hold_result"] != "updated" or status["simulated"] != 0):
        raise ValueError("learned without a real updated result")


def parse_log(status):
    if status["log"] != 1:
        raise ValueError("invalid log format")
    for key in ("usb_monitor", "total", "first", "next"):
        if type(status[key]) is not int or status[key] < 0:
            raise ValueError(f"invalid {key}")
    entries = []
    for raw in status["e"]:
        if type(raw) is not list or len(raw) != len(LOG_FIELDS) or any(type(v) is not int for v in raw):
            raise ValueError("invalid log entry")
        entry = dict(zip(LOG_FIELDS, raw))
        entry["hold"] = LEARN_NAMES["hold"][entry["hold"]] if entry["hold"] < len(LEARN_NAMES["hold"]) else "?"
        entries.append(entry)
    status["e"] = entries
    return status


def summarize(values):
    if not values:
        return None
    return {"n": len(values), "avg": round(sum(values) / len(values), 1), "min": min(values), "max": max(values)}


def summarize_log(entries):
    """Statistics of the valid samples taken without a USB host (battery
    only), split by whether the load hold held its inhibitor."""
    battery = [e for e in entries if e["link"] & LOG_VALID and not e["link"] & LOG_USB]
    report = {"entries": len(entries), "battery_samples": len(battery),
              "usb_samples": sum(1 for e in entries if e["link"] & LOG_USB)}
    for name, group in (("hold", [e for e in battery if e["link"] & LOG_INHIBITOR]),
                        ("no_hold", [e for e in battery if not e["link"] & LOG_INHIBITOR])):
        discharge = [-e["ma"] for e in group]
        report[name] = {
            "discharge_ma": summarize(discharge),
            "at_least_required": sum(1 for v in discharge if v >= REQUIRED_MA),
            "mv": summarize([e["mv"] for e in group]),
            "temp_deci_c": summarize([e["temp"] for e in group]),
            "rm_first": group[0]["rm"] if group else None,
            "rm_last": group[-1]["rm"] if group else None,
            "seconds": round((group[-1]["time_ms"] - group[0]["time_ms"]) / 1000, 1) if group else 0,
        }
    # First samples after a light sleep: the gauge's Current() (1 s
    # conversion) and AverageCurrent() (IIR, about 14.5 s) mostly cover the
    # sleep. An estimate, not a meter reading (docs/bringup.md).
    woke = [e for e in battery if e["link"] & LOG_WAKE]
    report["after_wake"] = {"n": len(woke), "current_ma": summarize([-e["ma"] for e in woke]),
                            "average_ma": summarize([-e["avg_ma"] for e in woke])}
    # Mean discharge over the whole battery-only span from the gauge's RM
    # (1 mAh resolution): sleeps included.
    span_s = (battery[-1]["time_ms"] - battery[0]["time_ms"]) / 1000 if battery else 0
    report["battery_span"] = {"seconds": round(span_s, 1),
                              "rm_drop_mah": battery[0]["rm"] - battery[-1]["rm"] if battery else 0,
                              "mean_ma_from_rm": round((battery[0]["rm"] - battery[-1]["rm"]) * 3600 / span_s, 1)
                              if span_s >= 600 else None}
    report["required_ma"] = REQUIRED_MA
    report["vdq_seen"] = any(e["op"] & OP_VDQ for e in battery)
    report["edv2_seen"] = any(e["op"] & OP_EDV2 for e in battery)
    report["fcc_values"] = sorted({e["fcc"] for e in battery})
    report["hold_states"] = sorted({e["hold"] for e in entries})
    report["gaps_ms_max"] = max((b["time_ms"] - a["time_ms"] for a, b in zip(entries, entries[1:])), default=0)
    return report


def read_log(port):
    entries = []
    sequence = 0
    while True:
        status = request(port, f"LEARN LOG {sequence}")
        if status["first"] > sequence and sequence != 0:
            raise DeviceError(f"telemetry overwritten while reading: first {status['first']} > {sequence}")
        entries.extend(status["e"])
        if status["next"] >= status["total"] or not status["e"]:
            return status, entries
        sequence = status["next"]


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
    commands.add_parser("learn-log", help="read the RAM telemetry ring and summarize battery-only samples")
    commands.add_parser("learn-end", help="end of the load hold: EDV2 snapshot, result, protection (read-only)")
    test = commands.add_parser("learn-test", help="development: simulated warning after DELAY s, simulated end "
                                                  "60 s later, on a running hold; never written, never learned")
    test.add_argument("kind", choices=("end", "stop"))
    test.add_argument("delay", type=int)
    args = parser.parse_args(argv)
    if args.command == "capacity" and not 100 <= args.mah <= 6000:
        parser.error("capacity must be 100..6000 mAh")
    if args.command == "learn-hold" and not 1 <= args.minutes <= 1440:
        parser.error("minutes must be 1..1440")
    if args.command == "learn-test" and not 0 <= args.delay <= 3600:
        parser.error("delay must be 0..3600")
    operation = {
        "status": "STATUS",
        "learn": "LEARN",
        "learn-stop": "LEARN STOP",
        "learn-end": "LEARN END",
    }.get(args.command)
    if args.command == "capacity":
        operation = f"CAPACITY {args.mah}"
    elif args.command == "learn-hold":
        operation = f"LEARN HOLD {args.minutes}"
    elif args.command == "learn-test":
        operation = f"LEARN TEST {args.kind.upper()} {args.delay}"
    try:
        with open_serial(args.port) as port:
            if args.command == "learn-log":
                status, entries = read_log(port)
                print(json.dumps({"total": status["total"], "first": status["first"],
                                  "usb_monitor": status["usb_monitor"], "entries": entries,
                                  "summary": summarize_log(entries)}, indent=1))
                return 0
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
