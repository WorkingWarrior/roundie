#!/usr/bin/env python3
"""Build a REP application for linux-x64.rep1 (docs/native-apps.md, N1).

The same source as the ESP32-S3 package, compiled by the host C compiler as a
self-contained x86-64 shared object: no C library, no start files, no
dependencies, bound at load (-z now), non-executable stack. The flags match the
Xtensa examples' except for the target-specific ones.

usage: roundie_native_app.py --entry rep_app_entry_v3 -o sound.app.so sound.c [-DNAME=VALUE ...]

Developer builds (docs/native-apps.md, N8) pass the same production loader
validation: --debug adds DWARF and frame pointers for GDB, and
--sanitize-undefined makes undefined behaviour trap (SIGILL) without importing
a sanitizer runtime.
"""
import argparse
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

FLAGS = [
    "-std=c11", "-Os", "-Wall", "-Wextra", "-Werror",
    "-fPIC", "-fvisibility=hidden", "-ffunction-sections", "-fdata-sections",
    "-fno-builtin", "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
    "-nostdlib", "-nostartfiles", "-shared",
    "-Wl,--gc-sections", "-Wl,-z,now", "-Wl,-z,noexecstack", "-Wl,--hash-style=gnu",
    "-Wl,--undefined=rep_abi_metadata",
]

DEBUG_FLAGS = ["-g", "-Og", "-fno-omit-frame-pointer"]
UNDEFINED_FLAGS = ["-fsanitize=undefined", "-fsanitize-trap=undefined"]


def build(source, output, entry, defines=(), compiler="cc", debug=False, sanitize_undefined=False):
    flags = list(FLAGS)
    if debug:
        flags = [flag for flag in flags if flag != "-Os"] + DEBUG_FLAGS
    if sanitize_undefined:
        flags += UNDEFINED_FLAGS
    command = [*shlex.split(compiler), *flags, f"-Wl,-e,{entry}", f"-I{ROOT / 'include'}",
               *(f"-D{define}" for define in defines), str(source), "-o", str(output)]
    subprocess.run(command, check=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("source")
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("--entry", required=True,
                        choices=("rep_app_entry", "rep_app_entry_v2", "rep_app_entry_v3"))
    parser.add_argument("-D", dest="defines", action="append", default=[])
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--debug", action="store_true", help="DWARF, -Og and frame pointers for GDB")
    parser.add_argument("--sanitize-undefined", action="store_true",
                        help="trap on undefined behaviour (UBSan without a runtime)")
    arguments = parser.parse_args(argv)
    try:
        build(arguments.source, arguments.output, arguments.entry, arguments.defines, arguments.cc,
              arguments.debug, arguments.sanitize_undefined)
    except subprocess.CalledProcessError as error:
        return error.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
