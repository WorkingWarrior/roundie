"""CI gate (tools/ci/gate.py, docs/ci.md): the aggregation of job results and
its agreement with .github/workflows/ci.yml. Runs without GitHub Actions."""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "ci"))

import gate  # noqa: E402

WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"


def green_needs():
    return {job: {"result": "success", "outputs": {}} for job in gate.REQUIRED_NEEDS}


def green_jobs(extra=()):
    jobs = [{"name": name, "status": "completed", "conclusion": "success"} for name in gate.REQUIRED_JOBS]
    # Jobs that are not required: Summary, and the gate itself while it runs.
    jobs += [{"name": "Summary", "status": "completed", "conclusion": "success"},
             {"name": "CI gate", "status": "in_progress", "conclusion": None}]
    jobs += list(extra)
    return {"total_count": len(jobs), "jobs": jobs}


def evaluate(needs, jobs):
    needs_text = needs if isinstance(needs, str) else json.dumps(needs)
    jobs_text = jobs if isinstance(jobs, str) or jobs is None else json.dumps(jobs)
    return gate.evaluate(needs_text, jobs_text)


def set_job(jobs, name, **fields):
    for job in jobs["jobs"]:
        if job["name"] == name:
            job.update(fields)
            return jobs
    raise AssertionError(name)


class AggregationTests(unittest.TestCase):
    def assert_fails(self, problems, job, text):
        self.assertTrue(problems, "the gate passed")
        self.assertTrue(any(entry[1] == job and text in entry[2] for entry in problems), problems)

    def test_all_success_passes(self):
        self.assertEqual(evaluate(green_needs(), green_jobs()), [])

    def test_single_failure_fails(self):
        needs = green_needs()
        needs["manager"]["result"] = "failure"
        jobs = set_job(green_jobs(), "roundie-manager (typecheck, unit tests)", conclusion="failure")
        problems = evaluate(needs, jobs)
        self.assert_fails(problems, "manager", "result failure")
        self.assert_fails(problems, "roundie-manager (typecheck, unit tests)", "conclusion 'failure'")

    def test_cancelled_fails(self):
        needs = green_needs()
        needs["host"]["result"] = "cancelled"
        self.assert_fails(evaluate(needs, green_jobs()), "host", "result cancelled")
        jobs = set_job(green_jobs(), "Host tests and validators", conclusion="cancelled")
        self.assert_fails(evaluate(green_needs(), jobs), "Host tests and validators", "conclusion 'cancelled'")

    def test_skipped_fails(self):
        needs = green_needs()
        needs["desktop-build"]["result"] = "skipped"
        self.assert_fails(evaluate(needs, green_jobs()), "desktop-build", "result skipped")
        jobs = set_job(green_jobs(), "Desktop build (Meson, SDL3, Vulkan)", conclusion="skipped")
        self.assert_fails(evaluate(green_needs(), jobs), "Desktop build (Meson, SDL3, Vulkan)", "conclusion 'skipped'")

    def test_missing_result_fails(self):
        needs = green_needs()
        del needs["workflow-lint"]
        self.assert_fails(evaluate(needs, green_jobs()), "workflow-lint", "no result")
        needs = green_needs()
        needs["components"] = {"outputs": {}}
        self.assert_fails(evaluate(needs, green_jobs()), "components", "no result")
        jobs = green_jobs()
        jobs["jobs"] = [job for job in jobs["jobs"] if job["name"] != "Virtual Hardware Lab"]
        jobs["total_count"] = len(jobs["jobs"])
        self.assert_fails(evaluate(green_needs(), jobs), "Virtual Hardware Lab", "not in this run attempt")

    def test_job_not_completed_fails(self):
        jobs = set_job(green_jobs(), "Workflow syntax (actionlint)", status="in_progress", conclusion=None)
        self.assert_fails(evaluate(green_needs(), jobs), "Workflow syntax (actionlint)", "status 'in_progress'")

    def test_failed_preparatory_job_fails(self):
        # The CI image failed: every job after it is skipped, which GitHub may
        # report as passing in a list of required checks. The gate does not.
        needs = green_needs()
        needs["ci-image"]["result"] = "failure"
        for job in ("host", "manager", "desktop-build", "desktop"):
            needs[job]["result"] = "skipped"
        jobs = set_job(green_jobs(), "CI image (Arch Linux snapshot, Mesa lavapipe)", conclusion="failure")
        problems = evaluate(needs, jobs)
        self.assert_fails(problems, "ci-image", "result failure")
        self.assert_fails(problems, "host", "result skipped")
        self.assert_fails(problems, "CI image (Arch Linux snapshot, Mesa lavapipe)", "conclusion 'failure'")
        needs = green_needs()
        needs["components"]["result"] = "failure"
        self.assert_fails(evaluate(needs, green_jobs()), "components", "result failure")

    def test_one_firmware_variant_fails(self):
        # Even if needs reported the matrix as success, the variant fails the gate.
        name = "Firmware waveshare_s3_touch_amoled_1_75c rep"
        jobs = set_job(green_jobs(), name, conclusion="failure")
        problems = evaluate(green_needs(), jobs)
        self.assertEqual(problems, [("jobs", name, "conclusion 'failure'")])
        needs = green_needs()
        needs["firmware"]["result"] = "failure"
        self.assert_fails(evaluate(needs, green_jobs()), "firmware", "result failure")

    def test_one_desktop_group_fails(self):
        name = "Desktop ThreadSanitizer"
        jobs = set_job(green_jobs(), name, conclusion="failure")
        self.assertEqual(evaluate(green_needs(), jobs), [("jobs", name, "conclusion 'failure'")])
        needs = green_needs()
        needs["desktop"]["result"] = "failure"
        self.assert_fails(evaluate(needs, green_jobs()), "desktop", "result failure")

    def test_invalid_input_fails(self):
        self.assert_fails(evaluate("not json", green_jobs()), "needs", "invalid JSON")
        self.assert_fails(evaluate("", green_jobs()), "needs", "invalid JSON")
        self.assert_fails(evaluate([], green_jobs()), "needs", "not a JSON object")
        needs = green_needs()
        needs["host"] = "success"
        self.assert_fails(evaluate(needs, green_jobs()), "host", "malformed entry")
        self.assert_fails(evaluate(green_needs(), None), "jobs", "invalid JSON")
        self.assert_fails(evaluate(green_needs(), "{"), "jobs", "invalid JSON")
        self.assert_fails(evaluate(green_needs(), {"message": "Not Found"}), "jobs", "not a jobs API response")
        jobs = green_jobs()
        jobs["jobs"].append("Summary")
        jobs["total_count"] = len(jobs["jobs"])
        self.assert_fails(evaluate(green_needs(), jobs), "jobs", "malformed job entry")

    def test_incomplete_input_fails(self):
        jobs = green_jobs()
        jobs["total_count"] += 1  # a second page that was not fetched
        self.assert_fails(evaluate(green_needs(), jobs), "jobs", "incomplete response")
        jobs = green_jobs()
        del jobs["total_count"]
        self.assert_fails(evaluate(green_needs(), jobs), "jobs", "incomplete response")
        self.assert_fails(evaluate({}, green_jobs()), "firmware", "no result")

    def test_duplicate_job_name_fails(self):
        name = "Host tests and validators"
        jobs = green_jobs([{"name": name, "status": "completed", "conclusion": "failure"}])
        self.assert_fails(evaluate(green_needs(), jobs), name, "2 jobs with this name")

    def test_unknown_need_fails(self):
        needs = green_needs()
        needs["new-job"] = {"result": "success"}
        self.assert_fails(evaluate(needs, green_jobs()), "new-job", "not required by the gate")


class CommandLineTests(unittest.TestCase):
    def run_gate(self, needs, jobs):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "jobs.json"
            if jobs is not None:
                path.write_text(json.dumps(jobs))
            return subprocess.run([sys.executable, str(ROOT / "tools/ci/gate.py"), "--needs", json.dumps(needs),
                                   "--jobs", str(path)], capture_output=True, text=True, timeout=30)

    def test_pass_exits_zero(self):
        result = self.run_gate(green_needs(), green_jobs())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("CI gate: PASS", result.stdout)

    def test_failure_exits_one_and_names_the_job(self):
        needs = green_needs()
        needs["firmware"]["result"] = "failure"
        result = self.run_gate(needs, green_jobs())
        self.assertEqual(result.returncode, 1)
        self.assertIn("[needs] firmware: result failure", result.stdout)

    def test_missing_jobs_file_exits_one(self):
        result = self.run_gate(green_needs(), None)
        self.assertEqual(result.returncode, 1)
        self.assertIn("cannot read", result.stdout)
        self.assertIn("[jobs] jobs: invalid JSON", result.stdout)


def workflow_jobs(text):
    """Top-level jobs of the workflow: {id: block text}. The workflow's own
    layout (two-space job ids under `jobs:`) is what actionlint checks."""
    body = text.split("\njobs:\n", 1)[1]
    parts = re.split(r"^  ([a-z0-9-]+):\n", body, flags=re.M)
    return dict(zip(parts[1::2], parts[2::2]))


def inline_list(block, key):
    match = re.search(rf"^\s+{key}: \[(.*)\]$", block, re.M)
    return [item.strip() for item in match.group(1).split(",")] if match else None


class WorkflowAgreementTests(unittest.TestCase):
    """The gate's lists must match the workflow."""

    @classmethod
    def setUpClass(cls):
        cls.jobs = workflow_jobs(WORKFLOW.read_text())

    def test_gate_needs_every_job_except_summary(self):
        gate_block = self.jobs["gate"]
        needs = inline_list(gate_block, "needs")
        self.assertEqual(sorted(needs), sorted(set(self.jobs) - {"gate", "summary"}))
        self.assertEqual(sorted(needs), sorted(gate.REQUIRED_NEEDS))

    def test_gate_runs_always_and_masks_nothing(self):
        gate_block = self.jobs["gate"]
        self.assertRegex(gate_block, re.compile(r"^    name: CI gate$", re.M))
        self.assertRegex(gate_block, re.compile(r"^    if: always\(\)$", re.M))
        self.assertNotIn("continue-on-error", WORKFLOW.read_text())
        self.assertNotIn("|| true", gate_block)
        self.assertIn("tools/ci/gate.py", gate_block)

    def test_summary_does_not_gate(self):
        # Summary is not required; the gate does not depend on it.
        self.assertNotIn("summary", inline_list(self.jobs["gate"], "needs"))

    def test_required_job_names_match_the_workflow(self):
        names = []
        for job, block in self.jobs.items():
            if job in ("gate", "summary"):
                continue
            name = re.search(r"^    name: (.*)$", block, re.M).group(1)
            if job == "firmware":
                boards = inline_list(block, "board")
                profiles = inline_list(block, "profile")
                self.assertEqual(name, "Firmware ${{ matrix.board }} ${{ matrix.profile }}")
                self.assertEqual(tuple(boards), gate.FIRMWARE_BOARDS)
                self.assertEqual(tuple(profiles), gate.FIRMWARE_PROFILES)
                names += [f"Firmware {board} {profile}" for board in boards for profile in profiles]
            elif job == "desktop":
                self.assertEqual(name, "${{ matrix.title }}")
                titles = re.findall(r"^            title: (.*)$", block, re.M)
                self.assertEqual(tuple(titles), gate.DESKTOP_TITLES)
                names += titles
            else:
                self.assertNotIn("${{", name, job)
                names.append(name)
        self.assertEqual(sorted(names), sorted(gate.REQUIRED_JOBS))
        self.assertEqual(len(set(gate.REQUIRED_JOBS)), len(gate.REQUIRED_JOBS))

    def test_every_idf_image_reference_has_one_pinned_digest(self):
        # M9: the firmware toolchain is pinned by image digest in the workflow,
        # the CI Containerfile and the documented local command.
        digests = set()
        for path in (WORKFLOW, ROOT / "tools" / "ci" / "Containerfile", ROOT / "docs" / "ci.md"):
            for reference in re.findall(r"espressif/idf:[^\s`\"']*", path.read_text()):
                pinned = re.fullmatch(r"espressif/idf:v6\.1@sha256:([0-9a-f]{64})", reference)
                if path.suffix == ".md" and pinned is None:
                    continue  # prose: "espressif/idf:v6.1 by digest", "@sha256:81893c71…"
                self.assertIsNotNone(pinned, f"{path.name}: {reference}")
                digests.add(pinned.group(1))
        self.assertEqual(len(digests), 1, digests)

    def test_firmware_build_fails_when_dependencies_lock_changes(self):
        # A cache miss lets the component manager resolve components; the
        # build must not silently rewrite the lock.
        block = self.jobs["firmware"]
        self.assertIn('cp dependencies.lock "$RUNNER_TEMP/dependencies.lock"', block)
        self.assertIn('cmp dependencies.lock "$RUNNER_TEMP/dependencies.lock"', block)


if __name__ == "__main__":
    unittest.main()
