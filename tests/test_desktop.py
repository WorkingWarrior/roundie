"""Linux desktop target (docs/roundie-core.md, M1): production launcher, UI bridge
and package manager under ASan/UBSan, driven by scripts in headless mode."""
from pathlib import Path
import array
import hashlib
import os
import re
import shutil
import statistics
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
from xtensa_toolchain import TOOLCHAIN
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
wait-nav HOME 5000
screenshot home.png
{home_to_list}wait-nav LIST 5000
screenshot list.png
mark
tap {refresh}
wait-refreshed 5000
tap {first}
wait-nav RESULT 5000
screenshot result.png
tap {back}
wait-nav LIST 5000
key boot
swipe {swipe}
"""

# linux-x64.rep1 packages: (name, example source, entry, id, title, build arguments).
FAULT_SOURCE = "fault_elf/fault.c"
# The firmware's fault build flags (examples/fault_elf/CMakeLists.txt).
FAULT_CC = ("--cc", "cc -Wno-infinite-recursion -fno-optimize-sibling-calls")
NATIVE_APPS = (
    ("hello", "hello_elf/hello.c", "rep_app_entry", "org.roundie.hello", "Hello", ()),
    ("watch", "watch_elf/watch.c", "rep_app_entry_v3", "org.roundie.watch", "Zegarek", ()),
    ("stopwatch", "stopwatch_elf/stopwatch.c", "rep_app_entry_v3", "org.roundie.stopwatch", "Stoper", ()),
    ("sound", "sound_elf/sound.c", "rep_app_entry_v3", "org.roundie.sound", "Dźwięk", ()),
    ("event", "event_elf/event.c", "rep_app_entry_v2", "org.roundie.events", "Events", ()),
    ("ui", "ui_elf/ui.c", "rep_app_entry_v3", "org.roundie.ui", "UI", ()),
    ("round", "round_elf/round.c", "rep_app_entry_v3", "org.roundie.round", "Round", ()),
    ("hang", FAULT_SOURCE, "rep_app_entry_v2", "org.roundie.hang", "Hang", ("-DFAULT_KIND=1",)),
    # Faults the desktop attributes (docs/native-apps.md, N7): load, store, call, stack.
    ("fault-load", FAULT_SOURCE, "rep_app_entry_v2", "org.roundie.fault.load", "Load", ("-DFAULT_KIND=2",)),
    ("fault-store", FAULT_SOURCE, "rep_app_entry_v2", "org.roundie.fault.store", "Store", ("-DFAULT_KIND=3",)),
    ("fault-fetch", FAULT_SOURCE, "rep_app_entry_v2", "org.roundie.fault.fetch", "Fetch", ("-DFAULT_KIND=5",)),
    ("fault-stack", FAULT_SOURCE, "rep_app_entry_v2", "org.roundie.fault.stack", "Stack",
     ("-DFAULT_KIND=7", *FAULT_CC)),
    # Developer builds (N8): undefined behaviour traps; DWARF for GDB.
    ("fault-undefined", FAULT_SOURCE, "rep_app_entry_v2", "org.roundie.fault.undefined", "Undefined",
     ("-DFAULT_KIND=8", "--sanitize-undefined", "--debug")),
    ("event-debug", "event_elf/event.c", "rep_app_entry_v2", "org.roundie.events", "Events", ("--debug",)),
)

# On the 1.85B panel: a swipe up from Home's bottom strip opens the list, and
# a swipe right from the left-edge strip closes an application or returns
# from the list to Home (components/roundie_launcher/render.c).
HOME_TO_LIST = "down 180 320\nwait 50\nmove 180 250\nwait 50\nmove 180 150\nwait 50\nup\n"
CLOSE_APP = "down 10 180\nwait 50\nmove 60 182\nwait 50\nmove 150 185\nwait 50\nup\n"
# M10: Home is the watch face. Boot and the end of an application show it;
# the list is one swipe up away (docs/watch-face-platform.md).
TO_LIST = "wait-nav HOME 5000\n" + HOME_TO_LIST + "wait-nav LIST 5000\n"
# The Watch application, second on the list with Hello (1.85B).
OPEN_WATCH = TO_LIST + "tap 180 218\n"

# Canvas points of the controls, in panel pixels of each profile.
BOARDS = {
    "1.85b": {"size": 360, "back": "180 295", "refresh": "180 295", "first": "180 135",
              "swipe": "180 300 180 120 300", "home_to_list": HOME_TO_LIST},
    "1.75c": {"size": 466, "back": "233 382", "refresh": "233 382", "first": "233 175",
              "swipe": "233 388 233 155 300",
              "home_to_list": "down 233 414\nwait 50\nmove 233 324\nwait 50\nmove 233 194\nwait 50\nup\n"},
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


    def test_audio_configuration_matches_rep_profile(self):
        config = ROOT / "sdkconfig.rep"
        if not config.exists():
            self.skipTest("no sdkconfig.rep; build the rep firmware first")
        self.assertIn("CONFIG_ROUNDIE_AUDIO=y", config.read_text().splitlines())
        self.assertEqual(defines(ROOT / "platform/linux/include/sdkconfig.h")["CONFIG_ROUNDIE_AUDIO"], "1")


class DesktopRunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        missing = missing_tools()
        if missing:
            raise unittest.SkipTest("missing " + ", ".join(missing))
        if not LVGL.exists():
            raise unittest.SkipTest("managed_components/lvgl__lvgl missing; build the firmware first")
        # ROUNDIE_KEEP_TEST_WORK keeps builds, data directories and logs for
        # diagnosis (tools/ci/run_tests.py sets it in CI).
        cls.temporary = tempfile.TemporaryDirectory(prefix="roundie-desktop-test-",
                                                    delete="ROUNDIE_KEEP_TEST_WORK" not in os.environ)
        cls.work = Path(cls.temporary.name)
        build = cls.work / "build"
        # ROUNDIE_DESKTOP_SANITIZE=thread runs the same tests under ThreadSanitizer.
        sanitize = os.environ.get("ROUNDIE_DESKTOP_SANITIZE", "address,undefined")
        for command in (["meson", "setup", str(build), str(ROOT), "-Db_sanitize=" + sanitize,
                         "-Dbuildtype=debug", "-Ddesktop=enabled"], ["ninja", "-C", str(build)]):
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode != 0:
                raise RuntimeError(f"{command[0]} failed ({result.returncode}):\n"
                                   + result.stdout[-6000:] + result.stderr[-6000:])
        cls.binary = build / "roundie-desktop"
        # The desktop's package target is linux-x64.rep1: the same example
        # sources, built for x86-64 (docs/native-apps.md, N1-N3).
        cls.native = {}
        cls.binaries = {}
        for stem, source, entry, app_id, title, arguments in NATIVE_APPS:
            binary = cls.work / f"{stem}.app.so"
            subprocess.run(["python3", str(ROOT / "tools/roundie_native_app.py"), str(ROOT / "examples" / source),
                            "--entry", entry, "-o", str(binary), *arguments],
                           check=True, capture_output=True)
            cls.binaries[stem] = binary
            package = cls.work / f"{stem}.rpk"
            subprocess.run(["python3", str(ROOT / "tools/roundie_package.py"), "build", str(binary),
                            "--output", str(package), "--id", app_id, "--name", title, "--version", "1.0.0",
                            "--target", "linux-x64.rep1"], check=True, capture_output=True)
            cls.native[stem] = package
        cls.packages = [cls.native["hello"], cls.native["watch"]]

    @classmethod
    def tearDownClass(cls):
        if "ROUNDIE_KEEP_TEST_WORK" not in os.environ:
            cls.temporary.cleanup()

    def run_desktop(self, board, script, *, install=True, packages=None, native=False, timeout=60,
                    environment=None, virtual=False, directory=None, arguments=()):
        """directory: reuse an earlier run's data directory (a process restart);
        arguments: further command-line arguments, such as --install-face."""
        if directory is None:
            directory = Path(tempfile.mkdtemp(dir=self.work))
        (directory / "script.txt").write_text(script)
        command = [str(self.binary), "--headless", "--board", board, "--data", str(directory / "data"),
                   "--script", "script.txt"]
        if native:
            command.append("--native-apps")
        if virtual:
            command.append("--virtual-time")
        command += list(arguments)
        if install:
            for package in self.packages if packages is None else packages:
                command += ["--install", str(package)]
        # No sound from the tests on the session's output unless a test captures it.
        env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                   ASAN_OPTIONS="detect_leaks=1", TSAN_OPTIONS="halt_on_error=1", SDL_AUDIO_DRIVER="dummy")
        env.update(environment or {})
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
                for name in ("home.png", "result.png", "list.png"):
                    self.assertEqual(png_size(directory / name), (size, size))
                self.assertNotEqual((directory / "result.png").read_bytes(),
                                    (directory / "list.png").read_bytes())
                self.assertNotEqual((directory / "home.png").read_bytes(),
                                    (directory / "list.png").read_bytes())
                # The home package went through the real installer.
                self.assertTrue((directory / "data/storage/packages/org.roundie.watch/active").exists())

    def test_touch_off_the_button_does_nothing(self):
        script = TO_LIST + "mark\ntap 180 60\nwait-refreshed 1000\n"
        result, _ = self.run_desktop("1.85b", script, install=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn("timed out", result.stderr)

    def test_slide_off_a_button_still_clicks(self):
        """Production behaviour, not a desktop artefact: LVGL keeps a pressed
        button pressed when the finger slides off (LV_OBJ_FLAG_PRESS_LOCK), so
        the release still clicks it, on the board's touch panel as here."""
        script = ("wait-nav HOME 5000\n" + BOARDS["1.75c"]["home_to_list"] +
                  "wait-nav LIST 5000\ntap 233 175\nwait-nav RESULT 5000\nswipe 233 382 465 400 400\nwait-nav LIST 3000\n")
        result, _ = self.run_desktop("1.75c", script)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_power_menu(self):
        """M2: the firmware's power GUI (main/power_gui.c) on the emulated board.
        Menu buttons are canvas points of main/power_gui.c on the 1.85B panel."""
        script = """\
wait-nav HOME 5000
screenshot before.png
key boot
wait 300
screenshot menu.png
tap 180 198
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
expect-nav HOME
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
expect-nav HOME
"""
        result, directory = self.run_desktop("1.85b", script)
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertNotIn("runtime error", result.stderr)
        before = (directory / "before.png").read_bytes()
        self.assertNotEqual((directory / "menu.png").read_bytes(), before)
        # USB storage mode is refused on the desktop and Anuluj closes the menu.
        self.assertIn('power state=active lit=1 sleeps=0 screen_offs=0 message="Niedostępne na komputerze"',
                      output)
        self.assertEqual((directory / "closed.png").read_bytes(), before)
        # The waking tap lands on the Home strip but only wakes the screen (expect-nav HOME).
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
                   ASAN_OPTIONS="detect_leaks=0",  # the graphics stack leaks on unload
                   SDL_AUDIO_DRIVER="dummy")
        subprocess.run(["gamescope", "--backend", "headless", "-W", "932", "-H", "932", "--",
                        str(directory / "drive.sh")], capture_output=True, timeout=120, env=env)
        log = (directory / "app.log").read_text()
        self.assertIn("exit 0", log, log)
        self.assertNotIn("runtime error", log)
        self.assertIn("boot pressed", log)
        self.assertGreaterEqual(log.count("Creating swapchain"), 2, "resize did not recreate the swapchain")
        self.assertIn("vulkan: swapchain 700x500, panel shown 466x466 at 117,17", log)
        return log, directory

    # Home's bottom strip dragged up, in window pixels (the 1.75C panel at 117,17).
    GAMESCOPE_HOME_TO_LIST = "press 350 431; drag 350 431 350 211 10; release\n"

    def test_window_in_gamescope(self):
        """Vulkan presentation, mouse, keys and resizing through a real
        compositor. The application checks every transition; the driver drags
        and clicks."""
        log, directory = self.run_in_gamescope(
            "wait-nav HOME 15000\nwait-nav LIST 15000\nwait-nav RESULT 15000\nwait 500\nstatus\nquit\n",
            "gamescopectl screenshot $PWD/resized.png\nsleep 1\n" + self.GAMESCOPE_HOME_TO_LIST +
            "sleep 2\nclick 350 153\n")
        self.assertIn("launcher state=RESULT", log)
        self.assertIn("request=1", log)  # the launch came from the mouse
        self.assertEqual(png_size(directory / "resized.png"), (700, 500))  # the resized window

    def test_drag_gestures_in_gamescope(self):
        """A drag up from Home's bottom strip opens the list; a drag on the list
        scrolls it without launching; a drag from the left-edge strip goes back
        to Home (the launcher's own edge handler)."""
        log, _ = self.run_in_gamescope(
            "wait-nav HOME 15000\nwait-nav LIST 15000\nwait 6000\nexpect-nav LIST\n"
            "wait-nav HOME 15000\nwait 300\nstatus\nquit\n",
            self.GAMESCOPE_HOME_TO_LIST + "sleep 1\n"
            "press 350 192; drag 350 192 350 40 10; release\nsleep 4.5\n"
            "press 127 250; drag 127 250 347 255 12; release\n")
        self.assertIn("launcher state=HOME", log)
        self.assertIn("request=0", log)

    def test_pointer_outside_the_panel_in_gamescope(self):
        """A release outside the window still arrives (no stuck press), and a
        press in the letterbox around the panel is not a touch."""
        log, _ = self.run_in_gamescope(
            "wait-nav HOME 15000\nwait-nav LIST 15000\nmark\nwait-refreshed 15000\nwait 4000\nexpect-nav LIST\n"
            "wait-nav RESULT 15000\nwait 300\nstatus\nquit\n",
            self.GAMESCOPE_HOME_TO_LIST + "sleep 1\n"
            # Refresh, dragged off the right side of the window: still a click
            # (see test_slide_off_a_button_still_clicks).
            "press 350 399; drag 350 399 900 420 12; release\nsleep 1\n"
            # From the letterbox onto the first entry: ignored.
            "press 40 250; drag 40 250 350 192 8; release\nsleep 4.5\n"
            "click 350 192\n")
        self.assertIn("launcher state=RESULT", log)
        self.assertIn("request=1", log)

    def test_window_in_nested_kwin_at_scale_2(self):
        """HiDPI: a virtual KWin output at scale 2 in its own D-Bus session and
        configuration directories (never the user's session). The window gets
        device pixels; pointer events come from KWin's fake-input protocol."""
        tools = [tool for tool in ("kwin_wayland", "dbus-run-session", "wayland-scanner", "cc")
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
        events = []
        def point(x, y):
            events.append(f"move {100 + x} {50 + y}")
        # The panel shows at zoom 2 in 932x932 device pixels: one logical point per panel pixel.
        point(233, 414)
        events += ["sleep 200", "down", "sleep 100"]
        for step in range(1, 13):
            point(233, 414 - step * 18)
            events.append("sleep 40")
        events += ["sleep 150", "up", "sleep 1500"]  # swipe up from Home's bottom strip -> list
        point(10, 233)
        events += ["sleep 200", "down", "sleep 100"]
        for step in range(1, 13):
            point(10 + step * 18, 233 + step // 4)
            events.append("sleep 40")
        events += ["sleep 150", "up", "sleep 500"]  # left-edge swipe -> Home
        (directory / "input.txt").write_text("\n".join(events) + "\n")
        (directory / "script.txt").write_text(
            "wait-nav HOME 15000\nwait-nav LIST 20000\nwait-nav HOME 20000\nwait 300\nstatus\nquit\n")
        installs = " ".join(f"--install {package}" for package in self.packages)
        (directory / "inner.sh").write_text(f"""#!/bin/bash
cd {directory}
SDL_VIDEO_DRIVER=wayland {self.binary} --board 1.75c --zoom 1 --data {directory}/appdata {installs} --script script.txt > app.log 2>&1 &
app=$!
sleep 3.5
{injector} < input.txt > inject.log 2>&1
wait $app
echo "exit $?" >> app.log
""")
        (directory / "inner.sh").chmod(0o755)
        env = {name: value for name, value in os.environ.items()
               if name not in ("WAYLAND_DISPLAY", "DISPLAY", "XDG_SESSION_ID", "DBUS_SESSION_BUS_ADDRESS")}
        env.update(XDG_CONFIG_HOME=str(directory / "config"), XDG_CACHE_HOME=str(directory / "cache"),
                   XDG_DATA_HOME=str(directory / "data"), ASAN_OPTIONS="detect_leaks=0",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", SDL_AUDIO_DRIVER="dummy")
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
        # A forced window rule maps the window borderless with its 466x466
        # (logical) client area at (100,50). A KWin script that moved the window
        # after mapping did not hold on KWin 6.8.2: the move was undone when the
        # client next committed (measured), and setting noBorder took effect
        # only after the frame geometry, leaving the client area 36 px lower.
        (directory / "config/kwinrulesrc").write_text(
            "[General]\ncount=1\nrules=roundie\n\n[roundie]\nDescription=Roundie desktop test window\n"
            "title=Roundie\ntitlematch=2\nnoborder=true\nnoborderrule=2\nposition=100,50\npositionrule=2\n")
        kwin(str(directory / "inner.sh"))
        log = (directory / "app.log").read_text()
        self.assertIn("exit 0", log, log + (directory / "inject.log").read_text())
        self.assertNotIn("runtime error", log)
        self.assertIn("466x466, 932x932 pixels, density 2.00", log)
        self.assertIn("vulkan: swapchain 932x932, panel shown 932x932 at 0,0", log)
        self.assertIn("launcher state=HOME", log)
        self.assertIn("request=0", log)  # both gestures navigated; nothing launched

    def test_watch_faces_on_home(self):
        """M10 on the emulated 1.85B, virtual time: two faces installed apart
        from activation, switching, 100 switches, Home left for the list and an
        application and back, a light sleep, persistence across a restart, and a
        corrupt preferred face falling back to the system face. VHL model, not
        hardware evidence (docs/watch-face-platform.md)."""
        faces = {}
        for name in ("classic_analog", "minimal_digital"):
            output = Path(tempfile.mkdtemp(dir=self.work)) / (name + ".rwf")
            subprocess.run(["python3", str(ROOT / "tools/roundie_face.py"), "build",
                            str(ROOT / "faces" / name / "face.xml"), "--output", str(output)],
                           check=True, capture_output=True)
            faces[name] = output
        analog, minimal = "org.roundie.face.analog", "org.roundie.face.minimal"
        switches = "".join(f"face-select {face}\nwait-face {face} 3000\n" for face in (minimal, analog) * 50)
        script = (
            "wait-nav HOME 5000\nwait-face system 5000\nface-status\n"
            f"face-select {analog}\nwait-face {analog} 3000\nscreenshot analog.png\n"
            f"face-select {minimal}\nwait-face {minimal} 3000\nscreenshot minimal.png\n"
            + switches + "face-status\n"
            # Home to the list (the face parks), an application, back to Home.
            + HOME_TO_LIST + "wait-nav LIST 5000\nface-status\n"
            "tap 180 135\nwait-nav RESULT 5000\ntap 180 295\nwait-nav LIST 5000\n"
            + CLOSE_APP + f"wait-nav HOME 5000\nwait-face {analog} 3000\n"
            # Light sleep: parked while asleep, resumed after the wake.
            "power-sleep 30\nwait-power light_sleep 3000\nface-status\nwait 500\nkey boot\n"
            f"wait-power active 3000\nwait-face {analog} 3000\nface-status\nquit\n")
        installs = ["--install-face", str(faces["classic_analog"]), "--install-face", str(faces["minimal_digital"])]
        first, directory = self.run_desktop("1.85b", script, virtual=True, timeout=180,
                                            arguments=installs)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        statuses = [dict(re.findall(r'(\w+)=("[^"]*"|\S+)', line)) for line in first.stdout.splitlines()
                    if line.startswith("face active=")]
        self.assertEqual(len(statuses), 5, first.stdout)
        self.assertEqual((statuses[0]["active"], statuses[0]["count"]), ("system", "2"))
        self.assertEqual(statuses[1]["active"], analog)
        self.assertEqual(int(statuses[1]["switches"]), 102)
        self.assertEqual((statuses[1]["failures"], statuses[1]["refused"]), ("0", "0"))
        self.assertEqual(statuses[2]["parked"], "1")  # the list is shown
        self.assertEqual(statuses[3]["parked"], "1")  # asleep
        self.assertEqual(statuses[4]["parked"], "0")
        self.assertGreater(int(statuses[4]["resumes"]), int(statuses[3]["resumes"]))
        self.assertNotEqual((directory / "analog.png").read_bytes(), (directory / "minimal.png").read_bytes())
        self.assertNotIn("runtime error", first.stderr)
        # Restart: the preference and the installed faces persist.
        second, _ = self.run_desktop("1.85b", f"wait-nav HOME 5000\nwait-face {analog} 5000\nface-status\nquit\n",
                                     install=False, virtual=True, directory=directory)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertIn(f"preferred={analog}", second.stdout)
        self.assertIn("count=2", second.stdout)
        # A corrupt preferred face is never shown: the system face, with the error.
        package = directory / "data/storage/faces" / (analog + ".rwf")
        data = bytearray(package.read_bytes())
        data[300] ^= 0x01
        package.write_bytes(bytes(data))
        third, _ = self.run_desktop("1.85b", "wait-nav HOME 5000\nwait-face system 5000\nwait 500\nface-status\n"
                                    "quit\n", install=False, virtual=True, directory=directory)
        self.assertEqual(third.returncode, 0, third.stdout + third.stderr)
        self.assertIn('error="package-digest"', third.stdout)
        self.assertIn(f"preferred={analog}", third.stdout)

    def test_empty_registry_lists_no_packages(self):
        script = "wait-nav HOME 5000\n" + BOARDS["1.75c"]["home_to_list"] + "wait-nav LIST 5000\nscreenshot empty.png\n"
        result, directory = self.run_desktop("1.75c", script, install=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(png_size(directory / "empty.png"), (466, 466))

    # ---- Native linux-x64.rep1 applications (docs/native-apps.md, M5a) ----------

    def test_native_apps_through_the_launcher(self):
        """The real launcher starts Watch (ABI 3, UI bridge) from the list,
        closes it back to the Home face and runs Hello (ABI 1) from the list;
        both unload completely."""
        script = (OPEN_WATCH + "wait-app RUNNING 5000\nwait 500\nscreenshot watch.png\napp\n" + CLOSE_APP +
                  "wait-app UNLOADED 5000\n" + TO_LIST + "app\ntap 180 135\nwait-nav RESULT 5000\n"
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

    def test_sound_plays_through_sdl_audio(self):
        """Sound (ABI 3) streams its 16 kHz mono tune through the desktop's SDL3
        backend of the REP Audio contract; SDL's disk driver records the output."""
        script = (TO_LIST + "audio\ntap 180 135\nwait-app RUNNING 5000\nwait 300\ntap 180 224\n"
                  "wait 1000\naudio\nwait 9000\naudio\n" + CLOSE_APP +
                  "wait-app UNLOADED 5000\napp\naudio\nresources\nquit\n")
        result, directory = self.run_desktop("1.85b", script, packages=[self.native["sound"]], native=True,
                                             environment={"SDL_AUDIO_DRIVER": "disk"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("runtime error", result.stderr)
        self.assertIn("Speaker ready on SDL driver disk", result.stderr)
        audio = re.findall(r"audio initialized=1 playing=(\d) plays=(\d+) errors=0 volume=(\d+) stream=(\d)",
                           result.stdout)
        self.assertEqual(audio, [("0", "0", "0", "0"), ("1", "0", "35", "1"), ("0", "1", "35", "0"),
                                 ("0", "1", "35", "0")])
        self.assertIn('app state=UNLOADED instance=1 abi=3 completed=1 loader=0 result=0', result.stdout)
        self.assertIn('outstanding=0 busy=0 error=""', result.stdout)
        rate = int(re.search(r"format=S16LE channels=2 freq=(\d+)", result.stderr).group(1))
        samples = array.array("h", (directory / "data/sdlaudio.raw").read_bytes())
        left = samples[0::2]
        self.assertEqual(left, samples[1::2])  # mono source on both channels
        audible = [index for index, value in enumerate(left) if abs(value) > 200]
        self.assertGreater(len(audible), 0)
        duration = (audible[-1] - audible[0]) / rate
        self.assertGreater(duration, 5.0)
        self.assertLess(duration, 8.0)
        # The first note, from the spacing of zero crossings over 0.2 s: G5
        # (784 Hz). This run is in real time, so a loaded host may let the ring
        # run dry and silence be fed for a few ms (docs/virtual-hardware.md); a
        # gap in the crossings is not part of the note's period.
        window = left[audible[0]:audible[0] + rate // 5]
        crossings = [index for index in range(1, len(window)) if (window[index - 1] < 0) != (window[index] < 0)]
        spacings = [b - a for a, b in zip(crossings, crossings[1:])]
        median = statistics.median(spacings)
        periods = [spacing for spacing in spacings if spacing < 2 * median]
        self.assertGreater(len(periods), len(spacings) * 0.9)
        self.assertAlmostEqual(rate / (2 * statistics.mean(periods)), 784, delta=20)

    def test_sound_closed_while_playing_releases_the_stream(self):
        """Closing Sound mid-tune: the manager releases the instance's stream
        before unload and the next instance can open one."""
        cycle = ("tap 180 135\nwait-app RUNNING 5000\nwait 300\ntap 180 224\nwait 700\naudio\n" + CLOSE_APP +
                 "wait-app UNLOADED 5000\n" + TO_LIST + "audio\n")
        script = TO_LIST + cycle * 3 + "app\nresources\nquit\n"
        result, _ = self.run_desktop("1.85b", script, packages=[self.native["sound"]], native=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("runtime error", result.stderr)
        audio = re.findall(r"audio initialized=1 playing=(\d) plays=(\d+) errors=0 volume=\d+ stream=(\d) .* busy=(\d)",
                           result.stdout)
        self.assertEqual(audio, [("1", "0", "1", "1"), ("0", "1", "0", "0"), ("1", "1", "1", "1"),
                                 ("0", "2", "0", "0"), ("1", "2", "1", "1"), ("0", "3", "0", "0")])
        self.assertIn('app state=UNLOADED instance=3 abi=3 completed=3 loader=0 result=0', result.stdout)
        self.assertIn('outstanding=0 busy=0 error=""', result.stdout)
        self.assertIn("resources image_mappings=0", result.stdout)

    def test_native_cycles_release_everything(self):
        """Repeated load/start/stop/unload: no image mapping, descriptor or
        runtime resource outlives an instance (ASan leak check at exit)."""
        # Home to list costs 7 script lines per stopwatch cycle; 30 cycles fit the 512-line limit.
        cycles = [("hello", "tap 180 135\nwait-nav RESULT 5000\ntap 180 295\nwait-nav LIST 5000\n", 100),
                  ("stopwatch", "tap 180 135\nwait-app RUNNING 5000\nwait-nav ACTIVE 5000\nwait 100\n"
                   "swipe 10 180 150 185 150\nwait-app UNLOADED 5000\n" + TO_LIST, 30)]
        for stem, cycle, count in cycles:
            with self.subTest(app=stem):
                script = TO_LIST + "resources\n" + cycle * count + "app\nresources\nquit\n"
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
        script = (TO_LIST + "tap 180 135\nwait-app RUNNING 5000\nwait-nav ACTIVE 5000\nwait 300\n" +
                  CLOSE_APP + "wait 6000\nstatus\napp\nscreenshot hung.png\ntap 180 180\nwait 200\nstatus\n"
                  "quit\n")
        result, directory = self.run_desktop("1.85b", script, packages=[self.native["hang"]], native=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("has not returned for", result.stderr)
        self.assertIn("cannot be stopped safely", result.stderr)
        self.assertEqual(result.stdout.count("launcher state=EXITING painted=EXITING"), 2)
        self.assertIn("app state=RUNNING instance=1 abi=2", result.stdout)
        self.assertEqual(png_size(directory / "hung.png"), (360, 360))

    # ---- Deterministic application tests in virtual time (M5b) -----------------
    # --virtual-time: the clock moves only while the script waits, in steps the
    # executor and the system worker finish first, so two runs of a script give
    # the same output and the same screenshots (docs/native-apps.md, N9).

    def run_deterministic(self, stem, script, screenshots, real_time_fields=(), **options):
        """Runs a script twice in virtual time; returns the first run. Fields
        set by a real-time device (the SDL audio clock) are not compared."""
        runs = [self.run_desktop("1.85b", script, packages=[self.native[stem]], native=True, virtual=True,
                                 **options) for _ in range(2)]
        for result, _ in runs:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("runtime error", result.stderr)
        (first, first_directory), (second, second_directory) = runs
        compared = []
        for result, _ in runs:
            text = result.stdout
            for field in real_time_fields:
                text = re.sub(rf"\b{field}=\d+", f"{field}=*", text)
            compared.append(text)
        self.assertEqual(compared[0], compared[1])
        for name in screenshots:
            self.assertEqual((first_directory / name).read_bytes(), (second_directory / name).read_bytes(), name)
        return first, first_directory

    def test_events_lifecycle_in_virtual_time(self):
        """Events (ABI 2): a 250 ms timer, a user event, suspend and resume
        (RE1 commands), and the instance's resources reclaimed at unload."""
        script = (TO_LIST + "tap 180 135\nwait-app RUNNING 5000\napp-stats\nwait 1000\napp-stats\n"
                  "app-event 7\nwait 10\napp-stats\napp-control suspend\nwait-app SUSPENDED 1000\nwait 1000\n"
                  "app-stats\napp-control resume\nwait-app RUNNING 1000\nwait 500\napp-stats\nscreenshot events.png\n"
                  + CLOSE_APP + "wait-app UNLOADED 5000\nwait 200\napp\napp-stats\nresources\nquit\n")
        result, _ = self.run_deterministic("event", script, ["events.png"])
        stats = re.findall(r"app-stats instance=1 state=(\w+) events=(\d+) timers=(\d+) users=(\d+) ui=0 last=(\d+) "
                           r"queue=0 dropped=0 coalesced=0 rejected=0 acquired=\d+ released=\d+ reclaimed=(\d+)",
                           result.stdout)
        self.assertEqual(stats, [("RUNNING", "0", "0", "0", "0", "0"), ("RUNNING", "4", "4", "0", "0", "0"),
                                 ("RUNNING", "5", "4", "1", "7", "0"), ("SUSPENDED", "5", "4", "1", "7", "0"),
                                 ("RUNNING", "7", "6", "1", "7", "0"), ("UNLOADED", "7", "6", "1", "7", "1")])
        self.assertIn("app-control suspend result=ESP_OK", result.stdout)
        self.assertIn('app state=UNLOADED instance=1 abi=2 completed=1 loader=0 result=0', result.stdout)
        self.assertIn('outstanding=0 busy=0 error=""', result.stdout)
        self.assertIn("resources image_mappings=0", result.stdout)

    def test_ui_bridge_input_in_virtual_time(self):
        """UI (ABI 3): two touches and one injected press reach the counter;
        a press of an unknown widget is refused; the UI is destroyed at stop."""
        script = (TO_LIST + "tap 180 135\nwait-app RUNNING 5000\nwait-nav ACTIVE 5000\nwait 3000\n"
                  "tap 180 236\nwait 100\ntap 180 236\nwait 100\nui-press 5\nwait 100\nui-press 99\nwait 100\n"
                  "app-stats\nscreenshot ui.png\n" + CLOSE_APP +
                  "wait-app UNLOADED 5000\napp\napp-stats\nresources\nquit\n")
        result, _ = self.run_deterministic("ui", script, ["ui.png"])
        self.assertEqual(re.findall(r"ui-press accepted=(\d)", result.stdout), ["1", "0"])
        self.assertEqual(result.stderr.count("ui counter: physical touch"), 2)
        self.assertEqual(result.stderr.count("ui counter: injected event"), 1)
        self.assertIn("ui stop: UI update/destroy admission close", result.stderr)
        self.assertIn("app-stats instance=1 state=RUNNING events=6 timers=3 users=0 ui=3", result.stdout)
        self.assertIn('app state=UNLOADED instance=1 abi=3 completed=1 loader=0 result=0', result.stdout)
        self.assertIn('outstanding=0 busy=0 error=""', result.stdout)
        self.assertIn("reclaimed=1", result.stdout)
        self.assertIn("resources image_mappings=0", result.stdout)

    def test_round_widgets_in_virtual_time(self):
        """Round (ABI 3): list choice, slider value, edge button and dialog
        through the RE1 value injection, one injection at a time."""
        script = (TO_LIST + "tap 180 135\nwait-app RUNNING 5000\nwait-nav ACTIVE 5000\nwait 500\n"
                  "screenshot r0.png\nui-value 4 2\nwait 100\nui-value 5 75\nwait 1200\nscreenshot r1.png\n"
                  "ui-press 6\nwait 300\nscreenshot r2.png\nui-value 7 1\nwait 1200\nscreenshot r3.png\n"
                  "app-stats\n" + CLOSE_APP + "wait-app UNLOADED 5000\napp\nresources\nquit\n")
        shots = ["r0.png", "r1.png", "r2.png", "r3.png"]
        result, directory = self.run_deterministic("round", script, shots)
        self.assertEqual(re.findall(r"ui-(?:value|press) accepted=(\d)", result.stdout), ["1"] * 4)
        images = {(directory / name).read_bytes() for name in shots}
        self.assertEqual(len(images), 4)
        self.assertIn("app-stats instance=1 state=RUNNING events=7 timers=3 users=0 ui=4", result.stdout)
        self.assertIn("round destroy", result.stderr)
        self.assertIn('outstanding=0 busy=0 error=""', result.stdout)
        self.assertIn("resources image_mappings=0", result.stdout)

    def test_stopwatch_timing_in_virtual_time(self):
        """Stopwatch: start, lap after 2.5 s, stop after 1.2 s more; the
        display and the lap are the same on every run."""
        script = (TO_LIST + "tap 180 135\nwait-app RUNNING 5000\nwait-nav ACTIVE 5000\nwait 200\n"
                  "tap 180 300\nwait 2500\ntap 180 96\nwait 1000\ntap 180 300\nwait 500\nscreenshot stopwatch.png\n"
                  "app-stats\n" + CLOSE_APP + "wait-app UNLOADED 5000\napp\nresources\nquit\n")
        result, _ = self.run_deterministic("stopwatch", script, ["stopwatch.png"])
        self.assertIn("app-stats instance=1 state=RUNNING events=40 timers=37 users=0 ui=3", result.stdout)
        self.assertIn('outstanding=0 busy=0 error=""', result.stdout)
        self.assertIn("resources image_mappings=0", result.stdout)

    def test_watch_follows_the_emulated_rtc(self):
        """Watch: the 1.85B RTC set through RT1 and a wall-clock jump move the
        hands; the system worker's RTC check runs in virtual time too."""
        script = (TO_LIST + "tap 180 135\nwait-app RUNNING 5000\nwait-nav ACTIVE 5000\nwait 500\nscreenshot unset.png\n"
                  "rt1 SET 1792314126\nrt1 TZ CET-1CEST,M3.5.0,M10.5.0/3\nwait 1500\nscreenshot set.png\n"
                  "wall-jump 3600\nwait 1500\nscreenshot jumped.png\nclock\nquit\n")
        shots = ["unset.png", "set.png", "jumped.png"]
        result, directory = self.run_deterministic("watch", script, shots)
        self.assertIn("!RT1 0000d0e5 OK state=0 os=0 rtc=1792314127 wall=1792314127 err=0", result.stdout)
        self.assertIn("clock valid=1 sync=1 wall=1792317730", result.stdout)
        self.assertEqual(len({(directory / name).read_bytes() for name in shots}), 3)

    def test_sound_in_virtual_time(self):
        """Sound: the stream opens, plays and is released at close. SDL takes
        samples in real time, so only the common prefix of two recordings is
        compared; it carries the same tune."""
        script = (TO_LIST + "audio\ntap 180 135\nwait-app RUNNING 5000\nwait 300\ntap 180 224\n"
                  "wait 1000\naudio\nwait 9000\n" + CLOSE_APP + "wait-app UNLOADED 5000\napp\naudio\nresources\nquit\n")
        result, directory = self.run_deterministic("sound", script, [], real_time_fields=("ring", "underruns"),
                                                   environment={"SDL_AUDIO_DRIVER": "disk"})
        audio = re.findall(r"audio initialized=1 playing=(\d) plays=(\d+) errors=0 volume=(\d+) stream=(\d)",
                           result.stdout)
        self.assertEqual(audio, [("0", "0", "0", "0"), ("1", "0", "35", "1"), ("0", "1", "35", "0")])
        self.assertIn('outstanding=0 busy=0 error=""', result.stdout)
        self.assertIn("resources image_mappings=0", result.stdout)
        _, again = self.run_desktop("1.85b", script, packages=[self.native["sound"]], native=True, virtual=True,
                                    environment={"SDL_AUDIO_DRIVER": "disk"})
        # Where silence falls (before the stream, after the close, in an
        # underrun) follows the device's real-time clock; the samples do not.
        recordings = []
        for folder in (directory, again):
            samples = array.array("h", (folder / "data/sdlaudio.raw").read_bytes())[0::2]
            recordings.append([value for value in samples if value != 0])
        common = min(len(recording) for recording in recordings)
        self.assertGreater(common, 50000)
        self.assertEqual(recordings[0][:common], recordings[1][:common])
        rate = int(re.search(r"format=S16LE channels=2 freq=(\d+)", result.stderr).group(1))
        left = array.array("h", recordings[0][:common])
        audible = [index for index, value in enumerate(left) if abs(value) > 200]
        self.assertGreater(len(audible), 0)
        window = left[audible[0]:audible[0] + rate // 5]
        crossings = sum(1 for a, b in zip(window, window[1:]) if (a < 0) != (b < 0))
        self.assertAlmostEqual(crossings / 2 / 0.2, 784, delta=20)

    def test_storage_survives_close_and_restart(self):
        """REP Storage: Watch's theme, written when the closed instance
        unloads, is there after a process restart; a change made before the
        process ends with the application still running is lost, as after a
        board reset."""
        change = TO_LIST + "tap 180 135\nwait-app RUNNING 5000\nwait-nav ACTIVE 5000\nwait 500\nscreenshot {0}0.png\nui-press 2\nwait 500\n" \
                 "screenshot {0}1.png\n"
        first, directory = self.run_desktop("1.85b", change.format("a") + CLOSE_APP +
                                            "wait-app UNLOADED 5000\n" + TO_LIST + "wait 200\nquit\n",
                                            packages=[self.native["watch"]], native=True, virtual=True)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        blob = (directory / "data/storage/packages/org.roundie.watch/data").read_bytes()
        self.assertEqual(blob[:4], b"RPD1")
        length = int.from_bytes(blob[4:8], "little")
        self.assertEqual(blob[8:40], hashlib.sha256(blob[40:40 + length]).digest())
        self.assertEqual(blob[40:], bytes([1, 1]))
        # The restarted process ends while the application runs.
        second, _ = self.run_desktop("1.85b", change.format("b") + "quit\n", install=False, native=True,
                                     virtual=True, directory=directory)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        third, _ = self.run_desktop("1.85b", TO_LIST + "tap 180 135\nwait-app RUNNING 5000\nwait-nav ACTIVE 5000\nwait 500\n"
                                    "screenshot c0.png\nquit\n", install=False, native=True, virtual=True,
                                    directory=directory)
        self.assertEqual(third.returncode, 0, third.stdout + third.stderr)
        for result in (first, second, third):
            self.assertNotIn("app_storage", result.stderr)
            self.assertNotIn("runtime error", result.stderr)
        shot = lambda name: (directory / name).read_bytes()
        self.assertNotEqual(shot("a0.png"), shot("a1.png"))
        self.assertEqual(shot("a1.png"), shot("b0.png"))  # saved at close, read after restart
        self.assertEqual(shot("b0.png"), shot("c0.png"))  # unsaved at the end of the process
        self.assertEqual((directory / "data/storage/packages/org.roundie.watch/data").read_bytes(), blob)

    # ---- Application faults and developer builds (M5b) --------------------------

    def test_application_fault_is_named_and_the_next_session_starts_on_the_list(self):
        """A fault in application code ends the process (nothing can recover
        the address space) after a report naming the package; the next
        session warns as the firmware does, shows only the built-in face and
        stays on the list, like the board's trial boot after Safe Mode."""
        faults = {"fault-load": ("org.roundie.fault.load", "SIGSEGV at image+"),
                  "fault-store": ("org.roundie.fault.store", "SIGSEGV at image+"),
                  "fault-fetch": ("org.roundie.fault.fetch", "SIGSEGV outside its image"),
                  "fault-stack": ("org.roundie.fault.stack", "SIGSEGV at image+"),
                  "fault-undefined": ("org.roundie.fault.undefined", "SIGILL at image+")}
        launch = TO_LIST + "tap 180 135\nwait-app RUNNING 5000\nwait 3000\nquit\n"
        for stem, (app_id, report) in faults.items():
            with self.subTest(fault=stem):
                crashed, directory = self.run_desktop("1.85b", launch, packages=[self.native[stem],
                                                      self.native["watch"]], native=True, virtual=True)
                self.assertNotEqual(crashed.returncode, 0, crashed.stdout + crashed.stderr)
                self.assertIn(f"E elf: application {app_id} faulted: {report}", crashed.stderr)
                self.assertEqual((directory / "data/app-loaded").read_text(), app_id)
                restarted, _ = self.run_desktop("1.85b", "wait 1500\napp\nface-status\nexpect-nav LIST\nquit\n", install=False,
                                                native=True, virtual=True, directory=directory)
                self.assertEqual(restarted.returncode, 0, restarted.stdout + restarted.stderr)
                self.assertIn(f"Previous session failed with package {app_id} loaded", restarted.stderr)
                self.assertIn("app state=UNLOADED instance=0", restarted.stdout)
                self.assertIn("fallback_only=1", restarted.stdout)
                self.assertFalse((directory / "data/app-loaded").exists())
                # A clean session after that starts on the face Home again.
                normal, _ = self.run_desktop("1.85b", "wait-nav HOME 5000\nquit\n", install=False, native=True,
                                             virtual=True, directory=directory)
                self.assertEqual(normal.returncode, 0, normal.stdout + normal.stderr)
                self.assertNotIn("Previous session", normal.stderr)
                if stem == "fault-undefined" and shutil.which("addr2line"):
                    offset = re.search(r"image\+(0x[0-9a-f]+)", crashed.stderr).group(1)
                    line = subprocess.run(["addr2line", "-e", str(self.binaries[stem]), offset],
                                          capture_output=True, text=True, check=True).stdout
                    source = (ROOT / "examples" / FAULT_SOURCE).read_text().splitlines()
                    number = int(line.strip().rsplit(":", 1)[1])
                    self.assertIn("largest + 1", source[number - 1])

    def test_gdb_breaks_in_application_source(self):
        """A --debug application: GDB resolves its symbols from the loaded
        image, stops at a source line and unwinds into the manager."""
        if shutil.which("gdb") is None:
            self.skipTest("gdb not installed")
        directory = Path(tempfile.mkdtemp(dir=self.work))
        (directory / "script.txt").write_text(TO_LIST + "tap 180 135\nwait-app RUNNING 5000\n"
                                              "wait 1000\nquit\n")
        commands = ["set pagination off", "set debuginfod enabled off", "set breakpoint pending on",
                    "break event.c:event", "run", "bt", "kill", "quit"]
        arguments = [argument for command in commands for argument in ("-ex", command)]
        result = subprocess.run(["gdb", "-q", "-batch", "-nx", *arguments, "--args", str(self.binary), "--headless",
                                 "--board", "1.85b", "--data", str(directory / "data"), "--script", "script.txt",
                                 "--native-apps", "--virtual-time", "--install", str(self.native["event-debug"])],
                                cwd=directory, capture_output=True, text=True, timeout=300,
                                env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0", SDL_AUDIO_DRIVER="dummy"))
        output = result.stdout + result.stderr
        self.assertRegex(output, r"hit Breakpoint 1, event \(context=0x[0-9a-f]+, event=0x[0-9a-f]+\) at "
                                 r"\S*examples/event_elf/event\.c:\d+")
        self.assertRegex(output, r"#1 .* roundie_elf_step \(")
        self.assertNotIn("is not at the expected address", output)

    def test_native_apps_are_opt_in(self):
        """Without --native-apps every launch is refused and nothing is loaded."""
        script = TO_LIST + "tap 180 135\nwait-nav RESULT 5000\napp\nresources\nquit\n"
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
                result, directory = self.run_desktop("1.85b", TO_LIST + "quit\n", packages=[package],
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
        script = TO_LIST + "tap 180 135\nwait-nav RESULT 5000\napp\nresources\nquit\n"
        result, _ = self.run_desktop("1.85b", script, packages=[package], native=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("binding at", result.stderr)
        self.assertIn('callbacks=0 outstanding=0 busy=0 error="loader-relocation"', result.stdout)
        self.assertIn("resources image_mappings=0", result.stdout)


    # ---- Virtual hardware lab (M6, docs/virtual-hardware.md) ---------------------
    # The lab's commands change the emulated peripherals while Roundie Core runs;
    # with --virtual-time every scenario gives the same output on every run.

    def run_lab(self, script, *, board="1.85b", real_time_fields=(), **options):
        """Runs a lab script twice in virtual time and returns the first run and
        its "hw" lines as dictionaries."""
        runs = [self.run_desktop(board, script, virtual=True, **options) for _ in range(2)]
        texts = []
        for result, _ in runs:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("runtime error", result.stderr)
            self.assertNotIn("ERROR: AddressSanitizer", result.stderr)
            text = result.stdout
            for field in real_time_fields:
                text = re.sub(rf"\b{field}=\d+", f"{field}=*", text)
            texts.append(text)
        self.assertEqual(texts[0], texts[1])
        result, directory = runs[0]
        states = [dict(field.split("=", 1) for field in line.split()[1:])
                  for line in result.stdout.splitlines() if line.startswith("hw ")]
        return result, directory, states

    def test_lab_battery_discharge_and_charge(self):
        """A 100 mAh cell at 6 % under 600 mA runs flat in 36 s: the gauge's
        state of charge falls, the board loses power and stays off; USB starts
        it and charges the cell at 250 mA; unplugged, it runs on the battery."""
        script = (TO_LIST + "battery capacity 100\nbattery soc 6\nbattery load 600 10\nhw\nwait 20000\nhw\n"
                  "wait 20000\nhw\nusb 1\nwait-nav HOME 5000\nhw\nwait 60000\nhw\nusb 0\nwait 1000\nhw\nquit\n")
        result, _, states = self.run_lab(script, install=False)
        summary = [(state["powered"], state["off"], state["soc"], state["current"]) for state in states]
        self.assertEqual(summary, [("1", "none", "6", "-600"), ("1", "none", "3", "-600"), ("0", "depleted", "0", "0"),
                                   ("1", "none", "0", "250"), ("1", "none", "4", "250"), ("1", "none", "4", "-600")])
        # 250 mA for 60 s is 4167 uAh.
        self.assertEqual(int(states[4]["charge_uah"]) - int(states[3]["charge_uah"]), 4167)
        self.assertLess(int(states[1]["mv"]), int(states[1]["ocv"]))  # the voltage sags under load
        self.assertGreater(int(states[3]["mv"]), int(states[3]["ocv"]))  # and rises while charging
        # The firmware's sensor worker reads the falling state of charge through the gauge.
        self.assertIn("gauge SoC 5% (accuracy unverified), flags=0x0009", result.stderr)
        self.assertIn("roundie-desktop: power lost: depleted", result.stderr)
        self.assertIn("roundie-desktop: the board is off (depleted)", result.stderr)
        self.assertIn("flags=0x0008", result.stderr)  # charging: not discharging

    def test_lab_pmic_battery_on_the_1_75c(self):
        """The 1.75C's AXP2101 profile: the gauge percent is an uncalibrated
        estimate (UNCALIBRATED, not initialized); the status flags carry VBUS,
        presence and the charge direction, and standby on a full cell is not
        charging."""
        script = ("wait-nav HOME 5000\n" + BOARDS["1.75c"]["home_to_list"] + "wait-nav LIST 5000\n"
                  "hw\nusb 1\nwait 6000\nhw\nbattery soc 100\nwait 6000\nhw\n"
                  "usb 0\nwait 6000\nhw\nquit\n")
        result, _, states = self.run_lab(script, board="1.75c", install=False)
        self.assertEqual([state["usb"] for state in states], ["0", "1", "1", "0"])
        self.assertIn("Gauge gauge unverified, present=1, initialized=0, error=ESP_OK", result.stderr)
        # On the battery: present, discharging (STATUS1 0x08, STATUS2 0x40).
        self.assertIn("gauge SoC 80% (accuracy unverified), flags=0x0840", result.stderr)
        # USB on a full cell: VBUS good, present, standby (charge complete).
        self.assertIn("gauge SoC 100% (accuracy unverified), flags=0x2800", result.stderr)
        self.assertNotIn("gauge SoC unavailable", result.stderr)

    def test_lab_screen_off_and_wake(self):
        """The power menu's screen off and BOOT wake; a failed display
        re-initialisation is reported and the GUI's repaint restores the panel."""
        off_and_wake = "key boot\nwait 300\ntap 180 116\nwait-power screen_off 2000\n{shot}key boot\nwait 300\n"
        # The list fades in after the swipe from Home; let it settle first.
        script = (TO_LIST + "wait 500\nscreenshot on.png\n" + off_and_wake.format(shot="screenshot off.png\n") +
                  "screenshot woke.png\ndisplay-fault restore\n" + off_and_wake.format(shot="") +
                  "power\nscreenshot lost.png\ndisplay-fault none\n" + off_and_wake.format(shot="") +
                  "power\nscreenshot restored.png\nquit\n")
        result, directory, _ = self.run_lab(script)
        reinit = re.findall(r"Display re-initialised after screen off: (\w+)", result.stderr)
        self.assertEqual(reinit, ["ESP_OK", "ESP_ERR_TIMEOUT", "ESP_OK"])
        self.assertEqual(result.stdout.count("power state=active lit=1"), 2)
        shot = lambda name: (directory / name).read_bytes()
        self.assertNotEqual(shot("off.png"), shot("on.png"))
        for name in ("woke.png", "lost.png", "restored.png"):
            self.assertEqual(shot(name), shot("on.png"), name)

    def test_lab_power_lifecycle(self):
        """M8-B on the emulated board, in virtual time and run twice: the state
        model through screen-off, a timer wake (screen stays off), BOOT and
        touch wakes, refusal by an inhibitor, a double release, an expired
        (leaked) inhibitor and a refused second request. VHL model, not
        hardware evidence (docs/power-management-m8-b.md)."""
        script = (
            "wait-nav HOME 5000\npm\n"
            # 1: screen off and on.
            "key boot\nwait 300\ntap 180 116\nwait-power screen_off 2000\npm\nkey boot\nwait-power active 2000\n"
            # 2, 12: a timed light sleep; the pure timer wake keeps the screen off.
            "power-sleep 2\npower-sleep 2\nwait-power light_sleep 3000\npm\nwait-power screen_off 5000\npm\n"
            "key boot\nwait-power active 2000\n"
            # 3, 19: an inhibitor refuses sleep; a second release is stale.
            "inhibit 1 60000\npower-sleep 2\nwait 1500\npm\nrelease\nrelease\n"
            # 14: BOOT wakes.
            "power-sleep 30\nwait-power light_sleep 3000\nwait 500\nkey boot\nwait-power active 3000\n"
            # 13: touch wakes.
            "power-sleep 30\nwait-power light_sleep 3000\nwait 500\ntap 180 180\nwait-power active 3000\npm\n"
            # 19: a leaked inhibitor expires.
            "inhibit 2 500\nwait 1000\npm\nquit\n")
        result, _, _ = self.run_lab(script, install=False)
        pms = [dict(field.split("=", 1) for field in line.split()[1:])
               for line in result.stdout.splitlines() if line.startswith("pm ")]
        self.assertEqual(len(pms), 7)
        self.assertEqual(pms[0]["state"], "ACTIVE")
        self.assertEqual((pms[1]["state"], pms[1]["from"]), ("SCREEN_OFF", "ACTIVE"))
        # 5: the second request is refused while the first is pending.
        self.assertEqual(re.findall(r"^power-sleep (\w+)", result.stdout, re.M),
                         ["accepted", "refused", "accepted", "accepted", "accepted"])
        self.assertEqual(pms[2]["state"], "LIGHT_SLEEP_PENDING")
        # 12: timer wake, screen stays off, resumed through RESUMING.
        self.assertEqual((pms[3]["state"], pms[3]["from"], pms[3]["timer_wakes"]), ("SCREEN_OFF", "RESUMING", "1"))
        # 3: the inhibitor cancels the request; nothing slept.
        self.assertEqual((pms[4]["state"], pms[4]["from"], pms[4]["last_refusal"]),
                         ("ACTIVE", "LIGHT_SLEEP_PENDING", "inhibited"))
        self.assertEqual((pms[4]["light_sleeps"], pms[4]["inhibitors"]), ("1", "1"))
        self.assertIn("sleep refused: inhibited", result.stderr)
        # 19: double release is stale (3), not applied.
        self.assertEqual(re.findall(r"^release result=(\d)", result.stdout, re.M), ["0", "3"])
        # 13, 14: BOOT and touch wakes, screen on.
        self.assertEqual(result.stderr.count("woke after 500 ms by BOOT or touch"), 2)
        self.assertEqual((pms[5]["state"], pms[5]["user_wakes"], pms[5]["light_sleeps"]), ("ACTIVE", "2", "3"))
        self.assertEqual((pms[5]["stale"], pms[5]["inhibitors"], pms[5]["rejected"]), ("1", "0", "0"))
        # 19: a leaked inhibitor expires and is counted.
        self.assertEqual((pms[6]["expired"], pms[6]["inhibitors"], pms[6]["acquired"]), ("1", "0", "2"))
        self.assertIn("sleep inhibitor(s) expired (leak)", result.stderr)

    def test_lab_rtc_failure_and_restore(self):
        """The PCF85063 missing, then back; a power cut keeps its time on the
        backup cell and loses it without one."""
        script = (TO_LIST + "rt1 SET 1792314126\nrtc-fault absent\nwait-clock invalid 7000\nrt1 GET\n"
                  "rtc-fault none\nrt1 SET 1792314126\nwait-clock valid 7000\nclock\npower-cut\nwait 2000\nhw\n"
                  "power-restore\nwait-nav HOME 5000\nclock\nrtc-backup 0\npower-cut\nwait 1000\npower-restore\n"
                  "wait-nav HOME 5000\nrt1 GET\nclock\nquit\n")
        result, _, states = self.run_lab(script, install=False)
        replies = [line for line in result.stdout.splitlines() if line.startswith("!RT1 ")]
        clocks = [dict(field.split("=", 1) for field in line.split()[1:])
                  for line in result.stdout.splitlines() if line.startswith("clock ")]
        self.assertIn("RTC_READ state=1 os=-1 rtc=-1 wall=-1 err=261", replies[1])  # ESP_ERR_NOT_FOUND
        self.assertIn("marked unsynchronized after RTC failure", result.stderr)
        self.assertEqual((states[0]["powered"], states[0]["off"]), ("0", "cut"))
        # On the backup cell the chip counted the 2 s without power.
        self.assertEqual(clocks[1]["valid"], "1")
        self.assertEqual(int(clocks[1]["wall"]) - int(clocks[0]["wall"]), 2)
        # Without it the oscillator-stop flag is set and time is not trusted.
        self.assertIn("RTC_INVALID state=3 os=1", replies[3])
        self.assertEqual((clocks[2]["valid"], clocks[2]["wall"]), ("0", "-1"))

    def test_lab_unresponsive_peripherals(self):
        """A touch controller that times out: one log line, no input, and
        touch works again at the firmware's retry 5 s after the failure. A gauge that times out reports an
        I2C error at the next sample and valid data once it answers."""
        script = (TO_LIST + "touch-fault timeout\nmark\ntap 180 135\nwait 500\nexpect-nav LIST\n"
                  "touch-fault none\nwait 4000\ntap 180 135\nwait 500\nexpect-nav LIST\nwait 1000\ntap 180 135\n"
                  "wait-nav RESULT 5000\ngauge-fault timeout\nwait 6000\n"
                  "gauge-fault none\nwait 6000\nquit\n")
        result, _, _ = self.run_lab(script, packages=[self.native["hello"]])
        self.assertEqual(result.stderr.count("Touch read failed: ESP_ERR_TIMEOUT"), 1)
        gauge = re.findall(r"Gauge ([\w ]+), present=(\d)", result.stderr)
        self.assertEqual(gauge, [("valid", "1"), ("I2C error", "0"), ("valid", "1")])

    def test_lab_storage_faults(self):
        """Storage: too little space refuses an application's data write, an I/O
        error fails a launch, a missing medium refuses package work; with the
        fault cleared the same application starts."""
        back = "tap 180 295\nwait-nav LIST 3000\n"
        script = (TO_LIST + "tap 180 135\nwait-app RUNNING 5000\nwait-nav ACTIVE 5000\nwait 500\nui-press 2\n"
                  "wait 500\nstorage-limit 8\n" +
                  CLOSE_APP + "wait-app UNLOADED 5000\n" + TO_LIST + "wait 200\nstorage-limit 0\n"
                  "storage-fault io\ntap 180 135\nwait 1000\nstatus\nscreenshot io.png\n" + back +
                  "storage-fault absent\ntap 180 135\nwait 1000\nstatus\n" + back +
                  "storage-fault none\ntap 180 135\nwait-app RUNNING 5000\napp\nquit\n")
        result, directory, _ = self.run_lab(script, packages=[self.native["watch"]], native=True)
        self.assertIn("org.roundie.watch: 2 bytes not saved: error 257 (data-storage-reserve)", result.stderr)
        self.assertFalse((directory / "data/storage/packages/org.roundie.watch/data").exists())
        launches = re.findall(r"launcher state=RESULT .* launch=(-?\d+)", result.stdout)
        self.assertEqual(launches, ["-1", "259"])  # ESP_FAIL from the I/O error, ESP_ERR_INVALID_STATE
        self.assertIn("app state=RUNNING instance=2 abi=3", result.stdout)

    def test_lab_storage_call_faults_keep_data(self):
        """REP Storage through the production package manager: Watch saves its
        theme when it closes; an open, write, fsync or rename that fails during
        that save is reported, the application unloads normally, no temporary
        file is left and the theme saved before stays; once the fault clears,
        the next save replaces it. Each attempt starts after a software reset
        and opens Watch from the list."""
        change = TO_LIST + "tap 180 135\nwait-app RUNNING 5000\nwait-nav ACTIVE 5000\nwait 500\nui-press 2\nwait 500\n"
        close = CLOSE_APP + "wait-app UNLOADED 5000\n" + TO_LIST + "wait 200\napp\n"
        calls = ("write", "fsync", "rename", "open")
        script = change + close + "reboot sw\n"
        for call in calls:
            # The theme saved before is read at start (1), pressed to 2 and not saved.
            script += change + f"storage-fault io {call}\nhw\n" + close + "storage-fault none\nreboot sw\n"
        script += change + close + "quit\n"
        result, directory, states = self.run_lab(script, packages=[self.native["watch"]], native=True)
        self.assertEqual([state["storage"] for state in states], [f"io:{call}" for call in calls])
        self.assertEqual(result.stdout.count("ui-press accepted=1"), 6)
        not_saved = re.findall(r"org\.roundie\.watch: 2 bytes not saved: error (-?\d+) \(([\w-]+)\)", result.stderr)
        self.assertEqual(not_saved, [("-1", "data-commit")] * 3 + [("-1", "data-open")])
        self.assertNotIn("data-corrupt", result.stderr)
        unloads = re.findall(r'app state=UNLOADED instance=1 .* error="([^"]*)"', result.stdout)
        self.assertEqual(unloads, [""] * 6)
        # Every start read theme 1, so the last save, without a fault, wrote 2.
        package = directory / "data/storage/packages/org.roundie.watch"
        blob = (package / "data").read_bytes()
        self.assertEqual(blob[:4], b"RPD1")
        self.assertEqual(blob[8:40], hashlib.sha256(blob[40:]).digest())
        self.assertEqual(blob[40:], bytes([1, 2]))
        self.assertFalse((package / "data.tmp").exists())

    def test_lab_state_is_not_inherited(self):
        """lab-state is read only by the process a virtual reset starts. One
        left by an earlier process, and a partial lab-state.new, are discarded
        at a fresh start, which begins with the default machine; --resume
        refuses a state file without its last line and applies none of it."""
        stale = "lab.reset 1\nlab.script_next 1\nvhw.powered 0\nvhw.off 3\nvhw.usb 1\nvhw.storage 2\nvhw.charge 1000\n"
        directory = Path(tempfile.mkdtemp(dir=self.work))
        data = directory / "data"
        data.mkdir()
        (data / "lab-state").write_text(stale + "lab.end 1\n")
        (data / "lab-state.new").write_text(stale)
        result, _ = self.run_desktop("1.85b", "hw\nquit\n", install=False, virtual=True, directory=directory)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("discarded lab-state left by an earlier process", result.stderr)
        self.assertIn("discarded lab-state.new left by an earlier process", result.stderr)
        self.assertIn("hw powered=1 off=none usb=0 battery=1 soc=80 ", result.stdout)
        self.assertIn(" storage=none ", result.stdout)
        self.assertFalse((data / "lab-state").exists())
        self.assertFalse((data / "lab-state.new").exists())
        # A partial state file named by --resume.
        partial = directory / "partial-state"
        partial.write_text(stale)
        command = [str(self.binary), "--headless", "--board", "1.85b", "--data", str(data), "--script", "script.txt",
                   "--virtual-time", "--resume", str(partial)]
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=60,
                                env=dict(os.environ, SDL_AUDIO_DRIVER="dummy"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("incomplete state, not applied", result.stderr)
        self.assertNotIn("hw ", result.stdout)

    def test_lab_audio_faults_and_close_during_playback(self):
        """Sound: an underrun is counted; an output device that fails mid-tune
        releases the stream with an error; the closed application leaves
        nothing behind and the next instance plays."""
        script = (TO_LIST + "tap 180 135\nwait-app RUNNING 5000\nwait 300\ntap 180 224\nwait 1000\n"
                  "audio-underrun\nwait 200\naudio\naudio-fault unavailable\nwait 200\naudio\n" + CLOSE_APP +
                  "wait-app UNLOADED 5000\naudio-fault none\n" + TO_LIST + "tap 180 135\nwait-app RUNNING 5000\n"
                  "wait 300\ntap 180 224\nwait 500\naudio\n" + CLOSE_APP + "wait-app UNLOADED 5000\napp\naudio\n"
                  "resources\nquit\n")
        result, _, _ = self.run_lab(script, packages=[self.native["sound"]], native=True, real_time_fields=("ring", "phase"))
        audio = re.findall(r"audio initialized=1 playing=(\d) plays=(\d+) errors=(\d+) volume=(\d+) stream=(\d) "
                           r"phase=(\d) ring=\d+ underruns=(\d+) pauses=(\d+) busy=(\d)", result.stdout)
        # Deterministic: the counters (the device's since start), the stream and
        # its owner. The phase of an open stream depends on how far the real-time
        # SDL device has read (docs/virtual-hardware.md), but it is always
        # playing (1) or feeding silence after the underrun (2), never waiting
        # or paused; a released stream keeps its last phase, which means nothing.
        self.assertEqual([(playing, plays, errors, volume, stream, underruns, pauses, busy)
                          for playing, plays, errors, volume, stream, _, underruns, pauses, busy in audio],
                         [("1", "0", "0", "35", "1", "1", "0", "1"), ("0", "1", "1", "35", "0", "1", "0", "0"),
                          ("1", "1", "1", "35", "1", "1", "0", "1"), ("0", "2", "1", "35", "0", "1", "0", "0")])
        for line in audio:
            if line[4] == "1":
                self.assertIn(line[5], ("1", "2"))
        self.assertIn("lab: buffered audio dropped", result.stderr)
        self.assertIn("write: output device unavailable (lab)", result.stderr)
        self.assertIn('app state=UNLOADED instance=2 abi=3 completed=2 loader=0 result=0', result.stdout)
        self.assertIn('outstanding=0 busy=0 error=""', result.stdout)
        self.assertIn("resources image_mappings=0", result.stdout)

    def test_lab_restart_light_sleep_and_power_loss(self):
        """The power menu's restart is a software reset of the process; light
        sleep draws the sleep current; PWR held 2 s on battery switches the
        board off and 1 s switches it on; with USB the latch cannot; a watchdog
        reset restarts it."""
        script = (TO_LIST + "key boot\nwait 300\ntap 180 157\nwait-nav HOME 5000\nhw\nkey boot\nwait 300\n"
                  "tap 180 75\nwait-power light_sleep 3000\nwait 1000\nhw\nkey boot\nwait-power active 3000\nhw\n"
                  "key pwr 2500\nhw\nwait 100\nkey pwr 1200\nwait-nav HOME 5000\nhw\nusb 1\nkey pwr 3000\nwait 100\n"
                  "hw\nreboot wdt\nwait-nav HOME 5000\nhw\nquit\n")
        result, _, states = self.run_lab(script, install=False)
        summary = [(state["powered"], state["off"], state["sleeping"], state["current"], state["cycles"])
                   for state in states]
        self.assertEqual(summary, [("1", "none", "0", "-120", "0"), ("1", "none", "1", "-10", "0"),
                                   ("1", "none", "0", "-120", "0"), ("0", "latch", "0", "0", "0"),
                                   ("1", "none", "0", "-120", "1"), ("1", "none", "0", "250", "1"),
                                   ("1", "none", "0", "250", "1")])
        starts = re.findall(r"roundie-desktop: (start after \w+ reset|the board is off \(\w+\)|power on)", result.stderr)
        self.assertEqual(starts, ["start after sw reset", "the board is off (latch)", "power on",
                                  "start after wdt reset"])
        self.assertIn("power menu restart: planned software reset", result.stderr)

    def test_hung_application_in_virtual_time(self):
        """A callback that never returns: after a second of real time, virtual
        time moves on without it, so the stall is reported at 5000 ms and the
        script runs on, the same on every run."""
        script = (TO_LIST + "tap 180 135\nwait-app RUNNING 5000\nwait-nav ACTIVE 5000\nwait 300\n" +
                  CLOSE_APP + "wait 6000\nstatus\napp\nscreenshot hung.png\nquit\n")
        result, _, _ = self.run_lab(script, packages=[self.native["hang"]], native=True)
        self.assertIn("instance 1 has not returned for 5000 ms", result.stderr)
        self.assertIn("app state=RUNNING instance=1 abi=2", result.stdout)

    def test_lab_panel_controls(self):
        """Each control of the panel is a lab command: clicks at frame pixels
        give the command and its effect, and the frame shows the state."""
        script = (TO_LIST + "panel-screenshot before.png\npanel-click 100 73\npanel-click 120 290\n"
                  "panel-click 92 64\nhw\npanel-screenshot after.png\nquit\n")
        result, directory, states = self.run_lab(script, install=False)
        self.assertEqual(re.findall(r"panel-click (.+)", result.stdout),
                         ["usb 1", "audio-fault unavailable", "usb 1"])
        self.assertEqual((states[0]["usb"], states[0]["audio"], states[0]["current"]), ("1", "unavailable", "250"))
        self.assertNotEqual((directory / "before.png").read_bytes(), (directory / "after.png").read_bytes())
        self.assertEqual(png_size(directory / "after.png"), (360, 392))
        # Off a control: the script stops.
        failed, _ = self.run_desktop("1.85b", TO_LIST + "panel-click 5 5\nquit\n", install=False)
        self.assertEqual(failed.returncode, 1)
        self.assertIn("no panel control there", failed.stderr)

    def test_lab_panel_window_in_gamescope(self):
        """--lab-panel opens the panel in a second Vulkan window; a mouse click
        on it applies the command while Roundie Core runs."""
        if shutil.which("gamescope") is None or shutil.which("xdotool") is None:
            self.skipTest("missing gamescope or xdotool")
        directory = Path(tempfile.mkdtemp(dir=self.work))
        (directory / "script.txt").write_text("wait 7000\nhw\nquit\n")
        (directory / "drive.sh").write_text(f"""#!/bin/bash
cd {directory}
{self.binary} --board 1.85b --zoom 1 --lab-panel --data {directory}/data --script script.txt > app.log 2>&1 &
app=$!
sleep 3
window=$(xdotool search --name 'Roundie virtual hardware' | head -1)
xdotool windowactivate --sync $window
sleep 0.5
xdotool mousemove --sync 100 73
sleep 0.2
xdotool mousedown 1; sleep 0.15; xdotool mouseup 1
sleep 1
gamescopectl screenshot {directory}/panel.png
wait $app
echo "exit $?" >> app.log
""")
        (directory / "drive.sh").chmod(0o755)
        env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", ASAN_OPTIONS="detect_leaks=0",
                   SDL_AUDIO_DRIVER="dummy")
        subprocess.run(["gamescope", "--backend", "headless", "-W", "800", "-H", "800", "--",
                        str(directory / "drive.sh")], capture_output=True, timeout=120, env=env)
        log = (directory / "app.log").read_text()
        self.assertIn("exit 0", log, log)
        self.assertNotIn("runtime error", log)
        self.assertIn("vulkan: swapchain 360x392, panel shown 360x392", log)
        self.assertIn("roundie-desktop: lab panel: usb 1", log)
        self.assertRegex(log, r"hw powered=1 off=none usb=1 .* current=250 ")


if __name__ == "__main__":
    unittest.main()
