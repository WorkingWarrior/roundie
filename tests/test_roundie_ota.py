"""Offline protocol and host behavior. No serial device access; IDF is mocked."""
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import roundie_ota as tool


def image(size=1024):
    raw = bytearray(size)
    raw[0:2] = b"\xe9\x03"
    struct.pack_into("<H", raw, 12, 9)
    struct.pack_into("<I", raw, 32, 0xabcd5432)
    raw[48:53] = b"0.2.1"
    raw[80:91] = b"roundie_rep"
    raw[176:208] = bytes(range(32))
    return bytes(raw)


def status():
    return dict(phase="IDLE", session="00000000", received=0, total=0, running="ota_0",
                address=0x10000, ota_state="VALID", other="ota_1", other_state="NO_METADATA",
                other_image=0, other_elf="", boot="ota_0", version="0.2.1", elf="a" * 64,
                digest="", rollback=0, checked=1, passed=1, last_error=0)


def reply(request_id, state=None, code=0):
    return f"!RO1 {request_id} {code} {json.dumps(state or status())}\n".encode()


class FakeSerial:
    def __init__(self, failure=None):
        self.failure = failure
        self.incoming = bytearray()
        self.commands = []
        self.state = status()
        self.data = bytearray()
        self.expected = None
        self.max_payload = 0

    def write(self, wire):
        fields = wire.strip().decode().split(" ")
        _, request_id, operation, *args = fields
        self.commands.append(operation)
        if self.failure == "short":
            return len(wire) - 1
        if self.failure == "disconnect" or (self.failure == "data_disconnect" and operation == "DATA"):
            raise OSError("disconnected")
        if self.failure == "timeout":
            return len(wire)
        if self.failure == "malformed":
            self.incoming.extend(f"!RO1 {request_id} garbage\n".encode())
            return len(wire)
        if self.failure == "truncated":
            self.incoming.extend(f"!RO1 {request_id} 0 {{".encode())
            return len(wire)
        if self.failure == "oversized":
            self.incoming.extend(f"!RO1 {request_id} 0 ".encode() + b"x" * 1100 + b"\n")
            return len(wire)
        if operation == "BEGIN":
            self.state.update(phase="RECEIVING", session=args[0], total=int(args[1]))
            self.expected = args[2]
        elif operation == "DATA":
            assert args[0] == self.state["session"] and int(args[1]) == len(self.data)
            block = bytes.fromhex(args[2])
            self.max_payload = max(self.max_payload, len(block))
            self.data.extend(block)
            self.state["received"] = len(self.data)
            if self.failure == "wrong_progress":
                self.state["received"] -= 1
        elif operation == "END":
            assert len(self.data) == self.state["total"]
            assert hashlib.sha256(self.data).hexdigest() == self.expected
            self.state.update(phase="READY", digest=self.expected, other_image=1,
                              other_elf=self.data[176:208].hex())
            if self.failure == "wrong_identity":
                self.state["other_elf"] = "b" * 64
        elif operation == "ABORT":
            self.state.update(phase="IDLE", session="00000000")
        self.incoming.extend(b"I (123) ordinary log\r\n")
        self.incoming.extend(reply("ffffffff"))
        self.incoming.extend(reply(request_id, self.state))
        return len(wire)

    def read(self, count):
        if self.incoming:
            data = bytes(self.incoming[:count])
            del self.incoming[:count]
            return data
        time.sleep(0.001)
        return b""


class OTATests(unittest.TestCase):
    def test_actual_boot_gate_normal_and_armed(self):
        with tempfile.TemporaryDirectory() as directory:
            for armed in (0, 1):
                executable = str(Path(directory) / f"boot-gate-{armed}")
                subprocess.run(["cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror",
                                "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
                                "-Wl,--gc-sections", "-DCONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1",
                                f"-DCONFIG_ROUNDIE_OTA_TEST_UNCONFIRMED_RESET={armed}",
                                "-I" + str(ROOT / "tests/ota_mocks"), "-I" + str(ROOT / "main"),
                                "-I" + str(ROOT / "components/roundie_ota/include"),
                                "-I" + str(ROOT / "components/roundie_boot_guard/include"),
                                "-I" + str(ROOT / "components/roundie_storage/include"),
                                "-I" + str(ROOT / "components/roundie_platform/include"),
                            "-I" + str(ROOT / "platform/linux/include"), "-pthread",
                                str(ROOT / "main/ota_diagnostics.c"), str(ROOT / "tests/test_ota_boot_gate.c"),
                                str(ROOT / "components/roundie_storage/claim.c"), str(ROOT / "tests/io_worker_stub.c"),
                                "-o", executable], check=True)
                for state in (0, 1, 2, 3, 4, -1, -2):
                    for fallback in (0, 1):
                        for health in ("pass", "fail", "timeout", "startup"):
                            with self.subTest(armed=armed, state=state, fallback=fallback, health=health):
                                subprocess.run([executable, str(state), str(fallback), health],
                                               check=True, timeout=5, stdout=subprocess.DEVNULL)

    def test_manager_and_parser_with_mock_idf_and_sanitizers(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "ota-test")
            subprocess.run(["cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-g",
                            "-I" + str(ROOT / "tests/ota_mocks"),
                            "-I" + str(ROOT / "components/roundie_ota/include"),
                            str(ROOT / "components/roundie_ota/ota.c"),
                            str(ROOT / "components/roundie_ota/ota_protocol.c"),
                            str(ROOT / "tests/test_ota.c"), "-lcrypto", "-o", executable], check=True)
            subprocess.run([executable], timeout=10, check=True)

    def test_shared_line_framer_and_rtc_compatibility(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "framer-test")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I" + str(ROOT / "components/roundie_time/include"),
                            "-I" + str(ROOT / "components/roundie_board/include"),
                            str(ROOT / "components/roundie_board/rtc_time.c"),
                            str(ROOT / "components/roundie_time/time_protocol.c"), str(ROOT / "tests/test_time_protocol.c"),
                            "-o", executable], check=True)
            subprocess.run([executable], timeout=10, check=True)

    def test_image_identity_size_and_streaming_hash(self):
        raw = image()
        info = tool.inspect_image(io.BytesIO(raw))
        self.assertEqual(info["digest"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(info["elf"], raw[176:208].hex())
        self.assertEqual(info["size"], len(raw))
        for bad in (b"", raw[:287], bytes(1024), raw[:12] + b"\x00\x00" + raw[14:],
                    raw[:80] + b"other" + raw[85:], raw + bytes(tool.CAPACITY)):
            with self.subTest(length=len(bad)), self.assertRaises(tool.DeviceError):
                tool.inspect_image(io.BytesIO(bad))

    def test_transfer_chunks_and_separate_activation(self):
        port = FakeSerial()
        raw = image(16384)
        ready = tool.upload(port, io.BytesIO(raw), progress=lambda _: None)
        self.assertEqual(port.data, raw)
        self.assertLessEqual(port.max_payload, 512)
        self.assertEqual(ready["digest"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(port.commands[-1], "END")
        self.assertNotIn("ACTIVATE", port.commands)

    def test_upload_failure_aborts_and_never_activates(self):
        for failure in ("data_disconnect", "wrong_progress", "wrong_identity"):
            port = FakeSerial(failure)
            with self.subTest(failure=failure), self.assertRaises((tool.TransportError, tool.DeviceError)):
                tool.upload(port, io.BytesIO(image()), progress=lambda _: None)
            self.assertIn("ABORT", port.commands)
            self.assertNotIn("ACTIVATE", port.commands)

    def test_lost_begin_reply_still_attempts_abort(self):
        with patch.object(tool, "request", side_effect=[status(), tool.TransportError("lost BEGIN"), status()]) as request:
            with self.assertRaises(tool.TransportError):
                tool.upload(None, io.BytesIO(image()))
        self.assertTrue(request.call_args_list[-1].args[1].startswith("ABORT "))

    def test_invalid_running_state_or_pending_selection_prevents_begin(self):
        for changed in ({"ota_state": "PENDING_VERIFY"}, {"ota_state": "UNDEFINED"},
                        {"passed": 0}, {"boot": "ota_1"}, {"phase": "RECEIVING"}):
            port = FakeSerial()
            port.state.update(changed)
            with self.subTest(changed=changed), self.assertRaises(tool.DeviceError):
                tool.upload(port, io.BytesIO(image()))
            self.assertEqual(port.commands, ["STATUS"])

    def test_transport_failures_have_bounded_deadlines(self):
        for failure in ("short", "disconnect", "timeout", "malformed", "truncated", "oversized"):
            started = time.monotonic()
            with self.subTest(failure=failure), self.assertRaises(tool.TransportError):
                tool.request(FakeSerial(failure), "STATUS", timeout=0.025)
            self.assertLess(time.monotonic() - started, 0.25)

    def test_reply_schema_and_device_error(self):
        good = status()
        self.assertIsNone(tool.parse_reply(b"ordinary diagnostic log\n", "1234abcd"))
        self.assertEqual(tool.parse_reply(reply("1234abcd"), "1234abcd"), {**good, "refusal": "none"})
        for changed in ({"received": -1}, {"received": 100, "total": 50}, {"ota_state": "garbage"},
                        {"elf": "xx"}, {"phase": "BOGUS"}, {"passed": 2}, {"session": "x"},
                        {"total": tool.CAPACITY + 1}, {"running": "nvs"}, {"version": 1}):
            with self.subTest(changed=changed), self.assertRaises(tool.TransportError):
                tool.parse_reply(reply("1234abcd", {**good, **changed}), "1234abcd")
        with self.assertRaises(tool.DeviceError):
            tool.parse_reply(reply("1234abcd", code=258), "1234abcd")

    def test_busy_and_precondition_refusals(self):
        """P1 fix 3: 503 is transient (DeviceBusy, retried); 259 names a precondition."""
        good = status()
        refused = {**good, "refusal": "io"}
        with self.assertRaises(tool.DeviceBusy):
            tool.parse_reply(reply("1234abcd", refused, code=tool.BUSY_CODE), "1234abcd")
        instructions = {"app": "unload --instance", "transfer": "abort --session", "package": "busy 0",
                        "safe-mode": "roundie_safe.py reboot", "not-ready": "activate"}
        for reason, hint in instructions.items():
            with self.subTest(reason=reason):
                with self.assertRaises(tool.DeviceError) as caught:
                    tool.parse_reply(reply("1234abcd", {**good, "refusal": reason}, code=259), "1234abcd")
                self.assertNotIsInstance(caught.exception, tool.DeviceBusy)
                self.assertIn("precondition, not retried", str(caught.exception))
                self.assertIn(hint, str(caught.exception))
        for state, code in (({**good, "refusal": "none"}, tool.BUSY_CODE), (refused, 259),
                            ({**good, "refusal": "bogus"}, 259)):
            with self.subTest(state=state["refusal"], code=code), self.assertRaises(tool.TransportError):
                tool.parse_reply(reply("1234abcd", state, code=code), "1234abcd")
        # An image from before fix 3 sends no "refusal"; OTA from it still parses.
        self.assertEqual(tool.parse_reply(reply("1234abcd", good), "1234abcd")["refusal"], "none")

    def test_request_retries_only_busy(self):
        port = FakeSerial()
        busy = tool.DeviceBusy("busy")
        with patch.object(tool, "read_serial_reply", return_value=b""), \
                patch("roundie_time.time.sleep") as sleep:
            with patch.object(tool, "parse_reply", side_effect=[busy, busy, {"phase": "IDLE"}]) as parse:
                self.assertEqual(tool.request(port, "STATUS"), {"phase": "IDLE"})
                self.assertEqual(parse.call_count, 3)
            self.assertEqual(sleep.call_count, 2)
            with patch.object(tool, "parse_reply", side_effect=[busy] * 5) as parse, \
                    self.assertRaises(tool.DeviceBusy):
                tool.request(port, "STATUS")
            self.assertEqual(parse.call_count, tool.retry_busy.__defaults__[0])
            refusal = tool.DeviceError("OTA refused (259, precondition, not retried)")
            with patch.object(tool, "parse_reply", side_effect=[refusal, {"phase": "IDLE"}]) as parse, \
                    self.assertRaises(tool.DeviceError):
                tool.request(port, "STATUS")
            self.assertEqual(parse.call_count, 1)

    def test_verify_requires_valid_boot_and_expected_identity(self):
        tool.verify(status())
        for changed in ({"ota_state": "PENDING_VERIFY"}, {"ota_state": "UNDEFINED"},
                        {"boot": "ota_1"}, {"phase": "READY"}, {"checked": 0}, {"passed": 0}):
            with self.subTest(changed=changed), self.assertRaises(tool.DeviceError):
                tool.verify({**status(), **changed})
        with self.assertRaises(tool.DeviceError):
            tool.verify(status(), {"elf": "b" * 64})

    def test_meaningful_exit_codes(self):
        for error, expected in ((tool.TransportError("timeout"), 2), (tool.DeviceError("rejected"), 3)):
            with patch.object(tool, "open_serial", side_effect=error), patch("sys.stderr", new_callable=io.StringIO):
                self.assertEqual(tool.main(["--port", "HOST_TEST_ONLY", "status"]), expected)


if __name__ == "__main__":
    unittest.main()
