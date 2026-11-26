#!/usr/bin/env python3
"""Device measurement: io_worker under CPU load, park latency, watchdog feed gap (EABI P1 step 4).

    python3 tools/hardware_validation/run_p1_prio.py --serial 28:84:85:87:98:7C --label v1

Development image only (RS1 IOSTAT, IOPARK and BURN need CONFIG_ROUNDIE_DEV_ELF).
It is a measurement, not a gate: it records numbers and fails only on errors.

Load cases: RS1 BURN spins one task per selected core for BURN_MS without
blocking (core 2 = both cores). During each burn the runner measures, in order:

- IOPARK x3: how long system_worker waits for io_worker to park (limit 250 ms);
- RS1 STATUS x3: request-to-reply seconds of system_worker;
- RP1 FSBENCH XREAD 1024 4096 0 W: the read on io_worker, wall time and io_us.

After the burn, IOSTAT gives the longest gap between two io_worker watchdog
feeds during the case. A workload case without burn (XWRITE/XREAD 1024 KiB,
package build+install, launch, stop, unload, remove) gives the feed gap of
ordinary io_worker work. Nothing erases NVS, Flash or storage; the runner
deletes the bench files and removes the package it installed.
"""

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_m8 as m8  # noqa: E402
import run_e0  # noqa: E402
import run_e0_stall as stall  # noqa: E402
import roundie_elf  # noqa: E402
from roundie_time import DeviceError, TransportError  # noqa: E402

BURN_MS = 6000
CASES = (("idle", None), ("core1_p1", (1, 1)), ("both_p1", (2, 1)), ("core0_p2", (0, 2)), ("core1_p2", (1, 2)),
         ("both_p2", (2, 2)))
PACKAGE_ID = "org.roundie.p1prio"
ELF = m8.ROOT / "examples/stopwatch_elf/build/stopwatch.app.elf"


def rs1(device, operation, timeout=4.0):
    def call(port):
        code, payload, seconds, _, _ = stall.exchange(port, "!RS1", operation, timeout)
        return code, json.loads(payload), seconds
    return device.session(call)


def xbench(device, operation):
    def call(port):
        started = time.monotonic()
        result = stall.xbench_request(port, operation, 60.0)
        result["wall_s"] = round(time.monotonic() - started, 3)
        return {k: result[k] for k in ("code", "wall_s", "io_us", "total_us", "chunk_max_us", "migrations")}
    return device.session(call)


def package(path, *arguments):
    command = [sys.executable, str(m8.ROOT / "tools" / "roundie_package.py"), "--port", path]
    result = subprocess.run(command + list(arguments), cwd=m8.ROOT, capture_output=True, text=True, timeout=180)
    return result.returncode, result.stdout + result.stderr


def iostat(device, reset=False):
    code, body, _ = rs1(device, "IOSTAT RESET" if reset else "IOSTAT")
    if code != 0 or body.get("iodev") != "IOSTAT":
        raise DeviceError(f"IOSTAT failed: {code} {body}")
    return {k: body[k] for k in ("feed_gap_max_us", "parks", "park_max_us", "park_timeouts", "priority")}


def load_case(device, burn):
    iostat(device, reset=True)
    case = {"burn": burn}
    started = time.monotonic()
    if burn is not None:
        code, body, _ = rs1(device, f"BURN {BURN_MS} {burn[0]} {burn[1]}")
        if code != 0 or body.get("started", 0) < 1:
            raise DeviceError(f"BURN refused: {code} {body}")
        case["burn_tasks"] = body["started"]
        time.sleep(0.3)
    parks = []
    for _ in range(3):
        code, body, seconds = rs1(device, "IOPARK")
        parks.append({"parked": body["parked"], "park_us": body["park_us"], "reply_s": round(seconds, 3)})
    case["iopark"] = parks
    case["status_reply_s"] = [round(rs1(device, "STATUS")[2], 3) for _ in range(3)]
    case["xread"] = xbench(device, "XREAD 1024 4096 0 W")
    remaining = started + BURN_MS / 1000 + 0.5 - time.monotonic()
    if remaining > 0:
        time.sleep(remaining)
    case["iostat"] = iostat(device)
    return case


def workload_case(device, path, out):
    iostat(device, reset=True)
    case = {"xwrite": xbench(device, "XWRITE 1024 4096 0 W"), "xread": xbench(device, "XREAD 1024 4096 0 W")}
    artifact = out / "p1prio.rpk"
    package(path, "remove", PACKAGE_ID)  # left over from an interrupted run; not installed is fine
    steps = (("build", ("build", str(ELF), "--output", str(artifact), "--id", PACKAGE_ID,
                        "--name", "P1 prio", "--version", "1.0.0")),
             ("install", ("upload", str(artifact), "--install")))
    for name, arguments in steps:
        code, text = package(path, *arguments)
        if code != 0:
            raise DeviceError(f"package {name}: {text}")
    time.sleep(2.0)
    code, text = package(path, "launch", PACKAGE_ID)
    if code != 0:
        raise DeviceError(f"launch: {text}")
    time.sleep(2.0)
    instance = device.session(lambda port: roundie_elf.request(port, "STATUS"))["instance"]
    device.session(lambda port: roundie_elf.request(port, f"STOP {instance}"))
    time.sleep(0.5)
    device.session(lambda port: roundie_elf.request(port, f"UNLOAD {instance}"))
    time.sleep(0.5)
    code, text = package(path, "remove", PACKAGE_ID)
    case["removed"] = code == 0
    case["iostat"] = iostat(device)
    return case


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--serial", required=True)
    parser.add_argument("--label", required=True, help="image variant, e.g. v1 or v0")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args(argv)
    out = args.out or m8.ROOT / "build-ci" / "eabi" / f"p1-prio-{args.label}"
    out.mkdir(parents=True, exist_ok=True)
    path, serial = m8.discover(None, args.serial, m8.BY_ID)
    device = run_e0.Device(path, m8.Monitor())
    report = {"serial": serial, "label": args.label, "burn_ms": BURN_MS, "cases": {}}
    try:
        status = device.call("safe")
        if status["safe"] != 0:
            raise DeviceError(f"board not in normal operation: {status}")
        uptime = device.call("safe", "HEALTH")["uptime_us"]
        prep = xbench(device, "XWRITE 1024 4096 0 W")
        if prep["code"] != 0:
            raise DeviceError(f"XWRITE 1024 refused ({prep['code']})")
        for name, burn in CASES:
            report["cases"][name] = load_case(device, burn)
            time.sleep(1.0)
        report["cases"]["workload"] = workload_case(device, path, out)
        for kib in (1024,):
            device.call("fsbench", f"DEL {kib}")
        final = device.call("safe")
        report["final"] = {k: final[k] for k in ("safe", "reset", "failures")}
        report["no_reset"] = device.call("safe", "HEALTH")["uptime_us"] > uptime
    except (DeviceError, TransportError, OSError, KeyError) as error:
        report["error"] = str(error)
    report["result"] = "MEASURED" if "error" not in report and report.get("no_reset") else "FAIL"
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    summary = {name: {"park_us": [p["park_us"] for p in case.get("iopark", [])],
                      "parked": [p["parked"] for p in case.get("iopark", [])],
                      "status_s": case.get("status_reply_s"),
                      "xread_wall_s": case["xread"]["wall_s"], "xread_io_ms": case["xread"]["io_us"] // 1000,
                      "feed_gap_ms": case["iostat"]["feed_gap_max_us"] // 1000}
               for name, case in report["cases"].items() if "iostat" in case}
    print(json.dumps({"result": report["result"], "error": report.get("error"), "summary": summary}))
    return 0 if report["result"] == "MEASURED" else 1


if __name__ == "__main__":
    sys.exit(main())
