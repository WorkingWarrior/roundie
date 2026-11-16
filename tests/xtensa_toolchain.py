"""The Xtensa toolchain directory for the host tests' Xtensa ELF parts: the
ROUNDIE_XTENSA_TOOLCHAIN environment variable, else the directory of
xtensa-esp32s3-elf-gcc on PATH, else the IDF v6.1 install location
(esp-15.2.0_20251204) under the home directory."""
import os
from pathlib import Path
import shutil


def _find():
    configured = os.environ.get("ROUNDIE_XTENSA_TOOLCHAIN")
    if configured:
        return Path(configured)
    compiler = shutil.which("xtensa-esp32s3-elf-gcc")
    if compiler:
        return Path(compiler).parent
    return Path.home() / ".espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin"


TOOLCHAIN = _find()
