"""linux-x64.rep1 applications (docs/native-apps.md, N1-N3): the example
sources built for x86-64, the x86-64 validator under ASan/UBSan with malformed,
truncated and forbidden ELFs, and the per-target package check."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import roundie_native_app
import roundie_package
from roundie_time import DeviceError

from xtensa_toolchain import TOOLCHAIN

EXAMPLES = (("hello", "rep_app_entry", 1), ("event", "rep_app_entry_v2", 2), ("ui", "rep_app_entry_v3", 3),
            ("round", "rep_app_entry_v3", 3), ("sound", "rep_app_entry_v3", 3),
            ("stopwatch", "rep_app_entry_v3", 3), ("watch", "rep_app_entry_v3", 3))

# Each source is a valid ABI 3 application plus one forbidden construct.
APP = r'''
#include "rep/rep_app_v3.h"
REP3_APP_METADATA;
static int32_t pass(const rep_context_v2_t *context) { (void)context; return 0; }
static int32_t event(const rep_context_v2_t *context, const rep_event_v2_t *event)
{ (void)context; (void)event; return 0; }
REP2_EXPORT const rep_descriptor_v3_t *rep_app_entry_v3(const rep_api_v3_t *api)
{
    static const rep_descriptor_v3_t descriptor = {REP_ABI_V3, sizeof(descriptor),
        pass, pass, event, pass, pass, pass, pass, REP3_CAP_UI};
    EXTRA_USE;
    return rep_abi_v3(REP_ABI_V3) == api ? &descriptor : 0;
}
'''

FORBIDDEN = {
    # name: (extra declarations, extra statement, extra flags, expected error)
    "libc-import": ("int puts(const char *);", "puts(\"x\")", [], "import:puts"),
    "dependency": ("", "(void)0", ["-Wl,--no-as-needed", "-lm"], "dynamic-dependency"),
    "constructor": ("volatile int hit;\n__attribute__((constructor)) static void early(void) { hit = 1; }",
                    "(void)0", [], "initializer"),
    "tls": ("static __thread int counter;", "counter++", [], "segment-TLS-or-interpreter"),
    "ifunc": ("static void real(void) {}\nstatic void (*pick(void))(void) { return real; }\n"
              "void chosen(void) __attribute__((ifunc(\"pick\")));", "chosen()", [], "OS-ABI"),
    "execstack": ("", "(void)0", ["-Wl,-z,execstack"], "executable-stack"),
    "wrong-abi": ("const void *rep_abi_v1(unsigned);", "(void)rep_abi_v1(1)", [], "import:rep_abi_v1"),
    "lazy": ("", "(void)0", ["-Wl,-z,lazy"], "OK"),
    # ld.so would bind it to the desktop's service (the loader also checks bindings).
    "service-name": ('#include "rep/rep_storage_v1.h"\n'
                     "REP2_EXPORT const rep_storage_v1_t *rep_storage_v1(uint32_t v) { (void)v; return 0; }",
                     "(void)rep_storage_v1(1)", [], "service-symbol-defined"),
}


def checker(directory, x86_64):
    output = directory / ("check-x86_64" if x86_64 else "check-xtensa")
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    *(["-DROUNDIE_TARGET_LINUX_X64=1"] if x86_64 else []),
                    "-I" + str(ROOT / "include"), "-I" + str(ROOT / "components/roundie_elf/include"),
                    str(ROOT / "tools/validate_elf.c"), str(ROOT / "components/roundie_elf/validate.c"),
                    str(ROOT / "components/roundie_elf/validate_x86_64.c"), "-o", str(output)], check=True)
    return output


def verdict(check, path):
    result = subprocess.run([str(check), str(path)], capture_output=True, text=True)
    if result.returncode == 0:
        return "OK"
    return result.stderr.strip().removeprefix("ELF rejected: ")


class NativeElfTests(unittest.TestCase):
    def test_examples_validator_and_forbidden_constructs(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            x86_64 = checker(directory, True)
            xtensa = checker(directory, False)
            for stem, entry, abi in EXAMPLES:
                for standard in ("c11", "c2x"):
                    output = directory / f"{stem}-{standard}.app.so"
                    roundie_native_app.build(ROOT / "examples" / f"{stem}_elf" / f"{stem}.c", output, entry,
                                             compiler=f"cc -std={standard}")
                    self.assertEqual(verdict(x86_64, output), "OK", output.name)
                    self.assertEqual(verdict(xtensa, output), "class-endian-version", output.name)
                    probe = subprocess.run(["readelf", "-dW", str(output)], capture_output=True, text=True, check=True)
                    self.assertNotIn("NEEDED", probe.stdout)
            cases = []
            for name, (declarations, statement, flags, expected) in FORBIDDEN.items():
                source = directory / f"{name}.c"
                source.write_text(declarations + "\n" + APP.replace("EXTRA_USE", statement))
                output = directory / f"{name}.so"
                subprocess.run(["cc", *roundie_native_app.FLAGS, "-Wl,-e,rep_app_entry_v3",
                                "-I" + str(ROOT / "include"), *flags, str(source), "-o", str(output)], check=True)
                self.assertEqual(verdict(x86_64, output), expected, name)
                cases += [str(output), expected]
            hang = directory / "hang.app.so"
            roundie_native_app.build(ROOT / "examples/fault_elf/fault.c", hang, "rep_app_entry_v2", ["FAULT_KIND=1"])
            self.assertEqual(verdict(x86_64, hang), "OK")
            if (TOOLCHAIN / "xtensa-esp32s3-elf-gcc").exists():
                env = dict(os.environ, PATH=str(TOOLCHAIN) + os.pathsep + os.environ["PATH"])
                build = directory / "xtensa-hello"
                subprocess.run(["cmake", "-S", str(ROOT / "examples/hello_elf"), "-B", str(build)],
                               env=env, check=True, capture_output=True)
                subprocess.run(["cmake", "--build", str(build)], env=env, check=True, capture_output=True)
                self.assertEqual(verdict(xtensa, build / "hello.app.elf"), "OK")
                self.assertEqual(verdict(x86_64, build / "hello.app.elf"), "class-endian-version")
                cases += [str(build / "hello.app.elf"), "class-endian-version"]
            # N3: RPK1 unchanged, only the target differs; each build accepts its own.
            package = directory / "sound.rpk"
            info = roundie_package.build(directory / "sound-c11.app.so", package, "org.roundie.sound", "Sound",
                                         "1.0.0", "linux-x64.rep1")
            self.assertEqual((info["target"], info["abi"]), ("linux-x64.rep1", 3))
            with self.assertRaisesRegex(DeviceError, "package-incompatible"):
                roundie_package.inspect(package)
            with self.assertRaisesRegex(DeviceError, "class-endian-version"):
                roundie_package.build(directory / "sound-c11.app.so", directory / "esp.rpk", "org.roundie.sound",
                                      "Sound", "1.0.0")
            if (TOOLCHAIN / "xtensa-esp32s3-elf-gcc").exists():
                esp = directory / "hello-esp.rpk"
                roundie_package.build(build / "hello.app.elf", esp, "org.roundie.hello", "Hello", "1.0.0")
                with self.assertRaisesRegex(DeviceError, "package-incompatible|class-endian-version"):
                    roundie_package.inspect(esp, "linux-x64.rep1")
            driver = directory / "driver"
            subprocess.run(["cc", "-std=gnu2x", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-I" + str(ROOT / "include"), "-I" + str(ROOT / "components/roundie_elf/include"),
                            str(ROOT / "tests/test_elf_x86_64.c"), str(ROOT / "components/roundie_elf/validate.c"),
                            str(ROOT / "components/roundie_elf/validate_x86_64.c"), "-o", str(driver)], check=True)
            result = subprocess.run([str(driver), str(directory / "sound-c11.app.so"), *cases],
                                    capture_output=True, text=True, timeout=300)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("x86-64 validation PASS", result.stdout)
            print(result.stdout.strip(), file=sys.stderr)


if __name__ == "__main__":
    unittest.main()
