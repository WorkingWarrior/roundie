#!/usr/bin/env python3
"""RWF1 watch faces: compile declarative XML on the host, inspect packages,
and drive the RF1 store on a device (docs/watch-face-packages.md).

The XML is only an authoring format: the device receives the compiled,
bounded binary and never parses XML. Nothing in a face is executed."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets
import struct
import sys
import time
import xml.etree.ElementTree as ElementTree

ROOT = Path(__file__).resolve().parents[1]

HEADER = 64
MANIFEST = 96
SECTIONS = 16
PREFIX = HEADER + MANIFEST + SECTIONS
RESOURCE_ENTRY = 16
PACKAGE_MAX = 65536
DEFINITION_MAX = 4096
ELEMENTS_MAX = 32
SETTINGS_MAX = 4
OPTIONS_MAX = 8
STRINGS_MAX = 1024
STRING_MAX = 63
RESOURCES_MAX = 8
RESOURCE_BYTES_MAX = 49152
DEPTH_MAX = 4
NO_PARENT = 0xFF
NO_TEXT = 0xFFFF
NO_PREVIEW = 0xFF
RADIUS_CIRCLE = 0xFFFF
ABI_MAJOR = 1
ABI_MINOR = 0
BACKEND_DECLARATIVE = 1

NEED_SECOND, NEED_BATTERY, NEED_TAP = 1, 2, 4
CAP_RESOURCES, CAP_SETTINGS, CAP_BATTERY = 1, 2, 4
FORMAT_RGB565, FORMAT_A8 = 1, 2

LABEL, RECT, ARC, IMAGE, HAND, TICKS = 1, 2, 3, 4, 5, 6
FONTS = {"14": 0, "20": 1, "28": 2, "48": 3}
ALIGN = {"left": 0, "center": 1, "right": 2}
HANDS = {"hour": 0, "minute": 1, "second": 2}
BINDINGS = {"battery": 1, "second": 2, "minute": 3, "hour": 4}
CONDITIONS = {"always": 0, "time-valid": 1, "time-invalid": 2, "battery-valid": 3, "charging": 4,
              "not-charging": 7}
SETTING_IS, SETTING_NOT = 5, 6
ACTIONS = {"none": 0, "cycle": 1, "refresh": 2}
TOKENS = {"HH", "H", "hh", "h", "MM", "SS", "DD", "D", "MO", "MON", "MONTH", "WD", "WEEKDAY", "YYYY", "UTC",
          "OFF", "TZ", "BAT", "CHG"}

ID = re.compile(r"[a-z][a-z0-9]*(?:\.[a-z][a-z0-9]*)+\Z")
VERSION = re.compile(r"(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\Z")
KEY = re.compile(r"[a-z0-9_]{1,15}\Z")
HEX64 = re.compile(r"[0-9a-f]{64}\Z")


class FaceError(Exception):
    """The source or package is invalid."""


def valid_id(value):
    if not 3 <= len(value) < 48 or ID.fullmatch(value) is None:
        raise FaceError(f"invalid face id {value!r}: 3..47 lowercase bytes in dotted letter/digit segments")
    return value


def template_tokens(text):
    """The tokens of a label template, as the device parses it."""
    tokens = []
    i = 0
    while i < len(text):
        if text.startswith("{{", i):
            i += 2
        elif text[i] == "{":
            close = text.find("}", i + 1)
            if close < 0 or text[i + 1:close] not in TOKENS:
                raise FaceError(f"unknown token in {text!r}")
            tokens.append(text[i + 1:close])
            i = close + 1
        elif text[i] == "}":
            raise FaceError(f"unmatched '}}' in {text!r}")
        else:
            if ord(text[i]) < 0x20 or ord(text[i]) == 0x7f:
                raise FaceError(f"control character in {text!r}")
            i += 1
    return tokens


def integer(node, name, default=None, low=-32768, high=65535):
    raw = node.get(name)
    if raw is None:
        if default is None:
            raise FaceError(f"<{node.tag}> needs {name}")
        return default
    if re.fullmatch(r"-?[0-9]{1,6}", raw) is None:
        raise FaceError(f"<{node.tag}> {name}={raw!r} is not an integer")
    value = int(raw)
    if not low <= value <= high:
        raise FaceError(f"<{node.tag}> {name}={value} outside {low}..{high}")
    return value


def position(node, name, default=None):
    return integer(node, name, default, -360, 720)


def color(node, name, default=None):
    raw = node.get(name)
    if raw is None:
        if default is None:
            raise FaceError(f"<{node.tag}> needs {name}")
        return default
    if re.fullmatch(r"#[0-9a-fA-F]{6}", raw) is None:
        raise FaceError(f"<{node.tag}> {name}={raw!r} is not #RRGGBB")
    return int(raw[1:], 16)


def choice(node, name, table, default):
    raw = node.get(name, default)
    if raw not in table:
        raise FaceError(f"<{node.tag}> {name}={raw!r}: one of {', '.join(table)}")
    return table[raw]


def read_netpbm(path):
    """Binary PGM (P5, A8) or PPM (P6, RGB565): no image library needed."""
    data = Path(path).read_bytes()
    match = re.match(rb"(P[56])\s+(?:#[^\n]*\n\s*)*([0-9]+)\s+([0-9]+)\s+(255)\s", data)
    if match is None:
        raise FaceError(f"{path}: expected a binary PGM (P5) or PPM (P6) with maxval 255")
    kind, width, height = match.group(1), int(match.group(2)), int(match.group(3))
    pixels = data[match.end():]
    if not (1 <= width <= 360 and 1 <= height <= 360):
        raise FaceError(f"{path}: {width}x{height} outside 1..360")
    if kind == b"P5":
        if len(pixels) != width * height:
            raise FaceError(f"{path}: pixel data length")
        return FORMAT_A8, width, height, bytes(pixels)
    if len(pixels) != width * height * 3:
        raise FaceError(f"{path}: pixel data length")
    out = bytearray()
    for i in range(0, len(pixels), 3):
        r, g, b = pixels[i], pixels[i + 1], pixels[i + 2]
        out += struct.pack("<H", (r >> 3) << 11 | (g >> 2) << 5 | b >> 3)
    return FORMAT_RGB565, width, height, bytes(out)


class Compiler:
    def __init__(self, source):
        self.source = Path(source)
        self.settings = []  # (key, options, default)
        self.resources = []  # (name, format, w, h, bytes)
        self.elements = []  # packed 32-byte entries
        self.kinds = []  # element types, for parent checks
        self.strings = bytearray()
        self.string_offsets = {}
        self.second = self.battery = self.tap = False

    def setting_index(self, key):
        for index, setting in enumerate(self.settings):
            if setting[0] == key:
                return index
        raise FaceError(f"unknown setting {key!r}")

    def string(self, text):
        encoded = text.encode("utf-8")
        if len(encoded) > STRING_MAX or b"\0" in encoded:
            raise FaceError(f"label text longer than {STRING_MAX} bytes: {text!r}")
        if text not in self.string_offsets:
            self.string_offsets[text] = len(self.strings)
            self.strings += encoded + b"\0"
        return self.string_offsets[text]

    def condition(self, node):
        raw = node.get("when", "always")
        match = re.fullmatch(r"setting:([a-z0-9_]+)(!?=)([0-9])", raw)
        if match:
            index = self.setting_index(match.group(1))
            value = int(match.group(3))
            if value >= self.settings[index][1]:
                raise FaceError(f"{raw!r}: option {value} out of range")
            return (SETTING_IS if match.group(2) == "=" else SETTING_NOT), index, value
        if raw not in CONDITIONS:
            raise FaceError(f"when={raw!r}: one of {', '.join(CONDITIONS)} or setting:key=N / setting:key!=N")
        code = CONDITIONS[raw]
        if code in (3, 4, 7):
            self.battery = True
        return code, 0, 0

    def action(self, node):
        code = choice(node, "action", ACTIONS, "none")
        target = 0
        if code == 1:
            target = self.setting_index(node.get("setting", ""))
        elif node.get("setting") is not None:
            raise FaceError(f"<{node.tag}> setting= only with action=cycle")
        if code != 0:
            self.tap = True
        return code, target

    def element(self, node, parent, depth):
        tag = node.tag
        kind = {"label": LABEL, "rect": RECT, "arc": ARC, "image": IMAGE, "hand": HAND, "ticks": TICKS}.get(tag)
        if kind is None:
            raise FaceError(f"unknown element <{tag}>")
        if len(self.elements) >= ELEMENTS_MAX:
            raise FaceError(f"more than {ELEMENTS_MAX} elements")
        if depth >= DEPTH_MAX:
            raise FaceError("rect nesting deeper than 3")
        variant = align = w = h = size = extra = 0
        x = y = 0
        color1 = color(node, "color", 0xFFFFFF)
        color2 = 0
        text = NO_TEXT
        if kind == LABEL:
            variant = choice(node, "font", FONTS, "20")
            align = choice(node, "align", ALIGN, "left")
            x, y = position(node, "x"), position(node, "y")
            w = integer(node, "w", 0, 0, 360)
            raw = node.get("text")
            if raw is None or raw == "":
                raise FaceError("<label> needs text")
            tokens = template_tokens(raw)
            self.second = self.second or "SS" in tokens
            self.battery = self.battery or "BAT" in tokens or "CHG" in tokens
            text = self.string(raw)
        elif kind == RECT:
            x, y = position(node, "x"), position(node, "y")
            w, h = integer(node, "w", None, 1, 360), integer(node, "h", None, 1, 360)
            radius = node.get("radius", "0")
            size = RADIUS_CIRCLE if radius == "circle" else integer(node, "radius", 0, 0, 0xFFFE)
            extra = integer(node, "border", 0, 0, 255)
            color2 = color(node, "border-color", 0xFFFFFF)
        elif kind == ARC:
            variant = choice(node, "bind", BINDINGS, None)
            if variant == 1:
                self.battery = True
            if variant == 2:
                self.second = True
            diameter = integer(node, "size", None, 1, 360)
            x, y, w, h = position(node, "x"), position(node, "y"), diameter, diameter
            size = integer(node, "width", None, 1, diameter // 2)
            color2 = color(node, "track", 0x202020)
        elif kind == IMAGE:
            name = node.get("resource")
            names = [resource[0] for resource in self.resources]
            if name not in names:
                raise FaceError(f"<image> unknown resource {name!r}")
            variant = names.index(name)
            x, y = position(node, "x"), position(node, "y")
        elif kind == HAND:
            variant = choice(node, "kind", HANDS, None)
            self.second = self.second or variant == 2
            x, y = position(node, "cx", 180), position(node, "cy", 180)
            w = integer(node, "length", None, 1, 180)
            size = integer(node, "width", None, 1, 16)
            extra = integer(node, "tail", 0, 0, 60)
        else:
            count = node.get("count")
            if count not in ("12", "60"):
                raise FaceError("<ticks> count must be 12 or 60")
            variant = int(count)
            x, y = position(node, "cx", 180), position(node, "cy", 180)
            w = integer(node, "radius", None, 1, 180)
            size = integer(node, "length", None, 1, w)
            extra = integer(node, "major", 0, 0, w)
            color2 = color(node, "major-color", color1)
        opacity = integer(node, "opacity", 255, 0, 255)
        condition, condition_setting, condition_value = self.condition(node)
        action, action_setting = self.action(node)
        index = len(self.elements)
        self.kinds.append(kind)
        self.elements.append(struct.pack("<BBBBhhHHIIHHBBBBBBBB", kind, parent, variant, align, x, y, w, h,
                                         color1, color2, text, size, opacity, condition, condition_setting,
                                         condition_value, action, action_setting, extra, 0))
        children = list(node)
        if children and kind != RECT:
            raise FaceError(f"<{tag}> cannot contain elements; only <rect> can")
        for child in children:
            self.element(child, index, depth + 1)

    def compile(self):
        text = self.source.read_text(encoding="utf-8")
        if "<!DOCTYPE" in text or "<!ENTITY" in text:
            raise FaceError("DTDs and entities are not accepted")
        try:
            root = ElementTree.fromstring(text)
        except ElementTree.ParseError as error:
            raise FaceError(f"XML: {error}") from error
        if root.tag != "face":
            raise FaceError("root element must be <face>")
        face_id = valid_id(root.get("id", ""))
        name = root.get("name", "")
        encoded_name = name.encode("utf-8")
        if not 1 <= len(encoded_name) < 32 or any(ord(c) < 0x20 for c in name):
            raise FaceError("name: 1..31 UTF-8 bytes, no control characters")
        version = root.get("version", "")
        if VERSION.fullmatch(version) is None:
            raise FaceError("version must be MAJOR.MINOR.PATCH")
        versions = [int(part) for part in version.split(".")]
        background = color(root, "background", 0x000000)
        preview = NO_PREVIEW
        body = []
        for node in root:
            if node.tag == "setting":
                key = node.get("key", "")
                if KEY.fullmatch(key) is None or any(s[0] == key for s in self.settings):
                    raise FaceError(f"setting key {key!r}: [a-z0-9_]{{1,15}}, unique")
                options = integer(node, "options", None, 2, OPTIONS_MAX)
                default = integer(node, "default", 0, 0, options - 1)
                if len(self.settings) >= SETTINGS_MAX:
                    raise FaceError(f"more than {SETTINGS_MAX} settings")
                self.settings.append((key, options, default))
            elif node.tag == "resource":
                rname = node.get("name", "")
                if not rname or any(r[0] == rname for r in self.resources):
                    raise FaceError(f"resource name {rname!r} missing or repeated")
                if len(self.resources) >= RESOURCES_MAX:
                    raise FaceError(f"more than {RESOURCES_MAX} resources")
                fmt, width, height, pixels = read_netpbm(self.source.parent / node.get("src", ""))
                self.resources.append((rname, fmt, width, height, pixels))
                if node.get("preview") == "true":
                    if preview != NO_PREVIEW:
                        raise FaceError("only one preview resource")
                    preview = len(self.resources) - 1
            else:
                body.append(node)
        for node in body:
            self.element(node, NO_PARENT, 0)
        if not self.elements:
            raise FaceError("a face needs at least one element")
        if len(self.strings) > STRINGS_MAX:
            raise FaceError(f"label text over {STRINGS_MAX} bytes in total")
        needs = (NEED_SECOND if self.second else 0) | (NEED_BATTERY if self.battery else 0) | \
            (NEED_TAP if self.tap else 0)
        definition = bytearray(b"RWFD" + struct.pack("<HBBHHI", 1, len(self.elements), len(self.settings),
                                                    len(self.strings), needs, background))
        for key, options, default in self.settings:
            definition += key.encode().ljust(16, b"\0") + bytes([options, default, 0, 0])
        for entry in self.elements:
            definition += entry
        definition += self.strings
        if len(definition) > DEFINITION_MAX:
            raise FaceError(f"definition over {DEFINITION_MAX} bytes")
        requires = (CAP_RESOURCES if self.resources else 0) | (CAP_SETTINGS if self.settings else 0) | \
            (CAP_BATTERY if self.battery else 0)
        table_end = PREFIX + len(self.resources) * RESOURCE_ENTRY
        offset = table_end + len(definition)
        table = bytearray()
        payload = bytearray()
        for _, fmt, width, height, pixels in self.resources:
            table += struct.pack("<HHHHII", fmt, width, height, 0, offset, len(pixels))
            offset += len(pixels)
            payload += pixels
        if len(payload) > RESOURCE_BYTES_MAX:
            raise FaceError(f"resources over {RESOURCE_BYTES_MAX} bytes")
        manifest = (face_id.encode().ljust(48, b"\0") + encoded_name.ljust(32, b"\0") +
                    struct.pack("<HHHHHBBI", *versions, ABI_MAJOR, ABI_MINOR, BACKEND_DECLARATIVE, preview, requires))
        sections = struct.pack("<IIH", table_end, len(definition), len(self.resources)) + bytes(6)
        body_bytes = manifest + sections + table + definition + payload
        total = HEADER + len(body_bytes)
        if total > PACKAGE_MAX:
            raise FaceError(f"package over {PACKAGE_MAX} bytes")
        digest = hashlib.sha256(body_bytes).digest()
        envelope = b"RWF1" + struct.pack("<HHI", 1, HEADER, total) + bytes(20) + digest
        return envelope + body_bytes


def compile_face(source, output):
    package = Compiler(source).compile()
    Path(output).write_bytes(package)
    return inspect(output)


def inspect(path):
    """Host-side summary; the device validates everything again."""
    data = Path(path).read_bytes()
    if len(data) < PREFIX or len(data) > PACKAGE_MAX or data[:4] != b"RWF1":
        raise FaceError("not an RWF1 package")
    total = struct.unpack_from("<I", data, 8)[0]
    if total != len(data) or hashlib.sha256(data[HEADER:]).digest() != data[32:64]:
        raise FaceError("package length or digest mismatch")
    face_id = data[HEADER:HEADER + 48].rstrip(b"\0").decode()
    name = data[HEADER + 48:HEADER + 80].rstrip(b"\0").decode("utf-8")
    version = struct.unpack_from("<HHH", data, HEADER + 80)
    return {"id": valid_id(face_id), "name": name, "version": "%d.%d.%d" % version, "size": len(data),
            "sha256": data[32:64].hex()}


# --- Device client (RF1, docs/watch-face-packages.md) -------------------------

def parse_reply(line, request_id):
    from roundie_time import DeviceError, TransportError
    prefix = f"!RF1 {request_id} ".encode()
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
        for key in ("phase", "received", "total", "count", "objects", "pool", "bytes", "loads",
                    "load_failures"):
            if type(status[key]) is not int or not 0 <= status[key] <= 0xffffffff:
                raise ValueError(key)
        if status["phase"] > 2 or status["count"] > 8 or not status["received"] <= status["total"] <= PACKAGE_MAX:
            raise ValueError("limits")
        for key in ("home", "parked", "gallery", "fallback_only"):
            if status[key] not in (0, 1):
                raise ValueError(key)
        for key, length in (("counters", 9), ("us", 5)):
            if (not isinstance(status[key], list) or len(status[key]) != length or
                    any(type(v) is not int or not 0 <= v <= 0xffffffff for v in status[key])):
                raise ValueError(key)
        for key in ("preferred", "active", "failed", "error"):
            if not isinstance(status[key], str) or len(status[key]) >= 48:
                raise ValueError(key)
        if not isinstance(status["faces"], list) or len(status["faces"]) != status["count"]:
            raise ValueError("faces")
        for face in status["faces"]:
            if (not isinstance(face, list) or len(face) != 3 or not isinstance(face[0], str) or
                    VERSION.fullmatch(face[1]) is None or face[2] not in (0, 1)):
                raise ValueError("face record")
            valid_id(face[0])
    except (ValueError, TypeError, KeyError, FaceError) as error:
        raise TransportError(f"malformed face response: {error}") from error
    if int(code) != 0:
        raise DeviceError(f"face request rejected ({int(code)}): {status['error']}")
    return status


def request(port, operation, timeout=8):
    from roundie_time import read_serial_reply, write_command
    request_id = secrets.token_hex(4)
    write_command(port, f"!RF1 {request_id} {operation}")
    return parse_reply(read_serial_reply(port, f"!RF1 {request_id} ".encode(), time.monotonic() + timeout, 1900),
                       request_id)


def upload(port, path, info):
    from roundie_time import DeviceError, TransportError
    session = secrets.token_hex(4)
    while session == "00000000":
        session = secrets.token_hex(4)
    begun = False
    try:
        request(port, f"BEGIN {session} {info['size']} {info['sha256']}")
        begun = True
        data = Path(path).read_bytes()
        if len(data) != info["size"]:
            raise DeviceError("package changed during upload")
        for offset in range(0, len(data), 512):
            chunk = data[offset:offset + 512]
            response = request(port, f"DATA {session} {offset} {chunk.hex()}")
            if response["received"] != offset + len(chunk) or response["phase"] != 1:
                raise TransportError("inconsistent transfer acknowledgement")
        result = request(port, f"END {session}")
        if result["phase"] != 2:
            raise TransportError("face was not validated READY")
        begun = False
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
    build = sub.add_parser("build")
    build.add_argument("source", type=Path)
    build.add_argument("--output", type=Path, required=True)
    sub.add_parser("inspect").add_argument("file", type=Path)
    upload_command = sub.add_parser("upload")
    upload_command.add_argument("file", type=Path)
    upload_command.add_argument("--install", action="store_true")
    for name in ("select", "remove"):
        sub.add_parser(name).add_argument("id")
    sub.add_parser("status")
    args = parser.parse_args(argv)
    sys.path.insert(0, str(ROOT / "tools"))
    try:
        if args.command == "build":
            result = compile_face(args.source, args.output)
        elif args.command == "inspect":
            result = inspect(args.file)
        else:
            from roundie_time import open_serial
            if args.port is None:
                parser.error("--port required for device operations")
            if args.command in ("select", "remove") and args.id != "system":
                valid_id(args.id)
            artifact = inspect(args.file) if args.command == "upload" else None
            with open_serial(args.port) as port:
                if args.command == "upload":
                    result = upload(port, args.file, artifact)
                    if args.install:
                        result = request(port, f"INSTALL {artifact['sha256']}")
                    result["artifact"] = artifact
                elif args.command in ("select", "remove"):
                    result = request(port, f"{args.command.upper()} {args.id}")
                else:
                    result = request(port, "STATUS")
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return 0
    except FaceError as error:
        print(f"Face error: {error}", file=sys.stderr)
        return 3
    except Exception as error:  # transport and device errors from roundie_time
        name = type(error).__name__
        if name in ("TransportError", "DeviceError") or isinstance(error, OSError):
            print(f"{name}: {error}", file=sys.stderr)
            return 2 if name == "TransportError" else 3
        raise


if __name__ == "__main__":
    raise SystemExit(main())
