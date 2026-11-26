"""Production preflight and manager with mock IDF/loader, under ASan/UBSan."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import roundie_elf

ROOT = Path(__file__).resolve().parents[1]
from xtensa_toolchain import TOOLCHAIN


SHORT_LIMIT = 700


class ElfTests(unittest.TestCase):
    def test_validation_lifecycle_and_target_sdk_layout(self):
        with tempfile.TemporaryDirectory() as temporary:
            env = dict(os.environ, PATH=str(TOOLCHAIN) + os.pathsep + os.environ["PATH"])
            example = str(Path(temporary) / "example")
            subprocess.run(["cmake", "-S", str(ROOT / "examples/hello_elf"), "-B", example], env=env, check=True, capture_output=True)
            subprocess.run(["cmake", "--build", example], env=env, check=True, capture_output=True)
            event = str(Path(temporary) / "event")
            subprocess.run(["cmake", "-S", str(ROOT / "examples/event_elf"), "-B", event], env=env, check=True, capture_output=True)
            subprocess.run(["cmake", "--build", event], env=env, check=True, capture_output=True)
            self.assertEqual(roundie_elf.inspect(Path(event) / "event.app.elf")["abi"], 2)
            executable = str(Path(temporary) / "tests")
            subprocess.run(["cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror", "-g",
                            "-fsanitize=address,undefined", "-pthread",
                            "-I" + str(ROOT / "tests/elf_mocks"), "-I" + str(ROOT / "include"),
                            "-I" + str(ROOT / "components/roundie_elf/include"),
                            "-I" + str(ROOT / "components/roundie_platform/include"),
                            "-I" + str(ROOT / "components/roundie_platform/idf"),
                            "-I" + str(ROOT / "components/roundie_app_ui/include"),
                            "-I" + str(ROOT / "components/roundie_audio/include"),
                            str(ROOT / "tests/test_elf.c"), str(ROOT / "components/roundie_elf/validate.c"),
                            "-o", executable], check=True)
            subprocess.run([executable, str(Path(example) / "hello.app.elf"), str(Path(event) / "event.app.elf")], check=True, timeout=20)
            staging = str(Path(temporary) / "staging")
            staging_command = ["cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror", "-g",
                            "-fsanitize=address,undefined", "-ffunction-sections", "-Wl,--gc-sections",
                            "-DCONFIG_ROUNDIE_DEV_PACKAGES=1",
                            "-I" + str(ROOT / "main"),
                            '-DDEVELOPMENT_DIRECTORY="' + str(Path(temporary) / "dev") + '"',
                            "-I" + str(ROOT / "tests/elf_mocks"), "-I" + str(ROOT / "tests/ota_mocks"),
                            "-I" + str(ROOT / "include"), "-I" + str(ROOT / "components/roundie_elf/include"),
                            "-I" + str(ROOT / "components/roundie_ota/include"),
                            "-I" + str(ROOT / "components/roundie_packages/include"),
                            "-I" + str(ROOT / "components/roundie_watchface/include"),
                            "-I" + str(ROOT / "components/roundie_app_ui/include"),
                            "-I" + str(ROOT / "components/roundie_boot_guard/include"),
                            "-I" + str(ROOT / "components/roundie_storage_mode/include"),
                            "-I" + str(ROOT / "components/roundie_storage/include"),
                            "-I" + str(ROOT / "components/roundie_platform/include"),
                            "-I" + str(ROOT / "platform/linux/include"), "-pthread",
                            str(ROOT / "components/roundie_storage_mode/storage_mode.c"),
                            str(ROOT / "tests/test_elf_development.c"), str(ROOT / "main/app_runtime.c"),
                            str(ROOT / "components/roundie_storage/bounded_io.c"),
                            str(ROOT / "components/roundie_storage/claim.c"),
                            str(ROOT / "components/roundie_elf/validate.c"),
                            str(ROOT / "components/roundie_ota/ota_protocol.c"), "-lcrypto"]
            subprocess.run([*staging_command, "-o", staging], check=True)
            subprocess.run([staging, str(Path(example) / "hello.app.elf")], check=True, timeout=10)
            # Substituted VFS: the same harness with every bounded call cut to
            # SHORT_LIMIT bytes, then EOF inside a 4 KiB chunk of the
            # development ELF read (read_candidate) on a larger ELF.
            stopwatch = str(Path(temporary) / "stopwatch")
            subprocess.run(["cmake", "-S", str(ROOT / "examples/stopwatch_elf"), "-B", stopwatch], env=env,
                           check=True, capture_output=True)
            subprocess.run(["cmake", "--build", stopwatch], env=env, check=True, capture_output=True)
            vfs = str(Path(temporary) / "staging-vfs")
            subprocess.run([*staging_command, "-I" + str(ROOT / "tests"), "-DROUNDIE_IO_READ=fake_read",
                            "-DROUNDIE_IO_WRITE=fake_write", "-DFAKE_VFS_SHORT=" + str(SHORT_LIMIT),
                            str(ROOT / "tests/fake_vfs.c"), "-o", vfs], check=True)
            subprocess.run([vfs, str(Path(example) / "hello.app.elf"), str(Path(stopwatch) / "stopwatch.app.elf")],
                           check=True, timeout=20)
            policy = str(Path(temporary) / "policy")
            subprocess.run(["cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror", "-g",
                            "-fsanitize=address,undefined", "-ffunction-sections", "-Wl,--gc-sections",
                            "-DCONFIG_ROUNDIE_APP_RUNTIME=1",
                            "-I" + str(ROOT / "tests/ota_mocks"),
                            "-I" + str(ROOT / "components/roundie_ota/include"),
                            "-I" + str(ROOT / "components/roundie_boot_guard/include"),
                            "-I" + str(ROOT / "components/roundie_storage/include"), "-I" + str(ROOT / "main"),
                            "-I" + str(ROOT / "components/roundie_platform/include"),
                            "-I" + str(ROOT / "platform/linux/include"), "-pthread", str(ROOT / "tests/io_worker_stub.c"),
                            str(ROOT / "components/roundie_storage/claim.c"),
                            str(ROOT / "tests/test_runtime_policy.c"), "-o", policy], check=True)
            subprocess.run([policy], check=True, timeout=10)
            for variant, reason in ((1, "import:esp_restart"), (2, "ABI-version")):
                subprocess.run(["cmake", "-S", str(ROOT / "examples/hello_elf"), "-B", example,
                                f"-DREP_TEST_VARIANT={variant}"], env=env, check=True, capture_output=True)
                subprocess.run(["cmake", "--build", example], env=env, check=True, capture_output=True)
                with self.assertRaisesRegex(roundie_elf.DeviceError, reason):
                    roundie_elf.inspect(Path(example) / "hello.app.elf")
            for probe in (0, 1, 2):
                path = Path(temporary) / f"probe-{probe}.elf"
                subprocess.run([str(TOOLCHAIN / "xtensa-esp32s3-elf-gcc"), "-std=c11", "-Os", "-fPIC",
                                "-fno-builtin", "-nostdlib", "-shared", "-mlongcalls", "-fvisibility=hidden",
                                "-Wl,-e,rep_app_entry", "-Wl,--undefined=rep_abi_metadata",
                                f"-DREP_COMPAT_PROBE={probe}", "-I" + str(ROOT / "include"),
                                str(ROOT / "tests/elf_compat_probe.c"), "-o", str(path)], check=True)
                with self.assertRaisesRegex(roundie_elf.DeviceError, "import:|TLS|unsupported-relocation") as rejected:
                    roundie_elf.inspect(path)
                print(f"Xtensa compatibility probe {probe}: {rejected.exception}")
            # Public header's exact Xtensa pointer layouts in C11 and C23.
            for standard in ("c11", "c23"):
                subprocess.run([str(TOOLCHAIN / "xtensa-esp32s3-elf-gcc"), f"-std={standard}",
                                "-Wall", "-Wextra", "-Werror", "-fsyntax-only", "-x", "c",
                                "-I" + str(ROOT / "include"), "-"], input='#include "rep/rep_app.h"\n#include "rep/rep_app_v2.h"\n#include "rep/rep_app_v3.h"\n'
                                      '#include "rep/rep_storage_v1.h"\n#include "rep/rep_audio_v1.h"\n',
                                text=True, check=True)


if __name__ == "__main__":
    unittest.main()
