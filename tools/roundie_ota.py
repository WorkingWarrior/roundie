#!/usr/bin/env python3
"""Bounded development USB OTA; SHA-256 is integrity, not origin authentication."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets
import struct
import sys
import time

from roundie_time import (BUSY_CODE, PRECONDITION_CODE, DeviceBusy, DeviceError, TransportError, open_serial,
                          read_serial_reply, retry_busy, write_command)

CHUNK = 512
CAPACITY = 0x380000
HEX64 = re.compile(r"[0-9a-f]{64}\Z")
# RO1 "refusal": why a request was refused before execution (docs/storage-io.md, P1 fix 3).
REFUSALS = {
    "none": "",
    "io": "io_worker is starting a step; retried automatically",
    "app": "an application ELF is loaded: stop and unload it first (`roundie_elf.py stop --instance N`, "
           "then `roundie_elf.py unload --instance N`)",
    "transfer": "an RE1/RP1/RF1 upload is open: finish it or abort it (`roundie_elf.py abort --session S` "
                "or `roundie_package.py abort --session S`)",
    "package": "a package job is running: wait until `roundie_package.py status` shows busy 0",
    "safe-mode": "the device is in Safe Mode, where a new image could never confirm: leave it first "
                 "(`roundie_safe.py reboot`)",
    "not-ready": "restart is not allowed in this OTA state: activate a verified image first",
}
PHASES = {"IDLE", "RECEIVING", "VERIFYING", "READY", "ACTIVATION_PENDING"}
STATES = {"NEW", "PENDING_VERIFY", "VALID", "INVALID", "ABORTED", "UNDEFINED", "NO_METADATA"}


def parse_reply(line, request_id):
    prefix = f"!RO1 {request_id} ".encode("ascii")
    if not line.startswith(prefix):
        return None
    try:
        if not line.endswith(b"\n") or len(line) > 1024:
            raise ValueError("length/framing")
        code, raw = line[len(prefix):].split(b" ", 1)
        if not re.fullmatch(rb"-?[0-9]{1,10}", code):
            raise ValueError("error code")
        status = json.loads(raw)
        required = {"phase", "session", "received", "total", "running", "address", "ota_state",
                    "other", "other_state", "other_image", "other_elf", "boot", "version", "elf",
                    "digest", "rollback", "checked", "passed", "last_error"}
        # Images before P1 fix 3 send no "refusal"; OTA from them must still work.
        if not isinstance(status, dict) or set(status) not in (required, required | {"refusal"}):
            raise ValueError("status fields")
        status.setdefault("refusal", "none")
        if status["phase"] not in PHASES or status["ota_state"] not in STATES or status["other_state"] not in STATES:
            raise ValueError("state")
        for field in ("received", "total"):
            if type(status[field]) is not int or not 0 <= status[field] <= CAPACITY:
                raise ValueError(field)
        if status["received"] > status["total"]:
            raise ValueError("progress")
        if not isinstance(status["session"], str) or not re.fullmatch(r"[0-9a-f]{8}", status["session"]):
            raise ValueError("session")
        for field in ("other_image", "rollback", "checked", "passed"):
            if type(status[field]) is not int or status[field] not in (0, 1):
                raise ValueError(field)
        for field in ("elf", "other_elf", "digest"):
            if not isinstance(status[field], str) or (status[field] and not HEX64.fullmatch(status[field])):
                raise ValueError(field)
        for field in ("running", "other", "boot"):
            if status[field] not in ("", "ota_0", "ota_1"):
                raise ValueError(field)
        if type(status["address"]) is not int or status["address"] not in (0, 0x10000, 0x390000):
            raise ValueError("address")
        if status["refusal"] not in REFUSALS:
            raise ValueError("refusal")
        if (int(code) == BUSY_CODE) != (status["refusal"] == "io"):
            raise ValueError("busy code/refusal")
        if type(status["last_error"]) is not int or not isinstance(status["version"], str) or len(status["version"]) > 32:
            raise ValueError("descriptor/result")
    except (ValueError, TypeError, KeyError) as error:
        raise TransportError(f"malformed OTA response: {error}") from error
    if int(code) == BUSY_CODE:
        raise DeviceBusy(f"OTA busy ({BUSY_CODE}): {REFUSALS['io']}")
    if int(code) == PRECONDITION_CODE and status["refusal"] != "none":
        raise DeviceError(f"OTA refused ({int(code)}, precondition, not retried): {REFUSALS[status['refusal']]}")
    if int(code) != 0:
        raise DeviceError(f"OTA rejected ({int(code)}): {status}")
    return status


def request(port, command, timeout=8.0):
    def send():
        request_id = secrets.token_hex(4)
        deadline = time.monotonic() + timeout
        write_command(port, f"!RO1 {request_id} {command}")
        reply = read_serial_reply(port, f"!RO1 {request_id} ".encode("ascii"), deadline, 1024)
        return parse_reply(reply, request_id)
    # BUSY (503) means the request was not executed, so sending it again is safe.
    return retry_busy(send)


def inspect_image(stream):
    """Bounded host preflight; target/IDF performs authoritative validation."""
    stream.seek(0, 2)
    size = stream.tell()
    if not 288 <= size <= CAPACITY:
        raise DeviceError("image does not fit the 3.5 MiB OTA slot")
    stream.seek(0)
    prefix = stream.read(288)
    if (len(prefix) != 288 or prefix[0] != 0xe9 or not 1 <= prefix[1] <= 16 or
            struct.unpack_from("<H", prefix, 12)[0] != 9 or
            struct.unpack_from("<I", prefix, 32)[0] != 0xabcd5432):
        raise DeviceError("not an ESP32-S3 application image with an IDF descriptor")
    project = prefix[80:112].split(b"\0", 1)[0]
    if project != b"roundie_rep":
        raise DeviceError("image is not identified as roundie_rep")
    digest = hashlib.sha256()
    digest.update(prefix)
    for block in iter(lambda: stream.read(8192), b""):
        digest.update(block)
    stream.seek(0)
    return {"size": size, "digest": digest.hexdigest(), "elf": prefix[176:208].hex(),
            "version": prefix[48:80].split(b"\0", 1)[0].decode("ascii", errors="replace")}


def upload(port, stream, progress=print):
    image = inspect_image(stream)
    before = request(port, "STATUS")
    if before["ota_state"] != "VALID" or not before["passed"] or before["boot"] != before["running"]:
        raise DeviceError("upload requires self-tested VALID firmware and unchanged boot selection")
    if before["phase"] != "IDLE":
        raise DeviceError("another update or activation is pending")
    session = secrets.token_hex(4)
    while session == "00000000":
        session = secrets.token_hex(4)
    begun = False
    try:
        # Mark before TX: a lost BEGIN reply can still leave a live session.
        begun = True
        request(port, f"BEGIN {session} {image['size']} {image['digest']}")
        offset = 0
        next_progress = 0
        while offset < image["size"]:
            block = stream.read(min(CHUNK, image["size"] - offset))
            if not block:
                raise DeviceError("image truncated during upload")
            status = request(port, f"DATA {session} {offset} {block.hex()}")
            offset += len(block)
            if status["phase"] != "RECEIVING" or status["received"] != offset:
                raise TransportError("device acknowledged inconsistent progress")
            if offset >= next_progress:
                progress(f"{offset}/{image['size']} bytes ({offset * 100 // image['size']}%)")
                next_progress = offset + 32768
        ready = request(port, f"END {session}")
        if (ready["phase"] != "READY" or ready["digest"] != image["digest"] or
                ready["other_elf"] != image["elf"] or not ready["other_image"]):
            raise DeviceError("verified image identity does not match upload")
        begun = False
        return ready
    finally:
        if begun:
            try:
                request(port, f"ABORT {session}", timeout=1.0)
            except (TransportError, DeviceError):
                pass  # Target expires incomplete sessions after 15 seconds.


def verify(status, image=None):
    if (status["ota_state"] != "VALID" or not status["checked"] or not status["passed"] or
            status["boot"] != status["running"] or status["phase"] != "IDLE"):
        raise DeviceError("running firmware is not self-tested VALID with stable boot selection")
    if image is not None and status["elf"] != image["elf"]:
        raise DeviceError("running ELF identity does not match expected image")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="explicit device path or stable /dev/serial/by-id alias")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    sub.add_parser("upload").add_argument("image", type=Path)
    activation = sub.add_parser("activate")
    activation.add_argument("--sha256", required=True, help="exact digest returned by upload")
    activation.add_argument("--restart", action="store_true", help="explicitly restart after selection")
    sub.add_parser("restart")
    sub.add_parser("abort").add_argument("--session", required=True)
    sub.add_parser("verify").add_argument("--image", type=Path)
    args = parser.parse_args(argv)
    if args.command == "activate" and not HEX64.fullmatch(args.sha256):
        parser.error("--sha256 must be 64 lowercase hexadecimal digits")
    if args.command == "abort" and (not re.fullmatch(r"[0-9a-f]{8}", args.session) or int(args.session, 16) == 0):
        parser.error("--session must be 8 nonzero lowercase hexadecimal digits")
    try:
        with open_serial(args.port) as port:
            if args.command == "upload":
                with args.image.open("rb") as stream:
                    status = upload(port, stream)
            elif args.command == "activate":
                status = request(port, f"ACTIVATE {args.sha256}")
                if status["phase"] != "ACTIVATION_PENDING" or status["boot"] != status["other"]:
                    raise DeviceError("boot selection did not match staged image")
                if args.restart:
                    status = request(port, "RESTART")
            elif args.command == "restart":
                status = request(port, "RESTART")
            elif args.command == "abort":
                status = request(port, f"ABORT {args.session}")
            else:
                status = request(port, "STATUS")
                if args.command == "verify":
                    image = None
                    if args.image is not None:
                        with args.image.open("rb") as stream:
                            image = inspect_image(stream)
                    verify(status, image)
        print(json.dumps(status, indent=2, sort_keys=True))
        return 0
    except TransportError as error:
        print(error, file=sys.stderr)
        return 2
    except (DeviceError, OSError) as error:
        print(error, file=sys.stderr)
        return 3
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
