#!/usr/bin/env python3
"""Explicit development-time UTC synchronization over Roundie's USB Serial/JTAG."""

import argparse
from datetime import datetime, timezone
import re
import secrets
import sys
import time

MIN_TIMESTAMP = 946684800
MAX_TIMESTAMP = 4102444799
TOLERANCE_SECONDS = 3.0
# ROUNDIE_SENSOR_NOT_FOUND with result OK: the board has no external RTC and
# the system clock alone carries the synchronized time.
NO_RTC = 1
ESP_ERR_NOT_FOUND = 0x105
MAX_REPLY = 256
REPLY = re.compile(
    rb"!RT1 ([0-9a-f]{8}) ([A-Z_]+) state=([0-6]) os=(-1|0|1) "
    rb"rtc=(-1|[0-9]{1,10}) wall=(-1|[0-9]{1,10}) err=(-?[0-9]{1,10})\r?\n"
)


class TransportError(Exception):
    """Serial I/O, framing, or deadline failure (exit 2)."""


class DeviceError(Exception):
    """Device RTC or wall-clock validation failure (exit 3)."""


class DeviceBusy(DeviceError):
    """The device answered BUSY (503): the request was not executed and may be sent again."""


# Reply codes shared by RP1/RE1/RF1/RO1 (docs/storage-io.md, P1 fix 3).
BUSY_CODE = 503  # ROUNDIE_IO_BUSY_CODE: transient, not executed; retry
PRECONDITION_CODE = 259  # ESP_ERR_INVALID_STATE: refused in the current state; do not retry
BUSY_PAYLOAD = {"format": 1, "error": "busy"}
BUSY_ATTEMPTS = 3
BUSY_DELAY_S = 0.2


def check_busy(code, status):
    """Raises DeviceBusy for the io_worker BUSY reply. The BUSY payload under any
    other code is malformed (ValueError), so a precondition refusal never retries."""
    if status == BUSY_PAYLOAD:
        if code != BUSY_CODE:
            raise ValueError("busy payload with code")
        raise DeviceBusy(f"device busy ({BUSY_CODE}): io_worker queue full, not executed")


def retry_busy(send, attempts=BUSY_ATTEMPTS, delay=BUSY_DELAY_S):
    """Calls send() again only after DeviceBusy, at most attempts times in all."""
    for attempt in range(attempts):
        try:
            return send()
        except DeviceBusy:
            if attempt + 1 == attempts:
                raise
            time.sleep(delay)


def parse_reply(line, request_id):
    """Ignore logs/other requests; reject malformed replies addressed to us."""
    prefix = f"!RT1 {request_id} ".encode("ascii")
    if not line.startswith(prefix):
        return None
    match = REPLY.fullmatch(line)
    if match is None:
        raise TransportError("invalid or truncated protocol reply")
    _, result, state, os_flag, rtc, wall, error = match.groups()
    status = dict(code=result.decode("ascii"), state=int(state), os=int(os_flag),
                  rtc=int(rtc), wall=int(wall), error=int(error))
    for key in ("rtc", "wall"):
        if status[key] != -1 and not MIN_TIMESTAMP <= status[key] <= MAX_TIMESTAMP:
            raise TransportError(f"out-of-range {key} timestamp in reply")
    if status["state"] == 0:
        if status["os"] != 0 or status["rtc"] == -1:
            raise TransportError("inconsistent valid RTC reply")
    elif status["rtc"] != -1:
        raise TransportError("invalid RTC advertised as synchronized")
    elif status["wall"] != -1 and not (status["state"] == NO_RTC and status["code"] == "OK"):
        raise TransportError("invalid RTC advertised as synchronized")
    if status["state"] in (0, 3):
        if status["os"] == -1:
            raise TransportError("missing oscillator state")
    elif status["os"] != -1:
        raise TransportError("oscillator state unavailable on RTC communication failure")
    return status


def request(port, operation, timeout=5.0):
    request_id = secrets.token_hex(4)
    # Take trusted UTC reference after opening the port, immediately before TX.
    reference = datetime.now(timezone.utc).timestamp()
    timestamp = int(reference)
    if operation == "SET" and not MIN_TIMESTAMP <= timestamp <= MAX_TIMESTAMP:
        raise DeviceError("host UTC timestamp is outside 2000–2099")
    command = f"!RT1 {request_id} {operation}"
    if operation == "SET":
        command += f" {timestamp}"
    started = time.monotonic()
    deadline = started + timeout
    write_command(port, command)
    line = read_serial_reply(port, f"!RT1 {request_id} ".encode("ascii"), deadline, MAX_REPLY)
    status = parse_reply(line, request_id)
    status["round_trip"] = time.monotonic() - started
    status["sent"] = timestamp
    return status


def write_command(port, command):
    """One bounded serial write; shared by the two development utilities."""
    wire = ("\n" + command + "\n").encode("ascii")
    try:
        if port.write(wire) != len(wire):
            raise TransportError("short serial write")
    except OSError as error:
        raise TransportError(f"serial disconnected or I/O failed: {error}") from error


def read_serial_reply(port, prefix, deadline, limit):
    """Read one addressed line, ignoring logs, without unbounded flush/read."""
    try:
        line = bytearray()
        discard = False
        while time.monotonic() < deadline:
            byte = port.read(1)
            if not byte:
                continue
            if len(line) >= limit:
                if line.startswith(prefix):
                    raise TransportError("oversized protocol reply")
                discard = True
            if not discard:
                line.extend(byte)
            if byte == b"\n":
                reply = bytes(line)
                addressed = not discard and reply.startswith(prefix)
                line.clear()
                discard = False
                if addressed:
                    return reply
    except OSError as error:
        raise TransportError(f"serial disconnected or I/O failed: {error}") from error
    raise TransportError("response timeout (device disconnected, interface disabled, or lost frame)")


def reference(status):
    """The synchronized clock: the external RTC, or the system clock without one."""
    return status["wall"] if status["state"] == NO_RTC else status["rtc"]


def require_valid(status, host_now=None):
    if status["code"] != "OK" or status["error"] != 0:
        raise DeviceError(f"{status['code']}: RTC state={status['state']}, "
                          f"OS={status['os']}, error={status['error']}")
    if status["wall"] == -1:
        raise DeviceError("system clock is unsynchronized")
    if status["state"] != NO_RTC:
        if status["state"] != 0 or status["os"] != 0:
            raise DeviceError("RTC or system clock is unsynchronized")
        if abs(status["rtc"] - status["wall"]) > 1:
            raise DeviceError("RTC and system clock differ by more than 1 second")
    if host_now is None:
        host_now = datetime.now(timezone.utc).timestamp()
    # Response samples precede reception; bound transport latency separately.
    if status["round_trip"] > 3.0:
        raise DeviceError("serial round trip exceeds the 3-second validation budget")
    difference = reference(status) - host_now
    if abs(difference) > TOLERANCE_SECONDS:
        raise DeviceError(f"device clock differs from host UTC by {difference:+.2f}s (limit 3s)")
    return difference


def utc_text(timestamp):
    if timestamp == -1:
        return "unavailable / unsynchronized"
    return datetime.fromtimestamp(timestamp, timezone.utc).isoformat()


def show_status(status):
    states = ("valid", "not found", "communication failure", "invalid", "unavailable",
              "battery absent", "uncalibrated")
    if status["state"] == NO_RTC and status["code"] == "OK":
        available = "no (board without external RTC; system clock only)"
    else:
        available = "yes" if status["state"] in (0, 3) else "no / communication unavailable"
    print(f"RTC available: {available}; state: {states[status['state']]}")
    print(f"RTC UTC: {utc_text(status['rtc'])}; OS: {status['os']}")
    print(f"System UTC: {utc_text(status['wall'])}")
    print(f"Response: {status['code']}; device error: {status['error']}; "
          f"round trip: {status['round_trip']:.3f}s")
    if status["rtc"] != -1:
        difference = status["rtc"] - datetime.now(timezone.utc).timestamp()
        print(f"RTC minus host UTC: approximately {difference:+.2f}s (seconds resolution)")


def open_serial(path):
    try:
        import serial
    except ImportError as error:
        raise TransportError("pyserial is required: python -m pip install pyserial") from error
    try:
        # Linux CDC ACM asserts both lines on open. Keep that pair: forcing both
        # low with separate pyserial ioctls resets this ESP32-S3 USB peripheral.
        # Both asserted is the verified idle pair, with hardware flow disabled.
        port = serial.Serial(port=None, baudrate=115200, timeout=0.1,
                             write_timeout=0.5, exclusive=True)
        port.dtr = True
        port.rts = True
        port.port = path
        port.open()
        return port
    except (OSError, serial.SerialException) as error:
        raise TransportError(f"cannot open {path}: {error}; check permissions and other monitors") from error


def restart_verify(path):
    """Read-only RTC retention check across one explicit esp_restart()."""
    with open_serial(path) as port:
        before = request(port, "GET")
        require_valid(before)
        started = time.monotonic()
        acknowledged = request(port, "RESTART")
        require_valid(acknowledged)
    # USB may disappear briefly. Bounded reconnect; no line-state reset sequence.
    deadline = time.monotonic() + 15
    last_error = "no response"
    while time.monotonic() < deadline:
        time.sleep(0.25)
        try:
            with open_serial(path) as port:
                after = request(port, "GET", timeout=1.0)
            if (after.get("code") == "RTC_READ" and after.get("state") == NO_RTC and
                    after.get("error") == ESP_ERR_NOT_FOUND):
                # Early boot: the board is not published yet (wall_clock.c
                # answers NOT_FOUND without touching state). Ask again.
                last_error = "board not yet published after restart (RTC_READ, NOT_FOUND)"
                continue
            require_valid(after)
            elapsed = time.monotonic() - started
            advance = reference(after) - reference(before)
            if advance < 1 or abs(advance - elapsed) > 2:
                raise DeviceError("clock progression across restart is inconsistent")
            show_status(after)
            print(f"Software restart retention passed: clock advanced {advance}s "
                  f"over approximately {elapsed:.2f}s; OS=0 and system UTC restored.")
            return
        except TransportError as error:
            last_error = str(error)
    raise TransportError(f"reconnect deadline expired after restart: {last_error}")


ZONE_RULE = re.compile(r"[A-Za-z<][A-Za-z0-9<>+\-,./:]{0,46}")


def host_zone_rule(path="/etc/localtime"):
    """POSIX TZ rule from the footer of a TZif version 2+ file (RFC 8536 §3.3)."""
    with open(path, "rb") as source:
        data = source.read()
    if not data.startswith(b"TZif") or data[4:5] not in (b"2", b"3", b"4") or not data.endswith(b"\n"):
        raise DeviceError(f"{path} has no TZif v2+ footer")
    rule = data[data.rindex(b"\n", 0, len(data) - 1) + 1:-1].decode("ascii")
    if ZONE_RULE.fullmatch(rule) is None:
        raise DeviceError(f"unsupported host time-zone rule {rule!r}")
    return rule


def zone_request(port, rule=None, timeout=5.0):
    """RT1 TZ: set the rule when given, then return the device's active rule."""
    request_id = secrets.token_hex(4)
    write_command(port, f"!RT1 {request_id} TZ" + (f" {rule}" if rule else ""))
    prefix = f"!RT1 {request_id} ".encode("ascii")
    line = read_serial_reply(port, prefix, time.monotonic() + timeout, MAX_REPLY)
    match = re.fullmatch(rb"!RT1 [0-9a-f]{8} ([A-Z_]+) zone=([ -~]{1,47})\r?\n", line)
    if match is None:
        raise TransportError("invalid TZ reply")
    code, active = match.group(1).decode(), match.group(2).decode()
    if code != "OK" or (rule is not None and active != rule):
        raise DeviceError(f"time zone not applied ({code}); active {active}")
    return active


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="serial path, preferably /dev/serial/by-id/…")
    parser.add_argument("command", choices=("status", "sync", "verify", "tz"))
    parser.add_argument("--rule", help="with tz: POSIX TZ rule; default: this host's /etc/localtime")
    parser.add_argument("--query", action="store_true", help="with tz: only report the active rule")
    parser.add_argument("--restart", action="store_true",
                        help="with verify, explicitly request software restart and check RTC retention")
    args = parser.parse_args(argv)
    if args.restart and args.command != "verify":
        parser.error("--restart is only supported with verify")
    try:
        if args.restart:
            restart_verify(args.port)
            return 0
        if args.command == "tz":
            rule = None if args.query else (args.rule or host_zone_rule())
            if rule is not None and ZONE_RULE.fullmatch(rule) is None:
                parser.error("invalid POSIX TZ rule")
            with open_serial(args.port) as port:
                print(f"Device time zone: {zone_request(port, rule)}")
            return 0
        with open_serial(args.port) as port:
            status = request(port, "SET" if args.command == "sync" else "GET")
            show_status(status)
            if args.command == "status":
                return 0
            require_valid(status)
            if args.command == "sync":
                # A separate, read-only transaction confirms the SET result.
                readback = request(port, "GET")
                require_valid(readback)
                if not reference(status) <= reference(readback) <= reference(status) + 3:
                    raise DeviceError("independent readback differs from SET acknowledgment")
                print("Synchronization confirmed by RTC progression, OS=0 and independent readback.")
            else:
                print("Read-only verification passed (RTC, system and host within tolerance).")
        return 0
    except TransportError as error:
        print(f"Transport error: {error}", file=sys.stderr)
        return 2
    except DeviceError as error:
        print(f"RTC/time validation error: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
