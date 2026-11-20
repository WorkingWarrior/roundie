#!/usr/bin/env python3
"""Symbolize the retained native crash record (RS1 CRASH, M9) offline.

The record holds run-time addresses, the SHA-256 of the loaded (stripped)
ELF and its section map. This tool finds the stripped `X.app.elf` with that
digest among the given files or directories, reads symbols from its sibling
`X.app.debug.elf` (the unstripped link output the example builds keep), maps
each address into the ELF address space and names the function. Addresses
outside the application can be named from the firmware ELF (--firmware).
Function names come from .symtab; the examples are built without -g, so there
are no line numbers.

  roundie_crash.py --port /dev/serial/by-id/...  --elf build/examples
  roundie_crash.py --report crash.json --elf a.app.elf --firmware build/roundie.elf
"""
import argparse
import hashlib
import json
import re
import secrets
import struct
import sys
import time
from pathlib import Path

SECTION_NAMES = ("text", "bss", "data", "rodata", "drlro")
EXCCAUSE = {0: "IllegalInstruction", 2: "InstructionFetchError", 3: "LoadStoreError", 6: "IntegerDivideByZero",
            9: "LoadStoreAlignment", 20: "InstFetchProhibited", 28: "LoadProhibited", 29: "StoreProhibited"}
SHT_SYMTAB = 2
STT_FUNC = 2
STT_OBJECT = 1


class Unsupported(Exception):
    """The firmware has no RS1 CRASH."""


def symbols(path):
    """FUNC and OBJECT symbols of a little-endian ELF32 file: (value, size, name)."""
    data = Path(path).read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError(f"{path}: not a little-endian ELF32 file")
    shoff, = struct.unpack_from("<I", data, 32)
    shentsize, shnum = struct.unpack_from("<HH", data, 46)
    headers = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    found = []
    for header in headers:
        if header[1] != SHT_SYMTAB:
            continue
        offset, size, link, entsize = header[4], header[5], header[6], header[9]
        strings = headers[link]
        for at in range(offset, offset + size, entsize):
            name, value, length, info, _, _ = struct.unpack_from("<IIIBBH", data, at)
            if info & 0xf in (STT_FUNC, STT_OBJECT) and name != 0:
                start = strings[4] + name
                found.append((value, length, data[start:data.index(b"\0", start)].decode(errors="replace")))
    return sorted(found)


def name_of(table, address):
    """symbol+offset for an address, or None."""
    best = None
    for value, size, name in table:
        if value <= address and (address < value + size or (size == 0 and best is None)):
            best = (value, name)
    if best is None:
        return None
    return best[1] if address == best[0] else f"{best[1]}+0x{address - best[0]:x}"


def locate(record, address):
    """(section index, ELF address) of a run-time address, or None; as
    roundie_elf_crash_locate() in components/roundie_elf/crash.c."""
    for index, (vaddr, addr, size) in enumerate(record["sections"]):
        if size == 0:
            continue
        base = addr
        if index == 0 and 0 <= address - record["text_exec"] < size:
            base = record["text_exec"]
        if 0 <= address - base < size:
            return index, vaddr + address - base
    return None


def find_application(digest, paths):
    """The debug ELF whose stripped sibling has the recorded digest."""
    candidates = []
    for path in map(Path, paths):
        candidates += sorted(path.rglob("*.app.elf")) if path.is_dir() else [path]
    for candidate in candidates:
        if candidate.name.endswith(".app.debug.elf") or not candidate.is_file():
            continue
        if hashlib.sha256(candidate.read_bytes()).hexdigest() == digest:
            debug = candidate.with_name(candidate.name[:-len(".app.elf")] + ".app.debug.elf")
            return candidate, (debug if debug.is_file() else None)
    return None, None


# M9.1 origins (components/roundie_elf/crash.c); only APP_* attribute the
# panic to the loaded application.
ORIGINS = {
    "none": "no panic recorded",
    "core": "firmware, no application code running",
    "core_while_app_active": "firmware outside the executor while an application ran: not attributed",
    "executor_core": "executor, no application code running",
    "executor_stack_overflow": "executor stack limit crossed with no application code running",
    "app_code": "application code (PC in the loaded text)",
    "app_callback": "executor during an application callback (firmware PC, e.g. an API call)",
    "app_stack_overflow": "application overflowed the executor stack (limit crossed during its code)",
}
PHASES = ("idle", "load", "callback", "unload", "unknown")
TRIGGERS = {0: "exception", 1: "executor stack monitor", 2: "stack monitor under another task"}


def symbolize(record, elf_paths=(), firmware=None):
    """Lines describing the record, application addresses first mapped to the ELF."""
    if not record.get("valid"):
        return ["No valid crash record (none since power-on, or a torn record)."]
    if not record.get("fault"):
        return ["No panic recorded since the last application load."]
    cause = record["cause"]
    lines = [f"Panic: pc=0x{record['pc']:08x} cause={cause} ({EXCCAUSE.get(cause, 'see EXCCAUSE / panic reason')}) "
             f"vaddr=0x{record['vaddr']:08x} sp=0x{record['sp']:08x} core={record['core']}"]
    if "origin" in record:
        lines.append(f"Origin: {record['origin']} - {ORIGINS.get(record['origin'], 'unknown')}")
        lines.append(f"  phase={record['phase']} trigger={TRIGGERS.get(record['trigger'], record['trigger'])}")
        guard, low, limit, high, armed = record["stack"]
        if high:
            lines.append(f"  executor stack: guard 0x{guard:08x}, red zone 0x{low:08x}-0x{limit:08x}, "
                         f"usable 0x{limit:08x}-0x{high:08x}, monitor {'armed' if armed else 'NOT armed'}")
        if not record["frame"]:
            lines.append("  exception frame not read (absent or in the guard page)")
    application = None
    if record["loaded"]:
        stripped, debug = find_application(record["digest"], elf_paths)
        lines.append(f"Application instance {record['instance']}, ELF SHA-256 {record['digest']}")
        if stripped is None:
            lines.append("  no given *.app.elf has this digest: addresses are mapped, not named")
        elif debug is None:
            lines.append(f"  matched {stripped}, but its .app.debug.elf is missing: not named")
        else:
            lines.append(f"  matched {stripped}; symbols from {debug.name}")
            application = symbols(debug)
    else:
        lines.append("No application was loaded at the panic: firmware fault.")
    firmware_table = symbols(firmware) if firmware else None
    frames = [("pc", record["pc"])] + [(f"#{i}", pc) for i, pc in enumerate(record["frames"])]
    if record.get("monitor_pc"):
        frames.append(("sp<", record["monitor_pc"]))
    if record["vaddr"] and record["loaded"] and locate(record, record["vaddr"]) is not None:
        frames.append(("vaddr", record["vaddr"]))
    for label, address in frames:
        where = locate(record, address) if record["loaded"] else None
        if where is not None:
            index, vaddr = where
            name = name_of(application, vaddr) if application is not None else None
            text = f"application {SECTION_NAMES[index]} 0x{vaddr:x}" + (f" {name}" if name else "")
        else:
            name = name_of(firmware_table, address) if firmware_table is not None else None
            text = "firmware" + (f" {name}" if name else "")
        lines.append(f"  {label:>5} 0x{address:08x}  {text}")
    return lines


def parse(line):
    match = re.fullmatch(rb"!RS1 [0-9a-f]{8} (-?[0-9]{1,10}) (\{.*\})\n?", line.strip(b"\n") + b"\n", re.S)
    if match is None:
        raise ValueError("not an RS1 reply")
    result = json.loads(match[2])
    if int(match[1]) != 0 or "crash" not in result:
        raise Unsupported("firmware has no RS1 CRASH")
    return validate(result["crash"])


def validate(record):
    if type(record.get("valid")) is not int or record["valid"] not in (0, 1):
        raise ValueError("valid")
    if not record["valid"]:
        return record
    numbers = [record[key] for key in ("loaded", "fault", "instance", "text_exec", "pc", "cause", "vaddr", "sp",
                                       "exception", "core")]
    numbers += [value for section in record["sections"] for value in section] + list(record["frames"])
    if (any(type(v) is not int or not 0 <= v <= 0xffffffff for v in numbers) or
            len(record["sections"]) != len(SECTION_NAMES) or any(len(s) != 3 for s in record["sections"]) or
            len(record["frames"]) > 8 or re.fullmatch(r"[0-9a-f]{64}", record["digest"]) is None):
        raise ValueError("crash record fields")
    if "origin" in record:  # M9.1 fields, all or none
        extra = [record["frame"], record["trigger"], record["monitor_pc"]] + list(record["stack"])
        if (any(type(v) is not int or not 0 <= v <= 0xffffffff for v in extra) or len(record["stack"]) != 5 or
                record["phase"] not in PHASES or record["origin"] not in ORIGINS):
            raise ValueError("crash record M9.1 fields")
    return record


def request(port, timeout=8.0):
    from roundie_time import read_serial_reply, write_command
    identity = secrets.token_hex(4)
    write_command(port, f"!RS1 {identity} CRASH")
    return parse(read_serial_reply(port, f"!RS1 {identity} ".encode(), time.monotonic() + timeout, 1024))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="ask the board (RS1 CRASH, read-only)")
    source.add_argument("--report", type=Path, help="a saved RS1 CRASH reply or its crash JSON object")
    parser.add_argument("--elf", action="append", default=[], help="*.app.elf file or directory (repeatable)")
    parser.add_argument("--firmware", type=Path, help="firmware ELF for addresses outside the application")
    parser.add_argument("--json", action="store_true", help="print the record instead of symbolizing it")
    args = parser.parse_args(argv)
    try:
        if args.port:
            from roundie_time import open_serial
            with open_serial(args.port) as port:
                record = request(port)
        else:
            text = args.report.read_bytes()
            record = parse(text) if text.lstrip().startswith(b"!RS1") else validate(json.loads(text).get("crash", json.loads(text)))
    except Unsupported as error:
        print(error, file=sys.stderr)
        return 3
    except (ValueError, KeyError, TypeError) as error:
        print(f"malformed crash record: {error}", file=sys.stderr)
        return 2
    except Exception as error:  # TransportError, DeviceError, OSError
        print(f"crash record unavailable: {error}", file=sys.stderr)
        return 1
    print(json.dumps(record, indent=2) if args.json else "\n".join(symbolize(record, args.elf, args.firmware)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
