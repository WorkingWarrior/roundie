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

**Timing.** *none*: no clock is involved. *virtual*: the desktop's
`--virtual-time` runs the same commands to the same states and outputs
(docs/virtual-hardware.md, "What is deterministic"). *real*: real time, the
SDL audio device's pace or a compositor; these tests can vary with machine
load. The reports carry the timing of every test, and the real-time tests are
in their own groups. A timeout was not raised for CI: if a real-time test
turns out to be unstable on the runners, that is a finding to investigate.

`tools/ci/run_tests.py --check-coverage` (in the host job) fails when a test
is in no group or in two primary groups, so a new test cannot drop out of CI.

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
That the device exists on GitHub's `ubuntu-24.04` runners is an assumption
until the first run on GitHub.

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
