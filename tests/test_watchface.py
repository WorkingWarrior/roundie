"""Watch face platform: compiler, real store, host lifecycle and text with ASan/UBSan."""
from pathlib import Path
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import roundie_face
from roundie_time import TransportError

HEADER = roundie_face.HEADER
MANIFEST_AT = HEADER
SECTIONS_AT = HEADER + roundie_face.MANIFEST
FACES = ("classic_analog", "minimal_digital", "information")


def resign(data):
    """Recomputes the envelope total and digest after a mutation."""
    data = bytearray(data)
    struct.pack_into("<I", data, 8, len(data))
    data[32:64] = hashlib.sha256(bytes(data[HEADER:])).digest()
    return bytes(data)


def mutate(data, offset, fmt, value):
    data = bytearray(data)
    struct.pack_into(fmt, data, offset, value)
    return resign(data)


def compile_sources(output, *sources, defines=()):
    command = ["cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined",
               "-I" + str(ROOT / "tests/ota_mocks"), "-I" + str(ROOT / "platform/linux/include"),
               "-I" + str(ROOT / "include"), "-I" + str(ROOT / "components/roundie_elf/include"),
               "-I" + str(ROOT / "components/roundie_platform/include"),
               "-I" + str(ROOT / "components/roundie_watchface"),
               "-I" + str(ROOT / "components/roundie_watchface/include"),
               "-I" + str(ROOT / "tests/app_ui"), "-I" + str(ROOT / "managed_components/lvgl__lvgl"),
               "-DLV_CONF_INCLUDE_SIMPLE", *defines, *map(str, sources), "-lcrypto", "-o", str(output)]
    subprocess.run(command, check=True)


class WatchFaceTests(unittest.TestCase):
    def build_faces(self, directory):
        packages = {}
        for face in FACES:
            output = directory / (face + ".rwf")
            first = roundie_face.compile_face(ROOT / "faces" / face / "face.xml", output)
            repeat = directory / (face + "-repeat.rwf")
            roundie_face.compile_face(ROOT / "faces" / face / "face.xml", repeat)
            self.assertEqual(output.read_bytes(), repeat.read_bytes())
            self.assertEqual(first["size"], output.stat().st_size)
            packages[face] = output
        source = (ROOT / "faces/classic_analog/face.xml").read_text(encoding="utf-8")
        newer = directory / "analog_v2" / "face.xml"
        newer.parent.mkdir()
        newer.write_text(source.replace('version="1.0.0"', 'version="1.1.0"'), encoding="utf-8")
        for resource in (ROOT / "faces/classic_analog").glob("*.p?m"):
            (newer.parent / resource.name).write_bytes(resource.read_bytes())
        packages["analog_v2"] = directory / "analog_v2.rwf"
        roundie_face.compile_face(newer, packages["analog_v2"])
        return packages

    def bad_packages(self, directory, packages):
        analog = packages["classic_analog"].read_bytes()
        info = packages["information"].read_bytes()
        definition = struct.unpack_from("<I", analog, SECTIONS_AT)[0]
        resources = struct.unpack_from("<H", info, SECTIONS_AT + 8)[0]
        self.assertGreater(resources, 0)
        table = roundie_face.PREFIX
        digest = bytearray(analog)
        digest[200] ^= 0x01
        cases = {
            "digest": (bytes(digest), "package-digest"),
            "trailing": (resign(analog + b"\0"), "package-trailing-bytes"),
            "native": (mutate(analog, MANIFEST_AT + 90, "<B", 2), "native-face-unsupported"),
            "abi2": (mutate(analog, MANIFEST_AT + 86, "<H", 2), "face-abi"),
            "path-id": (resign(bytearray(analog[:HEADER]) + b"../evil".ljust(48, b"\0") + analog[HEADER + 48:]),
                        "manifest-id"),
            "needs": (mutate(analog, definition + 10, "<H", 0), "definition-needs"),
            "resource": (mutate(info, table + 8, "<I", len(info) + 64), "resource-bounds"),
            "truncated": (info[:roundie_face.PREFIX + 4], "package-envelope"),
            "oversize": (resign(analog + bytes(roundie_face.PACKAGE_MAX)), "package-size"),
            "element": (mutate(analog, definition + 16 + 20, "<B", 99), "element-type"),
        }
        arguments = []
        for name, (data, reason) in cases.items():
            path = directory / ("bad-" + name + ".rwf")
            path.write_bytes(data)
            arguments.append(f"{path}={reason}")
        return arguments

    def test_store_host_and_text(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            packages = self.build_faces(directory)
            bad = self.bad_packages(directory, packages)
            faces = directory / "faces"
            executable = directory / "tests"
            component = ROOT / "components/roundie_watchface"
            compile_sources(executable, ROOT / "tests/test_watchface.c", component / "format.c",
                            component / "text.c", component / "host.c", component / "store.c",
                            defines=['-DFACES_DIRECTORY="' + str(faces) + '"'])
            arguments = [str(directory), str(packages["classic_analog"]), str(packages["minimal_digital"]),
                         str(packages["information"]), str(packages["analog_v2"]), *bad]
            subprocess.run([str(executable), *arguments], check=True, timeout=60)

    def test_compiler_refuses_bad_sources(self):
        good = (ROOT / "faces/minimal_digital/face.xml").read_text(encoding="utf-8")
        cases = {
            "doctype": good.replace("<face", '<!DOCTYPE face [<!ENTITY x "y">]><face', 1),
            "id": good.replace('id="org.roundie.face.minimal"', 'id="../minimal"'),
            "token": good.replace("{HH}", "{XX}", 1),
            "version": good.replace('version="1.0.0"', 'version="1.0"'),
        }
        with tempfile.TemporaryDirectory() as temporary:
            for name, text in cases.items():
                with self.subTest(case=name):
                    source = Path(temporary) / "face.xml"
                    source.write_text(text, encoding="utf-8")
                    with self.assertRaises((roundie_face.FaceError, Exception)):
                        roundie_face.compile_face(source, Path(temporary) / "out.rwf")

    def test_bounded_reply_validation(self):
        reply = dict(format=1, phase=0, received=0, total=0, count=1, objects=12, pool=40000, bytes=454,
                     home=1, parked=0, gallery=0, fallback_only=0, loads=1, load_failures=0,
                     counters=[0] * 9, us=[0] * 5,
                     preferred="", active="system", failed="", error="",
                     faces=[["org.roundie.face.minimal", "1.0.0", 1]])

        def frame(value, code=0):
            return f"!RF1 12345678 {code} {json.dumps(value)}\n".encode()

        self.assertEqual(roundie_face.parse_reply(frame(reply), "12345678"), reply)
        self.assertIsNone(roundie_face.parse_reply(frame(reply), "87654321"))
        for key, value in (("phase", 3), ("count", 9), ("counters", [0]), ("home", 2),
                           ("faces", [["../bad", "1.0.0", 1]]), ("error", "x" * 48)):
            with self.subTest(key=key):
                with self.assertRaises(TransportError):
                    roundie_face.parse_reply(frame(dict(reply, **{key: value})), "12345678")


if __name__ == "__main__":
    unittest.main()
