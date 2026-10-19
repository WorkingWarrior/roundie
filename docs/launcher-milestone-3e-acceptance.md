# Milestone 3E acceptance — 2026-10-19

**Historical record:** the subsequent [3E.1 correction](launcher-3e1-performance.md)
adds persistent ABI 1 completion UX and investigates the delays below. It does
not replace the original 517828 us observation or resolve the historical 3D
LittleFS panic's exact cause.

The system launcher and navigation are implemented and the automated host,
target and bounded device execution gates passed. B / ota_1 runs 0.3.5
VALID/normal/STABLE; unchanged A / ota_0 0.3.4 remains VALID/fallback. Both
original installed packages remain selected and healthy. Final navigation is
LIST, with no loaded ELF, app view, app resources, queued app events or transfer.
**Physical finger and visual usability acceptance is PENDING.** All new device
selection, app-button and Exit inputs below are explicitly INJECTED.

Architecture: [launcher](app-launcher.md), [system navigation](system-navigation.md).
The [3D LittleFS panic and inferred stack cause](packages-milestone-3d-acceptance.md#first-candidate-preserved-physical-failure)
remain unchanged historical evidence. This milestone does not establish its
exact cause or repeat completed recovery/power gates.

## Architecture and ownership decision

The existing system worker discovers the authoritative AppRegistry and copies
at most eight metadata records into a volatile snapshot. Successful package
scans publish an internal cache revision. No persistent format changes, boot
scan prerequisite, filesystem watcher or additional task exists. The GUI owns
33 persistent launcher/chrome LVGL objects; ELF widget handles cannot address
them. The existing diagnostic screen remains allocated separately.

The row callback copies exact ID and snapshot revision into one bounded mailbox.
Duplicate/stale/disabled selection is refused. The worker invokes the unchanged
PackageManager launch-by-ID validation and existing ELFManager/executor. Only
the executor runs native entry/callbacks. The GUI never enumerates LittleFS,
hashes packages, validates or loads ELF. A failed registry refresh retains old
metadata and closes selection until controlled retry. Low-memory launcher
creation fails without affecting boot confirmation and permits an explicit
diagnostic refresh retry; it has no allocation retry loop.

WAITING/REFRESHING/LIST/REQUESTED/LOADING/ACTIVE/EXITING/ERROR describe navigation,
not a second execution state machine. Request serial, snapshot revision and
runtime generation are distinct. ABI 1 completes and returns automatically;
ABI 2 has a system status presentation; ABI 3 uses the existing native UI view.
System Exit occupies the top central 120 × 44 area, above the existing UI
example's first label. It submits UNLOAD rather than an application input event.
Stop/destroy, acknowledged GUI deletion, handle retirement, executable release
and input-buffer retirement precede reopened list admission. Failed teardown
retains ownership. Screen transitions reset touch state and wait for release.

Initial discovery waits for normal/STABLE and confirmed VALID firmware. Safe
Mode, boot confirmation and ROM recovery remain independent. Existing Flash
write exclusion, deferred recovery/restart and watchdog policy are retained.
Native applications remain trusted unisolated code. No autostart was added.

## Host and pinned target evidence

Final unittest discovery passed **74 tests in 12.754 seconds**. This includes
separately reported C scenario groups, rather than 74 hardware experiments.
Production launcher/coordinator plus real pinned LVGL run under ASan/UBSan;
its package/runtime seams are mocked. The existing production runtime/UI harness
also retains a real launcher across native-view creation, system Exit and the
GUI destroy barrier, using host C callbacks and a mocked loader. Neither host
harness executes Xtensa instructions. Production POSIX package transaction,
ELF preflight, ABI layout/import, resource, OTA, BootGuard, RTC and storage suites
remain included.

After device acceptance, a focused ASan/UBSan harness rerun passed in 4.236 s
with added checks for system Exit while a UI update is queued and request
admission during an active native callback. The latter verifies code remains
loaded until the callback returns and cooperative processing subsequently
retires it. This test-only addition (`b9b1bc1`)
changes neither the frozen firmware nor device state.

Launcher tests cover empty/one/multiple/eight-entry registries, duplicate and
invalid publication, incompatible/corrupt rows, failed refresh with retained
metadata, cache invalidation, selection during refresh, removed/stale selection,
double admission, launch error, input-buffer/UI/resource retirement gates,
headless ABI 2 presentation, suspended Exit, duplicate/stale Exit, unexpected
runtime generation, GUI task ownership, persistent system objects, and
low-memory creation refusal followed by explicit retry. Existing runtime/UI
tests retain callback-error, queued work, recovery and failed/delayed teardown
coverage. The combined harness verifies Exit is not an ELF button event.

The first unsourced host-suite invocation had two environment import errors
(`IDF_PATH`/partition-table module). Activating the pinned IDF environment fixed
the invocation; no test was disabled or tolerance relaxed.

REP target compilation passed with exact IDF v6.1 commit
`fff9895c82d744c7237be8847347bdd1b07c6643`, loader 1.3.5 and the pinned GCC
15.2.0 toolchain. No compiler warning/error was reported. Layout validation and
independent esptool image inspection passed. ABI 1/2/3 layouts, import allowlists,
RPK1/RPA1, memory protection, executable PSRAM mapping, existing LCD buffering,
RTC behavior, partition table and rollback policy remain unchanged. Frozen
sdkconfig JSON equals the accepted 3D configuration. The newly built bootloader
artifact was not deployed.

| Frozen firmware | Value |
|---|---|
| Source implementation commit | `510a2df` |
| Version / project | 0.3.5 / roundie_rep |
| Image length | 725360 bytes |
| Image SHA-256 | `b0d09b9ea9d2e5b48e2a1b74cfe589aebacd2b8a048473ae967bdda6efeb46f7` |
| Embedded/full ELF SHA-256 | `62e7cd192e75192da2a879d0f369438c0f7fcd0ef68b447b55f29469cbdac390` |
| OTA destination | inactive B / ota_1, 0x390000 |
| Preserved A ELF | `69c27122a2321a3f8fb41afdadf28e301f479646719c782e5386def38ce9200c` |

## Read-only preflight and genuine OTA

Preflight checked the unique expected ESP32-S3, 16 MiB Flash, 8 MiB PSRAM,
exact REP table, original installed rollback-enabled bootloader, CRC-valid A/B
VALID metadata, expected image headers, normal/STABLE running A, idle OTA,
unloaded runtime, healthy LittleFS, RTC OS=0, package identities and available
recovery documentation/retained backup. Device reads were limited to one 64 KiB
prefix and two 288-byte headers: **66112 bytes total**. There was no full-Flash
read, erase, formatting, partition migration, package write or security change.

The preflight reset returned normally to A. Its first 12-second observation
ended at BootGuard OPERATIONAL, before the ordinary stability interval. A later
read-only status confirmed STABLE; retained prefix/header evidence was checked
again locally without repeating ROM reads or reset. Immediately before writing,
live identity/health/package checks passed again.

One frozen-image RO1 upload wrote only inactive B. Upload plus activation took
**74.334 seconds of host elapsed time**, including USB stop-and-wait. READY
verification and ACTIVATE acknowledged B NEW; captured boot showed
PENDING_VERIFY and successful self-test/VALID. Live status confirmed B selected,
running VALID, OTA IDLE, normal/STABLE, with unchanged A VALID as fallback.
NVS initialized ESP_OK, LittleFS mounted without repair, RTC remained valid
OS=0, and RO1/RS1/RT1/RE1/RP1 diagnostics worked. No switch back to A occurred.

## Installed applications and native execution

| ID | Display name | Version / ABI | Preserved full-package SHA-256 |
|---|---|---|---|
| org.roundie.hello | Roundie Hello Update | 1.1.0 / 1 | `9a36506f21136344e557235aaa8caa532a52551dd9eb28936ffa3a8a6d054cd1` |
| org.roundie.ui | Roundie UI | 1.0.0 / 3 | `76ab4c5be328397900de02784f0d5f3f8660a56ba82e974bf5b4c8e17d5bfcd7` |

Exactly four native runtime generations were used: two Hello and two UI.
No package was rebuilt, replaced, removed or installed; the removed ABI 2
example was not reinstalled. Original ABI 2 ELF/toolchain and headless navigation
compatibility passed on host only; no new physical ABI 2 claim is made.

Both Hello selections traversed the launcher mailbox and committed package
validation, executed real Xtensa ELF, returned **42**, unloaded and restored
LIST with zero app resources. Both UI selections resolved the exact preserved
package, ran the existing PSRAM-backed native ELF, and realized six logical
widgets/seven LVGL objects. Each delivered one explicitly INJECTED button event
through the existing bridge/dispatcher to real ELF code. Native logs and GUI
command application were observed. System Exit completed stop/destroy/unload
and restored the launcher; the second Exit was requested while SUSPENDED.

Device negatives rejected pipelined duplicate selection, invalid/missing/stale
selection, selection/second PackageManager launch while loaded, refresh while
loaded, duplicate Exit, retired Exit generation and retired widget input. Stale
input produced no later native callback. Ordinary callback/validation and GUI
creation/teardown failures are host evidence; corrupted native instructions or
intentional watchdog/panic were not executed on hardware.

The first acceptance script checked a changed text hash immediately after
native event delivery. Its captured GUI state still had submitted=7, applied=6,
pending=1. That host assertion raced the asynchronous command completion.
The same loaded third generation was resumed, applied=submitted and pending=0
were confirmed, and Exit completed; no extra launch or firmware correction
occurred. It remained active during this tooling interruption (113 timer events
in the final record), so that cycle is not a short uninterrupted latency sample.
The next cycle waited for GUI application before checking text change.
A final host dictionary-construction error affected reporting only; a separate
read-only final snapshot completed it without repeating any application cycle.

## Bounded timing samples

All values are on-device microseconds unless explicitly host seconds. These
individual observations and maxima are not worst-case or percentile guarantees.

| Measurement | Observations |
|---|---|
| Initial / explicit registry discovery | 75234 / 75228 us |
| Worker metadata copy/update, two records | 54–57 us in cycle samples |
| One-time launcher GUI construction | 46676 us |
| INJECTED selection admission function maximum | 23 us; excludes physical acquisition |
| Selection to worker admission | Hello 9380 / 9135; UI 9174 / 9165 us |
| Installed package validation/submission, last UI | 20351 / 96480 us; submission includes scan |
| ELF relocation/load | Hello 9492 / 9481; UI 9936 / 9838 us |
| ABI 1 native execution | 15953 / 16078 us |
| ABI 1 request to clean list admission | 139351 / 139251 us |
| ABI 3 request to observed runtime RUNNING/UI SHOW readiness | 499262 / 649253 us |
| ABI 3 init / start callback | 6393 / 1251; 6916 / 1184 us |
| Exit to worker admission | 9751 / 9461 us |
| Stop callback/shutdown / executable unload | 1911 / 1463; 1620 / 1401 us |
| GUI deletion barrier duration | 1613 / 1191 us |
| Exit to clean list admission | 39387 / 319300 us; latter starts suspended |
| Native UI command enqueue-to-apply observed maxima | 367526 / 517828 us |
| Injected button enqueue-to-native callback | 4342 / 4351 us |
| Navigation publication-to-GUI observed maximum | 343248 us in final diagnostics |
| Host Hello selection/completion sampling | 0.19177 / 0.19195 seconds |
| Host second UI selection/foreground sampling | 0.70195 seconds |
| Host Exit/return sampling | 0.09171 / 0.39277 seconds |

LCD completed-transfer diagnostics progressed from 348 before explicit discovery
to 607 at final observation, with zero transfer errors. The sampled counter
was **348 before and after** the explicit 75 ms discovery; `discovery_flushes=0`.
This is not proof of concurrent display progress or a stall-free GUI during
enumeration: that diagnostic is published once per GUI iteration, not directly
from the DMA interrupt. Source and host task assertions establish worker/GUI
separation; visible responsiveness during discovery remains part of PENDING
human acceptance. No display benchmark framework was introduced.

The measured approximately 0.5–0.65 s UI foreground readiness and up to 0.518 s
UI command latency are explicit limitations of this sample, not advertised
instantaneous responsiveness. Runtime SHOW acknowledgment, LVGL application,
LCD DMA completion and human-visible pixels are different evidence tiers.

## Stack, SRAM/PSRAM and LVGL accounting

The worker retains its **12288-byte stack**. Target compiler stack-usage output
reports worker 1680 B, launcher poll 720 B, snapshot publication 192 B,
RP1 command 1456 B, navigation diagnostic 256 B, package launch 576 B,
registry scan 480 B and inspection 1088 B. These are individual compiled frames,
not a proven complete maximum call chain. No new task or arbitrary stack
increase was introduced. The minimum observed remaining worker stack was
**5072 bytes** across this bounded device test (initial sample 5568 B).

Launcher archive symbols total **4131 writable bytes before alignment**,
including its 1296-byte shared status, 1120-byte discovery array, 232-byte worker
record and 1296-byte GUI copy. Development navigation diagnostics add a static
1296-byte copied status plus 1400-byte reply scratch. Complete ELF/package bodies
are not retained in launcher storage. LVGL uses the unchanged shared 64 KiB pool.

Live RS1 internal free was **241107 B before first executor use** and **228615 B
after all four generations**. The 12492-byte decrease includes the persistent
12 KiB executor stack/task/queue, a system resource created on first use. Each
executor-bracketed native run measured internal free **228595 B before/after**;
later UI input/recovery samples and final live free were consistently 228615 B.
These different snapshots have distinct call-frame timing and must not be
treated as a 20-byte app leak.

| Native run | PSRAM before / relocated / after (bytes) | Final acquired / released / reclaimed / outstanding |
|---|---|---|
| Hello 1 | 8382780 / 8382392 / 8382780 | 4 / 4 / 0 / 0 |
| Hello 2 | 8382780 / 8382392 / 8382780 | 4 / 4 / 0 / 0 |
| UI 1, interrupted observation | 8380764 / 8378612 / 8380764 | 246 / 245 / 1 / 0 |
| UI 2 | 8380764 / 8378612 / 8380764 | 20 / 19 / 1 / 0 |

Executor memory snapshots start after owning the input ELF and end before its
separate buffer retirement; completed busy=0 additionally verifies retirement.
They are not a current global free-PSRAM query after all system work. Each UI
retirement balanced **14 acquired / 14 released / 0 outstanding** bridge resources
and left zero app LVGL objects/retained bytes/queued commands. The final system
launcher still owns **33 LVGL objects**, pool free **43912 B**; the immediately
preceding UI teardown sample reported 43892 B, showing why allocator caches and
frame timing are not equivalent to application handle ownership. Total system
LVGL objects are deliberately not claimed to be zero.

## Final state, recovery scope and pending human gate

Final independent read-only replies confirmed B 0.3.5 selected/running VALID,
A 0.3.4 VALID/compatible fallback, normal/STABLE, OTA IDLE, no fault injection,
no native owner/input transfer, no app events/timers/subscriptions/resources,
no app view, launcher foreground with two entries, and no pending launch/Exit.
LittleFS remained **57344 / 9240576 B**, NVS boot initialization ESP_OK,
RTC valid/OS=0, packages/digests unchanged. No panic/assertion/WDT/brownout was
observed in the new boot and acceptance captures. Existing USB diagnostics work.

Flash exclusion, deferred restart/recovery, Safe Mode independence and fault
cleanup passed their preserved host suites. Current device policy/loaded-owner
and second-launch rejection were observed. No new package/OTA write was attempted
while loaded, no new forced Safe Mode, rollback or ROM-recovery experiment was
performed, and no whole-Flash read or power/endurance campaign was repeated.
Previously accepted recovery evidence retains its original scope.

**PENDING experiment:** human finger/visual launcher acceptance. Method: inspect
both names/versions for clipping; tap Hello and observe return; tap UI, press
TAP ME, then the separate top Exit; try a double tap while loading; confirm a
readable restored launcher. Acceptance: intended rows activate once, app counter
responds, Exit remains accessible, return is usable, with actual TOUCH counters
distinguished from INJECTED. No human/camera/actuator observation was available
for 3E. The earlier 3C USER-VERIFIED input gate does not establish this new UI.

## Reproduction and evidence

Raw logs, private device identity, bounded readbacks, frozen binaries and the
one-shot acceptance procedure are outside Git at
`/mnt/Dane/Development/roundie-backups/launcher-3e-2026-10-19/`.
Key records are `candidate.json`, `stack-usage.json`, `preflight-passed.json`,
`deploy-new.json`, `b-first-boot.log`, `b-confirmed.json`, `discovery.json`,
`hello-0/1.json`, `ui-0/1.json`, `asynchronous-gui-observation.json`,
`acceptance-results.json`, `final-device-state.json` and corresponding traffic.
The two acceptance-tool interruptions are preserved, not overwritten.

```sh
source /home/solve/.espressif/v6.1/esp-idf/export.sh
python -m unittest discover -s tests -p 'test_*.py' -v
idf.py -B build-rep -DROUNDIE_PROFILE=rep -DPROJECT_VER=0.3.5 build
python tools/validate_storage_layout.py --build build-rep
python tools/roundie_launcher.py --port "$PORT" status
```

Implementation commit: `510a2df`. Additional Exit coverage and acceptance
documentation are separate commits. No generated firmware/ELF/RPK, backup, raw
device log, private device
identifier or secret is committed. No push was performed. Work stops at 3E;
MTP, package UI, autostart and application multitasking remain outside scope.
