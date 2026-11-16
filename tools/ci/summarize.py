#!/usr/bin/env python3
"""Collects the CI reports (tools/ci/run_tests.py, build_firmware.sh,
build_desktop.sh) of one workflow run into one Markdown overview:

  tools/ci/summarize.py REPORTS_DIR [JOB_RESULTS_JSON]

REPORTS_DIR is searched recursively. JOB_RESULTS_JSON is the workflow's
`needs` context, so that a job that ended before writing a report (setup
failure, timeout, cancellation) still shows. The hardware checks that CI
cannot run follow, taken from docs/ci.md."""
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    reports = Path(sys.argv[1])
    needs = json.loads(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2] else {}
    lines = ["## Roundie CI", "", "| Result | Group | Pass | Fail | Error | Skip | s |", "|---|---|---|---|---|---|---|"]
    for path in sorted(reports.rglob("*.json")):
        try:
            report = json.loads(path.read_text())
        except json.JSONDecodeError:
            continue
        if not isinstance(report, dict) or "group" not in report:
            continue
        counts = report["counts"]
        lines.append(f"| {report['verdict']} | {report['group']}: {report['title']} | {counts['PASS']} | "
                     f"{counts['FAIL']} | {counts['ERROR']} | {counts['SKIP']} | {report['seconds']} |")
    builds = []
    for path in sorted(reports.rglob("*.md")):
        first = path.read_text().splitlines()[:3]
        if first and re.match(r"### (PASS|FAIL) — (firmware|desktop build)", first[0]):
            builds.append(f"| {first[0][4:8].strip()} | {first[0][11:]} | {first[2] if len(first) > 2 else ''} |")
    if builds:
        lines += ["", "| Result | Build | Detail |", "|---|---|---|"] + builds
    if needs:
        lines += ["", "| Job | Result |", "|---|---|"]
        lines += [f"| {name} | {value.get('result', '?')} |" for name, value in sorted(needs.items())]
    lines += ["", hardware_pending()]
    print("\n".join(lines))


def hardware_pending():
    text = (ROOT / "docs/ci.md").read_text()
    match = re.search(r"^## Hardware checks outside CI\n(.*?)(?=^## |\Z)", text, re.S | re.M)
    if match is None:
        return "### Hardware checks outside CI\n\nSee docs/ci.md."
    return "### Hardware checks outside CI (PENDING, not run by CI)\n" + match.group(1).rstrip() + "\n"


if __name__ == "__main__":
    main()
