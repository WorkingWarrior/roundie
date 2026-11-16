#!/usr/bin/env python3
"""RPK1/RP1 trusted development packages: explicit install and launch, no signing."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets
import struct
import subprocess
import sys
import tempfile
import time

import roundie_elf
from roundie_time import DeviceError, TransportError, open_serial, read_serial_reply, write_command

ROOT = Path(__file__).resolve().parents[1]
PREFIX = 224
MAX_FILE = PREFIX + 65536
ID = re.compile(r"[a-z][a-z0-9]*(?:\.[a-z][a-z0-9]*)+\Z")
VERSION = re.compile(r"(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\Z")
HEX64 = re.compile(r"[0-9a-f]{64}\Z")


def valid_id(value):
    if len(value) >= 64 or ID.fullmatch(value) is None:
        raise DeviceError("ID must be 3..63 lowercase ASCII bytes in dotted letter/digit segments")
    return value


def inspect(path, target="esp32s3.rep1"):
    path = Path(path).resolve()
    if not PREFIX + 52 <= path.stat().st_size <= MAX_FILE:
        raise DeviceError("package size out of bounds")
    with tempfile.TemporaryDirectory(prefix="rep-package-check-") as temporary:
        checker = str(Path(temporary) / "validate")
        subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "include"),
                        "-I" + str(ROOT / "components/roundie_elf/include"),
                        "-I" + str(ROOT / "components/roundie_packages/include"),
                        str(ROOT / "tools/validate_package.c"),
                        str(ROOT / "components/roundie_packages/format.c"),
                        *roundie_elf.checker_sources(target), "-lcrypto", "-o", checker],
                       check=True, capture_output=True)
        checked = subprocess.run([checker, str(path)], capture_output=True, text=True)
        if checked.returncode:
            raise DeviceError(checked.stderr.strip())
        result = json.loads(checked.stdout)
    data = path.read_bytes()
    result.update(id=data[64:128].split(b"\0", 1)[0].decode("ascii"),
                  name=data[128:192].split(b"\0", 1)[0].decode("utf-8"),
                  version=".".join(map(str, struct.unpack_from("<3H", data, 192))),
                  target=data[200:216].split(b"\0", 1)[0].decode("utf-8"),
                  platform_api=struct.unpack_from("<H", data, 216)[0],
                  elf_sha256=data[24:56].hex(), sha256=hashlib.sha256(data).hexdigest())
    return result


def build(elf, output, app_id, name, version, target="esp32s3.rep1"):
    valid_id(app_id)
    match = VERSION.fullmatch(version)
    if match is None or any(int(part) > 65535 for part in match.groups()):
        raise DeviceError("version must be canonical MAJOR.MINOR.PATCH with components 0..65535")
    encoded_name = name.encode("utf-8")
    if not 1 <= len(encoded_name) <= 63 or b"\0" in encoded_name:
        raise DeviceError("name must contain 1..63 UTF-8 bytes")
    info = roundie_elf.inspect(elf, target)
    payload = Path(elf).read_bytes()
    if len(payload) != info["size"] or hashlib.sha256(payload).hexdigest() != info["sha256"]:
        raise DeviceError("ELF changed while packaging")
    header = struct.pack("<4sHHIIII32s8x", b"RPK1", 1, 64, PREFIX + len(payload), 160,
                         len(payload), 0, hashlib.sha256(payload).digest())
    manifest = struct.pack("<64s64s3HH16sH6x", app_id.encode("ascii"), encoded_name,
                           *(int(part) for part in match.groups()), info["abi"], target.encode("ascii"), 1)
    # Validate exact candidate before publishing the requested host artifact.
    output = Path(output)
    with tempfile.TemporaryDirectory(prefix="rep-package-build-", dir=output.parent) as temporary:
        candidate = Path(temporary) / "candidate.rpk"
        candidate.write_bytes(header + manifest + payload)
        checked = inspect(candidate, target)
        candidate.replace(output)
    return checked


def parse_reply(line, request_id):
    prefix = f"!RP1 {request_id} ".encode()
    if not line.startswith(prefix):
        return None
    try:
        if len(line) > 1900 or not line.endswith(b"\n"):
            raise ValueError("framing")
        code, raw = line[len(prefix):].split(b" ", 1)
        if re.fullmatch(rb"-?[0-9]{1,10}", code) is None:
            raise ValueError("code")
        status = json.loads(raw)
        if status["format"] != 1:
            raise ValueError("format")
        for key in ("allowed", "busy"):
            if type(status[key]) is not int or status[key] not in (0, 1):
                raise ValueError(key)
        for key in ("phase", "received", "total", "count", "orphans", "elf_size"):
            if type(status[key]) is not int or not 0 <= status[key] <= 0xffffffff:
                raise ValueError(key)
        if not (status["phase"] <= 5 and status["received"] <= status["total"] <= MAX_FILE and status["count"] <= 8):
            raise ValueError("limits")
        if re.fullmatch(r"[0-9a-f]{8}", status["session"]) is None:
            raise ValueError("session")
        for key, length in (("us", 7), ("capacity", 3)):
            if (not isinstance(status[key], list) or len(status[key]) != length or
                    any(type(v) is not int or not 0 <= v <= 0xffffffff for v in status[key])):
                raise ValueError(key)
        if not isinstance(status["apps"], list) or len(status["apps"]) > 8:
            raise ValueError("apps")
        seen = set()
        for app in status["apps"]:
            if (not isinstance(app, list) or len(app) != 4 or not isinstance(app[0], str) or
                    not isinstance(app[1], str) or VERSION.fullmatch(app[1]) is None or
                    type(app[2]) is not int or not 0 <= app[2] <= 65535 or
                    type(app[3]) is not int or app[3] not in (0, 1, 2) or app[0] in seen):
                raise ValueError("app record")
            valid_id(app[0])
            seen.add(app[0])
        if (not isinstance(status["name"], str) or len(status["name"].encode("utf-8")) > 63 or
                not isinstance(status["error"], str) or len(status["error"]) >= 80 or
                not isinstance(status["sha256"], str) or
                (status["sha256"] and HEX64.fullmatch(status["sha256"]) is None)):
            raise ValueError("metadata")
    except (ValueError, TypeError, KeyError, DeviceError) as error:
        raise TransportError(f"malformed package response: {error}") from error
    if int(code) != 0:
        raise DeviceError(f"package rejected ({int(code)}): {status}")
    return status


def request(port, operation, timeout=8):
    request_id = secrets.token_hex(4)
    write_command(port, f"!RP1 {request_id} {operation}")
    return parse_reply(read_serial_reply(port, f"!RP1 {request_id} ".encode(),
                                        time.monotonic() + timeout, 1900), request_id)


def upload(port, path, info):
    session = secrets.token_hex(4)
    while session == "00000000":
        session = secrets.token_hex(4)
    begun = False
    started = time.monotonic()
    try:
        begun = True
        request(port, f"BEGIN {session} {info['size']} {info['sha256']}")
        with Path(path).open("rb") as stream:
            offset = 0
            while offset < info["size"]:
                data = stream.read(min(512, info["size"] - offset))
                if not data:
                    raise DeviceError("package changed during upload")
                response = request(port, f"DATA {session} {offset} {data.hex()}")
                offset += len(data)
                if response["received"] != offset or response["phase"] != 1:
                    raise TransportError("inconsistent transfer acknowledgement")
            if stream.read(1):
                raise DeviceError("package grew during upload")
        result = request(port, f"END {session}")
        if result["phase"] != 4:
            raise TransportError("package was not validated READY")
        begun = False
        result["host_upload_seconds"] = time.monotonic() - started
        return result
    finally:
        if begun:
            try:
                request(port, f"ABORT {session}", timeout=1)
            except (DeviceError, TransportError):
                pass


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port")
    sub = parser.add_subparsers(dest="command", required=True)
    pack = sub.add_parser("build")
    pack.add_argument("elf", type=Path)
    pack.add_argument("--output", type=Path, required=True)
    pack.add_argument("--id", required=True)
    pack.add_argument("--name", required=True)
    pack.add_argument("--version", required=True)
    pack.add_argument("--target", choices=roundie_elf.TARGETS, default="esp32s3.rep1")
    for name in ("inspect", "upload"):
        command = sub.add_parser(name)
        command.add_argument("file", type=Path)
        if name == "upload":
            command.add_argument("--install", action="store_true")
        else:
            command.add_argument("--target", choices=roundie_elf.TARGETS, default="esp32s3.rep1")
    for name in ("info", "launch", "remove"):
        sub.add_parser(name).add_argument("id")
    sub.add_parser("status")
    sub.add_parser("list")
    sub.add_parser("install").add_argument("--sha256", required=True)
    sub.add_parser("abort").add_argument("--session", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "build":
            result = build(args.elf, args.output, args.id, args.name, args.version, args.target)
        elif args.command == "inspect":
            result = inspect(args.file, args.target)
        else:
            if args.port is None:
                parser.error("--port required for device operations")
            if args.command in ("info", "launch", "remove"):
                valid_id(args.id)
            if args.command == "install" and HEX64.fullmatch(args.sha256) is None:
                raise DeviceError("invalid package digest")
            if args.command == "abort" and (re.fullmatch(r"[0-9a-f]{8}", args.session) is None or int(args.session, 16) == 0):
                raise DeviceError("invalid session")
            artifact = inspect(args.file) if args.command == "upload" else None
            with open_serial(args.port) as port:
                if args.command == "upload":
                    result = upload(port, args.file, artifact)
                    duration = result["host_upload_seconds"]
                    if args.install:
                        result = request(port, f"INSTALL {artifact['sha256']}")
                        result["host_upload_seconds"] = duration
                    result["artifact"] = artifact
                elif args.command in ("info", "launch", "remove"):
                    result = request(port, f"{args.command.upper()} {args.id}")
                elif args.command == "install":
                    result = request(port, f"INSTALL {args.sha256}")
                elif args.command == "abort":
                    result = request(port, f"ABORT {args.session}")
                else:
                    result = request(port, args.command.upper())
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return 0
    except TransportError as error:
        print(f"Transport error: {error}", file=sys.stderr)
        return 2
    except (DeviceError, OSError, subprocess.CalledProcessError, UnicodeError) as error:
        print(f"Package error: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
