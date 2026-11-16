#!/usr/bin/env python3
"""Build a REP application for linux-x64.rep1 (docs/native-apps.md, N1).

The same source as the ESP32-S3 package, compiled by the host C compiler as a
self-contained x86-64 shared object: no C library, no start files, no
dependencies, bound at load (-z now), non-executable stack. The flags match the
Xtensa examples' except for the target-specific ones.

usage: roundie_native_app.py --entry rep_app_entry_v3 -o sound.app.so sound.c [-DNAME=VALUE ...]
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


def build(source, output, entry, defines=(), compiler="cc"):
    command = [*shlex.split(compiler), *FLAGS, f"-Wl,-e,{entry}", f"-I{ROOT / 'include'}",
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
    arguments = parser.parse_args(argv)
    try:
        build(arguments.source, arguments.output, arguments.entry, arguments.defines, arguments.cc)
    except subprocess.CalledProcessError as error:
        return error.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
