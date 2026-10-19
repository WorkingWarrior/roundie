import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import roundie_launcher as launcher


class LauncherDiagnosticsTests(unittest.TestCase):
    def response(self, **changes):
        result = dict(nav=2, revision=1, request=0, generation=0, count=2, selected="",
                      pending=[0, 0], foreground=1, input=[0, 0, 0], result=[0, 0, 42],
                      us=[0] * 9, gui=[33, 32000], stack_min=4096, discovery_flushes=4)
        result.update(changes)
        return b'!RP1 12345678 0 ' + json.dumps(result).encode() + b'\n'

    def test_status_and_bounded_validation(self):
        with patch.object(launcher.secrets, "token_hex", return_value="12345678"), \
             patch.object(launcher, "write_command"), patch.object(launcher, "read_serial_reply") as read:
            read.return_value = self.response()
            self.assertEqual(launcher.request(None)["state"], "LIST")
            for changes in (dict(nav=8), dict(count=9), dict(pending=[2, 0]), dict(us=[0]),
                            dict(selected="../invalid"), dict(gui=[-1, 0]), dict(revision=True)):
                read.return_value = self.response(**changes)
                with self.assertRaises((launcher.DeviceError, launcher.TransportError)):
                    launcher.request(None)

    def test_injection_bounds(self):
        for operation in ("SELECT -1 org.roundie.ui", "SELECT 4294967296 org.roundie.ui",
                          "SELECT 1 ../bad", "EXIT 4294967296", "PRESS 12 15"):
            with self.assertRaises(launcher.DeviceError):
                launcher.request(None, operation)
