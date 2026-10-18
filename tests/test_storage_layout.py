"""Offline fault injection for artifacts; never invokes a serial tool."""

import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(os.environ.get("IDF_PATH", "/nonexistent")) / "components/partition_table"))
import flash_bringup as guard
import validate_storage_layout as validation
import patch_littlefs
import gen_esp32part as generator


class StorageHostTest(unittest.TestCase):
    def test_pinned_capacity_error_patch(self):
        source = (ROOT / "managed_components/joltwallet__littlefs/src/esp_littlefs.c").read_bytes()
        with self.assertRaisesRegex(ValueError, "source changed"):
            patch_littlefs.patched_source(source + b"\n")
        patched = patch_littlefs.patched_source(source)
        start = patched.index("esp_err_t get_total_and_used_bytes(esp_littlefs_t *efs, size_t *total_bytes, size_t *used_bytes) {")
        end = patched.index("\n}\n", start) + 3
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "capacity_helper.inc").write_text(patched[start:end])
            executable = str(directory / "capacity-test")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I" + temporary,
                            "-I" + str(ROOT / "tests/storage_mocks"),
                            str(ROOT / "tests/test_littlefs_capacity.c"),
                            "-o", executable], check=True)
            subprocess.run([executable], check=True, timeout=10)

    def test_actual_manager_with_mock_backend(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = str(Path(temporary) / "storage-test")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-fsanitize=address,undefined", "-g",
                "-I" + str(ROOT / "tests/storage_mocks"),
                "-I" + str(ROOT / "components/roundie_storage/include"),
                str(ROOT / "components/roundie_storage/storage.c"),
                str(ROOT / "tests/test_storage.c"), "-o", executable,
            ], check=True)
            subprocess.run([executable], check=True, timeout=10)


class DeploymentOrchestrationTest(unittest.TestCase):
    def test_rejected_build_never_opens_device(self):
        with mock.patch.object(sys, "argv", ["guard", "--port", "HOST_TEST_ONLY", "--write"]), \
             mock.patch.object(guard, "check_legacy_build", side_effect=ValueError("profile")), \
             mock.patch.object(guard, "read_device") as read_device, \
             mock.patch.object(guard.subprocess, "run") as run:
            with self.assertRaises(ValueError):
                guard.main()
            read_device.assert_not_called()
            run.assert_not_called()

    def test_default_is_read_only_and_write_uses_checked_snapshot(self):
        for write in (False, True):
            with self.subTest(write=write), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                build = root / "build"
                (build / "bootloader").mkdir(parents=True)
                (build / "bootloader/bootloader.bin").write_bytes(b"\xe9" + bytes(100))
                (build / "flasher_args.json").write_text(json.dumps({"flash_files": {}}))
                backup = root / "backup.bin"
                backup.write_bytes(b"host fixture")
                app = b"validated host fixture"
                (build / "roundie.bin").write_bytes(app)

                def after_preflight(*arguments):
                    self.assertEqual(arguments[-2], app)
                    (build / "roundie.bin").write_bytes(b"concurrent replacement")

                def pretend_write(command, **kwargs):
                    self.assertEqual(command[-3], "write-flash")
                    self.assertEqual(command[-2], "0x20000")
                    self.assertEqual(Path(command[-1]).read_bytes(), app)
                    self.assertNotEqual(Path(command[-1]), build / "roundie.bin")
                    self.assertTrue(kwargs["check"])

                argv = ["guard", "--port", "HOST_TEST_ONLY"] + (["--write"] if write else [])
                with mock.patch.object(sys, "argv", argv), \
                     mock.patch.object(guard, "ROOT", root), \
                     mock.patch.object(guard, "BACKUP", backup), \
                     mock.patch.object(guard, "check_legacy_build", return_value=app), \
                     mock.patch.object(guard, "read_device", return_value=b"host fixture") as reads, \
                     mock.patch.object(guard, "preflight", side_effect=after_preflight), \
                     mock.patch.object(guard.subprocess, "run", side_effect=pretend_write) as writes, \
                     contextlib.redirect_stdout(io.StringIO()):
                    guard.main()
                self.assertEqual(reads.call_count, 3)
                self.assertEqual(writes.call_count, 1 if write else 0)


class LayoutTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.build = Path(self.temporary.name)
        for relative in ["roundie-profile.txt", "partition_table/partition-table.bin",
                         "config/sdkconfig.json", "flasher_args.json", "ota_data_initial.bin",
                         "bootloader/bootloader.bin", "roundie_rep.bin"]:
            target = self.build / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / "build-rep" / relative, target)

    def validate(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return validation.validate_build(self.build, generator)

    def test_valid_build(self):
        self.validate()

    def test_table_faults(self):
        for fault in ("overlap", "unaligned", "bounds", "wrong_subtype", "flags"):
            with self.subTest(fault=fault):
                table = generator.PartitionTable.from_csv((ROOT / "partitions/rep.csv").read_text())
                if fault == "overlap":
                    table[4].offset = 0x380000
                elif fault == "unaligned":
                    table[3].offset += 0x1000
                elif fault == "bounds":
                    table[-1].size += 0x1000
                elif fault == "wrong_subtype":
                    table[5].subtype = 0x82
                else:
                    table[5].encrypted = True
                with self.assertRaises((ValueError, generator.InputError)):
                    validation.validate_table(table)

    def test_ota_metadata_must_be_erased(self):
        (self.build / "ota_data_initial.bin").write_bytes(bytes(0x2000))
        with self.assertRaisesRegex(ValueError, "OTA metadata"):
            self.validate()

    def test_profile_mismatch(self):
        (self.build / "roundie-profile.txt").write_text("legacy\n")
        with self.assertRaisesRegex(ValueError, "profile"):
            self.validate()

    def test_wrong_app_and_oversize(self):
        app = (self.build / "roundie_rep.bin").read_bytes()
        with self.assertRaisesRegex(ValueError, "identified REP"):
            validation.validate_app(bytes(288))
        with self.assertRaisesRegex(ValueError, "exceeds ota_0"):
            validation.validate_app(app + bytes(0x380001 - len(app)))

    def test_unsafe_config(self):
        config_path = self.build / "config/sdkconfig.json"
        original = json.loads(config_path.read_text())
        for key, bad in {"BOOTLOADER_APP_ROLLBACK_ENABLE": False,
                         "SPIRAM_XIP_FROM_PSRAM": True,
                         "PARTITION_TABLE_OFFSET": 0x9000,
                         "SECURE_FLASH_ENC_ENABLED": True}.items():
            with self.subTest(key=key):
                config_path.write_text(json.dumps({**original, key: bad}))
                with self.assertRaisesRegex(ValueError, "config mismatch"):
                    self.validate()

    def test_manifest_change_and_bootloader_overflow(self):
        path = self.build / "flasher_args.json"
        original = path.read_text()
        manifest = json.loads(original)
        manifest["flash_files"]["0x390000"] = "unexpected.bin"
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "manifest"):
            self.validate()
        path.write_text(original)
        (self.build / "bootloader/bootloader.bin").write_bytes(b"\xe9" + bytes(0x8000))
        with self.assertRaisesRegex(ValueError, "bootloader"):
            self.validate()

    def test_guard_rejects_rep_before_device_access(self):
        with self.assertRaisesRegex(ValueError, "legacy build profile"):
            guard.check_legacy_build(self.build)
        (self.build / "roundie-profile.txt").write_text("legacy\n")
        with self.assertRaisesRegex(ValueError, "pinned legacy table"):
            guard.check_legacy_build(self.build)
        shutil.copyfile(ROOT / "build/partition_table/partition-table.bin",
                        self.build / "partition_table/partition-table.bin")
        shutil.copyfile(self.build / "roundie_rep.bin", self.build / "roundie.bin")
        with self.assertRaisesRegex(ValueError, "embedded identity"):
            guard.check_legacy_build(self.build)

    def test_backup_rejection(self):
        invalid = self.build / "invalid-backup.bin"
        invalid.write_bytes(b"not a backup")
        with self.assertRaisesRegex(ValueError, "backup size/hash"):
            validation.validate_backup(invalid, self.validate(), generator)


if __name__ == "__main__":
    unittest.main()
