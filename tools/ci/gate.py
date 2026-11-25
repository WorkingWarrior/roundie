#!/usr/bin/env python3
"""CI gate: the one check that branch protection requires (docs/ci.md).

  tools/ci/gate.py --needs NEEDS_JSON --jobs JOBS_FILE

NEEDS_JSON is the workflow's `needs` context (`toJSON(needs)`). JOBS_FILE
is the response of GET /repos/{repo}/actions/runs/{run}/attempts/{attempt}/jobs
for this run attempt.

The gate passes only when every required job is `success` in both sources:

- `needs` gives one result per job id. GitHub does not document how it
  combines the variants of a matrix job into that one result.
- The jobs API gives every matrix variant by name, so a failed firmware
  configuration or desktop group is seen even if `needs` were to hide it.

Any other result is a failure: `failure`, `cancelled`, `skipped`, a job that
has not completed, and a missing or malformed entry. Exit status 0 means
every required job passed; 1 means the gate failed; the reasons are printed.

The lists below are the contract. tests/test_ci_gate.py checks them against
.github/workflows/ci.yml, so a job added to or removed from the workflow
without updating the gate fails the host tests.
"""
import argparse
import json
import sys

# Job ids in the gate's `needs`, including the preparatory jobs.
REQUIRED_NEEDS = (
    "workflow-lint",
    "firmware",
    "components",
    "ci-image",
    "host",
    "manager",
    "desktop-build",
    "desktop",
)

FIRMWARE_BOARDS = ("waveshare_s3_touch_lcd_1_85b", "waveshare_s3_touch_amoled_1_75c")
FIRMWARE_PROFILES = ("legacy", "rep", "rep-nodev")
DESKTOP_TITLES = (
    "REP x86-64 native applications (ASan/UBSan)",
    "Desktop ASan/UBSan, virtual time",
    "Desktop ASan/UBSan, real time",
    "Virtual Hardware Lab",
    "Graphics (gamescope, KWin, lavapipe)",
    "Desktop ThreadSanitizer",
)

# Job names as the jobs API reports them, one per matrix variant.
REQUIRED_JOBS = (
    ("Workflow syntax (actionlint)",)
    + tuple(f"Firmware {board} {profile}" for board in FIRMWARE_BOARDS for profile in FIRMWARE_PROFILES)
    + (
        "Managed components (LVGL for the desktop)",
        "CI image (Arch Linux snapshot, Mesa lavapipe)",
        "Host tests and validators",
        "roundie-manager (typecheck, unit tests)",
        "Desktop build (Meson, SDL3, Vulkan)",
    )
    + DESKTOP_TITLES
)


def check_needs(needs):
    """Returns a list of (job id, problem) for the `needs` context."""
    if not isinstance(needs, dict):
        return [("needs", "not a JSON object")]
    problems = []
    for job in REQUIRED_NEEDS:
        entry = needs.get(job)
        if entry is None:
            problems.append((job, "no result"))
            continue
        if not isinstance(entry, dict):
            problems.append((job, "malformed entry"))
            continue
        result = entry.get("result")
        if not isinstance(result, str):
            problems.append((job, "no result"))
        elif result != "success":
            problems.append((job, f"result {result}"))
    for job in sorted(set(needs) - set(REQUIRED_NEEDS)):
        problems.append((job, "in needs but not required by the gate (update tools/ci/gate.py)"))
    return problems


def check_jobs(response):
    """Returns a list of (job name, problem) for the jobs API response."""
    if not isinstance(response, dict) or not isinstance(response.get("jobs"), list):
        return [("jobs", "not a jobs API response")]
    jobs = response["jobs"]
    total = response.get("total_count")
    if not isinstance(total, int) or total != len(jobs):
        return [("jobs", f"incomplete response: {len(jobs)} jobs listed, total_count {total!r}")]
    by_name = {}
    for job in jobs:
        if not isinstance(job, dict) or not isinstance(job.get("name"), str):
            return [("jobs", "malformed job entry")]
        by_name.setdefault(job["name"], []).append(job)
    problems = []
    for name in REQUIRED_JOBS:
        entries = by_name.get(name, [])
        if not entries:
            problems.append((name, "not in this run attempt"))
            continue
        if len(entries) > 1:
            problems.append((name, f"{len(entries)} jobs with this name"))
            continue
        job = entries[0]
        if job.get("status") != "completed":
            problems.append((name, f"status {job.get('status')!r}"))
        elif job.get("conclusion") != "success":
            problems.append((name, f"conclusion {job.get('conclusion')!r}"))
    return problems


def evaluate(needs_text, jobs_text):
    """Returns (source, job, problem) tuples; an empty list means the gate passes."""
    problems = []
    try:
        needs = json.loads(needs_text)
    except (TypeError, ValueError) as error:
        problems.append(("needs", "needs", f"invalid JSON: {error}"))
    else:
        problems += [("needs", job, problem) for job, problem in check_needs(needs)]
    try:
        response = json.loads(jobs_text)
    except (TypeError, ValueError) as error:
        problems.append(("jobs", "jobs", f"invalid JSON: {error}"))
    else:
        problems += [("jobs", job, problem) for job, problem in check_jobs(response)]
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--needs", required=True, help="the workflow's needs context as JSON")
    parser.add_argument("--jobs", required=True, help="file with the jobs API response for this run attempt")
    arguments = parser.parse_args(argv)
    try:
        with open(arguments.jobs, encoding="utf-8") as stream:
            jobs_text = stream.read()
    except OSError as error:
        jobs_text = None
        print(f"cannot read {arguments.jobs}: {error}")
    problems = evaluate(arguments.needs, jobs_text)
    checked = len(REQUIRED_NEEDS) + len(REQUIRED_JOBS)
    if not problems:
        print(f"CI gate: PASS. {len(REQUIRED_NEEDS)} job ids in needs and {len(REQUIRED_JOBS)} jobs "
              f"(every matrix variant) succeeded.")
        return 0
    print(f"CI gate: FAIL. {len(problems)} problem(s) in {checked} required results:")
    for source, job, problem in problems:
        print(f"  [{source}] {job}: {problem}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
