"""Linux desktop target (docs/roundie-core.md, M1): production launcher, UI bridge
and package manager under ASan/UBSan, driven by scripts in headless mode."""
from pathlib import Path
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOLCHAIN = Path("/home/solve/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin")
LVGL = ROOT / "managed_components/lvgl__lvgl/lvgl.h"

# lv_conf.h name -> (firmware Kconfig name, value as lv_conf.h spells it)
LVGL_KCONFIG = {
    "LV_COLOR_DEPTH": ("CONFIG_LV_COLOR_DEPTH", None),
    "LV_MEM_SIZE": ("CONFIG_LV_MEM_SIZE_KILOBYTES", lambda kib: f"({kib} * 1024)"),
    "LV_DEF_REFR_PERIOD": ("CONFIG_LV_DEF_REFR_PERIOD", None),
    "LV_DPI_DEF": ("CONFIG_LV_DPI_DEF", None),
    "LV_DRAW_SW_DRAW_UNIT_CNT": ("CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT", None),
    "LV_DRAW_SW_COMPLEX": ("CONFIG_LV_DRAW_SW_COMPLEX", lambda value: "1" if value == "y" else "0"),
    "LV_DRAW_SW_SHADOW_CACHE_SIZE": ("CONFIG_LV_DRAW_SW_SHADOW_CACHE_SIZE", None),
    "LV_DRAW_SW_CIRCLE_CACHE_SIZE": ("CONFIG_LV_DRAW_SW_CIRCLE_CACHE_SIZE", None),
    "LV_FONT_MONTSERRAT_14": ("CONFIG_LV_FONT_MONTSERRAT_14", lambda value: "1" if value == "y" else "0"),
    "LV_USE_FLEX": ("CONFIG_LV_USE_FLEX", lambda value: "1" if value == "y" else "0"),
    "LV_USE_GRID": ("CONFIG_LV_USE_GRID", lambda value: "1" if value == "y" else "0"),
}

FLOW = """\
wait-nav RESULT 5000
screenshot result.png
tap {back}
wait-nav LIST 5000
screenshot list.png
mark
tap {refresh}
wait-refreshed 5000
tap {first}
wait-nav RESULT 5000
key boot
swipe {swipe}
"""

# Canvas points of the controls, in panel pixels of each profile.
BOARDS = {
    "1.85b": {"size": 360, "back": "180 295", "refresh": "180 295", "first": "180 135",
              "swipe": "180 300 180 120 300"},
    "1.75c": {"size": 466, "back": "233 382", "refresh": "233 382", "first": "233 175",
              "swipe": "233 388 233 155 300"},
}


def defines(path):
    values = {}
    for line in path.read_text().splitlines():
        match = re.match(r"#define\s+(\w+)\s+(.+?)\s*(?:/\*.*)?$", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def png_size(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise AssertionError(f"{path} is not a PNG")
    return struct.unpack(">II", data[16:24])


def missing_tools():
    missing = [tool for tool in ("meson", "ninja", "pkg-config") if shutil.which(tool) is None]
    if missing:
        return missing
    return [package for package in ("sdl3", "vulkan", "tfpsacrypto")
            if subprocess.run(["pkg-config", "--exists", package]).returncode != 0]


class DesktopContractTests(unittest.TestCase):
    def test_esp_err_values_match_idf(self):
        idf = os.environ.get("IDF_PATH")
        if not idf:
            self.skipTest("IDF_PATH not set")
        reference = defines(Path(idf) / "components/esp_common/include/esp_err.h")
        desktop = defines(ROOT / "platform/linux/include/esp_err.h")
        codes = [name for name in desktop if name == "ESP_OK" or name == "ESP_FAIL" or name.startswith("ESP_ERR_")]
        self.assertGreater(len(codes), 10)
        for name in codes:
            self.assertEqual(int(desktop[name], 0), int(reference[name], 0), name)

    def test_lvgl_configuration_matches_firmware(self):
        configs = [path for path in (ROOT / "sdkconfig", ROOT / "sdkconfig.rep") if path.exists()]
        if not configs:
            self.skipTest("no generated sdkconfig; build the firmware first")
        desktop = defines(ROOT / "platform/linux/lv_conf.h")
        self.assertEqual(desktop["LV_FONT_DEFAULT"], "&lv_font_montserrat_14")
        for config in configs:
            firmware = {}
            for line in config.read_text().splitlines():
                if line.startswith("CONFIG_LV_") and "=" in line:
                    name, value = line.split("=", 1)
                    firmware[name] = value
            self.assertEqual(firmware.get("CONFIG_LV_FONT_DEFAULT_MONTSERRAT_14"), "y", config.name)
            self.assertEqual(firmware.get("CONFIG_LV_DRAW_SW_ASM_NONE"), "y", config.name)
            for name, (kconfig, convert) in LVGL_KCONFIG.items():
                expected = firmware[kconfig]
                if convert is not None:
                    expected = convert(expected)
                self.assertEqual(desktop[name], expected, f"{config.name}: {name}")


class DesktopRunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        missing = missing_tools()
        if missing:
            raise unittest.SkipTest("missing " + ", ".join(missing))
        if not LVGL.exists():
            raise unittest.SkipTest("managed_components/lvgl__lvgl missing; build the firmware first")
        cls.temporary = tempfile.TemporaryDirectory(prefix="roundie-desktop-test-")
        cls.work = Path(cls.temporary.name)
        build = cls.work / "build"
        subprocess.run(["meson", "setup", str(build), str(ROOT), "-Db_sanitize=address,undefined",
                        "-Dbuildtype=debug", "-Ddesktop=enabled"], check=True, capture_output=True)
        subprocess.run(["ninja", "-C", str(build)], check=True, capture_output=True)
        cls.binary = build / "roundie-desktop"
        env = dict(os.environ, PATH=str(TOOLCHAIN) + os.pathsep + os.environ["PATH"])
        cls.packages = []
        for name, binary, app_id, title in (("hello_elf", "hello", "org.roundie.hello", "Hello"),
                                            ("watch_elf", "watch", "org.roundie.watch", "Zegarek")):
            target = cls.work / name
            subprocess.run(["cmake", "-S", str(ROOT / "examples" / name), "-B", str(target)],
                           check=True, capture_output=True, env=env)
            subprocess.run(["cmake", "--build", str(target)], check=True, capture_output=True, env=env)
            package = cls.work / f"{binary}.rpk"
            subprocess.run(["python3", str(ROOT / "tools/roundie_package.py"), "build",
                            str(target / f"{binary}.app.elf"), "--output", str(package), "--id", app_id,
                            "--name", title, "--version", "1.0.0"], check=True, capture_output=True)
            cls.packages.append(package)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_desktop(self, board, script, *, install=True):
        directory = Path(tempfile.mkdtemp(dir=self.work))
        (directory / "script.txt").write_text(script)
        command = [str(self.binary), "--headless", "--board", board, "--data", str(directory / "data"),
                   "--script", "script.txt"]
        if install:
            for package in self.packages:
                command += ["--install", str(package)]
        env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                   ASAN_OPTIONS="detect_leaks=1")
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=60, env=env)
        return result, directory

    def test_launcher_flow_on_both_panels(self):
        for board, points in BOARDS.items():
            with self.subTest(board=board):
                result, directory = self.run_desktop(board, FLOW.format(**points))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn("runtime error", result.stderr)
                # The BOOT key is emulated; its consumer comes with M2.
                self.assertIn("boot pressed", result.stderr)
                size = points["size"]
                for name in ("result.png", "list.png"):
                    self.assertEqual(png_size(directory / name), (size, size))
                self.assertNotEqual((directory / "result.png").read_bytes(),
                                    (directory / "list.png").read_bytes())
                # The home package went through the real installer.
                self.assertTrue((directory / "data/storage/packages/org.roundie.watch/active").exists())

    def test_touch_off_the_button_does_nothing(self):
        script = "wait-nav LIST 5000\nmark\ntap 180 60\nwait-refreshed 1000\n"
        result, _ = self.run_desktop("1.85b", script, install=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn("timed out", result.stderr)

    def test_empty_registry_lists_no_packages(self):
        script = "wait-nav LIST 5000\nscreenshot empty.png\n"
        result, directory = self.run_desktop("1.75c", script, install=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(png_size(directory / "empty.png"), (466, 466))


if __name__ == "__main__":
    unittest.main()
