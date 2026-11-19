# Continuous integration (M7)

`.github/workflows/ci.yml` runs on every pull request, on pushes to `main` and
on demand. It builds the four firmware configurations and runs the existing
host tests (`tests/`) in named groups. The tests and build commands are the
ones a developer runs locally. `tools/ci/` only selects them, runs them in
pinned containers and writes reports.

## Matrix

| Job | Environment | What it runs | Timing |
|---|---|---|---|
| Firmware 1.85B legacy, 1.85B rep, 1.75C legacy, 1.75C rep | `espressif/idf:v6.1` by digest | `tools/ci/build_firmware.sh BOARD PROFILE`: `idf.py build`, `idf.py size`; fails on any warning | build |
| Managed components | IDF image | `idf.py reconfigure`, checked against `dependencies.lock`; cached for the desktop jobs | build |
| CI image | Docker on `ubuntu-24.04` | `tools/ci/Containerfile`, cached by its own hash | build |
| Host tests and validators | CI image | group `host`: C harnesses (many under ASan/UBSan), ELF and package validators, OTA, clock, storage layout, REP deployment guards and desktop contract tests | none |
| roundie-manager | CI image | `npm ci --ignore-scripts`, `npm run typecheck`, `npm test` | none |
| Desktop build | CI image | `tools/ci/build_desktop.sh`: `meson setup`, `ninja`; fails on any warning | build |
| REP x86-64 native applications | CI image | group `native`: `test_native_elf` (validator under ASan/UBSan, malformed ELFs, packages) and the desktop's native-application tests | mixed, per test |
| Desktop ASan/UBSan, virtual time | CI image | group `desktop-virtual-time`: application lifecycle, UI bridge, round widgets, stopwatch, watch RTC, sound, storage across restarts; each run twice and compared where the test does so | virtual |
| Desktop ASan/UBSan, real time | CI image | group `desktop-real-time`: launcher flow on both panels, touch mapping, power menu, wall clock and RTC across resets, SDL audio playback, hung application | real |
| Virtual Hardware Lab | CI image | group `lab`: the `vhw` model under TSan and the lab scenarios (battery, screen, RTC, peripheral faults, storage I/O faults per call, restarts and power loss, audio faults, lab-state isolation) | virtual |
| Graphics | CI image | group `graphics`: gamescope headless (window, drag, pointer outside the panel, lab panel window) and nested KWin at scale 2 (HiDPI), all on Mesa lavapipe | real |
| Desktop ThreadSanitizer | CI image | group `tsan`: the threaded scenarios again, with the desktop built with `-Db_sanitize=thread` | mixed |
| Workflow syntax | actionlint image by digest | actionlint with shellcheck | none |
| Summary | runner | `tools/ci/summarize.py`: one table of every group, build and job | none |
| CI gate | runner | `tools/ci/gate.py`: fails unless every job above except Summary succeeded, each matrix variant included ([CI gate](#ci-gate)) | none |

**Timing.** *none*: no clock is involved. *virtual*: the desktop's
`--virtual-time` runs the same commands to the same states and outputs
(docs/virtual-hardware.md, "What is deterministic"). *real*: real time, the
SDL audio device's pace or a compositor; these tests can vary with machine
load. The reports carry the timing of every test, and the real-time tests are
in their own groups. A timeout was not raised for CI: if a real-time test
turns out to be unstable on the runners, that is a finding to investigate.

`tools/ci/run_tests.py --check-coverage` (in the host job) fails when a test
is in no group or in two primary groups, so a new test cannot drop out of CI.

## CI gate

`CI gate` is the only check that branch protection requires. Summary is not
a gate: it runs after failures so that it can report them, and it always
succeeds.

**When it runs.** The gate runs with `if: always()`, so it reports even when
an earlier job failed, was skipped or was cancelled.

**What it needs.** Its `needs` list holds every other job, including the
preparatory ones:

- `workflow-lint`
- `firmware` (the 4 variants)
- `components`
- `ci-image`
- `host`
- `manager`
- `desktop-build`
- `desktop` (the 6 groups)

**What it checks.** `tools/ci/gate.py` checks two sources and passes only
when both show `success` for everything:

1. The `needs` context: one result per job id. GitHub does not document how
   it combines a matrix job's variants into that one result.
2. This run attempt's jobs, from the Actions API (`actions: read`): one
   entry per matrix variant, 16 named jobs.

**What fails it.** Each of these is a failure, and the log names the job
and the reason:

- `failure`, `cancelled` or `skipped`;
- a job that has not completed;
- a missing job or result;
- a duplicate job name;
- an incomplete API response;
- malformed input.

A failed preparatory job (CI image, managed components) skips the jobs after
it. The gate reports those skipped jobs as failures.

**How it stays in step with the workflow.** The required lists are in
`gate.py`. `tests/test_ci_gate.py` (host group) checks them against
`ci.yml`: the gate's `needs`, every job and matrix name, `if: always()`, and
that no `continue-on-error` is used. A job added to the workflow without the
gate fails the host tests. The same file tests the aggregation: all green,
then a single failure, cancellation, skip or missing result, a failed
preparatory job, one firmware variant, one desktop group, and invalid or
incomplete input. Run it locally:

```bash
python3 -m unittest discover -s tests -p test_ci_gate.py
```

**Limits:**

- The gate trusts the workflow and `gate.py` of the commit under test. A pull
  request that edits them can change what the gate checks, so changes under
  `.github/` and `tools/ci/` need review before merging.
- After **Re-run failed jobs**, the gate checks the jobs of the new attempt.
  Verified on 2026-11-19 (run 44557496786, attempt 2, after an Actions cache
  egress-limit failure in Desktop ThreadSanitizer): the listing for the new
  attempt included the unchanged jobs as success, and the gate passed with
  all 16 jobs. If they are ever missing, the gate fails; use **Re-run all
  jobs**.

## Branch protection

**Active since 2026-11-19:** ruleset `main`, id 27651397, enforcement
Active, applied through the API after pull request #2 had run `CI gate` on
GitHub. The repository is public and owned by a user account with a single
collaborator.

**Rules**, one ruleset on the default branch:

| Rule | Setting |
|---|---|
| Restrict deletions | on |
| Block force pushes | on |
| Require a pull request before merging | on, **0** required approvals |
| Require status checks to pass | `CI gate`, source GitHub Actions (app id 15368); branches need not be up to date |
| Bypass list | empty |

**Effects:**

- Nobody pushes directly to `main`: a pull request is required, and with an
  empty bypass list that holds for the owner too.
- The owner merges their own pull request once `CI gate` is green. No
  approval is needed, so the rule that an author cannot approve their own
  pull request does not block them.
- Pull requests from others still need someone with write access to press
  merge. Only the owner has it.
- Pinning the check to GitHub Actions stops a commit status with the same
  name, posted by another app or token, from satisfying the rule.

**Owner bypass.** Adding *Repository admin* to the bypass list (always, or
for pull requests only) would let an admin merge with a red or running gate,
or push directly. The policy does not need that, so the list stays empty.
An admin can still edit or disable the ruleset in an emergency. That change
is explicit and appears in the ruleset history.

**The applied payload.** To recreate the ruleset, save the following as
`ruleset.json`:

```json
{
  "name": "main",
  "target": "branch",
  "enforcement": "active",
  "conditions": {"ref_name": {"include": ["~DEFAULT_BRANCH"], "exclude": []}},
  "bypass_actors": [],
  "rules": [
    {"type": "deletion"},
    {"type": "non_fast_forward"},
    {"type": "pull_request", "parameters": {
      "required_approving_review_count": 0,
      "dismiss_stale_reviews_on_push": false,
      "require_code_owner_review": false,
      "require_last_push_approval": false,
      "required_review_thread_resolution": false}},
    {"type": "required_status_checks", "parameters": {
      "strict_required_status_checks_policy": false,
      "do_not_enforce_on_create": false,
      "required_status_checks": [{"context": "CI gate", "integration_id": 15368}]}}
  ]
}
```

Then post it:

```bash
gh api --method POST repos/WorkingWarrior/roundie/rulesets --input ruleset.json
```

**Apply in the UI.** Use the same settings: Settings → Rules → Rulesets →
New ruleset → New branch ruleset, Target branches → Add target → Include
default branch, then add `CI gate` with source GitHub Actions under the
status checks rule.

**Recreate it only after `CI gate` has run on GitHub.** Otherwise pull
requests wait for a check that never reports.

GitHub added one parameter to the stored `pull_request` rule:
`require_extra_approval_for_unattributed_changes: true`. GitHub documents it
as applying to Copilot pull requests not attributed to a person, and as
having no effect when the ruleset requires zero approvals.

**Verification, 2026-11-19:**

- Verified through the API: `gh api repos/WorkingWarrior/roundie/rules/branches/main`
  lists `deletion`, `non_fast_forward`, `pull_request` (0 approvals) and
  `required_status_checks` (`CI gate`, integration 15368, not strict), all
  from ruleset 27651397. The ruleset's bypass list is empty
  (`current_user_can_bypass: never`). `branches/main` reports
  `protected: true`; another branch (`ci-gate`) has no rules.
- PENDING: a pull request merged under the ruleset. On it, `CI gate` should
  be marked *Required*, the merge should be blocked while the gate runs and
  allowed once it is green.
- PENDING: a pull request whose gate failed cannot be merged. Making the gate
  fail on purpose, for example with a test branch that breaks one test, is
  optional.
- Not attempted: a direct push of a new commit to `main`
  (`git push origin HEAD:main`) should be rejected with a rule violation.
  This is a real push attempt, so do it only when the owner approves it.

## Reports

- Each job's summary page: PASS, FAIL or SKIP per test with its timing, and the
  failure tracebacks and skip reasons. The Summary job collects them in one
  table, followed by the hardware checks below.
- Artifacts `reports-GROUP`: JSON, JUnit XML, Markdown and the full log.
- Artifacts `firmware-BOARD-PROFILE`: the build log, the warning count, the
  `idf.py size` output, the images and ELF and map files.
- After a failure, artifact `work-GROUP`: the desktop tests' work directories
  (the desktop build without object files, data directories, scripts,
  screenshots, application logs and sanitizer output).

`--strict` (always used in CI) fails a group on any skip, except those listed
in `ALLOWED_SKIPS` with their reason. A skip is never counted as a pass. The
only allowed skip is `test_legacy_guard_rejects_physical_rep_table`: it reads
a flash dump of the developer's board, which must not be in CI. `--strict`
also fails a group when the Xtensa toolchain is missing, since two tests
would otherwise drop their Xtensa parts without skipping.

## Environments

**Firmware**: `docker.io/espressif/idf:v6.1@sha256:81893c71…` (ESP-IDF commit
`fff9895c82d7`, Xtensa GCC esp-15.2.0_20251204). This is the same IDF and
toolchain as the local installation.

**CI image** (`tools/ci/Containerfile`): Arch Linux by digest, with its
packages from the Arch Linux Archive snapshot of 2026-11-15. Ubuntu runners
lack SDL3 and Mbed TLS 4 (TF-PSA-Crypto), which the desktop needs. The image
contains:

- GCC with the sanitizer runtimes, Meson, Ninja, SDL3 and the Vulkan loader;
- Mesa lavapipe (`vulkan-swrast`): Vulkan on the CPU, with no GPU or monitor;
- gamescope, Xwayland, xdotool, KWin and D-Bus for the graphics tests;
- `plasma-wayland-protocols`, which provides `fake-input.xml` (LGPL) for the
  HiDPI test. The package is installed, not copied into the repository. With
  it, the HiDPI test runs instead of skipping: it had only ever skipped on the
  development machine, and its first run here found and fixed a window
  placement fault in the test (docs/roundie-core.md);
- `kwin_wayland` without its file capability `cap_sys_nice` (`setcap -r` in
  the Containerfile): with `no-new-privileges` the kernel refuses to execute
  a file with capabilities. KWin then runs without real-time scheduling,
  which a headless test does not need;
- gdb, Node.js and npm;
- the Xtensa toolchain and the four ESP-IDF directories the host tests read,
  copied from the pinned IDF image. This is not a working IDF.

**Isolation**: `tools/ci/container.sh` runs every test command:

- as the calling user, never root;
- with `--network=none`, except `npm ci`;
- with `no-new-privileges`, and with no host socket or secret.

The native REP test applications run there. They are untrusted code, unisolated
from the desktop process (docs/native-apps.md). Graphics tests start their own
headless gamescope or a nested KWin with a private D-Bus session. They never
use a host display.

**Security**:

- `permissions: contents: read`, with `persist-credentials: false` on checkout;
- `pull_request`, never `pull_request_target`;
- every action is pinned by commit SHA, and both images by digest.

**Caches**: the CI image is cached under the hash of `tools/ci/Containerfile`,
which holds every pin. The managed components are cached under the hash of
`dependencies.lock` and the `idf_component.yml` files. A changed input gives a
new key. A job that finds no entry fails; it never falls back to another
entry. Builds and test results are never cached: every run builds the
firmware and the desktop from its own commit.

**`/dev/udmabuf`**: gamescope and KWin share buffers as dma-bufs. On Mesa
lavapipe there is no GPU to export them from, so Mesa uses the kernel's
`udmabuf` device (memory-backed, no GPU). Without it gamescope aborts and the
gamescope tests fail. `container.sh` passes the device in when the caller can
read and write it, and the graphics job makes it accessible on the runner.
On GitHub's `ubuntu-24.04` runners the device exists and the graphics group
passed, including the KWin HiDPI test (run 43879239058, 2026-11-16).

## Reproducing a job locally

Podman or Docker; the commands run from the repository root.

```bash
podman build -f tools/ci/Containerfile -t roundie-ci tools/ci
```

```bash
podman run --rm --network=none -v "$PWD:/work" -w /work docker.io/espressif/idf:v6.1@sha256:81893c71bb5e570088901f21def8684c25cd2a9020281bd01b843a7655edb18c tools/ci/build_firmware.sh waveshare_s3_touch_amoled_1_75c rep
```

With an exported IDF v6.1 environment, `tools/ci/build_firmware.sh BOARD
PROFILE` runs without a container. A desktop job needs `managed_components`
(any firmware build fetches it). The host job also needs the 1.85B builds in
`build-rep` and `build`, with `sdkconfig.rep` and `sdkconfig` (the README's
`idf.py` commands).

```bash
tools/ci/container.sh tools/ci/build_desktop.sh
```

```bash
tools/ci/container.sh python3 tools/ci/run_tests.py --strict lab
```

`tools/ci/run_tests.py --list` shows the groups and their tests. Without a
container, `python3 tools/ci/run_tests.py GROUP` runs a group on the host
tools: without `--strict`, missing tools are reported as skips. Reports go to
`ci-reports/`, and the desktop tests' work directories to `ci-reports/work/`.
To run one test as before, use
`python3 -m unittest discover -s tests -p test_desktop.py -k NAME`.

## Hardware checks outside CI

CI runs no hardware, so nothing below is verified by a green run. These remain
PENDING until they are checked on the boards:

- Flashing, boot and OTA rollback on the 1.85B and 1.75C boards, and
  recovery through ROM Download, also after TinyUSB has taken over USB
  (docs/storage.md, docs/usb-mtp.md).
- Power: battery curve and gauge or PMIC readings, the PWR latch, wake
  sources, light sleep and loss of power. The Virtual Hardware Lab models
  these as assumptions (docs/virtual-hardware.md).
- Display, touch and audio on the real panels and codec: finger and visual
  acceptance, touch accuracy at the round edge, timing of the panel's
  refreshes, and sound (docs/system-navigation.md, docs/stopwatch.md).
- Flash and PSRAM: concurrency under load, write latency and LittleFS integrity
  after power is interrupted (docs/storage.md).
- USB storage mode and MTP with real hosts (docs/usb-storage-mode-plan.md).

## Limits

- GitHub Actions results exist only after the workflow has run on GitHub;
  local runs of the same containers are evidence for the scripts, not for the
  runners.
- The first run builds the CI image from the Arch Linux Archive, which can be
  slow. Later runs load it from the cache, until the Containerfile changes or
  the cache entry expires after seven days without use.
- Real-time groups can be affected by runner load. Their results are kept
  apart from the deterministic groups.
- `test:ui` of roundie-manager (Playwright with browsers) is not in CI.
