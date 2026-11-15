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
        # The firmware's default font comes from the top-level CMakeLists, not Kconfig.
        cmake = (ROOT / "CMakeLists.txt").read_text()
        for name in ("LV_FONT_DEFAULT", "LV_FONT_CUSTOM_DECLARE"):
            match = re.search(rf'"{name}=([^"]+)"', cmake)
            self.assertIsNotNone(match, name)
            self.assertEqual(desktop[name], match.group(1), name)
        for config in configs:
            firmware = {}
            for line in config.read_text().splitlines():
                if line.startswith("CONFIG_LV_") and "=" in line:
                    name, value = line.split("=", 1)
                    firmware[name] = value
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

    def test_slide_off_a_button_still_clicks(self):
        """Production behaviour, not a desktop artefact: LVGL keeps a pressed
        button pressed when the finger slides off (LV_OBJ_FLAG_PRESS_LOCK), so
        the release still clicks it, on the board's touch panel as here."""
        script = "wait-nav RESULT 5000\nswipe 233 382 465 400 400\nwait-nav LIST 3000\n"
        result, _ = self.run_desktop("1.75c", script)
        self.assertEqual(result.returncode, 0, result.stderr)

    def run_in_gamescope(self, app_script, drive):
        """Runs the 1.75C launcher at zoom 2 in gamescope's headless backend (a
        virtual 932x932 monitor), resizes the window to 700x500 so that the
        panel shows at zoom 1 from (117,17), then runs the xdotool driver."""
        tools = [tool for tool in ("gamescope", "gamescopectl", "xdotool") if shutil.which(tool) is None]
        if tools:
            self.skipTest("missing " + ", ".join(tools))
        directory = Path(tempfile.mkdtemp(dir=self.work))
        (directory / "script.txt").write_text(app_script)
        installs = " ".join(f"--install {package}" for package in self.packages)
        (directory / "drive.sh").write_text(f"""#!/bin/bash
cd {directory}
press() {{ xdotool mousemove --sync $1 $2; sleep 0.2; xdotool mousedown 1; sleep 0.15; }}
drag() {{ for i in $(seq 1 $5); do xdotool mousemove --sync $(($1+($3-$1)*i/$5)) $(($2+($4-$2)*i/$5)); sleep 0.04; done; }}
release() {{ sleep 0.15; xdotool mouseup 1; }}
click() {{ press $1 $2; release; }}
{self.binary} --board 1.75c --zoom 2 --data {directory}/data {installs} --script script.txt > app.log 2>&1 &
app=$!
sleep 4
xdotool key b
xdotool windowsize --sync $(xdotool getactivewindow) 700 500
sleep 1.5
{drive}
wait $app
echo "exit $?" >> app.log
""")
        (directory / "drive.sh").chmod(0o755)
        env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                   ASAN_OPTIONS="detect_leaks=0")  # the graphics stack leaks on unload
        subprocess.run(["gamescope", "--backend", "headless", "-W", "932", "-H", "932", "--",
                        str(directory / "drive.sh")], capture_output=True, timeout=120, env=env)
        log = (directory / "app.log").read_text()
        self.assertIn("exit 0", log, log)
        self.assertNotIn("runtime error", log)
        self.assertIn("boot pressed", log)
        self.assertGreaterEqual(log.count("Creating swapchain"), 2, "resize did not recreate the swapchain")
        self.assertIn("vulkan: swapchain 700x500, panel shown 466x466 at 117,17", log)
        return log, directory

    def test_window_in_gamescope(self):
        """Vulkan presentation, mouse, keys and resizing through a real
        compositor. The application checks every transition; the driver clicks."""
        log, directory = self.run_in_gamescope(
            "wait-nav RESULT 15000\nwait-nav LIST 15000\nwait-nav RESULT 15000\nwait 500\nstatus\nquit\n",
            "gamescopectl screenshot $PWD/resized.png\nsleep 1\nclick 350 399\nsleep 2\nclick 350 153\n")
        self.assertIn("launcher state=RESULT", log)
        self.assertIn("request=2", log)  # the second launch came from the mouse
        self.assertEqual(png_size(directory / "resized.png"), (700, 500))  # the resized window

    def test_drag_gestures_in_gamescope(self):
        """A drag on the list scrolls it without launching; a drag from the
        left-edge strip goes home (the launcher's own edge handler)."""
        log, _ = self.run_in_gamescope(
            "wait-nav RESULT 15000\nwait-nav LIST 15000\nwait 6000\nexpect-nav LIST\n"
            "wait-nav RESULT 15000\nwait 300\nstatus\nquit\n",
            "click 350 399\nsleep 1\n"
            "press 350 192; drag 350 192 350 40 10; release\nsleep 4.5\n"
            "press 127 250; drag 127 250 347 255 12; release\n")
        self.assertIn("launcher state=RESULT", log)
        self.assertIn("request=2", log)

    def test_pointer_outside_the_panel_in_gamescope(self):
        """A release outside the window still arrives (no stuck press), and a
        press in the letterbox around the panel is not a touch."""
        log, _ = self.run_in_gamescope(
            "wait-nav RESULT 15000\nwait-nav LIST 15000\nwait 4000\nexpect-nav LIST\n"
            "wait-nav RESULT 15000\nwait 300\nstatus\nquit\n",
            # Back, dragged off the right side of the window: still a click (see
            # test_slide_off_a_button_still_clicks).
            "press 350 399; drag 350 399 900 420 12; release\nsleep 1\n"
            # From the letterbox onto the first entry: ignored.
            "press 40 250; drag 40 250 350 192 8; release\nsleep 4.5\n"
            "click 350 192\n")
        self.assertIn("launcher state=RESULT", log)
        self.assertIn("request=2", log)

    def test_window_in_nested_kwin_at_scale_2(self):
        """HiDPI: a virtual KWin output at scale 2 in its own D-Bus session and
        configuration directories (never the user's session). The window gets
        device pixels; pointer events come from KWin's fake-input protocol."""
        tools = [tool for tool in ("kwin_wayland", "dbus-run-session", "gdbus", "wayland-scanner", "cc")
                 if shutil.which(tool) is None]
        if tools:
            self.skipTest("missing " + ", ".join(tools))
        protocol = Path(os.environ.get("ROUNDIE_FAKE_INPUT_XML",
                                       "/usr/share/plasma-wayland-protocols/fake-input.xml"))
        if not protocol.exists():
            self.skipTest("fake-input.xml not found; set ROUNDIE_FAKE_INPUT_XML")
        directory = Path(tempfile.mkdtemp(dir=self.work))
        for mode in ("client-header", "private-code"):
            output = directory / ("fake-input.h" if mode == "client-header" else "fake-input.c")
            subprocess.run(["wayland-scanner", mode, str(protocol), str(output)], check=True)
        flags = subprocess.run(["pkg-config", "--cflags", "--libs", "wayland-client"], check=True,
                               capture_output=True, text=True).stdout.split()
        injector = directory / "fake-input"
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-I", str(directory), "-o", str(injector),
                        str(ROOT / "tests/desktop_fake_input.c"), str(directory / "fake-input.c"), *flags],
                       check=True)
        for name in ("config", "cache", "data/applications"):
            (directory / name).mkdir(parents=True)
        # KWin offers fake input only to clients that a desktop file names.
        (directory / "data/applications/roundie-fake-input.desktop").write_text(
            f"[Desktop Entry]\nType=Application\nName=Roundie input test\nExec={injector}\nNoDisplay=true\n"
            "X-KDE-Wayland-Interfaces=org_kde_kwin_fake_input\n")
        # Place the borderless 466x466 (logical) window at (100,50).
        (directory / "place.js").write_text(
            "for (const w of workspace.windowList()) {\n"
            "    if (w.caption.indexOf(\"Roundie\") === 0) {\n"
            "        w.noBorder = true;\n"
            "        w.frameGeometry = {x: 100, y: 50, width: 466, height: 466};\n"
            "    }\n}\n")
        events = []
        def point(x, y):
            events.append(f"move {100 + x} {50 + y}")
        # The panel shows at zoom 2 in 932x932 device pixels: one logical point per panel pixel.
        point(233, 382)
        events += ["sleep 200", "down", "sleep 150", "up", "sleep 1500"]  # Back -> list
        point(10, 233)
        events += ["sleep 200", "down", "sleep 100"]
        for step in range(1, 13):
            point(10 + step * 18, 233 + step // 4)
            events.append("sleep 40")
        events += ["sleep 150", "up", "sleep 500"]  # left-edge swipe -> home
        (directory / "input.txt").write_text("\n".join(events) + "\n")
        (directory / "script.txt").write_text(
            "wait-nav RESULT 15000\nwait-nav LIST 20000\nwait-nav RESULT 20000\nwait 300\nstatus\nquit\n")
        installs = " ".join(f"--install {package}" for package in self.packages)
        (directory / "inner.sh").write_text(f"""#!/bin/bash
cd {directory}
SDL_VIDEO_DRIVER=wayland {self.binary} --board 1.75c --zoom 1 --data {directory}/appdata {installs} --script script.txt > app.log 2>&1 &
app=$!
sleep 3
id=$(gdbus call --session --dest org.kde.KWin --object-path /Scripting --method org.kde.kwin.Scripting.loadScript {directory}/place.js place | grep -o '[0-9]\\+')
gdbus call --session --dest org.kde.KWin --object-path /Scripting/Script$id --method org.kde.kwin.Script.run > /dev/null
sleep 0.5
{injector} < input.txt > inject.log 2>&1
wait $app
echo "exit $?" >> app.log
""")
        (directory / "inner.sh").chmod(0o755)
        env = {name: value for name, value in os.environ.items()
               if name not in ("WAYLAND_DISPLAY", "DISPLAY", "XDG_SESSION_ID", "DBUS_SESSION_BUS_ADDRESS")}
        env.update(XDG_CONFIG_HOME=str(directory / "config"), XDG_CACHE_HOME=str(directory / "cache"),
                   XDG_DATA_HOME=str(directory / "data"), ASAN_OPTIONS="detect_leaks=0",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        def kwin(session):
            subprocess.run(["dbus-run-session", "--", "kwin_wayland", "--virtual", "--width", "932", "--height",
                            "932", "--scale", "2", "--no-lockscreen", "--socket", f"roundie-test-{os.getpid()}",
                            "--exit-with-session", session], capture_output=True, timeout=90, env=env,
                           cwd=directory)
        # The first start writes the output configuration; set its scale to 2.
        kwin("/bin/true")
        outputs = directory / "config/kwinoutputconfig.json"
        self.assertIn('"scale": 1,', outputs.read_text())
        outputs.write_text(outputs.read_text().replace('"scale": 1,', '"scale": 2,'))
        kwin(str(directory / "inner.sh"))
        log = (directory / "app.log").read_text()
        self.assertIn("exit 0", log, log + (directory / "inject.log").read_text())
        self.assertNotIn("runtime error", log)
        self.assertIn("466x466, 932x932 pixels, density 2.00", log)
        self.assertIn("vulkan: swapchain 932x932, panel shown 932x932 at 0,0", log)
        self.assertIn("request=2", log)

    def test_empty_registry_lists_no_packages(self):
        script = "wait-nav LIST 5000\nscreenshot empty.png\n"
        result, directory = self.run_desktop("1.75c", script, install=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(png_size(directory / "empty.png"), (466, 466))


if __name__ == "__main__":
    unittest.main()
