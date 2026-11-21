#!/usr/bin/env python3
"""M10 watch face checks on a development board over USB Serial/JTAG.

Non-destructive: faces are installed and selected through RF1, applications
are started through RP1 (INJECTED semantic selection, not touch proof), the
board is restarted with RT1 RESTART (esp_restart) and put into one timed light
sleep with RW1. Nothing erases Flash, writes partitions or uses deep sleep.

usage: run_m10.py --port PORT --report FILE.json --abi1-app ID --ui-app ID FACE.rwf...
The first face is the one kept selected at the end; at least two are needed.
"""
import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import roundie_face  # noqa: E402
import roundie_launcher  # noqa: E402
import roundie_power  # noqa: E402
from roundie_time import DeviceError, TransportError, open_serial, request as time_request  # noqa: E402


class Check:
    def __init__(self, port_name, report):
        self.port_name = port_name
        self.report = report

    def record(self, name, passed, **details):
        self.report["steps"].append({"step": name, "result": "PASS" if passed else "FAIL", **details})
        print(f"{'PASS' if passed else 'FAIL'} {name} {json.dumps(details)}", flush=True)
        return passed

    def face(self, operation, allow_error=False):
        with open_serial(self.port_name) as port:
            try:
                return roundie_face.request(port, operation)
            except DeviceError:
                if not allow_error:
                    raise
                return None

    def nav(self, operation="NAV"):
        with open_serial(self.port_name) as port:
            return roundie_launcher.request(port, operation)

    def wait(self, what, read, done, timeout):
        deadline = time.monotonic() + timeout
        value = read()
        while not done(value):
            if time.monotonic() > deadline:
                raise DeviceError(f"timed out waiting for {what}: {value}")
            time.sleep(0.1)
            value = read()
        return value

    def select(self, face_id):
        self.face(f"SELECT {face_id}")
        return self.wait(f"face {face_id}", lambda: self.face("STATUS"),
                         lambda s: s["active"] == face_id and s["home"] == 1, 5)

    def reconnect(self, timeout):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            try:
                return self.face("STATUS")
            except (OSError, TransportError, DeviceError) as error:
                last = error
                time.sleep(0.5)
        raise TransportError(f"board did not answer after restart: {last}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--abi1-app", required=True)
    parser.add_argument("--ui-app", required=True)
    parser.add_argument("faces", type=Path, nargs="+")
    args = parser.parse_args(argv)
    if len(args.faces) < 2:
        parser.error("at least two faces")
    infos = [roundie_face.inspect(path) for path in args.faces]
    ids = [info["id"] for info in infos]
    report = {"format": 1, "faces": [{"id": i["id"], "version": i["version"], "sha256": i["sha256"]} for i in infos],
              "steps": []}
    check = Check(args.port, report)
    ok = True
    try:
        base = check.face("STATUS")
        report["baseline"] = base
        ok &= check.record("baseline", base["fallback_only"] == 0 and base["home"] == 1,
                           active=base["active"], count=base["count"])

        # Install is separate from activation: what is shown does not change.
        for path, info in zip(args.faces, infos):
            if any(face[0] == info["id"] and face[1] == info["version"] and face[2] == 1
                   for face in check.face("STATUS")["faces"]):
                check.record(f"install {info['id']}", True, note="already installed, same version, healthy")
                continue
            with open_serial(args.port) as port:
                roundie_face.upload(port, path, info)
                after = roundie_face.request(port, f"INSTALL {info['sha256']}")
            ok &= check.record(f"install {info['id']}", after["active"] == base["active"], active=after["active"])

        # Each face activates and is preferred after success.
        for face_id in ids:
            status = check.select(face_id)
            ok &= check.record(f"select {face_id}", status["preferred"] == face_id and status["counters"][6] ==
                               base["counters"][6], objects=status["objects"], pool=status["pool"],
                               bytes=status["bytes"], activate_us=status["us"][1])

        # 100 switches: no failures, LVGL pool back to the same level.
        check.select(ids[0])
        before = check.face("STATUS")
        started = time.monotonic()
        for i in range(100):
            check.select(ids[(i + 1) % 2])
        after = check.face("STATUS")
        ok &= check.record("100 switches", after["active"] == ids[0] and after["counters"][6] == before["counters"][6]
                           and after["counters"][7] == before["counters"][7] and
                           after["pool"] + 256 >= before["pool"],
                           pool_before=before["pool"], pool_after=after["pool"],
                           switches=after["counters"][5] - before["counters"][5],
                           activate_max_us=after["us"][2], update_max_us=after["us"][3],
                           host_s=round(time.monotonic() - started, 1))

        # A corrupted transfer is refused at validation; the shown face stays.
        data = bytearray(args.faces[1].read_bytes())
        data[300] ^= 0x01
        corrupt = args.report.with_suffix(".corrupt.rwf")
        corrupt.write_bytes(bytes(data))
        refused = None
        try:
            with open_serial(args.port) as port:
                roundie_face.upload(port, corrupt, dict(infos[1]))
        except DeviceError as error:
            refused = str(error)
        finally:
            corrupt.unlink()
        status = check.face("STATUS")
        ok &= check.record("corrupt transfer refused", refused is not None and
                           status["active"] == ids[0] and status["phase"] == 0, error=refused)

        # Restart: the preference and the faces persist; Home shows the face.
        with open_serial(args.port) as port:
            time_request(port, "RESTART")
        time.sleep(2)
        status = check.reconnect(30)
        status = check.wait("face after restart", lambda: check.face("STATUS"),
                            lambda s: s["active"] == ids[0], 15)
        ok &= check.record("restart keeps the face", status["preferred"] == ids[0] and status["home"] == 1 and
                           status["load_failures"] == 0, load_us=status["us"][0], activate_us=status["us"][1])

        # REP applications (INJECTED): Home to the list, an ABI 1 RESULT, back
        # to the list; an ABI 3 application exited returns to Home and the face
        # resumes.
        check.wait("idle Home", check.nav, lambda n: n["state"] == "HOME", 10)
        resumes = check.face("STATUS")["counters"][8]
        check.nav("OPEN")  # INJECTED swipe up: Home to the list
        nav = check.wait("list from Home", check.nav, lambda n: n["state"] == "LIST" and n["count"] > 0, 10)
        check.nav(f"SELECT {nav['revision']} {args.abi1_app}")
        nav = check.wait("ABI 1 result", check.nav, lambda n: n["state"] in ("RESULT", "ERROR"), 10)
        ok &= check.record("ABI 1 application via the list", nav["state"] == "RESULT", result=nav["result"])
        check.nav(f"BACK {nav['request']}")
        nav = check.wait("list", check.nav, lambda n: n["state"] == "LIST", 5)
        hidden = check.face("STATUS")
        check.nav(f"SELECT {nav['revision']} {args.ui_app}")
        nav = check.wait("UI application", check.nav, lambda n: n["state"] == "ACTIVE", 10)
        time.sleep(1)
        check.nav(f"EXIT {nav['generation']}")
        check.wait("Home after exit", check.nav, lambda n: n["state"] == "HOME", 10)
        status = check.wait("face resumed", lambda: check.face("STATUS"),
                            lambda s: s["home"] == 1 and s["parked"] == 0, 5)
        ok &= check.record("Home after applications", hidden["home"] == 0 and status["active"] == ids[0] and
                           status["counters"][8] > resumes and status["counters"][6] == after["counters"][6],
                           resumes=status["counters"][8] - resumes)

        # One timed light sleep (M8-B): the face parks and resumes.
        with open_serial(args.port) as port:
            power_before = roundie_power.request(port, "SLEEP 5")
        time.sleep(1)
        power_after = roundie_power.status_after_wake(args.port, 40)
        status = check.wait("face after wake", lambda: check.face("STATUS"),
                            lambda s: s["parked"] == 0, 10)
        ok &= check.record("light sleep park and resume", power_after["sleeps"] == power_before["sleeps"] + 1 and
                           status["active"] == ids[0] and status["counters"][8] > resumes and
                           status["counters"][6] == after["counters"][6], resume_us=status["us"][4])
        report["final"] = status
    except (DeviceError, TransportError, OSError) as error:
        ok = False
        report["steps"].append({"step": "aborted", "result": "FAIL", "error": str(error)})
        print(f"FAIL aborted: {error}", file=sys.stderr)
    report["result"] = "PASS" if ok else "FAIL"
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
