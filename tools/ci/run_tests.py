#!/usr/bin/env python3
"""Runs the host test suite (tests/) in named groups and reports PASS, FAIL
and SKIP per test (docs/ci.md). The tests are the existing unittest modules;
this file only selects them and writes reports.

  tools/ci/run_tests.py --list
  tools/ci/run_tests.py --check-coverage
  tools/ci/run_tests.py GROUP... [--strict] [--reports DIR]

Every test belongs to exactly one primary group (--check-coverage). The tsan
group runs a subset again with the desktop built under ThreadSanitizer.

--strict, used in CI, also fails a group when a test is skipped or when a
tool that a test would silently do without is missing: a CI image that lacks
a dependency must not turn into a green run.

Reports: DIR/GROUP.json, DIR/GROUP.xml (JUnit) and DIR/GROUP.md; the Markdown
is also appended to $GITHUB_STEP_SUMMARY when that is set. With --reports,
ROUNDIE_KEEP_TEST_WORK keeps the desktop tests' work directories under
DIR/work for the failure artifacts.
"""
import argparse
import fnmatch
import io
import json
import os
from pathlib import Path
import shutil
import sys
import time
import unittest
import xml.etree.ElementTree as ElementTree

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "tests"

# Timing of a test's result: "none" (no clock involved), "virtual" (desktop
# --virtual-time: deterministic, compared across two runs where the test does
# so) or "real" (real time, SDL audio pace or a compositor: may vary with load).
DESKTOP = "test_desktop.DesktopRunTests."

VIRTUAL_TIME = [
    "test_events_lifecycle_in_virtual_time",
    "test_ui_bridge_input_in_virtual_time",
    "test_round_widgets_in_virtual_time",
    "test_stopwatch_timing_in_virtual_time",
    "test_watch_follows_the_emulated_rtc",
    "test_sound_in_virtual_time",
    "test_storage_survives_close_and_restart",
    "test_watch_faces_on_home",
]
REAL_TIME = [
    "test_launcher_flow_on_both_panels",
    "test_touch_off_the_button_does_nothing",
    "test_slide_off_a_button_still_clicks",
    "test_power_menu",
    "test_time_without_rtc_across_resets",
    "test_time_with_rtc",
    "test_wall_clock_jumps_leave_power_timeouts_alone",
    "test_sound_plays_through_sdl_audio",
    "test_sound_closed_while_playing_releases_the_stream",
    "test_hung_application_does_not_block_the_gui",
]
NATIVE = {
    "test_empty_registry_lists_no_packages": "real",
    "test_native_apps_through_the_launcher": "real",
    "test_native_cycles_release_everything": "real",
    "test_native_apps_are_opt_in": "real",
    "test_incompatible_and_interposing_packages_are_rejected": "real",
    "test_application_fault_is_named_and_the_next_session_starts_on_the_list": "virtual",
    "test_gdb_breaks_in_application_source": "real",
}
LAB = [
    "test_lab_battery_discharge_and_charge",
    "test_lab_pmic_battery_on_the_1_75c",
    "test_lab_screen_off_and_wake",
    "test_lab_power_lifecycle",
    "test_lab_rtc_failure_and_restore",
    "test_lab_unresponsive_peripherals",
    "test_lab_storage_faults",
    "test_lab_storage_call_faults_keep_data",
    "test_lab_state_is_not_inherited",
    "test_lab_audio_faults_and_close_during_playback",
    "test_lab_restart_light_sleep_and_power_loss",
    "test_hung_application_in_virtual_time",
    "test_lab_panel_controls",
]
GRAPHICS = [
    "test_window_in_gamescope",
    "test_drag_gestures_in_gamescope",
    "test_pointer_outside_the_panel_in_gamescope",
    "test_lab_panel_window_in_gamescope",
    "test_window_in_nested_kwin_at_scale_2",
]
# ThreadSanitizer: the scenarios with threads (executor, GUI, audio, storage
# workers) that docs/native-apps.md lists for its earlier manual TSan runs,
# plus the lab's storage and audio faults and the model's own TSan test.
TSAN = [
    "test_launcher_flow_on_both_panels",
    "test_events_lifecycle_in_virtual_time",
    "test_ui_bridge_input_in_virtual_time",
    "test_round_widgets_in_virtual_time",
    "test_stopwatch_timing_in_virtual_time",
    "test_watch_follows_the_emulated_rtc",
    "test_sound_in_virtual_time",
    "test_sound_closed_while_playing_releases_the_stream",
    "test_hung_application_in_virtual_time",
    "test_lab_storage_call_faults_keep_data",
    "test_lab_audio_faults_and_close_during_playback",
]

HOST_MODULES = [
    "test_app_ui", "test_board_1_75c", "test_boot_guard", "test_ci_gate", "test_display_completion", "test_elf",
    "test_endurance",
    "test_lcd_clock", "test_packages", "test_power", "test_rep_deployment", "test_roundie_budget", "test_roundie_clock", "test_roundie_crash",
    "test_roundie_display", "test_roundie_elf", "test_roundie_launcher", "test_roundie_ota",
    "test_roundie_safe", "test_roundie_time", "test_run_m8", "test_stack_overflow_model", "test_storage_layout",
    "test_storage_mode", "test_tone", "test_watchface",
]

# name: (title, [(pattern, timing)], environment, primary)
GROUPS = {
    "host": ("Host unit tests and validators",
             [(module + ".*", "none") for module in HOST_MODULES]
             + [("test_desktop.DesktopContractTests.*", "none")], {}, True),
    "native": ("REP x86-64 native applications (ASan/UBSan desktop)",
               [("test_native_elf.*", "none")] + [(DESKTOP + name, timing) for name, timing in NATIVE.items()],
               {}, True),
    "desktop-virtual-time": ("Desktop runtime, virtual time (ASan/UBSan)",
                             [(DESKTOP + name, "virtual") for name in VIRTUAL_TIME], {}, True),
    "desktop-real-time": ("Desktop runtime, real time (ASan/UBSan)",
                          [(DESKTOP + name, "real") for name in REAL_TIME], {}, True),
    "lab": ("Virtual Hardware Lab (model under TSan, desktop in virtual time)",
            [("test_vhw.*", "none")] + [(DESKTOP + name, "virtual") for name in LAB], {}, True),
    "graphics": ("Graphics: gamescope headless and nested KWin, Vulkan (real time)",
                 [(DESKTOP + name, "real") for name in GRAPHICS], {}, True),
    "tsan": ("Desktop runtime under ThreadSanitizer",
             [(DESKTOP + name, "virtual" if name in VIRTUAL_TIME + LAB else "real") for name in TSAN],
             {"ROUNDIE_DESKTOP_SANITIZE": "thread"}, False),
}

# Skips that --strict accepts, with why CI cannot provide the input. They are
# still reported as SKIP, never as PASS.
ALLOWED_SKIPS = {
    "test_rep_deployment.RepDeploymentTest.test_legacy_guard_rejects_physical_rep_table":
        "reads a private flash dump of the developer's board, which CI must not hold",
}

# Tools without which a test passes with less coverage instead of skipping.
SILENT = {
    "xtensa": ("test_desktop.DesktopRunTests.test_incompatible_and_interposing_packages_are_rejected",
               "test_native_elf.*"),
}


def discover():
    sys.path.insert(0, str(TESTS))
    suite = unittest.defaultTestLoader.discover(str(TESTS), top_level_dir=str(TESTS))
    tests = []

    def walk(item):
        for test in item:
            if isinstance(test, unittest.TestSuite):
                walk(test)
            else:
                tests.append(test)
    walk(suite)
    return tests


def match(test_id, patterns):
    for pattern, timing in patterns:
        if fnmatch.fnmatchcase(test_id, pattern):
            return timing
    return None


def check_coverage(tests):
    problems = []
    for test in tests:
        if test.id().startswith("unittest.loader."):
            problems.append(f"{test.id()}: import failed")
            continue
        owners = [name for name, (_, patterns, _, primary) in GROUPS.items()
                  if primary and match(test.id(), patterns) is not None]
        if len(owners) != 1:
            problems.append(f"{test.id()}: in {len(owners)} primary groups {owners}")
    ids = [test.id() for test in tests]
    for name, (_, patterns, _, _) in GROUPS.items():
        for pattern, _ in patterns:
            if not any(fnmatch.fnmatchcase(test_id, pattern) for test_id in ids):
                problems.append(f"group {name}: pattern {pattern} matches no test")
    return problems


def preflight(test_ids):
    """Missing tools that would weaken a test without a skip."""
    problems = []
    import xtensa_toolchain
    if not (xtensa_toolchain.TOOLCHAIN / "xtensa-esp32s3-elf-gcc").exists():
        if any(fnmatch.fnmatchcase(test_id, pattern) for test_id in test_ids for pattern in SILENT["xtensa"]):
            problems.append(f"Xtensa toolchain not found at {xtensa_toolchain.TOOLCHAIN} "
                            "(set ROUNDIE_XTENSA_TOOLCHAIN)")
    return problems


class Result(unittest.TextTestResult):
    def __init__(self, *arguments, **keywords):
        super().__init__(*arguments, **keywords)
        self.records = {}
        self.started = {}

    def record(self, test, status, detail=""):
        record = self.records.setdefault(test.id(), {"id": test.id(), "status": "PASS", "detail": "",
                                                     "seconds": 0.0, "subtests": []})
        # A failure wins over an earlier pass; a skip only when nothing else ran.
        order = {"PASS": 0, "SKIP": 1, "FAIL": 2, "ERROR": 3}
        if order[status] >= order[record["status"]]:
            record["status"] = status
            if detail:
                record["detail"] = detail

    def startTest(self, test):
        self.started[test.id()] = time.monotonic()
        super().startTest(test)

    def stopTest(self, test):
        super().stopTest(test)
        if test.id() in self.records:
            self.records[test.id()]["seconds"] = round(time.monotonic() - self.started.get(test.id(), 0), 1)

    def addSuccess(self, test):
        super().addSuccess(test)
        self.record(test, "PASS")

    def addFailure(self, test, error):
        super().addFailure(test, error)
        self.record(test, "FAIL", self._exc_info_to_string(error, test))

    def addError(self, test, error):
        super().addError(test, error)
        # setUpClass errors arrive with a _ErrorHolder, not the test.
        self.record(test, "ERROR", self._exc_info_to_string(error, test))

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        self.record(test, "SKIP", reason)

    def addSubTest(self, test, subtest, error):
        super().addSubTest(test, subtest, error)
        if error is not None:
            status = "FAIL" if issubclass(error[0], test.failureException) else "ERROR"
            self.record(test, status, f"{subtest.id()}\n" + self._exc_info_to_string(error, test))

    def addExpectedFailure(self, test, error):
        super().addExpectedFailure(test, error)
        self.record(test, "FAIL", "expected failure is not allowed in CI")

    def addUnexpectedSuccess(self, test):
        super().addUnexpectedSuccess(test)
        self.record(test, "FAIL", "unexpected success")


def run_group(name, tests, reports, strict):
    title, patterns, environment, _ = GROUPS[name]
    selected = [test for test in tests if match(test.id(), patterns) is not None]
    os.environ.update(environment)
    problems = preflight([test.id() for test in selected]) if strict else []

    log_path = reports / f"{name}.log"
    started = time.monotonic()
    with open(log_path, "w") as log:
        stream = Tee(sys.stderr, log)
        runner = unittest.TextTestRunner(stream=stream, verbosity=2, resultclass=Result, buffer=True)
        result = runner.run(unittest.TestSuite(selected))
    seconds = round(time.monotonic() - started, 1)

    # A skip or error of setUpClass is recorded as "setUpClass (module.Class)"
    # and applies to every test of that class that did not run.
    known = {test.id() for test in selected}
    fixtures = {test_id: record for test_id, record in result.records.items() if test_id not in known}
    records = []
    applied = set()
    for test in selected:
        record = result.records.get(test.id())
        if record is None:
            owner = test.id().rpartition(".")[0]
            fixture_id = next((name for name in fixtures if f"({owner})" in name), None)
            fixture = fixtures.get(fixture_id)
            applied.add(fixture_id)
            record = {"id": test.id(), "seconds": 0.0, "subtests": [],
                      "status": fixture["status"] if fixture else "ERROR",
                      "detail": fixture["detail"] if fixture else "no result recorded"}
        record["timing"] = match(test.id(), patterns)
        records.append(record)
    # Fixture errors that no test carries, such as a tearDownClass error.
    for fixture_id, record in fixtures.items():
        if fixture_id not in applied and record["status"] != "SKIP":
            record["timing"] = "none"
            records.append(record)

    counts = {status: sum(1 for record in records if record["status"] == status)
              for status in ("PASS", "FAIL", "ERROR", "SKIP")}
    failed = counts["FAIL"] + counts["ERROR"] > 0 or bool(problems)
    unexpected = [record["id"] for record in records
                  if record["status"] == "SKIP" and record["id"] not in ALLOWED_SKIPS]
    for record in records:
        if record["status"] == "SKIP" and record["id"] in ALLOWED_SKIPS:
            record["detail"] += f" [allowed in CI: {ALLOWED_SKIPS[record['id']]}]"
    if strict and unexpected:
        problems.append(f"{len(unexpected)} unexpected skip(s): a CI dependency is missing")
        failed = True
    verdict = "FAIL" if failed else ("SKIP" if counts["PASS"] == 0 and counts["SKIP"] else "PASS")
    summary = {"group": name, "title": title, "verdict": verdict, "seconds": seconds, "counts": counts,
               "problems": problems, "environment": environment, "tests": records}
    (reports / f"{name}.json").write_text(json.dumps(summary, indent=2) + "\n")
    write_junit(reports / f"{name}.xml", summary)
    markdown = render_markdown(summary)
    (reports / f"{name}.md").write_text(markdown)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as output:
            output.write(markdown)
    print(f"\n{name}: {verdict} ({counts['PASS']} pass, {counts['FAIL']} fail, {counts['ERROR']} error, "
          f"{counts['SKIP']} skip) in {seconds} s", file=sys.stderr)
    for problem in problems:
        print(f"{name}: {problem}", file=sys.stderr)
    return not failed


class Tee(io.TextIOBase):
    def __init__(self, *streams):
        self.streams = streams

    def write(self, text):
        for stream in self.streams:
            if not stream.closed:
                stream.write(text)
                stream.flush()
        return len(text)

    def flush(self):
        for stream in self.streams:
            if not stream.closed:
                stream.flush()


def write_junit(path, summary):
    suite = ElementTree.Element("testsuite", name=summary["group"], tests=str(len(summary["tests"])),
                                failures=str(summary["counts"]["FAIL"]), errors=str(summary["counts"]["ERROR"]),
                                skipped=str(summary["counts"]["SKIP"]), time=str(summary["seconds"]))
    for record in summary["tests"]:
        module, _, test = record["id"].rpartition(".")
        case = ElementTree.SubElement(suite, "testcase", classname=module, name=test, time=str(record["seconds"]))
        if record["status"] == "FAIL":
            ElementTree.SubElement(case, "failure", message="failed").text = record["detail"]
        elif record["status"] == "ERROR":
            ElementTree.SubElement(case, "error", message="error").text = record["detail"]
        elif record["status"] == "SKIP":
            ElementTree.SubElement(case, "skipped", message=record["detail"])
    for problem in summary["problems"]:
        case = ElementTree.SubElement(suite, "testcase", classname="ci", name="preflight")
        ElementTree.SubElement(case, "failure", message=problem)
    ElementTree.ElementTree(suite).write(path, encoding="unicode", xml_declaration=True)


def render_markdown(summary):
    counts = summary["counts"]
    lines = [f"### {summary['verdict']} — {summary['group']}: {summary['title']}", "",
             f"{counts['PASS']} pass, {counts['FAIL']} fail, {counts['ERROR']} error, {counts['SKIP']} skip; "
             f"{summary['seconds']} s", ""]
    for problem in summary["problems"]:
        lines.append(f"- **CI problem:** {problem}")
    if summary["problems"]:
        lines.append("")
    lines += ["| Result | Timing | Test | s |", "|---|---|---|---|"]
    for record in summary["tests"]:
        lines.append(f"| {record['status']} | {record['timing']} | `{record['id']}` | {record['seconds']} |")
    details = [record for record in summary["tests"] if record["status"] != "PASS" and record["detail"]]
    if details:
        lines += ["", "<details><summary>Failures and skip reasons</summary>", ""]
        for record in details:
            text = record["detail"]
            if len(text) > 4000:
                text = text[:4000] + "\n[truncated; see the group log artifact]"
            lines += [f"**{record['status']} `{record['id']}`**", "", "```", text.replace("```", "'''"), "```", ""]
        lines.append("</details>")
    lines.append("")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("groups", nargs="*", help="groups to run (see --list)")
    parser.add_argument("--list", action="store_true", help="list the groups and their tests")
    parser.add_argument("--check-coverage", action="store_true",
                        help="check that every test is in exactly one primary group")
    parser.add_argument("--strict", action="store_true", help="fail on skips and on missing silent tools")
    parser.add_argument("--reports", type=Path, default=ROOT / "ci-reports", help="report directory")
    arguments = parser.parse_args()

    tests = discover()
    if arguments.list:
        for name, (title, patterns, environment, primary) in GROUPS.items():
            extra = " ".join(f"{key}={value}" for key, value in environment.items())
            print(f"{name}: {title}{' [' + extra + ']' if extra else ''}{'' if primary else ' (re-run)'}")
            for test in tests:
                timing = match(test.id(), patterns)
                if timing is not None:
                    print(f"  {timing:7} {test.id()}")
        return 0
    if arguments.check_coverage:
        problems = check_coverage(tests)
        for problem in problems:
            print(problem, file=sys.stderr)
        print(f"{len(tests)} tests, {len(problems)} coverage problem(s)", file=sys.stderr)
        return 1 if problems else 0
    unknown = [name for name in arguments.groups if name not in GROUPS]
    if unknown or not arguments.groups:
        parser.error(f"unknown or no group: {unknown}; groups: {', '.join(GROUPS)}")
    if len([name for name in arguments.groups if GROUPS[name][2]]) and len(arguments.groups) > 1:
        parser.error("run a group with its own environment (tsan) on its own")

    arguments.reports.mkdir(parents=True, exist_ok=True)
    work = arguments.reports / "work"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir()
    os.environ["ROUNDIE_KEEP_TEST_WORK"] = "1"
    os.environ["TMPDIR"] = str(work.resolve())
    import tempfile
    tempfile.tempdir = None

    passed = True
    for name in arguments.groups:
        passed = run_group(name, tests, arguments.reports, arguments.strict) and passed
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
