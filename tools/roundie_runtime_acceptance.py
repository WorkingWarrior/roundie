#!/usr/bin/env python3
"""Bounded physical ABI 2 acceptance; explicit operator-selected device/images.

Writes only the existing RE1 staging file and runs trusted examples. No OTA,
restart, fault injection or raw Flash operations. Lost mutation acknowledgments
stop the sequence; inspect STATUS before deciding what to do next.
"""
import argparse
import json
from pathlib import Path
import time

import roundie_elf as elf
import roundie_ota as ota
import roundie_safe as safe
from roundie_time import DeviceError, open_serial


def require(condition, message):
    if not condition:
        raise DeviceError(message)


def await_events(port, generation, key, minimum, timeout=3):
    deadline = time.monotonic() + timeout
    while True:
        status = elf.request(port, "STATUS", timeout=1)
        require(status["instance"] == generation and status["state"] == 3 and not status["error"],
                f"runtime identity/state/error changed: {status}")
        if status[key] >= minimum:
            return status
        if time.monotonic() >= deadline:
            raise DeviceError(f"event deadline exceeded: {status}")
        time.sleep(0.02)


def refusal(port, operation):
    try:
        elf.request(port, operation)
    except DeviceError as error:
        require("rejected (259)" in str(error), f"unexpected refusal: {error}")
        return str(error)
    raise DeviceError(f"unsafe request accepted: {operation}; STOP and inspect, do not retry")


def acceptance(port, event_path, legacy_path, expected_elf, fallback_elf, cycles):
    require(1 <= cycles <= 3, "bounded acceptance allows 1..3 cycles")
    running = ota.request(port, "STATUS")
    require(running["elf"] == expected_elf and running["other_elf"] == fallback_elf and
            running["ota_state"] == running["other_state"] == "VALID" and running["phase"] == "IDLE" and
            running["checked"] == running["passed"] == running["rollback"] == 1,
            f"firmware/fallback acceptance guard failed: {running}")
    guard = safe.request(port, "STATUS")
    require(guard["safe"] == guard["latch"] == guard["failures"] == guard["test_build"] ==
            guard["fault_remaining"] == 0 and guard["stage"] == 4, "normal/STABLE required")
    artifact = elf.inspect(event_path)
    legacy = elf.inspect(legacy_path)
    require(artifact["abi"] == 2 and legacy["abi"] == 1, "example ABI mismatch")
    elf.upload(port, event_path, artifact)
    report = dict(firmware=running, event=artifact, legacy=legacy, cycles=[])
    previous_generation = 0
    for index in range(cycles):
        started_at = time.monotonic()
        started = elf.start(port, artifact["sha256"])
        generation = started["instance"]
        require(generation > previous_generation, "generation did not advance")
        record = dict(start=started, host_start_ms=(time.monotonic() - started_at) * 1000)
        record["timer_events"] = await_events(port, generation, "ticks", 3)
        elf.request(port, f"EVENT {generation} 42")
        injected = await_events(port, generation, "users", 1)
        require(injected["value"] == 42, "injected payload was not delivered")
        record["injected"] = injected
        record["invalid_transition"] = refusal(port, f"RESUME {generation}")
        record["stale_generation"] = refusal(port, f"EVENT {generation + 1000} 9")
        suspended = elf.control(port, "SUSPEND", generation)
        time.sleep(0.6)  # More than two 250-ms timer periods, bounded.
        quiet = elf.request(port, "STATUS")
        require(quiet["state"] == 9 and quiet["callbacks"] == suspended["callbacks"] and
                quiet["ticks"] == suspended["ticks"] and quiet["queue"] == 0,
                "ordinary callbacks executed while suspended")
        record["suspended"] = quiet
        record["suspended_event"] = refusal(port, f"EVENT {generation} 9")
        record["resumed"] = elf.control(port, "RESUME", generation)
        record["after_resume"] = await_events(port, generation, "ticks", quiet["ticks"] + 2)
        # Exercise direct stop from suspension on one of the bounded cycles.
        if index == 1:
            elf.control(port, "SUSPEND", generation)
        record["stopped"] = elf.control(port, "STOP", generation)
        record["event_after_stop"] = refusal(port, f"EVENT {generation} 7")
        record["duplicate_stop"] = elf.control(port, "STOP", generation)
        unloaded = elf.control(port, "UNLOAD", generation)
        record["unloaded"] = unloaded
        record["duplicate_unload"] = elf.control(port, "UNLOAD", generation)
        require(unloaded["resources"][3] == unloaded["alloc"] == unloaded["queue"] ==
                unloaded["timer"] == unloaded["subscriptions"] == unloaded["busy"] == 0,
                f"surviving application ownership: {unloaded}")
        require(not unloaded["over_budget"], "callback exceeded budget")
        record["host_cycle_ms"] = (time.monotonic() - started_at) * 1000
        report["cycles"].append(record)
        previous_generation = generation
    elf.upload(port, legacy_path, legacy)
    report["legacy_result"] = elf.run(port, legacy["sha256"])
    require(report["legacy_result"]["app_result"] == 42, "ABI 1 regression")
    report["final"] = elf.request(port, "RESOURCES")
    require(report["final"]["busy"] == report["final"]["resources"][3] == 0, "final runtime is busy")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--event", type=Path, required=True)
    parser.add_argument("--legacy", type=Path, required=True)
    parser.add_argument("--expected-elf", required=True)
    parser.add_argument("--fallback-elf", required=True)
    parser.add_argument("--cycles", type=int, choices=(1, 2, 3), default=3)
    args = parser.parse_args()
    with open_serial(args.port) as port:
        report = acceptance(port, args.event, args.legacy, args.expected_elf, args.fallback_elf, args.cycles)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
