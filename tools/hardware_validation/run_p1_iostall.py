#!/usr/bin/env python3
"""Device check: RS1 REQUEST enters Safe Mode without io_worker (EABI P1, docs/storage-io.md).

    python3 tools/hardware_validation/run_p1_iostall.py --serial 28:84:85:87:98:7C

Development image only (RS1 IOSTALL needs CONFIG_ROUNDIE_DEV_ELF).

Verified from the source: a running application is torn down by rep_executor
(roundie_elf_set_allowed sets the mailbox shutdown), not by io_worker. What
keeps the runtime busy on io_worker is an open RE1/RP1 transfer or a package
job: io_worker's poll aborts the transfer once admission closes. Cases:

- control: an open RE1 transfer, io_worker serving. io_worker aborts it, the
  latch is written to NVS before the restart (latch 1 after boot), and the
  restart comes well inside the teardown timeout;
- stall: an open RE1 transfer, then RS1 IOSTALL holds io_worker for longer than
  the timeout, then REQUEST. system_worker must restart into Safe Mode at the
  timeout (3 s), before the stall ends, without the NVS write (latch 0 after
  boot), as a planned restart (no fault_app);
- app_stall: the package running and io_worker stalled. Expected: the executor
  tears down without io_worker, so the clean path (latch 1, before the timeout).

The restart moment is the host time of the first HEALTH reply after boot minus
its uptime. Each case ends with RS1 REBOOT and waits past the stability window.
Nothing erases NVS, Flash or storage.
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
import roundie_elf  # noqa: E402
from roundie_time import DeviceError, TransportError  # noqa: E402

PACKAGE_ID = "org.roundie.eabiprobe"
TIMEOUT_S = 3.0  # ROUNDIE_SAFE_TEARDOWN_TIMEOUT_US
STALL_MS = 8000
ELF = m8.ROOT / "examples/stopwatch_elf/build/stopwatch.app.elf"


def probe(device, operation="STATUS"):
    try:
        return device.call("safe", operation)
    except (DeviceError, TransportError, OSError):
        return None


def wait(what, function, accept, timeout, interval=0.2):
    deadline = time.monotonic() + timeout
    while True:
        value = function()
        if value is not None and accept(value):
            return value
        if time.monotonic() >= deadline:
            raise DeviceError(f"timeout waiting for {what}")
        time.sleep(interval)


def package(path, *arguments):
    command = [sys.executable, str(m8.ROOT / "tools" / "roundie_package.py"), "--port", path]
    result = subprocess.run(command + list(arguments), cwd=m8.ROOT, capture_output=True, text=True, timeout=120)
    return result.returncode, result.stdout + result.stderr


def run_case(device, path, stall, transfer):
    case = {}
    status = probe(device)
    if status is None or status["safe"] != 0:
        raise DeviceError(f"board not in normal operation: {status}")
    elf = device.session(lambda port: roundie_elf.request(port, "STATUS"))
    if elf["state"] != 0:
        raise DeviceError(f"an ELF is loaded (state {elf['state']}); stop and unload it first")
    if transfer:
        info = roundie_elf.inspect(ELF)
        begin = f"BEGIN 5a5a0001 {info['size']} {info['sha256']}"
        case["begin"] = device.session(lambda port: roundie_elf.request(port, begin))["busy"]
    else:
        code, text = package(path, "launch", PACKAGE_ID)
        if code != 0:
            raise DeviceError(f"launch: {text}")
        time.sleep(1.0)
        case["elf_state"] = device.session(lambda port: roundie_elf.request(port, "STATUS"))["state"]
    if stall:
        case["iostall"] = probe(device, f"IOSTALL {STALL_MS}") is not None
    sent = time.monotonic()
    reply = probe(device, "REQUEST")
    case["request_reply"] = reply is not None
    time.sleep(0.5)
    # REQUEST sets safe at once, so only a reply from a boot after REQUEST counts.
    health = wait("Safe Mode boot after REQUEST", lambda: probe(device, "HEALTH"),
                  lambda s: s["safe"] == 1 and time.monotonic() - s["uptime_us"] / 1e6 > sent, 30)
    received = time.monotonic()
    restart_after = received - health["uptime_us"] / 1e6 - sent
    case["after"] = {k: health[k] for k in ("safe", "reason", "latch", "failures", "fault_app", "raw_reset")}
    case["restart_after_s"] = round(restart_after, 3)
    checks = {
        "request_accepted": case["request_reply"],
        "safe_request": health["safe"] == 1 and health["reason"] == "request",
        "planned": health["fault_app"] == "" and health["failures"] == 0,
        "busy_before": case.get("begin") == 1 if transfer else case.get("elf_state") == 3,
    }
    if stall:
        checks["stall_armed"] = case["iostall"]
    if stall and transfer:
        checks["no_nvs_write"] = health["latch"] == 0
        checks["restart_at_timeout"] = TIMEOUT_S - 0.3 <= restart_after < STALL_MS / 1000 - 1.0
    else:
        checks["latch_persisted"] = health["latch"] == 1
        checks["restart_before_timeout"] = restart_after < TIMEOUT_S - 0.5
    case["checks"] = checks
    try:
        device.call("safe", "REBOOT")
    except (DeviceError, TransportError):
        pass
    time.sleep(3.0)
    wait("normal operation after REBOOT", lambda: probe(device), lambda s: s["safe"] == 0, 90, 1.0)
    time.sleep(15.0)  # past the stability window, so the trial and latch are cleared
    normal = probe(device)
    checks["normal_again"] = normal is not None and normal["safe"] == 0 and normal["reason"] == "none"
    return case


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--serial", required=True)
    parser.add_argument("--out", type=Path, default=m8.ROOT / "build-ci" / "eabi" / "p1-iostall")
    args = parser.parse_args(argv)
    path, serial = m8.discover(None, args.serial, m8.BY_ID)
    device = run_e0.Device(path, m8.Monitor())
    report = {"serial": serial, "cases": {}}
    args.out.mkdir(parents=True, exist_ok=True)
    try:
        for name, stall, transfer in (("control", False, True), ("stall", True, True), ("app_stall", True, False)):
            report["cases"][name] = run_case(device, path, stall, transfer)
    except (DeviceError, TransportError, OSError, KeyError) as error:
        report["error"] = str(error)
    checks = {f"{name}.{key}": value for name, case in report["cases"].items() for key, value in case["checks"].items()}
    report["result"] = "PASS" if len(report["cases"]) == 3 and all(checks.values()) and "error" not in report else "FAIL"
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"result": report["result"], "checks": checks, "error": report.get("error"),
                      "restart_after_s": {n: c.get("restart_after_s") for n, c in report["cases"].items()}}))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
