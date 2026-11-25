#!/usr/bin/env python3
"""Device check of the Safe Mode cause diagnostics (docs/safe-mode.md "Latch cause").

    python3 tools/hardware_validation/run_e0_diag.py --serial 28:84:85:87:98:7C

1. RS1 CRASH STORED and RS1 CAUSE before: NVS occupancy (nvs_get_stats).
2. Build a package from examples/fault_elf store.app.elf (a store through
   null in the first timer callback), install it and launch it.
3. The panic enters Safe Mode (reason core). RS1 CRASH STORED must hold the
   NVS copy (store 0) and RS1 CAUSE a valid cause record; NVS occupancy after.
4. RS1 REBOOT back to normal; the copy and the cause survive; the package is
   removed.

Nothing erases NVS, Flash or storage. The panic is deliberate and is the same
fault_elf case as docs/m9-stabilization.md §4.
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
from roundie_time import DeviceError, TransportError  # noqa: E402

PACKAGE_ID = "e0.diag.fault"
SAFE_CORE = 5
SITE_RETAINED = 1


def probe(device, operation="STATUS"):
    try:
        return device.call("safe", operation)
    except (DeviceError, TransportError, OSError):
        return None


def wait(what, function, accept, timeout):
    deadline = time.monotonic() + timeout
    while True:
        value = function()
        if value is not None and accept(value):
            return value
        if time.monotonic() >= deadline:
            raise DeviceError(f"timeout waiting for {what}")
        time.sleep(1.0)


def package(path, *arguments):
    command = [sys.executable, str(m8.ROOT / "tools" / "roundie_package.py")]
    if arguments[0] != "build":
        command += ["--port", path]
    result = subprocess.run(command + list(arguments), cwd=m8.ROOT, capture_output=True, text=True, timeout=120)
    return result.returncode, result.stdout + result.stderr


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--serial", required=True)
    parser.add_argument("--out", type=Path, default=m8.ROOT / "build-ci" / "e0" / "a4-diag")
    args = parser.parse_args(argv)
    path, serial = m8.discover(None, args.serial, m8.BY_ID)
    device = run_e0.Device(path, m8.Monitor())
    report = {"serial": serial, "checks": {}}
    checks = report["checks"]
    args.out.mkdir(parents=True, exist_ok=True)

    def save():
        (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")

    try:
        status = probe(device)
        if status is None or status["safe"] != 0:
            raise DeviceError(f"board not in normal operation: {status}")
        report["before"] = {"crash": probe(device, "CRASH STORED")["crash"], "cause": probe(device, "CAUSE")}
        artifact = args.out / "fault-store.rpk"
        code, text = package(path, "build", str(m8.ROOT / "examples/fault_elf/build/store.app.elf"),
                             "--output", str(artifact), "--id", PACKAGE_ID, "--name", "E0 diag fault",
                             "--version", "1.0.0")
        if code != 0:
            raise DeviceError(f"package build: {text}")
        code, text = package(path, "upload", str(artifact), "--install")
        if code != 0:
            raise DeviceError(f"package install: {text}")
        time.sleep(2.0)
        package(path, "launch", PACKAGE_ID)  # the panic may cut the reply
        safe = wait("Safe Mode after the panic", lambda: probe(device),
                    lambda s: s["safe"] == 1, 60)
        report["safe"] = {k: safe[k] for k in ("reason", "fault_app", "failures")}
        crash = probe(device, "CRASH STORED")["crash"]
        cause = probe(device, "CAUSE")
        report["after_panic"] = {"crash": crash, "cause": cause}
        before = report["before"]["crash"]["nvs"]
        report["nvs_delta_used_entries"] = crash["nvs"][1] - before[1]
        checks["safe_core"] = safe["reason"] == "core"
        checks["copy_stored"] = crash["store"] == 0 and crash.get("valid") == 1 and crash["raw_reset"] == 4
        checks["cause_valid"] = (cause["cause"]["valid"] == 1 and cause["cause"]["latch"] == SAFE_CORE and
                                 cause["cause"]["site"] == SITE_RETAINED)
        checks["copy_within_limit"] = crash["size"] <= crash["limit"]
        save()
        try:
            device.call("safe", "REBOOT")
        except (DeviceError, TransportError):
            pass
        time.sleep(3.0)
        normal = wait("normal operation after REBOOT", lambda: probe(device), lambda s: s["safe"] == 0, 90)
        time.sleep(15.0)  # past the stability window, so the latch is cleared
        normal = probe(device)
        crash = probe(device, "CRASH STORED")["crash"]
        cause = probe(device, "CAUSE")
        report["after_reboot"] = {"status": {k: normal[k] for k in ("safe", "reason", "stage")},
                                  "crash": crash, "cause": cause}
        checks["normal_again"] = normal["safe"] == 0 and normal["reason"] == "none"
        checks["copy_survives"] = crash.get("valid") == 1 and crash["store"] == -1
        checks["cause_survives"] = cause["cause"]["valid"] == 1
        code, text = package(path, "remove", PACKAGE_ID)
        checks["package_removed"] = code == 0
    except (DeviceError, TransportError, OSError, KeyError) as error:
        report["error"] = str(error)
    report["result"] = "PASS" if checks and all(checks.values()) and "error" not in report else "FAIL"
    save()
    print(json.dumps({"result": report["result"], "checks": checks, "error": report.get("error"),
                      "nvs_delta_used_entries": report.get("nvs_delta_used_entries")}))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
