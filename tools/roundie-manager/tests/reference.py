"""Generate ignored golden vectors using unchanged production Python and C.

Requests run through production request()/write_command(), not a second encoder.
RP1 responses are Python-parser-validated samples (not device captures).
The package is built and inspected with the existing C/ELF validator.
"""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import roundie_package as package
import roundie_ota as ota
import roundie_safe as safe
from xtensa_toolchain import TOOLCHAIN

OUTPUT = Path(__file__).parent / "generated"
OUTPUT.mkdir(exist_ok=True)
REQUEST_ID = "12345678"
SESSION = "abcdef01"
DIGEST = hashlib.sha256(b"golden package").hexdigest()
STATUS = dict(format=1, allowed=1, busy=0, phase=0, session="00000000", received=0,
              total=0, count=1, orphans=0, capacity=[1, 9240576, 24576], us=[0]*7,
              error="", apps=[["org.roundie.hello", "1.0.0", 1, 0]],
              name="Hello", sha256=DIGEST, elf_size=1692)


class CapturePort:
    def write(self, data):
        self.wire = data
        return len(data)


def response(status, code=0):
    return f"!RP1 {REQUEST_ID} {code} {json.dumps(status, separators=(',', ':'), ensure_ascii=False)}\n".encode()


def main():
    vectors = []
    binary = bytes(range(256)) * 2
    operations = ["STATUS", "LIST", "INFO org.roundie.hello",
                  f"BEGIN {SESSION} 65760 {DIGEST}",
                  f"DATA {SESSION} 0 000a0d21ff525031",
                  f"DATA {SESSION} 512 {binary.hex()}",
                  f"END {SESSION}", f"INSTALL {DIGEST}", f"ABORT {SESSION}"]
    for operation in operations:
        port = CapturePort()
        with patch.object(package.secrets, "token_hex", return_value=REQUEST_ID), \
                patch.object(package, "read_serial_reply", return_value=response(STATUS)):
            package.request(port, operation)
        vectors.append(dict(protocol="RP1", id=REQUEST_ID, operation=operation, wire=port.wire.hex()))
    # Existing request functions are also authoritative for diagnostic framing.
    for protocol, module in (("RO1", ota), ("RS1", safe)):
        port = CapturePort()
        with patch.object(module.secrets, "token_hex", return_value=REQUEST_ID), \
                patch.object(module, "read_serial_reply", return_value=b"unused"), \
                patch.object(module, "parse_reply", return_value={}):
            module.request(port, "STATUS")
        vectors.append(dict(protocol=protocol, id=REQUEST_ID, operation="STATUS", wire=port.wire.hex()))

    # Independently check the transfer vectors against the actual firmware parser.
    with tempfile.TemporaryDirectory(prefix="roundie-manager-reference-") as temporary:
        tmp = Path(temporary)
        c_source = tmp / "reference.c"
        c_source.write_text('''#include <stdio.h>
#include <string.h>
#include "roundie_ota_protocol.h"
int main(void) {
    char line[1200];
    while (fgets(line, sizeof(line), stdin)) {
        size_t size = strlen(line);
        if (size && line[size - 1] == '\\n') { line[size - 1] = 0; }
        roundie_ota_command_t command;
        if (roundie_transfer_parse(line, false, "!RP1 ", "INSTALL", &command) != 0) { return 1; }
    }
    return 0;
}
''')
        executable = tmp / "reference"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-ffunction-sections",
                        "-I" + str(ROOT / "tests/ota_mocks"),
                        "-I" + str(ROOT / "components/roundie_ota/include"),
                        str(c_source), str(ROOT / "components/roundie_ota/ota_protocol.c"),
                        "-Wl,--gc-sections", "-o", str(executable)], check=True, capture_output=True)
        transfer_ops = [op for op in operations if not op.startswith(("INFO", "LIST"))]
        subprocess.run([str(executable)], input="".join(f"!RP1 {REQUEST_ID} {op}\n" for op in transfer_ops), text=True, check=True)

        env = dict(os.environ)
        env["PATH"] = str(TOOLCHAIN) + os.pathsep + env["PATH"]
        build = tmp / "hello"
        subprocess.run(["cmake", "-S", str(ROOT / "examples/hello_elf"), "-B", str(build)], env=env, check=True, capture_output=True)
        subprocess.run(["cmake", "--build", str(build)], env=env, check=True, capture_output=True)
        artifact = tmp / "manager-reference.rpk"
        metadata = package.build(build / "hello.app.elf", artifact, "org.roundie.managercheck", 'Manager "Próba"', "1.0.0")
        data = artifact.read_bytes()
        (OUTPUT / "package.json").write_text(json.dumps(dict(metadata=metadata, hex=data.hex())))

    good = response(STATUS)
    package.parse_reply(good, REQUEST_ID)
    error = response(dict(STATUS, error="package-admission"), 259)
    try:
        package.parse_reply(error, REQUEST_ID)
    except package.DeviceError:
        pass
    else:
        raise AssertionError("Python accepted error as success")
    (OUTPUT / "vectors.json").write_text(json.dumps(dict(requests=vectors, responses=[good.hex(), error.hex()])))
    print(f"Production reference: {len(vectors)} request vectors, 2 Python-validated responses, firmware transfer parser PASS; real RPK1 {len(data)} B")


if __name__ == "__main__":
    main()
