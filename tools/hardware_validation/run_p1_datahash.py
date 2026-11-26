#!/usr/bin/env python3
"""Device check: the watch theme survives a relaunch, by app data hash (EABI P1 fix 4).

    python3 tools/hardware_validation/run_p1_datahash.py --serial 28:84:85:87:98:7C

Development image only (RP1 DATAHASH needs CONFIG_ROUNDIE_DEV_PACKAGES). It
replaces judging the screen. The watch (examples/watch_elf) stores two bytes
{1, theme} and advances the theme on a press of its hands widget (the second
widget it creates). DATAHASH returns the length and SHA-256 of that blob, so
the theme can be read from the hash. Steps, each with the app stopped and
unloaded before DATAHASH:

1. h0: the stored theme t0 (no blob counts as theme 0);
2. launch, press the hands, exit: h1 must be {1, t0+1};
3. launch, exit without input: h2 must equal h1 (a launch does not rewrite it);
4. launch, press, exit: h3 must be {1, t0+2}. Only a watch that read the stored
   theme at init gets there; one starting at theme 0 would write {1, 1};
5. launch, press, exit: h4 must be {1, t0}, which leaves the board as found.

Steps 4 and 5 together tell "read" from "started at 0" for every t0. Nothing
erases NVS, Flash or storage; only the watch's own data blob is written, by
the watch.
"""

import argparse
import hashlib
import json
import secrets
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_m8 as m8  # noqa: E402
import run_e0  # noqa: E402
import roundie_elf  # noqa: E402
import roundie_ui  # noqa: E402
from roundie_time import DeviceError, TransportError, read_serial_reply, write_command  # noqa: E402

WATCH = "org.roundie.watch"
THEMES = 3  # THEME_COUNT in examples/watch_elf/watch.c
WIDGETS = 4  # view, hands, date, time


def blob_hash(theme):
    return hashlib.sha256(bytes((1, theme))).hexdigest()


EMPTY = hashlib.sha256(b"").hexdigest()


def datahash(device, package_id):
    def call(port):
        request_id = secrets.token_hex(4)
        prefix = f"!RP1 {request_id} ".encode()
        write_command(port, f"!RP1 {request_id} DATAHASH {package_id}")
        line = read_serial_reply(port, prefix, time.monotonic() + 8.0, 256)
        code, payload = line[len(prefix):].split(b" ", 1)
        result = json.loads(payload)
        if result.get("format") != 1 or type(result.get("datahash")) is not int or len(result.get("sha256", "")) != 64:
            raise TransportError(f"malformed DATAHASH reply: {line!r}")
        if int(code) != 0:
            raise DeviceError(f"DATAHASH rejected ({int(code)})")
        return {"length": result["datahash"], "sha256": result["sha256"]}
    return device.session(call)


def theme_of(blob):
    if blob["length"] == 0 and blob["sha256"] == EMPTY:
        return 0
    for theme in range(THEMES):
        if blob["length"] == 2 and blob["sha256"] == blob_hash(theme):
            return theme
    return None


def package(path, *arguments):
    command = [sys.executable, str(m8.ROOT / "tools" / "roundie_package.py"), "--port", path]
    result = subprocess.run(command + list(arguments), cwd=m8.ROOT, capture_output=True, text=True, timeout=120)
    return result.returncode, result.stdout + result.stderr


def elf_status(device):
    return device.session(lambda port: roundie_elf.request(port, "STATUS"))


def ui_status(device):
    return device.session(lambda port: roundie_ui.request(port, "STATUS"))


def run_watch(device, path, presses):
    """Launch the watch, press its hands `presses` times, stop and unload it."""
    rc, text = package(path, "launch", WATCH)
    if rc != 0:
        raise DeviceError(f"launch: {text}")
    deadline = time.monotonic() + 10.0
    while True:
        ui = ui_status(device)
        if ui["widgets"] == WIDGETS and ui["pending"] == 0:
            break
        if time.monotonic() >= deadline:
            raise DeviceError(f"watch UI not ready: {ui}")
        time.sleep(0.2)
    accepted = ui["input_accepted"]
    hands = ui["view"] + 1
    for _ in range(presses):
        device.session(lambda port: roundie_ui.request(port, f"PRESS {ui['generation']} {hands}"))
        time.sleep(0.5)
    after = ui_status(device)
    instance = elf_status(device)["instance"]
    device.session(lambda port: roundie_elf.request(port, f"STOP {instance}"))
    time.sleep(0.5)
    device.session(lambda port: roundie_elf.request(port, f"UNLOAD {instance}"))
    time.sleep(0.5)
    state = elf_status(device)["state"]
    if state != 0:
        raise DeviceError(f"watch not unloaded (state {state})")
    return after["input_accepted"] - accepted


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--serial", required=True)
    parser.add_argument("--out", type=Path, default=m8.ROOT / "build-ci" / "eabi" / "p1-datahash")
    args = parser.parse_args(argv)
    path, serial = m8.discover(None, args.serial, m8.BY_ID)
    device = run_e0.Device(path, m8.Monitor())
    report = {"serial": serial, "steps": {}}
    checks = {}
    args.out.mkdir(parents=True, exist_ok=True)
    try:
        status = device.call("safe")
        if status["safe"] != 0:
            raise DeviceError(f"board not in normal operation: {status}")
        if elf_status(device)["state"] != 0:
            raise DeviceError("an ELF is loaded; stop and unload it first")
        steps = report["steps"]
        steps["h0"] = datahash(device, WATCH)
        t0 = theme_of(steps["h0"])
        checks["h0_known_theme"] = t0 is not None
        if t0 is None:
            raise DeviceError(f"stored blob is not a watch theme: {steps['h0']}")
        report["t0"] = t0
        for name, presses, expected in (("h1", 1, (t0 + 1) % THEMES), ("h2", 0, None),
                                        ("h3", 1, (t0 + 2) % THEMES), ("h4", 1, t0)):
            accepted = run_watch(device, path, presses)
            steps[name] = datahash(device, WATCH)
            steps[name]["presses_accepted"] = accepted
            checks[f"{name}_input"] = accepted == presses
            if expected is None:
                checks["h2_equals_h1"] = steps["h2"] == {**steps["h1"], "presses_accepted": 0}
            else:
                checks[f"{name}_theme_{expected}"] = steps[name]["length"] == 2 and \
                    steps[name]["sha256"] == blob_hash(expected)
        checks["unknown_id_rejected"] = datahash_missing(device)
        final = device.call("safe")
        report["final"] = {k: final[k] for k in ("safe", "reset", "failures")}
        checks["final_normal"] = final["safe"] == 0 and final["failures"] == 0
    except (DeviceError, TransportError, OSError, KeyError) as error:
        report["error"] = str(error)
    report["checks"] = checks
    report["result"] = "PASS" if checks and all(checks.values()) and "error" not in report else "FAIL"
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"result": report["result"], "t0": report.get("t0"), "checks": checks,
                      "error": report.get("error")}))
    return 0 if report["result"] == "PASS" else 1


def datahash_missing(device):
    """DATAHASH of a package that is not installed is refused, not an empty blob."""
    try:
        datahash(device, "org.roundie.not-installed")
    except DeviceError:
        return True
    return False


if __name__ == "__main__":
    sys.exit(main())
