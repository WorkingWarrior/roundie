#!/usr/bin/env python3
"""Device check: BUSY (503) vs precondition refusal (259) (EABI P1 fix 3, docs/storage-io.md).

    python3 tools/hardware_validation/run_p1_refusal.py --serial 28:84:85:87:98:7C

Development image only (RS1 IOSTALL needs CONFIG_ROUNDIE_DEV_ELF). Cases:

- status: RO1 STATUS answers 0 with refusal "none";
- app: the eabiprobe package running; RO1 BEGIN answers 259 with refusal "app",
  and roundie_ota.request raises DeviceError (not DeviceBusy) with the unload
  instruction. The app is then stopped and unloaded;
- transfer: an open RE1 transfer; RO1 BEGIN answers 259 with refusal "transfer".
  The transfer is then aborted;
- busy: io_worker stalled (IOSTALL), then more RE1 STATUS lines than its queue
  holds (2). Each line beyond the queue gets 503 with the BUSY payload, the rest
  0 after the stall; no 259 BUSY.

The "io" refusal (OTA BEGIN between an io_worker check and claim) cannot be
produced on demand; it is host-tested (tests/test_runtime_policy.c). Nothing
erases NVS, Flash or storage, and no OTA image is written: every BEGIN is refused.
"""

import argparse
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
import roundie_ota  # noqa: E402
from roundie_time import (BUSY_CODE, BUSY_PAYLOAD, PRECONDITION_CODE, DeviceBusy, DeviceError,  # noqa: E402
                          TransportError, read_serial_reply, write_command)

PACKAGE_ID = "org.roundie.eabiprobe"
ELF = m8.ROOT / "examples/stopwatch_elf/build/stopwatch.app.elf"
SESSION = "5a5a0003"
FLOOD = 5
STALL_MS = 2000
# Any well-formed BEGIN: refused before the OTA manager sees it.
BEGIN = f"BEGIN 5a5a0004 4096 {'0' * 64}"


def raw(port, protocol, operation, timeout=4.0):
    request_id = secrets.token_hex(4)
    prefix = f"!{protocol} {request_id} ".encode()
    write_command(port, f"!{protocol} {request_id} {operation}")
    line = read_serial_reply(port, prefix, time.monotonic() + timeout, 2048)
    code, payload = line[len(prefix):].split(b" ", 1)
    return int(code), json.loads(payload)


def flood(port):
    """FLOOD RE1 STATUS lines at once; returns {request_id: code, payload}."""
    ids = [secrets.token_hex(4) for _ in range(FLOOD)]
    for request_id in ids:
        write_command(port, f"!RE1 {request_id} STATUS")
    replies = {}
    deadline = time.monotonic() + STALL_MS / 1000 + 4.0
    line = bytearray()
    while len(replies) < FLOOD and time.monotonic() < deadline:
        byte = port.read(1)
        if not byte:
            continue
        if byte != b"\n":
            line += byte
            continue
        text = bytes(line)
        line = bytearray()
        for request_id in ids:
            prefix = f"!RE1 {request_id} ".encode()
            if text.startswith(prefix):
                code, payload = text[len(prefix):].split(b" ", 1)
                replies[request_id] = (int(code), json.loads(payload))
    return [replies.get(request_id) for request_id in ids]


def elf_status(device):
    return device.session(lambda port: roundie_elf.request(port, "STATUS"))


def package(path, *arguments):
    command = [sys.executable, str(m8.ROOT / "tools" / "roundie_package.py"), "--port", path]
    result = subprocess.run(command + list(arguments), cwd=m8.ROOT, capture_output=True, text=True, timeout=120)
    return result.returncode, result.stdout + result.stderr


def ota_begin(device):
    code, reply = device.session(lambda port: raw(port, "RO1", BEGIN))
    try:
        device.session(lambda port: roundie_ota.request(port, BEGIN))
        host = "accepted"
    except DeviceBusy as error:
        host = f"busy: {error}"
    except DeviceError as error:
        host = str(error)
    return code, reply.get("refusal"), host


def case_app(device, path):
    rc, text = package(path, "launch", PACKAGE_ID)
    if rc != 0:
        raise DeviceError(f"launch: {text}")
    time.sleep(1.0)
    before = elf_status(device)
    code, refusal, host = ota_begin(device)
    instance = before["instance"]
    device.session(lambda port: roundie_elf.request(port, f"STOP {instance}"))
    time.sleep(0.5)
    device.session(lambda port: roundie_elf.request(port, f"UNLOAD {instance}"))
    time.sleep(0.5)
    after = elf_status(device)
    return {"elf_state": before["state"], "code": code, "refusal": refusal, "host": host,
            "state_after": after["state"],
            "checks": {"running": before["state"] != 0, "code_259": code == PRECONDITION_CODE,
                       "refusal_app": refusal == "app",
                       "host_instruction": "precondition, not retried" in host and "unload --instance" in host,
                       "unloaded": after["state"] == 0}}


def case_transfer(device):
    info = roundie_elf.inspect(ELF)
    begin = f"BEGIN {SESSION} {info['size']} {info['sha256']}"
    opened = device.session(lambda port: roundie_elf.request(port, begin))["busy"]
    code, refusal, host = ota_begin(device)
    device.session(lambda port: roundie_elf.request(port, f"ABORT {SESSION}"))
    after = elf_status(device)
    return {"code": code, "refusal": refusal, "host": host,
            "checks": {"open": opened == 1, "code_259": code == PRECONDITION_CODE,
                       "refusal_transfer": refusal == "transfer",
                       "host_instruction": "precondition, not retried" in host and "abort --session" in host,
                       "aborted": after["busy"] == 0}}


def case_busy(device):
    device.call("safe", f"IOSTALL {STALL_MS}")
    replies = device.session(flood)
    codes = [reply[0] if reply else None for reply in replies]
    busy = [reply for reply in replies if reply and reply[0] == BUSY_CODE]
    time.sleep(STALL_MS / 1000)
    after = elf_status(device)
    return {"codes": codes,
            "checks": {"all_answered": None not in codes,
                       "some_busy": len(busy) >= 1,
                       "busy_payload": all(reply[1] == BUSY_PAYLOAD for reply in busy),
                       "only_0_or_503": all(code in (0, BUSY_CODE) for code in codes if code is not None),
                       "serving_after": after["busy"] in (0, 1)}}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--serial", required=True)
    parser.add_argument("--out", type=Path, default=m8.ROOT / "build-ci" / "eabi" / "p1-refusal")
    args = parser.parse_args(argv)
    path, serial = m8.discover(None, args.serial, m8.BY_ID)
    device = run_e0.Device(path, m8.Monitor())
    report = {"serial": serial, "cases": {}}
    args.out.mkdir(parents=True, exist_ok=True)
    try:
        status = device.call("safe")
        if status["safe"] != 0:
            raise DeviceError(f"board not in normal operation: {status}")
        if elf_status(device)["state"] != 0:
            raise DeviceError("an ELF is loaded; stop and unload it first")
        code, reply = device.session(lambda port: raw(port, "RO1", "STATUS"))
        report["cases"]["status"] = {"checks": {"code_0": code == 0, "refusal_none": reply.get("refusal") == "none"}}
        report["cases"]["app"] = case_app(device, path)
        report["cases"]["transfer"] = case_transfer(device)
        report["cases"]["busy"] = case_busy(device)
        final = device.call("safe")
        report["final"] = {k: final[k] for k in ("safe", "reset", "failures")}
    except (DeviceError, TransportError, OSError, KeyError) as error:
        report["error"] = str(error)
    checks = {f"{name}.{key}": value for name, case in report["cases"].items() for key, value in case["checks"].items()}
    if "final" in report:
        checks["final.normal"] = report["final"]["safe"] == 0 and report["final"]["failures"] == 0
    report["result"] = "PASS" if len(report["cases"]) == 4 and all(checks.values()) and "error" not in report else "FAIL"
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"result": report["result"], "checks": checks, "error": report.get("error"),
                      "busy_codes": report["cases"].get("busy", {}).get("codes")}))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
