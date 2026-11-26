#!/usr/bin/env python3
"""No direct VFS calls on /storage outside the storage module (docs/storage-io.md, P1 fix 2).

Firmware code reaches /storage only through the guarded roundie_io_* helpers
of components/roundie_storage (roundie_bounded_io.h), so the development
owner assertion sees every operation. This check scans the firmware sources
(main/, components/) for calls of the libc VFS functions and fails on any
outside the allowed places:

- components/roundie_storage/ (the helpers themselves, the mount);
- files that work only on another mount (FILES_OTHER_MOUNT);
- a line with a "vfs-ok:" comment naming the non-/storage target.

    python3 tools/ci/check_storage_vfs.py [ROOT]

Exit 0 when clean, 1 with one "file:line: call" per violation.
"""
import re
import sys
from pathlib import Path

FUNCTIONS = ("open", "close", "read", "write", "lseek", "fstat", "fsync", "stat", "lstat", "access",
             "rename", "unlink", "remove", "mkdir", "rmdir", "opendir", "readdir", "closedir",
             "fopen", "fclose", "fread", "fwrite", "fgets", "fputs", "fprintf", "fseek", "ftell",
             "fflush", "ftruncate", "truncate", "statvfs")
# A call: the name not preceded by an identifier character, ".", "->" or "_".
CALL = re.compile(r"(?<![A-Za-z0-9_.>])(" + "|".join(FUNCTIONS) + r")\s*\(")
ALLOWED_TREES = ("components/roundie_storage/", "components/roundie_elf/linux/")
FILES_OTHER_MOUNT = {
    "main/storage_import.c": "USB FAT drive /usbdisk",
    "main/storage_usb.c": "USB FAT drive /usbdisk",
}
MARKER = "vfs-ok:"


def strip(text):
    """Blanks comments and string/char literals, keeping line numbers. A
    comment holding the marker keeps the marker, so the line can be allowed."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(MARKER if MARKER in text[i:end] else "")
            i = end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            body = text[i:end]
            out.append(("\n" * body.count("\n")) + (MARKER if MARKER in body else ""))
            i = end
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def violations(root, files=None):
    root = Path(root)
    if files is None:
        files = sorted(list(root.glob("main/*.c")) + list(root.glob("components/**/*.c")))
    found = []
    for path in files:
        relative = Path(path).resolve().relative_to(root.resolve()).as_posix()
        if relative.startswith(ALLOWED_TREES) or relative in FILES_OTHER_MOUNT:
            continue
        for number, line in enumerate(strip(Path(path).read_text()).splitlines(), 1):
            if MARKER in line:
                continue
            for match in CALL.finditer(line):
                found.append(f"{relative}:{number}: {match.group(1)}()")
    return found


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    root = Path(argv[0]) if argv else Path(__file__).resolve().parents[2]
    found = violations(root)
    for item in found:
        print(item)
    if found:
        print(f"{len(found)} direct VFS call(s) outside roundie_storage: use the roundie_io_* helpers "
              f"(roundie_bounded_io.h), or mark a non-/storage target with '{MARKER} <target>'.")
        return 1
    print("storage VFS rule: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
