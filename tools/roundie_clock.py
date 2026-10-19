#!/usr/bin/env python3
"""Read-only RTC/system diagnostics; never SET, sync, restart or alter NVS.

One sample: --port PATH. Bounded series: --port PATH --at 0 30 120 300.
Requires firmware 0.2.4 DIAG. JSON keeps raw timestamps and conservative bounds.
"""

import argparse
import json
import re
import secrets
import subprocess
import sys
import time

from roundie_time import (DeviceError, TransportError, open_serial,
                          read_serial_reply, write_command)

FIELDS = ("state", "os", "rtc", "err", "valid", "wb", "wa", "mb", "ma",
          "usable", "lo", "hi", "sync", "at")
REPLY = re.compile(rb"!RT1 ([0-9a-f]{8}) DIAG " + b" ".join(
    key.encode() + rb"=(-?[0-9]{1,19})" for key in FIELDS) + rb"\r?\n")


def parse_reply(line, request_id):
    match = REPLY.fullmatch(line)
    if match is None or match[1].decode() != request_id:
        raise TransportError("invalid clock diagnostic reply (requires firmware DIAG)")
    sample = dict(zip(FIELDS, map(int, match.groups()[1:])))
    if (sample["state"] not in range(7) or sample["os"] not in (-1, 0, 1) or
            any(sample[k] not in (0, 1) for k in ("valid", "usable")) or
            not 0 <= sample["sync"] <= 0xffffffff or
            not 0 <= sample["at"] <= sample["ma"] or
            not 0 <= sample["mb"] <= sample["ma"] <= 2**63 - 1):
        raise TransportError("inconsistent clock diagnostic fields")
    if sample["usable"]:
        duration = sample["ma"] - sample["mb"]
        wall_duration = sample["wa"] - sample["wb"]
        if (sample["state"] != 0 or sample["os"] != 0 or sample["err"] != 0 or
                sample["valid"] != 1 or not 946684800 <= sample["rtc"] <= 4102444799 or
                not 946684800000000 <= sample["wb"] < 4102444800000000 or
                not 946684800000000 <= sample["wa"] < 4102444800000000 or
                duration > 1000000 or wall_duration < 0 or
                abs(wall_duration - duration) > 2000 or
                sample["lo"] != sample["rtc"] * 1000000 - sample["wa"] or
                sample["hi"] != (sample["rtc"] + 1) * 1000000 - sample["wb"]):
            raise TransportError("inconsistent clock offset enclosure")
    return sample


def query(port, timeout=3.0):
    request_id = secrets.token_hex(4)
    hm0 = time.monotonic_ns()
    hw0 = time.time_ns()
    write_command(port, f"!RT1 {request_id} DIAG")
    line = read_serial_reply(port, f"!RT1 {request_id} ".encode(),
                             hm0 / 1e9 + timeout, 512)
    hw1 = time.time_ns()
    hm1 = time.monotonic_ns()
    sample = parse_reply(line, request_id)
    sample.update(host_wall_before_ns=hw0, host_wall_after_ns=hw1,
                  host_mono_before_ns=hm0, host_mono_after_ns=hm1,
                  round_trip_us=(hm1 - hm0) / 1000,
                  read_duration_us=sample["ma"] - sample["mb"],
                  uptime_us=sample["ma"])
    sample["host_reference_usable"] = abs((hw1 - hw0) - (hm1 - hm0)) <= 2000000
    if sample["usable"]:
        sample["offset_class"] = ("measurable" if sample["lo"] > 0 or sample["hi"] < 0
                                  else "bounded_unresolved")
        sample["uncertainty_width_us"] = sample["hi"] - sample["lo"]
        if sample["host_reference_usable"]:
            # RTC snapshot occurs somewhere inside host TX/RX bracket.
            sample["rtc_minus_host_us"] = [sample["rtc"] * 1000000 - hw1 // 1000,
                                            (sample["rtc"] + 1) * 1000000 - hw0 // 1000]
    else:
        sample["offset_class"] = "unusable"
    return sample


def trend(samples):
    """Feasible constant relative-rate enclosure, not a fitted ppm estimate.

    All sample pairs constrain rate, including full one-second uncertainty
    and unknown snapshot instant. Zero inside bounds means unresolved drift.
    This model does not prove constant frequency or either clock's UTC accuracy.
    """
    unavailable = lambda reason: dict(classification="unavailable", reason=reason)
    if len(samples) < 2:
        return unavailable("need at least two samples")
    if not all(s["usable"] and s["host_reference_usable"] for s in samples):
        return unavailable("invalid sample or host reference discontinuity")
    low, high = -float("inf"), float("inf")
    for i, first in enumerate(samples):
        for second in samples[i + 1:]:
            dt_min = second["mb"] - first["ma"]
            dt_max = second["ma"] - first["mb"]
            if (dt_min <= 0 or first["sync"] != second["sync"] or
                    first["at"] != second["at"]):
                return unavailable("device restart, clock set or overlapping samples")
            # Detect a wall-clock step between device observations as well as
            # within one read; no normal firmware slew is enabled.
            if abs((second["wb"] - first["wb"]) -
                   (second["mb"] - first["mb"])) > 2000:
                return unavailable("device wall/monotonic discontinuity")
            host_elapsed = second["host_mono_before_ns"] - first["host_mono_before_ns"]
            host_wall = second["host_wall_before_ns"] - first["host_wall_before_ns"]
            if host_elapsed <= 0 or abs(host_wall - host_elapsed) > max(2000000, host_elapsed // 1000):
                return unavailable("host reference step or excessive slew")
            lower = second["lo"] - first["hi"]
            upper = second["hi"] - first["lo"]
            low = max(low, min(lower / dt_min, lower / dt_max) * 1e6)
            high = min(high, max(upper / dt_min, upper / dt_max) * 1e6)
    if low > high:
        return unavailable("no constant-rate model fits all sample bounds")
    return dict(classification="supported_trend" if low > 0 or high < 0 else "unresolved",
                relative_rate_ppm_bounds=[low, high],
                model="constant RTC-minus-system rate; interval bounds, no confidence level")


def host_sync_state():
    try:
        result = subprocess.run(["timedatectl", "show", "-p", "NTPSynchronized", "-p", "NTP"],
                                capture_output=True, text=True, timeout=2, check=False)
        return result.stdout.strip() if result.returncode == 0 else "unobservable"
    except (OSError, subprocess.TimeoutExpired):
        return "unobservable"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--at", type=float, nargs="+", default=[0],
                        help="sample offsets in seconds; 1..16 strictly increasing values, 0..600")
    parser.add_argument("--json", help="exclusive-create JSON record, never overwrite")
    args = parser.parse_args(argv)
    if (not 1 <= len(args.at) <= 16 or any(not 0 <= t <= 600 for t in args.at) or
            any(b <= a for a, b in zip(args.at, args.at[1:]))):
        parser.error("--at requires 1..16 increasing offsets within 0..600 seconds")
    record = dict(model="RTC-system enclosure [R*1e6-wa, (R+1)*1e6-wb] us",
                  host_sync=host_sync_state(), samples=[])
    try:
        # Reserve output before any wait/transport work; partial records retain
        # observations if later sampling fails or the operator interrupts.
        with open(args.json, "x") if args.json else open('/dev/null', 'w') as output:
            try:
                with open_serial(args.port) as port:
                    start = time.monotonic()
                    for at in args.at:
                        while time.monotonic() < start + at:
                            time.sleep(min(1.0, max(0, start + at - time.monotonic())))
                        sample = query(port)
                        record["samples"].append(sample)
                        enclosure = (f"[{sample['lo']/1e6:+.6f}, {sample['hi']/1e6:+.6f}]s"
                                     if sample["usable"] else "unavailable")
                        print(f"uptime={sample['uptime_us']/1e6:.3f}s "
                              f"RTC state={sample['state']} OS={sample['os']} "
                              f"{sample['offset_class']} "
                              f"RTC-system={enclosure} "
                              f"read={sample['read_duration_us']}us RTT={sample['round_trip_us']/1000:.3f}ms",
                              flush=True)
                    record["trend"] = trend(record["samples"])
                    print(json.dumps(record["trend"]), flush=True)
            finally:
                json.dump(record, output, indent=2)
                output.write("\n")
        return 0 if all(s["usable"] for s in record["samples"]) else 3
    except (TransportError, DeviceError, OSError) as error:
        print(f"Clock diagnostic failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
