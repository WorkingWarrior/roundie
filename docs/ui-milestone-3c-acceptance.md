# Milestone 3C acceptance — 2026-10-19

Milestone 3C's autonomous source, host, target and bounded physical-device gates
are complete. **Finger interaction and human visual correctness are PENDING.**
The deployed device ends normal/STABLE with no active ELF/application UI and
two VALID firmware slots. No Milestone 3D, launcher, packages, registry or MTP
work was started.

## Architecture decision and limits

[app-ui.md](app-ui.md) records the ownership/concurrency review and API contract;
[input-bridge.md](input-bridge.md) records input origins and manual acceptance.
One retained system-owned view supports labels/buttons through a copied FIFO.
Only the existing GUI task manipulates LVGL. Only `rep_executor` calls native
callbacks. No LVGL/display/touch pointers cross the SDK. ABI 3 is explicit;
original ABI 1/2 public layouts remain unchanged. Imports remain an exact
version-matched allowlist, with existing relocation/alignment limits intact.

Limits: 1 view, 8 logical widgets including root, 95 UTF-8 text bytes plus NUL,
12 copied commands of 160 bytes, at most 4 commands per GUI iteration, 44 × 44
minimum buttons, geometry bounded within 360 × 360. Label/button root layout
uses the center of the round screen. The existing shared 64 KiB LVGL pool is
the absolute dynamic envelope; creation admission has a 16 KiB incremental
ceiling, 12 KiB pre-create threshold and 8 KiB pool reserve. Fixed command,
slot and GUI-text arrays do not grow with updates. This is not a separate
allocator or a claim that all transient renderer allocations fit in 16 KiB.
The built Xtensa UI component has **3118 bytes of writable static symbols**
plus linker alignment (below a 4 KiB budget): FIFO 1920, logical slots 96,
GUI records 896 and fixed state 206 bytes. Thus its backing envelope is the
existing shared 64 KiB LVGL pool plus below 4 KiB static UI state. Existing
native ELF/SDK memory, event FIFO and display DMA buffers remain separately
budgeted, rather than being hidden in an incremental widget allocation figure.

Stop closes new work, cancels events/timers and finishes native stop; GUI deletes
the view and acknowledges the reserved control mailbox before STOPPED/unload.
Destroy runs once and resources/code retire afterward. A one-second GUI timeout
retains loaded ownership and Flash exclusion and reports failure; no forced
free/unload exists. This failure path was tested on host, not induced on hardware.

## Verified host and target evidence

Pinned ESP-IDF v6.1 commit `fff9895c82d744c7237be8847347bdd1b07c6643`, Xtensa GCC
15.2.0 (`esp-15.2.0_20251204`), loader 1.3.5 and LVGL 9.5.0 were retained. No
managed component/IDF installation was modified. Host tests use real pinned
LVGL and production manager/bridge/backend/example C, with mocked IDF, loader,
addresses and time. They do not execute Xtensa instructions.

```sh
. /home/solve/.espressif/v6.1/esp-idf/export.sh
python -m unittest discover -s tests
idf.py -B build-rep -DROUNDIE_PROFILE=rep -DPROJECT_VER=0.3.3 build
python tools/validate_storage_layout.py --build build-rep
cmake -S examples/ui_elf -B examples/ui_elf/build
cmake --build examples/ui_elf/build
python tools/roundie_elf.py inspect examples/ui_elf/build/ui.app.elf
```

All commands passed (exit 0). **69 unittest cases passed in 10.585 s**, including
the production UI/runtime/LVGL C harness under ASan/UBSan. Cases cover copied
payloads, UTF-8, invalid geometry/type/parent/owner, stale/duplicate handles,
widget/FIFO limits, FIFO order, GUI task ownership, event saturation, no recursive
callbacks, suspend/resume, failed/partial initialization, GUI creation refusal,
recovery during an active callback and delayed teardown. Old relocation,
rounded-text/alignment, staging, Flash policy and boot/OTA tests remain intact.
Unknown imports/ABI and libc/libgcc/TLS probes remain refused. Public headers
pass pinned Xtensa C11/C23 layout checks, including unchanged v1/v2 layouts.

Target build and REP layout checks passed without suppressing global warnings.
Firmware uses 710672 of 3670016 slot bytes, leaving 2959344 bytes. Memory
protection, rollback and executable PSRAM instruction alias remain enabled;
broad symbol tables, XIP-from-PSRAM, shared objects, recovery fault build and
unconfirmed-reset test are disabled. No partition/bootloader/security change.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| Firmware 0.3.3 binary | 710672 | `13e21e63c87c61a91c1c6862ad1bf408601b781252f86efdde390e1aad49a3ca` |
| Firmware embedded ELF identity | — | `5c0d5eb2ed9b4fe4918620f2833298cd9c2a3274bfbb229cff96ae90db270142` |
| Independent UI ELF, ABI 3 | 3668 | `340c9146c8d354ea88e367cce4c6831ccee99d54bb0125a18fe537545215cb64` |
| Preserved event ELF, ABI 2 | 2116 | `8aaee0a856ad590a34ca5931404c8c62cd7b6fc499135b9a1bd397f9cbc9aa6b` |
| Preserved original hello ELF, ABI 1 | 1692 | `5edbfbf49c72890dd80f33263d2e52ffeeba9bfb0c5ef577047428a7360f0c88` |

UI ELF: ET_DYN Xtensa ELF32; 884 code bytes, 1221-byte worst-case data allocation,
31 supported relocations and exactly one import, `rep_abi_v3`. It contains its
own counter/event logic. Firmware does not implement the example's counter.
The uptime label counts delivered one-second ticks and pauses during suspend.

## Verified physical preflight and OTA

Read-only preflight confirmed unique USB `303a:1001`, serial/MAC
`28:84:85:87:98:7C`, ESP32-S3, 16 MiB Flash, security state, exact `rep-v1` table,
the installed rollback bootloader and CRC-valid VALID A/B metadata. Reads were
limited to a 65536-byte prefix and two 288-byte image headers (66112 bytes).
The existing factory backup and previously verified ROM recovery instructions
were present. No full-Flash readback, format, erase, repartition or eFuse work.
After the read-only ROM visit the board returned to A normal/STABLE; its boot
reported 8 MiB PSRAM, mounted LittleFS and NVS ESP_OK. RTC GET was valid, OS=0.

Initial A 0.3.2 identity:
`32e888585278c7590ffc47f50a9bdf0d153de531df5244ddf9ec74441174dbde`.
Initial B 0.3.1 identity:
`64e17722a3d8665399ca049eb1277e423e6bd6c3775b3057c008b0f6510a4298`.

Genuine RO1 streamed the frozen image into **inactive B**, beginning at
`0x390000`, preserving running A. END verified the binary SHA-256 and embedded
ELF identity; ACTIVATE selected B NEW. Acknowledged RESTART produced B
PENDING_VERIFY at boot-log 1171 ms, then VALID after the critical self-test
at 3301 ms. BootGuard reached STABLE. Host upload/activation/restart-ACK duration
was **72.749 s**, which is transport time, not GUI latency.

Boot logs confirmed 16777216-byte Flash, 8388608-byte PSRAM, NVS ESP_OK, exact
REP layout and LittleFS mounted/capacity-valid. A remained VALID and compatible.
No A image bytes, bootloader, table, NVS user data or unrelated filesystem paths
were written. The existing development staging file and ordinary BootGuard
persistence were used as authorized. No unexpected panic/WDT/brownout occurred.

## Verified physical native UI and input path

Three complete UI generations (1, 2, 3) each loaded once, initialized/started,
created six logical widgets and **seven real LVGL objects**, delivered three
timer events and four INJECTED button events, suspended/resumed and stopped/
destroyed/unloaded. One stopped from suspension. No reload occurred between
callbacks within a generation. Logs show native execution using the same
reused allocation addresses across generations:

```text
text_write=0x3c0a1ea8 text_execute=0x420a21d8 data=0x3c0a222c
code=884 data_allocation=1221 relocations=31
```

Each injected event used the exact active generation and realized button. It
entered the ordinary FIFO and reached ELF `on_event`, incremented ELF state,
submitted a copied label update and produced GUI-applied `Counter: 1`, `2`, `3`
and `4` text hashes. The timer updated the uptime label independently. Captured
native logs identify INJECTED origin; device TOUCH counts stayed **zero**.
This proves bidirectional native semantic routing on ESP32-S3, not finger touch.

LCD completed-DMA counters progressed from creation → timer updates → teardown:
cycle 1 **906 → 933 → 977**, cycle 2 **996 → 1023 → 1084**, cycle 3
**1085 → 1112 → 1152**, with **zero transfer errors**. These are transfer
completion observations, not photographs or proof of visual correctness.

Suspension hid the view, closed input admission and kept callbacks unchanged
for a 300 ms observation window. Resume restored the same retained view and
accepted another native button event. Invalid/stale IDs/generations, suspended,
stopped and unloaded input were rejected. The ELF's init self-check refused
invalid pointers/ID/owner; its stop callback verified update/destroy admission
was closed. No event callback followed unload.

For every running, suspended and stopped UI generation, both unsafe RO1 BEGIN
and RE1 staging BEGIN were rejected with 259 before writing; OTA remained IDLE
and no transfer session began. Native code was never deliberately corrupted,
panicked or subjected to a physical FIFO/endurance stress test.

The exact preserved ABI 1 binary subsequently returned **42**, recovered heap
exactly and ended with resources `[4,4,0,0]`. The preserved ABI 2 binary delivered
two timers and user value **42**, then unloaded with resources `[9,8,1,0]`.
These were actual unchanged Xtensa binaries, not rebuilt compatibility substitutes.

## Measured timings and memory

Values below are per-cycle observed maxima, in **microseconds** from the device's
monotonic `esp_timer_get_time`. Timestamp representation has 1 us granularity;
absolute measurement accuracy was not calibrated. FreeRTOS tick is 10 ms and
the configured LVGL refresh period is 33 ms. No P95/P99/worst-case guarantee is
claimed from three short cycles.

| Metric | Cycle 1 | Cycle 2 | Cycle 3 |
|---|---:|---:|---:|
| Bridge enqueue validation/copy/lock maximum | 60 | 60 | 57 |
| Enqueue → GUI apply maximum | 310338 | 310322 | 309532 |
| Single widget creation maximum | 3232 | 3217 | 3219 |
| Text/property application maximum | 1078 | 1061 | 1067 |
| INJECTED event enqueue → native callback maximum | 1308 | 896 | 2889 |
| Native callback maximum | 7270 | 6797 | 7343 |
| GUI deletion operation | 1656 | 1293 | 1672 |
| Maximum command FIFO depth | 6 | 6 | 6 |
| Incremental retained LVGL allocation peak, bytes | 1844 | 1776 | 1804 |

The approximately **310 ms** apply delay is material. In cycle 1 the creation
queue maximum was 26669 us, the first counter update increased it to 289128 us,
and the post-resume update increased it to 310338 us. The association with
screen transitions/initial redraw is an inference from phase ordering; no
display-render tracing or optimization experiment was performed. A short LVGL
mutation time is not low end-to-end display latency. Input metrics exclude USB,
CST816S acquisition/hit testing and physical LCD response. Teardown time above
excludes waiting for GUI service. Native callbacks stayed below 100 ms.

Every UI generation recovered these global heap measurements **exactly**:

| Measurement | Before load | After unload |
|---|---:|---:|
| Internal free SRAM | 243495 | 243495 |
| Largest internal block | 163840 | 163840 |
| Free PSRAM / loader code-allocation heap | 8380764 | 8380764 |
| Largest code-allocation block | 8257536 | 8257536 |

Before/after retain the same 3668-byte submitted input, which the executor
releases before busy clears. Persistent executor infrastructure and fixed UI
arrays are system storage, not per-app resources. Each UI generation ended
with runtime resource counts **`[32,31,1,0]`** (one timer reclaimed), zero SDK
allocations, subscriptions/events/UI commands/widgets and zero realized LVGL
objects. UI-specific acquired/released counts balanced; no live app handles
remained.

The shared LVGL pool is separate from those global heap figures: free bytes
were **52960 → 52940**, **52940 → 52952**, **52952 → 52940** over the three
cycles. Thus its final free count is 20 bytes below the initial sample and
varies non-monotonically. Exact shared-pool recovery is **not** claimed. Source
attribution of that small shared renderer/allocator difference and long-term
stability remain unmeasured; no extra endurance work was run. Application
root/descendant deletion and zero owned objects/resources were independently
observed through the GUI backend.

## Verified Safe Mode and final device state

A fourth UI generation (6, after the compatibility examples) was started and
an RS1 recovery request closed admission while its view existed. Captured logs
show its native destroy before the subsequent Safe Mode boot. The controller's
existing deferred NVS/restart path remained in use; it waits for busy to clear
after acknowledged GUI/ELF cleanup. The host test separately covers recovery
arriving inside an active callback and verifies no premature memory release.

Safe Mode started without LittleFS mount, active executor/ELF or application UI.
START, RUN, ordinary events and semantic UI injection were refused. Resources,
widgets and commands were zero. RS1 REBOOT then completed the normal trial and
returned to STABLE without switching slots. Final diagnostics independently
confirmed:

| Final item | Observed state |
|---|---|
| Running/selected B `ota_1` | 0.3.3, VALID, embedded ELF `5c0d5eb2ed9b4fe4918620f2833298cd9c2a3274bfbb229cff96ae90db270142` |
| Fallback A `ota_0` | 0.3.2, VALID, compatible, rollback eligible |
| BootGuard | normal, stage 4 STABLE, latch/failures/trial/fault=0 |
| OTA | IDLE, checked/passed/rollback=1, last_error=0 |
| ELF/runtime | UNLOADED, busy/executed=0, all resource counters zero after reboot |
| Application UI | no view/widgets/realized objects/queued commands/input registrations |
| System screen | restored by GUI owner; normal boot and LCD DMA progressing |
| LCD final sample | 364 completed transfers, zero errors |
| Storage | NVS ESP_OK; LittleFS mounted, valid capacity, used=20480 |
| RTC | GET OK, valid, OS=0; no clock-discipline change or accuracy claim |
| Diagnostics | RO1, RS1, RT1, RE1 and UI STATUS operational |

The staged UI ELF is retained; no autostart was added. Actual finger hit testing,
visible counter/layout/readability, full sensor-to-pixel latency, physical
overload, long endurance, power interruption and battery-dependent recovery
remain **PENDING**. Trusted native code remains unisolated. Earlier RTC,
battery calibration and PSRAM/Flash-concurrency limitations are unchanged.

## Reproduce the pending finger/visual gate

Run from this checkout with the recorded physical device attached. These
commands stage the exact UI example and start it; START prints its generation.
No firmware rewrite is needed.

```sh
PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_28:84:85:87:98:7C-if00
python tools/roundie_elf.py --port "$PORT" upload examples/ui_elf/build/ui.app.elf
python tools/roundie_elf.py --port "$PORT" start --sha256 340c9146c8d354ea88e367cce4c6831ccee99d54bb0125a18fe537545215cb64
python tools/roundie_ui.py --port "$PORT" status
```

Follow [the five manual steps](input-bridge.md#manual-acceptance-pending-until-actually-performed)
using real finger taps and display observation; do not use `roundie_ui.py press`.
Use the printed generation for `suspend/resume/stop/unload --instance` in
`roundie_elf.py`. Finish with stop/unload and verify zero resources/widgets.

## Evidence archive and Git

Implementation commit: **`0c540dc`** (`feat(ui): bridge native ABI 3 views and input
through the GUI owner`). The follow-up documentation commit records these
measurements; its hash is reported in the final task response. Git was clean
after implementation commit; only task documentation changed after deployment.
Final staged review and `git diff --check` precede the documentation commit;
the final working-tree state is reported after that commit. No push occurred.

Raw binaries/logs/JSON and the once-guarded bounded evidence script are preserved
outside Git at `/mnt/Dane/Development/roundie-backups/ui-3c-2026-10-19/`:
`candidate.json`, `preflight-passed.json`, `deploy-{ready,new,restart,timing}.json`,
`b-first-boot.log`, `b-confirmed.json`, `ui-cycle-{1,2,3}.json`, `apps.log`,
`compatibility.json`, `safe-{request,status}.json`, `safe-boot.log`,
`normal-final-boot.log`, `final.json`, host/build/layout logs and frozen artifacts.
They are local evidence, not committed generated content.
