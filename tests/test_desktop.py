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

# linux-x64.rep1 packages: (example, entry, id, name, defines).
NATIVE_APPS = (
    ("hello", "rep_app_entry", "org.roundie.hello", "Hello", ()),
    ("watch", "rep_app_entry_v3", "org.roundie.watch", "Zegarek", ()),
    ("stopwatch", "rep_app_entry_v3", "org.roundie.stopwatch", "Stoper", ()),
    ("sound", "rep_app_entry_v3", "org.roundie.sound", "Dźwięk", ()),
    ("hang", "rep_app_entry_v2", "org.roundie.hang", "Hang", ("FAULT_KIND=1",)),
)

# On the 1.85B panel: a swipe up from the home application's bottom strip
# opens the list, and a swipe right from the left-edge strip closes an
# application (components/roundie_launcher/render.c).
HOME_TO_LIST = "down 180 320\nwait 50\nmove 180 250\nwait 50\nmove 180 150\nwait 50\nup\n"
CLOSE_APP = "down 10 180\nwait 50\nmove 60 182\nwait 50\nmove 150 185\nwait 50\nup\n"

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


    def test_power_configuration_matches_rep_profile(self):
        config = ROOT / "sdkconfig.rep"
        if not config.exists():
            self.skipTest("no sdkconfig.rep; build the rep firmware first")
        firmware = dict(line.split("=", 1) for line in config.read_text().splitlines()
                        if line.startswith("CONFIG_ROUNDIE_POWER") and "=" in line)
        desktop = defines(ROOT / "platform/linux/include/sdkconfig.h")
        self.assertEqual(firmware.get("CONFIG_ROUNDIE_POWER"), "y")
        for name in ("CONFIG_ROUNDIE_POWER_SCREEN_TIMEOUT_SECONDS", "CONFIG_ROUNDIE_POWER_SLEEP_SAFETY_SECONDS"):
            self.assertEqual(desktop[name], firmware[name], name)


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
        # The desktop's package target is linux-x64.rep1: the same example
        # sources, built for x86-64 (docs/native-apps.md, N1-N3).
        cls.native = {}
        for stem, entry, app_id, title, defines in NATIVE_APPS:
            binary = cls.work / f"{stem}.app.so"
            subprocess.run(["python3", str(ROOT / "tools/roundie_native_app.py"), str(ROOT / "examples" /
                            ("fault_elf/fault.c" if stem == "hang" else f"{stem}_elf/{stem}.c")),
                            "--entry", entry, "-o", str(binary), *(f"-D{define}" for define in defines)],
                           check=True, capture_output=True)
            package = cls.work / f"{stem}.rpk"
            subprocess.run(["python3", str(ROOT / "tools/roundie_package.py"), "build", str(binary),
                            "--output", str(package), "--id", app_id, "--name", title, "--version", "1.0.0",
                            "--target", "linux-x64.rep1"], check=True, capture_output=True)
            cls.native[stem] = package
        cls.packages = [cls.native["hello"], cls.native["watch"]]

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_desktop(self, board, script, *, install=True, packages=None, native=False, timeout=60):
        directory = Path(tempfile.mkdtemp(dir=self.work))
        (directory / "script.txt").write_text(script)
        command = [str(self.binary), "--headless", "--board", board, "--data", str(directory / "data"),
                   "--script", "script.txt"]
        if native:
            command.append("--native-apps")
        if install:
            for package in self.packages if packages is None else packages:
                command += ["--install", str(package)]
        env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                   ASAN_OPTIONS="detect_leaks=1")
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=timeout, env=env)
        return result, directory

    def test_launcher_flow_on_both_panels(self):
        for board, points in BOARDS.items():
            with self.subTest(board=board):
                result, directory = self.run_desktop(board, FLOW.format(**points))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn("runtime error", result.stderr)
                # The emulated BOOT key reaches the firmware's power menu (M2).
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

    def test_power_menu(self):
        """M2: the firmware's power GUI (main/power_gui.c) on the emulated board.
        Menu buttons are canvas points of main/power_gui.c on the 1.85B panel."""
        script = """\
wait-nav RESULT 5000
screenshot before.png
key boot
wait 300
screenshot menu.png
tap 180 157
wait 300
power
tap 180 280
wait 300
screenshot closed.png
key boot
wait 300
tap 180 116
wait-power screen_off 2000
power
screenshot off.png
tap 180 295
wait-power active 2000
wait 300
expect-nav RESULT
key boot
wait 300
tap 180 75
wait-power light_sleep 3000
wait 500
key boot
wait-power active 3000
wait 300
power
screenshot woke.png
expect-nav RESULT
"""
        result, directory = self.run_desktop("1.85b", script)
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertNotIn("runtime error", result.stderr)
        before = (directory / "before.png").read_bytes()
        self.assertNotEqual((directory / "menu.png").read_bytes(), before)
        # Restart is refused on the desktop and Anuluj closes the menu.
        self.assertIn('power state=active lit=1 sleeps=0 screen_offs=0 message="Niedostępne na komputerze"',
                      output)
        self.assertEqual((directory / "closed.png").read_bytes(), before)
        # The waking tap lands on Back but only wakes the screen (expect-nav RESULT).
        self.assertIn("power state=screen_off lit=0", output)
        self.assertNotEqual((directory / "off.png").read_bytes(), before)
        self.assertIn("light sleep (emulated)", output)
        self.assertIn("woke after", output)
        self.assertIn("power state=active lit=1 sleeps=1 screen_offs=1", output)
        self.assertEqual((directory / "woke.png").read_bytes(), before)

    def run_time_script(self, board, script):
        result, directory = self.run_desktop(board, script, install=False)
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertNotIn("runtime error", output)
        clocks = [dict(field.split("=", 1) for field in line.split()[1:])
                  for line in result.stdout.splitlines() if line.startswith("clock ")]
        replies = [line for line in result.stdout.splitlines() if line.startswith("!RT1 ")]
        return output, clocks, replies, directory

    def test_time_without_rtc_across_resets(self):
        """M3 on the 1.75C model: only a trusted SET makes time valid; resets
        that keep the RTC domain keep it, power loss and brownout do not."""
        script = """\
clock
rt1 SET 1792314126
clock
rt1 TZ CET-1CEST,M3.5.0,M10.5.0/3
clock
reset sw
clock
reset panic
clock
reset wdt
clock
reset power
clock
rt1 SET 1792314126
reset brownout
clock
rt1 SET 1792314126
reset pin
clock
"""
        output, clocks, replies, directory = self.run_time_script("1.75c", script)
        self.assertEqual(clocks[0]["valid"], "0")
        self.assertEqual(clocks[0]["local"], "0")
        self.assertEqual(replies[0], "!RT1 0000d0e5 OK state=1 os=-1 rtc=-1 wall=1792314126 err=0")
        # 2026-10-18 09:02:06 UTC; days since 1970 and ms of day in UTC, then CEST.
        self.assertEqual((clocks[1]["valid"], clocks[1]["days"]), ("1", "20744"))
        self.assertEqual(int(clocks[1]["ms"]) // 1000, 9 * 3600 + 2 * 60 + 6)
        self.assertEqual(int(clocks[2]["ms"]) // 1000, 11 * 3600 + 2 * 60 + 6)
        self.assertEqual(int(clocks[2]["revision"]), int(clocks[1]["revision"]) + 1)
        self.assertEqual((directory / "data/clock-tz").read_text(), "CET-1CEST,M3.5.0,M10.5.0/3\n")
        for kept in clocks[3:6]:
            self.assertEqual((kept["valid"], kept["sync"]), ("1", "1"))
            self.assertLessEqual(abs(int(kept["wall"]) - 1792314126), 2)
            self.assertEqual(kept["zone"], "CET-1CEST,M3.5.0,M10.5.0/3")
        for lost in clocks[6:9]:
            self.assertEqual((lost["valid"], lost["wall"], lost["local"]), ("0", "-1", "0"))
        self.assertEqual(len(clocks), 9)
        self.assertIn("kept across reset reason 1", output)

    def test_time_with_rtc(self):
        """M3 on the 1.85B model: the PCF85063 starts with its oscillator-stop
        flag, keeps time through power loss, and its faults invalidate time."""
        script = """\
clock
rt1 GET
rt1 SET 1792314126
clock
reset power
clock
rtc-fault stop
wait-clock invalid 7000
rt1 GET
rt1 SET 1792314126
rtc-fault none
rt1 GET
rt1 SET 1792314126
rtc-fault bus
wait-clock invalid 7000
rt1 GET
rtc-fault none
wait-clock valid 7000
clock
time-drift chip 100000
time-advance 10000
reset sw
clock
"""
        output, clocks, replies, _ = self.run_time_script("1.85b", script)
        self.assertEqual(clocks[0]["valid"], "0")
        self.assertTrue(replies[0].startswith("!RT1 0000d0e5 RTC_INVALID state=3 os=1 rtc=-1 wall=-1"), replies[0])
        self.assertRegex(replies[1], r"OK state=0 os=0 rtc=179231412[67] wall=179231412[67] err=0")
        self.assertEqual(clocks[1]["valid"], "1")
        # Power loss: the battery-backed RTC restores time.
        self.assertEqual((clocks[2]["valid"], clocks[2]["sync"]), ("1", "1"))
        self.assertIn("RTC_INVALID state=3 os=1", replies[2])
        # Writing a stopped oscillator passes readback but fails the progress check.
        self.assertIn("RTC_VERIFY", replies[3])
        # After a failed SET only another SET makes time valid again.
        self.assertRegex(replies[4], r"OK state=0 os=0 rtc=\d+ wall=-1 ")
        self.assertRegex(replies[5], r"OK state=0 os=0 rtc=\d+ wall=\d+ ")
        self.assertIn("RTC_READ state=2", replies[6])
        self.assertEqual(clocks[3]["valid"], "1")
        # The chip's 10% error over 10 s enters the time synced after the reset.
        self.assertEqual(int(clocks[4]["wall"]) - int(clocks[3]["wall"]), 11)

    def test_wall_clock_jumps_leave_power_timeouts_alone(self):
        """Screen timeout and sleep run on monotonic time; a wall-clock jump of a
        day either way changes neither, a monotonic advance does. In light
        sleep the RTC timer's rate error enters the wall clock."""
        script = """\
rt1 SET 1792314126
wall-jump 86400
wait 500
power
clock
wall-jump -172800
wait 500
power
clock
time-advance 31000
wait-power screen_off 2000
power
tap 180 295
wait-power active 2000
time-drift rtc 100000
clock
key boot
wait 300
tap 180 75
wait-power light_sleep 3000
time-advance 10000
key boot
wait-power active 3000
clock
"""
        output, clocks, _, _ = self.run_time_script("1.85b", script)
        self.assertIn("power state=active lit=1 sleeps=0 screen_offs=0", output)
        self.assertNotIn("power state=active lit=1 sleeps=0 screen_offs=1", output)
        self.assertIn("power state=screen_off lit=0 sleeps=0 screen_offs=1", output)
        self.assertEqual(int(clocks[0]["days"]) - int(clocks[1]["days"]), 2)
        self.assertEqual({clock["valid"] for clock in clocks}, {"1"})
        self.assertEqual({clock["sync"] for clock in clocks}, {"1"})
        before, after = clocks[2], clocks[3]
        elapsed_us = int(after["mono"]) - int(before["mono"])
        wall_ms = (int(after["days"]) - int(before["days"])) * 86400000 + int(after["ms"]) - int(before["ms"])
        self.assertAlmostEqual(wall_ms - elapsed_us / 1000, 1000, delta=150)

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

    # ---- Native linux-x64.rep1 applications (docs/native-apps.md, M5a) ----------

    def test_native_apps_through_the_launcher(self):
        """The real launcher starts the home Watch (ABI 3, UI bridge), closes it
        and runs Hello (ABI 1) from the list; both unload completely."""
        script = ("wait-app RUNNING 5000\nwait 500\nscreenshot watch.png\napp\n" + HOME_TO_LIST +
                  "wait-app UNLOADED 5000\nwait-nav LIST 5000\napp\ntap 180 135\nwait-nav RESULT 5000\n"
                  "app\nresources\nquit\n")
        result, directory = self.run_desktop("1.85b", script, native=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("runtime error", result.stderr)
        self.assertIn("native applications enabled", result.stderr)
        self.assertIn('app state=RUNNING instance=1 abi=3', result.stdout)
        self.assertIn('app state=UNLOADED instance=1 abi=3 completed=1 loader=0 result=0', result.stdout)
        self.assertIn('outstanding=0 busy=0 error=""', result.stdout)
        self.assertIn("Hello from a native Roundie ELF application!", result.stderr)
        self.assertIn('app state=UNLOADED instance=2 abi=1 completed=2 loader=0 result=42', result.stdout)
        self.assertIn("resources image_mappings=0", result.stdout)
        self.assertEqual(png_size(directory / "watch.png"), (360, 360))

    def test_native_cycles_release_everything(self):
        """Repeated load/start/stop/unload: no image mapping, descriptor or
        runtime resource outlives an instance (ASan leak check at exit)."""
        cycles = [("hello", "tap 180 135\nwait-nav RESULT 5000\ntap 180 295\nwait-nav LIST 5000\n", 100),
                  ("stopwatch", "tap 180 135\nwait-app RUNNING 5000\nwait-nav ACTIVE 5000\nwait 100\n" +
                   CLOSE_APP + "wait-app UNLOADED 5000\nwait-nav LIST 5000\n", 36)]
        for stem, cycle, count in cycles:
            with self.subTest(app=stem):
                script = "wait-nav LIST 5000\nresources\n" + cycle * count + "app\nresources\nquit\n"
                result, _ = self.run_desktop("1.85b", script, packages=[self.native[stem]], native=True,
                                             timeout=300)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertNotIn("runtime error", result.stderr)
                self.assertNotIn("still mapped", result.stderr)
                resources = re.findall(r"resources image_mappings=(\d+) descriptors=(\d+)", result.stdout)
                self.assertEqual(len(resources), 2)
                self.assertEqual(resources[0], resources[1])
                self.assertEqual(resources[1][0], "0")
                self.assertIn(f"instance={count} ", result.stdout)
                self.assertIn(f"completed={count} ", result.stdout)
                self.assertIn('outstanding=0 busy=0 error=""', result.stdout)

    def test_hung_application_does_not_block_the_gui(self):
        """A callback that never returns keeps the executor; the GUI keeps
        running, the exit request is admitted, and the limitation is reported."""
        script = ("wait-nav LIST 5000\ntap 180 135\nwait-app RUNNING 5000\nwait-nav ACTIVE 5000\nwait 300\n" +
                  CLOSE_APP + "wait 6000\nstatus\napp\nscreenshot hung.png\ntap 180 180\nwait 200\nstatus\n"
                  "quit\n")
        result, directory = self.run_desktop("1.85b", script, packages=[self.native["hang"]], native=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("has not returned for", result.stderr)
        self.assertIn("cannot be stopped safely", result.stderr)
        self.assertEqual(result.stdout.count("launcher state=EXITING painted=EXITING"), 2)
        self.assertIn("app state=RUNNING instance=1 abi=2", result.stdout)
        self.assertEqual(png_size(directory / "hung.png"), (360, 360))

    def test_native_apps_are_opt_in(self):
        """Without --native-apps every launch is refused and nothing is loaded."""
        script = "wait-nav LIST 5000\ntap 180 135\nwait-nav RESULT 5000\napp\nresources\nquit\n"
        result, _ = self.run_desktop("1.85b", script, packages=[self.native["hello"]])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("native applications are disabled", result.stderr)
        self.assertIn("app state=UNLOADED instance=0", result.stdout)
        self.assertNotIn("Hello from", result.stderr)

    def test_incompatible_and_interposing_packages_are_rejected(self):
        # An ESP32-S3 package or a truncated one fails installation.
        truncated = self.work / "truncated.rpk"
        truncated.write_bytes(self.native["hello"].read_bytes()[:-100])
        rejected = [(truncated, None)]
        if (TOOLCHAIN / "xtensa-esp32s3-elf-gcc").exists():
            env = dict(os.environ, PATH=str(TOOLCHAIN) + os.pathsep + os.environ["PATH"])
            target = self.work / "xtensa-hello"
            subprocess.run(["cmake", "-S", str(ROOT / "examples/hello_elf"), "-B", str(target)],
                           check=True, capture_output=True, env=env)
            subprocess.run(["cmake", "--build", str(target)], check=True, capture_output=True, env=env)
            esp = self.work / "hello-esp.rpk"
            subprocess.run(["python3", str(ROOT / "tools/roundie_package.py"), "build",
                            str(target / "hello.app.elf"), "--output", str(esp), "--id", "org.roundie.hello",
                            "--name", "Hello", "--version", "1.0.0"], check=True, capture_output=True)
            rejected.append((esp, "package-incompatible"))
        for package, error in rejected:
            with self.subTest(package=package.name):
                result, directory = self.run_desktop("1.85b", "wait-nav LIST 5000\nquit\n", packages=[package],
                                                     native=True)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(f"install {package}", result.stderr)
                if error is not None:
                    self.assertIn(error, result.stderr)
                self.assertFalse(any((directory / "data/storage/packages").glob("*/active")))
        # A valid ELF exporting a C library name: ld.so binds the call to the
        # process's puts, the loader's binding check refuses it before the entry.
        source = self.work / "interpose.c"
        source.write_text(
            '#include "rep/rep_app_v3.h"\nREP3_APP_METADATA;\n'
            "static int32_t pass(const rep_context_v2_t *c) { (void)c; return 0; }\n"
            "static int32_t event(const rep_context_v2_t *c, const rep_event_v2_t *e) { (void)c; (void)e; return 0; }\n"
            "REP2_EXPORT int puts(const char *text) { (void)text; return 7; }\n"
            "REP2_EXPORT const rep_descriptor_v3_t *rep_app_entry_v3(const rep_api_v3_t *api)\n{\n"
            "    static const rep_descriptor_v3_t d = {REP_ABI_V3, sizeof(d), pass, pass, event, pass, pass, pass,"
            " pass, REP3_CAP_UI};\n"
            '    return rep_abi_v3(REP_ABI_V3) == api && puts("x") == 7 ? &d : 0;\n}\n')
        binary = self.work / "interpose.app.so"
        package = self.work / "interpose.rpk"
        subprocess.run(["python3", str(ROOT / "tools/roundie_native_app.py"), str(source), "--entry",
                        "rep_app_entry_v3", "-o", str(binary)], check=True, capture_output=True)
        subprocess.run(["python3", str(ROOT / "tools/roundie_package.py"), "build", str(binary), "--output",
                        str(package), "--id", "org.roundie.interpose", "--name", "Interpose", "--version", "1.0.0",
                        "--target", "linux-x64.rep1"], check=True, capture_output=True)
        script = "wait-nav LIST 5000\ntap 180 135\nwait-nav RESULT 5000\napp\nresources\nquit\n"
        result, _ = self.run_desktop("1.85b", script, packages=[package], native=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("binding at", result.stderr)
        self.assertIn('callbacks=0 outstanding=0 busy=0 error="loader-relocation"', result.stdout)
        self.assertIn("resources image_mappings=0", result.stdout)


if __name__ == "__main__":
    unittest.main()
