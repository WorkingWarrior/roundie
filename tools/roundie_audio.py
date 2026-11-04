#!/usr/bin/env python3
"""Bounded RA1 audio diagnostics: status, a test tone and stop. The tone volume is a
share of the system cap; the speaker connected to header H1 is the only audible check."""
import argparse
import json
import re
import secrets
import sys
import time

from roundie_time import DeviceError, TransportError, open_serial, read_serial_reply, write_command


def parse_reply(line, request_id):
    prefix = f"!RA1 {request_id} ".encode()
    if not line.startswith(prefix):
        return None
    try:
        if not line.endswith(b"\n") or len(line) > 320:
            raise ValueError("length/framing")
        code_text, payload = line[len(prefix):].split(b" ", 1)
        code = int(code_text)
        status = json.loads(payload)
        for key in ("ready", "ack", "playing"):
            if status[key] not in (0, 1):
                raise ValueError(f"invalid {key}")
        for key in ("plays", "errors", "volume", "cap"):
            if type(status[key]) is not int or status[key] < 0:
                raise ValueError(f"invalid {key}")
        if re.fullmatch(r"[0-9a-f]{6}", status["id"]) is None:
            raise ValueError("invalid id")
    except (ValueError, KeyError, TypeError) as error:
        raise TransportError(f"malformed audio response: {error}") from error
    if code != 0:
        raise DeviceError(f"audio command rejected ({code}): {status}")
    return status


def request(port, operation, timeout=4.0):
    if len(operation) > 40 or re.fullmatch(r"[A-Z0-9 ]+", operation) is None:
        raise DeviceError("invalid bounded audio command")
    request_id = secrets.token_hex(4)
    deadline = time.monotonic() + timeout
    write_command(port, f"!RA1 {request_id} {operation}")
    line = read_serial_reply(port, f"!RA1 {request_id} ".encode(), deadline, 320)
    return parse_reply(line, request_id)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status")
    commands.add_parser("stop")
    tone = commands.add_parser("tone")
    tone.add_argument("frequency", type=int, help="50..8000 Hz")
    tone.add_argument("duration_ms", type=int, help="10..5000 ms")
    tone.add_argument("volume", type=int, help="0..100, a share of the system cap")
    args = parser.parse_args(argv)
    try:
        operation = {"status": "STATUS", "stop": "STOP"}.get(args.command)
        if args.command == "tone":
            operation = f"TONE {args.frequency} {args.duration_ms} {args.volume}"
        with open_serial(args.port) as port:
            print(json.dumps(request(port, operation), indent=2))
        return 0
    except TransportError as error:
        print(f"Transport error: {error}", file=sys.stderr)
        return 2
    except DeviceError as error:
        print(f"Device error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
