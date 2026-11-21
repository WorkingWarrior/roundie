# Milestone 1: ESP-IDF v6.1 bring-up

**Current USB checkpoint — 3F, 2026-10-20:**
[feasibility NO-GO](usb-mtp-milestone-3f-acceptance.md); installed A 0.3.8
remains VALID/STABLE selected, B 0.3.9 VALID/compatible unchanged after
[3E.4](display-3e4-acceptance.md). Only an isolated CDC+MTP component/descriptor
probe was compiled, with 5 host descriptor checks and all 82 existing tests
passing. No MTP firmware, OTG takeover, OTA or physical recovery experiment
was performed. The older display checkpoints below remain historical.

Display work continues in the [Milestone 3E.3 acceptance record](display-3e3-acceptance.md)
and [rendering measurements](display-rendering.md). Earlier bring-up evidence
below remains historical and does not establish later performance or visual acceptance.

**Current display checkpoint — 3E.3, 2026-10-20:** A 0.3.8 VALID/STABLE,
original B 0.3.7 VALID/fallback. Selected 30 MHz request produces 26.667 MHz;
two internal DMA buffers of 20 lines overlap drawing and transfer. Controlled
full native-UI handlers are 78.4–81.8 ms; target <70 ms not reached. 40 MHz
single-buffer candidate passed bounded tests but is not selected. 82 host
tests, pinned v6.1 builds and three genuine A-only OTA candidates passed;
final independent state has zero DMA errors, zero app resources, unchanged
packages and no idle redraw. Final visual/finger acceptance remains PENDING.

**Historical display checkpoint — 3E.2, 2026-10-19:**
[The dedicated throughput record](display-performance.md) documents exact
1.85B clock/timing sources, 3 MHz ID reads followed by 20 MHz writes, the
unchanged internal DMA buffer/ownership, 80 passing host regression tests,
pinned v6.1 target build and genuine RO1 OTA. Final B 0.3.7 is VALID/STABLE;
original A 0.3.6 remains VALID/compatible. Full-frame handlers measured
99.8–104.9 ms versus 247.3–247.8 ms; no observed transfer failures, no ELF or
application resources left loaded. The user subsequently confirmed faster
rendering with slight progressive painting still visible. Detailed visual/touch
acceptance and precise CPU/outlier attribution were left open. Bring-up records
below retain their original scope and evidence limits.

**Milestone 3E:** [launcher/navigation acceptance](launcher-milestone-3e-acceptance.md)
records 0.3.5 VALID/STABLE in B, preserved A 0.3.4 VALID, unchanged installed
Hello/UI packages, and real native launcher-to-app-to-launcher execution with
INJECTED input. Finger/visual launcher usability is PENDING. Measured GUI command
latency and the discovery-counter evidence limitation are recorded explicitly.

**Milestone 3D:** [packages/registry acceptance](packages-milestone-3d-acceptance.md)
records the package-enabled 0.3.4 build and bounded deployment gates. Package
format, persistence and launch contracts are in [PackageManager](package-manager.md).
Milestone 3C's actual finger/visual gate is now
[USER-VERIFIED](ui-milestone-3c-acceptance.md#user-verified-physical-touch-and-visual-acceptance),
separate from earlier automated injection logs; it was not repeated for 3D.

**Milestone 3C:** see [Application UI](app-ui.md) and [Input bridge](input-bridge.md)
for the scoped native UI extension and its separate host/target/device evidence.
The existing LCD/DMA driver, GUI task and CST816S input mapping are reused.

**Milestone 3B physical runtime checkpoint, 2026-10-19:** firmware 0.3.2 reached
VALID/STABLE in A by genuine inactive-slot RO1 OTA; preserved B 0.3.1 remains
VALID/fallback. The event-driven ABI 2 example passed three complete native
load/start/event/suspend/resume/stop/destroy cycles with exact measured SRAM/
PSRAM recovery and zero outstanding resources. The original ABI 1 hello still
returns 42. See [lifecycle and physical measurements](app-lifecycle.md) and
[resource ownership](resource-manager.md). No Milestone 3C work was started.

**Milestone 3A bounded gate passed, 2026-10-19:** native loader 1.3.5, ABI 1,
standalone ELF, development staging and executor passed physical execution,
cleanup, negative-candidate and Safe Mode checks. B 0.3.1 is VALID/STABLE;
preserved A 0.2.5 is VALID/fallback. See [integration and evidence](elf-loader.md),
including the measured upstream alignment defect and correction. Historical hardware,
storage, OTA and RTC records below remain unchanged.

**RTC investigation checkpoint, 2026-10-19:** firmware 0.2.4/0.2.5 adds requested,
read-only RTC/system interval diagnostics; see the
[measurement contract and physical evidence](rtc-time-sync.md#clock-investigation--2026-10-19).
The historical two-second long-uptime failure and earlier retention results
are preserved. No periodic synchronization, RTC SET, oscillator calibration
or time-source change was introduced. The 61-test host suite, pinned v6.1 REP
build and layout validation passed. Genuine OTA confirmed final **A 0.2.5
VALID/running**, preserving **B 0.2.3 VALID/fallback**. One additional A-only
OTA corrected an early diagnostic counter-publication race found in final
review; the existing recovery path temporarily selected/reconfirmed B without
rewriting it. Early replies physically rejected unavailable state with zero
counters, then valid startup diagnostics passed. Final 30-second normal
observations enclosed RTC−system in approximately −0.064…+0.958 s, with OS=0
and one startup initialization. The conclusion remains
**Case D / insufficient evidence**: four valid OS=0 samples on 0.2.4
over 300 s enclosed RTC−system around +0.645…+1.647 s; approximately 2 ms reads
and 8.57–9.99 ms USB RTT did not resolve drift (feasible rate interval includes
zero). No corrective discipline is justified by these data. The unchanged
one-second integer agreement and progression checks passed, while longer
agreement and absolute host-UTC accuracy remain PENDING. This is an RTC
investigation within the current platform; it does not start another milestone
or close older visual, battery, fault-injection or endurance gates.

## Milestone 2C.2 BootGuard and Safe Mode — 2026-10-19

BootGuard failure tracking, sparse NVS latch, one-shot normal trial, early
shared USB diagnostics and optional LCD-only Safe Mode are implemented.
See [BootGuard ownership/persistence](boot-guard.md) and
[Safe Mode commands and validation](safe-mode.md). Host tests and target
builds are distinct from the bounded physical acceptance sequence recorded
there. Historical 2C.1 evidence and the long-uptime clock discrepancy below
remain unchanged. No ELF runtime or later milestone was started.

**Bounded physical gate passed:** genuine OTA/confirmation, explicit Safe Mode
entry and normal exit, same-image RTC warm retention, guarded selection and
reconfirmation of original A, and one three-incomplete-startup test in already
VALID diagnostic firmware. Both slots remained VALID during that fault test;
Safe Mode stopped further automatic resets. A late-runtime policy gap found
by a failing host assertion was corrected and revalidated in 0.2.3. Final
normal 0.2.3 B passed OTA/confirmation and explicit recovery/normal-trial
controls; A remains original 0.2.1 VALID/fallback. Final BootGuard is STABLE,
safe=0, failures/latch/trial=0, test_build=0 and fault_remaining=0. NVS,
LittleFS, RTC/OS=0 and USB diagnostics passed their focused health checks.
Exact identities, evidence scope and remaining physical fault/visual checks
are in [the final recovery record](safe-mode.md#final-restoration-and-remaining-evidence).

## Milestone 2C.1 OTA foundation — 2026-10-18

Local streamed USB OTA, explicit activation, core self-test confirmation and
rollback eligibility are implemented; see [OTA ownership, commands, source
contract and validation gates](ota.md). A bounded read-only device inspection
confirmed the installed rollback-capable 2B bootloader, original 0.1.0 image,
VALID A metadata/CRC and an unbootable erased B header. Firmware resumed and
RT1 GET passed afterwards. The user then explicitly approved the frozen 0.2.1
app-only A bootstrap and genuine RO1 OTA into B. Both passed: B startup was
captured in PENDING_VERIFY and then VALID after its core self-test. That normal
sequence initially established normal 0.2.1 in both VALID slots.
LittleFS mounted with unchanged total/used, NVS initialized ESP_OK, and RTC
retained OS=0 and correct progression without SET. The bootloader was preserved.
The later separately authorized automatic rollback also passed, as summarized
below. Normal A/B success alone did not verify it. Full normal-test evidence is
in [the physical execution record](ota.md#authorized-physical-a-to-b-execution--2026-10-18).
This does not close older visual/touch, battery, power or endurance questions.

**Controlled rollback preparation:** a fresh read-only preflight reconfirmed
normal VALID B, compatible VALID A, exact installed bootloader/table/metadata
and normal B bytes. Source commit `1f03b79` makes armed-test refusals hold
unconfirmed without entering ordinary explicit rollback. Twelve focused host
tests (including 112 normal/armed boot-gate combinations) and a separate
`0.2.1-rollback-test` target build passed. The image is frozen outside Git;
its complete identity, write scope, single-experiment approval checkpoint and
separately authorized redundancy-restoration plan are in
[the preparation record](ota.md#controlled-automatic-rollback--preparation-2026-10-18).
**PHYSICAL AUTOMATIC ROLLBACK: PASSED** after separate explicit approval of the
668,384-byte frozen image. Genuine RO1 upload to A, NEW selection, test A's
PENDING_VERIFY, passed core checks and deliberate unconfirmed restart were
captured. The bootloader automatically loaded normal B; fresh RO1 observed
A ABORTED, B VALID/running with original normal ELF identity and passed core
self-test. LittleFS/NVS/RTC recovery checks passed without SET/format/erase.
No second experiment or A repair occurred within that experiment. Manual
rollback remains unverified.
See [actual rollback evidence and timing limits](ota.md#physical-automatic-rollback--authorized-execution-2026-10-18).

**Current deployment (Milestone 2C.1, 2026-10-19):** the connected device
uses REP v1 with OTA A at `0x10000`, OTA B at `0x390000`, and provisioned
LittleFS at `0x710000`. The separately authorized single restoration passed:
A is VALID/running, B VALID/fallback, both original normal 0.2.1 with identical
ELF identities and the armed test disabled. READY/digest verification preceded
activation; the captured normal A self-test confirmed PENDING_VERIFY to VALID.
Fresh independent RO1 status reported `rollback=1`, passed core checks and
an idle manager. NVS/LittleFS/RTC service checks passed without SET/format/erase.
An initial long-running B RTC/system comparison failed by two seconds; the
unchanged one-second check passed after the planned ROM preflight return and
after A startup. Continuous clock agreement was not fixed or established.
See [restoration evidence and limits](ota.md#confirmed-ab-redundancy-restoration--2026-10-19).
The manufacturer-layout installation commands below are historical. Use the
[local OTA workflow](ota.md) for separately authorized future updates. The
bootstrap-only REP updater intentionally rejects the changed A/B metadata;
the legacy updater rejects the REP table. Raw IDF flash targets are disabled after CMake
regeneration. Historical generated build trees must be reconfigured before
relying on that block; direct esptool commands always bypass it.

**Milestone 2B device validation:** complete early v6.1 bootloader/table
logs were captured on software restart; REP boots OTA A with ELF identity
`f929fe67d`. NVS returned `ESP_OK`, LittleFS mounted 9,240,576 total /
8,192 used bytes, and the one-shot 256-byte filesystem/remount test passed.
RTC remained valid with OS=0; BQ27220 was accessible (about 3,973 mV,
reported 84%, calibration unverified); ST77916 variant 2 initialized.
One 185-second serial smoke capture had no panic, watchdog failure or
unexpected reboot. A short software restart remounted storage without
repeating the diagnostic and retained RTC progression. The existing host
UTC comparison failed by approximately 3.6–3.8 seconds against its unchanged
3-second threshold; no RTC SET was issued. Full image identities and
separate evidence tiers are in the migration record. Latest-image visual
and touch interaction remain pending.

**Observed reset limitation:** esptool RTS reset after migration remained
in ROM. The operator's USB disconnect/reconnect without BOOT returned to
normal operation; no second flash was needed. For deliberately entering
ROM with an unbootable image, use the tested BOOT/reconnect procedure in
[rom-recovery.md](rom-recovery.md), preserving the battery connection and
checking whether USB removal actually stopped the board.

**Board target:** User-identified Waveshare ESP32-S3 Touch-LCD-1.85B; the
printed PCB revision was not visible. The documented Rev1.1 pinout was the
working premise, and actual LCD/touch behavior is recorded below.

**Evidence scope:** Source integration, target compilation, USB-only device
tests on 2026-10-17, USB-plus-battery synchronization/restart tests, and one
user-performed approximately 90-minute battery-backed RTC hardware PWR-off
retention test on 2026-10-18. RTC retention and one battery-connected manual
ROM recovery/normal-return cycle passed under their recorded conditions;
see the [recovery evidence](rom-recovery.md). Active battery-only operation,
cold-start reliability, full latest-image visual/touch verification and the
long-run gate remain open.

## Build

**Milestone 2A addendum:** the default `legacy` profile preserves this
bring-up workflow. Explicit commands are `idf.py -B build
-DROUNDIE_PROFILE=legacy build` and, for the incompatible future REP layout,
`idf.py -B build-rep -DROUNDIE_PROFILE=rep build`. Do not deploy REP through
the factory-app updater. It now rejects missing/wrong profile markers,
changed generated legacy tables, and REP embedded app identities before
opening the serial port. See [storage/build profiles](storage.md) and the
[unexecuted migration proposal](flash-migration.md). Historical results and
remaining Milestone 1 hardware checks below retain their original scope.

The selected ESP-IDF is tag `v6.1`, commit
`fff9895c82d744c7237be8847347bdd1b07c6643`. The local toolchain used
for this gate is `xtensa-esp-elf esp-15.2.0_20251204`. The IDF Python
environment is installed with `./install.sh esp32s3` in that exact IDF
checkout.
The top-level CMake file rejects any other IDF commit.

```sh
cd /mnt/Dane/Development/roundie
. /home/solve/.espressif/v6.1/esp-idf/export.sh
idf.py set-target esp32s3 build
```

`set-target` resets generated configuration and performs a clean target
build. It does not flash the board. `sdkconfig`, `build/`, and
`managed_components/` are generated; `sdkconfig.defaults` and
`dependencies.lock` are the tracked inputs.

**Do not use `idf.py flash` or `idf.py app-flash` on the inspected device.** Its existing
partition table differs from this temporary single-app build table, and
the default flash command would rewrite that table; both generated commands
place the app at `0x10000`. The board's read-back table has NVS at `0x9000`, OTA data at
`0xd000`, factory app at `0x20000`, another app at `0x6c0000`, and
assets at `0xac0000`. The earlier authorized bring-up installation wrote
the bootloader at `0x0` and app at `0x20000`; it left the installed
partition table and data partitions in place. This is a device-specific
installation method, not a final REP partition design.

### Guarded deployment for this diagnostic image

**Architectural decision for Milestone 1:** Keep the generated single-app
table solely for target compilation. For the already inspected 1.85B, update
only its existing factory slot at `0x20000`; do not change the installed
partition table, bootloader, OTA metadata, NVS, or other data. This is not
the REP production layout: spec §§15, 18, 23, and 25 require a later,
separately reviewed two-slot OTA layout without a factory application.

**Verified source/backup facts:** The original 16 MiB backup named below has
SHA-256 `46d2d36244a9e303cc61a65fb29d9a71b9708d41d181e9e5b5befb9261039de7`.
Its installed table has factory offset `0x20000`, size `0x692000`, one OTA
slot at `0x6c0000`, and assets at `0xac0000`; its OTA metadata is erased.
The pinned IDF v6.1 bootloader's `bootloader_utility.c` defaults to the
factory image when both OTA records are empty and a factory slot exists.
The current generated `build/flasher_args.json` instead writes a single-app
table at `0x8000` and an app at `0x10000`. The earlier device installation
already installed a v6.1 bootloader. Its 21,120-byte image in the verified
post-flash readback has SHA-256
`c7a70b9af9d599095d524cac280838544d074bbd088f9a2e88d483aed95df26d`.
The current clean build differs in five header bytes and the final image
digest; its code region matches. The guarded updater checks that known
installed image and code compatibility on the live device before any write.

After a successful `idf.py set-target esp32s3 build`, with the v6.1
environment exported, use this read-only preflight on the specific inspected
unit and its preserved backup:

```sh
python tools/flash_bringup.py --port /dev/ttyACM0
```

Under a separately authorized device-flash operation, the same command with
`--write` repeats preflight and writes only `build/roundie.bin` to the
installed factory slot:

```sh
python tools/flash_bringup.py --port /dev/ttyACM0 --write
```

**Fail-closed checks:** The script verifies the original backup's size and
hash, reads only the live partition-table sector, two OTA-metadata sectors,
and the bootloader image range, then requires an exact table match, erased
OTA metadata, the known installed bootloader and matching code region, the unchanged diagnostic
flash manifest, and an app whose sector-rounded size fits the factory slot.
It does not perform a full-Flash readback. A failed check stops deployment;
do not bypass it by using IDF's printed flash command. A different table,
active OTA selection, or unrecognized bootloader requires a reviewed deployment proposal.
No repartitioning or image relocation is part of this milestone. A passing
preflight is a layout check, not proof of boot or recovery behavior.

The component manager lock resolves:

| Component | Revision |
|---|---|
| ESP-IDF | 6.1.0 / commit above |
| `lvgl/lvgl` | 9.5.0 |
| `espressif/esp_lcd_st77916` | 2.0.2 |
| `espressif/esp_lcd_touch_cst816s` | 1.1.1 |
| transitive `espressif/esp_lcd_touch` | 1.2.1 |
| transitive `espressif/cmake_utilities` | 0.5.3 |

## Configuration and decisions

**Verified source fact:** The manufacturer's 1.85B BSP at commit
`139e6db584f3737fcfc6a958ee83b79fb69d317c` supplies two ST77916
command tables selected by register 0x04 and uses GPIO10/11 for the common
I²C bus. It also creates both new and legacy I²C controllers, as recorded in
[the hardware audit](hardware-audit.md). Only the two panel command tables
were copied into `lcd_init_tables.c`; the vendor BSP is not linked.

**Architectural decision (spec §§2.3, 3.2, 23):** One `i2c_master.h` bus
at 100 kHz owns CST816S, PCF85063, and BQ27220 access. The touch controller
uses Espressif's CST816S component and its panel IO on that same bus. The RTC
and gauge each get an explicit device handle when their probe responds, with
narrow read-only register access. The gauge reads voltage (0x08) and status (0x0A), as defined by
[TI's BQ27220 reference manual](https://www.ti.com/lit/ug/sluubd4a/sluubd4a.pdf).
The register address sent before each read does not alter gauge
configuration or calibration. No `i2c_bus` or legacy controller is present.

**Architectural decision for this gate (spec §§12.7, 25):** One 14,400-byte
internal DMA-capable LVGL strip buffer keeps the first asynchronous transfer
path easy to inspect. The SPI bus permits that strip plus command overhead;
the ESP-IDF completion ISR notifies the GUI task, and LVGL's flush-wait
callback blocks that task until completion before reusing the buffer. A
transfer timeout restarts the device rather than reusing a buffer still owned
by DMA. The spec's proposed two-buffer strategy and final buffering policy
remain open until board measurements.

**Architectural decision for P1 supervision:** ESP-IDF initializes the task
watchdog at boot with a 10-second timeout and panic/reboot policy. The GUI
task subscribes after setup and feeds from its own loop only after
`lv_timer_handler()` and health logging return. A panel-I/O call that blocks
inside `esp_lcd_panel_draw_bitmap()` therefore cannot be masked by another
task feeding the watchdog. The existing two-second completion wait restarts
on timeout before LVGL can reuse the DMA buffer. On watchdog panic, the
whole device reboots; no task attempts to free or reuse an in-flight buffer.
Ten seconds gives substantial margin over one 14,400-byte transfer at the
configured 3 MHz QSPI clock, but the actual worst-case GUI path and recovery
on the physical board remain pending measurements.

**Device observation and remaining assumption:** The user identified the
unit as a 1.85B but could not inspect its PCB revision. Its LCD reported
`00 02 7f 7f`, one of the two documented IDs; unknown IDs fail startup and
no table is guessed. QSPI starts at 3 MHz,
well below the vendor's 80 MHz. The backlight PWM is active-high on GPIO5,
5 kHz, initially 60%. LCD TE is not used. The copied tables include the
vendor's controller commands, but no TE synchronization is configured.

`sdkconfig.defaults` selects ESP32-S3, 16 MB flash, Octal PSRAM at 80 MHz,
and an 8 KiB main-task stack for the small LVGL loop. Generated v6.1 config
uses DIO flash at 80 MHz, the default single-app partition table, and does
not enable PSRAM XIP. This temporary partition choice does not resolve the
later storage/OTA architecture in spec §18.

## Diagnostic image behavior

Boot logs identify the firmware version, build timestamp, ELF SHA-256 prefix, IDF
version, detected flash and PSRAM size, free and largest internal/PSRAM
blocks, three I²C probes, RTC/gauge sample states, LCD ID/initialization,
and touch press/release. The 360 × 360 screen displays identity, memory,
device states, RTC validity, gauge voltage/estimated SoC, touch feedback,
a 30/60/100% backlight
button, color swatches, and an animated spinner. Tapping a swatch displays
a solid red, green, blue, or white field; tap the field to return. Touch
reads are triggered by the CST816S IRQ and continue while a press is active;
the controller can stop answering I²C when idle. A failed LCD enqueue stops
new pixel transfers; a completion timeout restarts the device before LVGL
can reuse the DMA buffer. Every five minutes the image logs free and largest
SRAM/PSRAM blocks and the LCD transfer-error count. The long-run test was
stopped before the first periodic sample, so no heap trend is established.

The firmware does not access NVS or a filesystem, erase flash, start USB
device mode, use Wi-Fi, or configure power/sleep features.

## Validation results

| Evidence tier | Result |
|---|---|
| Host/static | Both copied ST77916 tables matched the pinned vendor source byte-for-byte. Source scan found zero legacy I²C/I²S includes or `i2c_bus_create` uses in project C/H files. |
| ESP32-S3 target compilation | Clean `idf.py set-target esp32s3 build` under pinned v6.1 exited 0, with zero compiler warnings and zero errors. This proves target compilation only. |
| Physical device | User confirmed 1.85B, working Roundie page/controls after idle, correct red/green/blue/white fields, moving spinner, and coherent center plus four edge touch regions with release. ESP32-S3 revision 0.2 reported 16 MB flash and 8 MB embedded Octal PSRAM; panel ID selected variant 2; CST816S ID read 181. RTC ACKed/read once with voltage-low flag, then did not ACK on later USB-only resets; gauge did not ACK. PCB revision, battery behavior, and manual recovery remain unverified. |
| Long-running/fault injection | A short monitor run had an LVGL flush watchdog failure before the wait-callback fix. The later 65-second interrupt-driven touch run had 52 press/release pairs and zero touch I²C errors, LCD transfer errors, watchdog reports, or panics in its log. A planned 30-minute run was stopped at the user's request after about two minutes, before its first five-minute health sample. Endurance and fault injection remain pending. |

### P1 remediation validation, 2026-10-17

| Evidence tier | Result and limit |
|---|---|
| Host/source | Inspected the checked touch callback registration and failure cleanup, bus-error return path, GUI-only watchdog feed, and generated flashing offsets. No legacy I²C/I²S include or `i2c_bus_create` use was added to project sources. Offline preflight accepted the recorded post-flash layout and rejected a changed table, non-erased OTA metadata, and an unrecognized bootloader. These checks do not simulate an actual board failure. |
| ESP32-S3 target compilation | `idf.py set-target esp32s3 build` exited 0 after regenerating config; generated `sdkconfig` has `CONFIG_ESP_TASK_WDT_PANIC=y` and a 10-second timeout. An incremental `idf.py build` after the final touch cleanup change also exited 0. Final `build/roundie.bin` is 569,968 bytes, SHA-256 `0c34a13a8ab047493e8138262b99f43b97be61e6303da00366620832a1cd7ceb`. IDF emitted Kconfig/CMake notices during the clean build; no project compiler error was reported. |
| Physical device | **Not tested for these changes.** No preflight was run against the live unit and no firmware was flashed. Bus-creation/IRQ failure behavior, worst-case LCD timing, watchdog reset, boot, and recovery remain unverified. |
| Long-running/fault injection | Not run for this remediation. The original pending Milestone 1 checks remain pending. |

### Final autonomous validation, 2026-10-17

The user first requested repository-only validation, then explicitly
authorized application-only deployment and an unattended USB smoke test.
The guarded deployment procedure below was used after that authorization.
No manual board or touch interaction, full-Flash readback, or endurance test
was performed.

| Evidence tier | Result and limit |
|---|---|
| Source/host — I²C | `start_i2c()` returns the bus-creation error, records it for the three dependent devices, and `roundie_board_start()` releases partial resources before returning that error. Optional probe failures remain per-device status after a successful bus start. Project sources use `driver/i2c_master.h`; a scan found no legacy I²C/I²S include or legacy controller call in `main/` or `components/`. Forced bus failure was not run on hardware. |
| Source/host — CST816S | The constructor receives a null callback; Roundie then checks `esp_lcd_touch_register_interrupt_callback()` and deletes the touch and IO handles if it fails. `roundie_board_stop()` deletes the touch handle before its IO and I²C bus. Touch on this new image, ISR failure, and repeated teardown remain untested on hardware. |
| Source/host — GUI watchdog | The GUI/main task registers with Task WDT after setup and feeds only after `lv_timer_handler()` returns. Generated config enables panic and a 10-second timeout. The two-second LCD completion wait and restart-before-buffer-reuse path remain in place. LCD timing, false resets, and actual watchdog recovery on this image are pending device tests; no artificial hang was introduced. |
| Source/host — safe deployment | The original 16 MiB backup still hashes to `46d2d36244a9e303cc61a65fb29d9a71b9708d41d181e9e5b5befb9261039de7`. Its parsed table has NVS `0x9000`, OTA metadata `0xd000`, factory app `0x20000` size `0x692000`, OTA app `0x6c0000`, and assets `0xac0000`. Offline `preflight()` accepted the saved post-flash table/bootloader and current build, then rejected a changed table, active OTA metadata, unknown installed bootloader, incompatible rebuilt bootloader code, unsupported flash manifest, invalid app header, oversized app, and changed backup. Mocked `main()` made three bounded reads and no write by default; mocked `--write` issued one factory-app write at `0x20000`. Ordinary `idf.py flash` and `idf.py app-flash` remain prohibited for this installed layout. |
| ESP32-S3 target build | One clean `. /home/solve/.espressif/v6.1/esp-idf/export.sh` then `idf.py set-target esp32s3 build` exited **0** on IDF commit `fff9895c82d744c7237be8847347bdd1b07c6643`. `image-info` reports project `roundie`, version `0.1.0`, IDF `v6.1`, ESP32-S3, and 16 MB Flash. The app is **569,968 bytes**, SHA-256 `c8e141a5a9fc478f42572f6a50b5cadf253c77726a39bea9ad528b5c022387f5`, unchanged in size from the prior P1 build. The bootloader is 21,120 bytes, SHA-256 `04fb63e6d82c1adc7a153ee8168bb9de9f4ea90a565631474c46d3962d4213ec`; its code region passed the updater's compatibility check against the installed bootloader. The log has **zero compiler warnings and zero errors**; it has five ESP-IDF `esp_wifi`/`wpa_supplicant` component-validation CMake warnings and IDF Kconfig notices. `dependencies.lock` still pins the listed component revisions. This is target compilation, not device validation. |
| Physical device — identity and deployment | The connected USB-Serial/JTAG device was ESP32-S3 revision v0.2, MAC `28:84:85:87:98:7c`, with detected 16 MB Flash and 8 MB embedded PSRAM. A live `python tools/flash_bringup.py --port /dev/ttyACM0` preflight exited **0** after bounded reads of the table, OTA metadata, and bootloader; it matched the inspected layout, erased OTA metadata, and known compatible bootloader. The separately authorized `--write` repeated preflight, then esptool erased only `0x20000–0xabfff`, wrote 569,968 app bytes at `0x20000`, and reported **“Hash of data verified”** before resetting. No command wrote the bootloader, partition table, NVS, OTA metadata, OTA app, or assets; their post-write bytes were not independently read back. The preserved full-Flash backup was not modified. |
| Physical device — unattended serial smoke | A single approximately five-minute `idf.py monitor` capture after deployment recorded one boot of `roundie` 0.1.0 / IDF v6.1, 16,777,216 B Flash, 8,388,608 B PSRAM, CST816S ID 181 and driver `ESP_OK`, LCD ID `00 02 7f 7f`, and ST77916 variant 2 initialization. At 301,604 ms, the GUI loop logged internal SRAM free/largest **284,575/204,800 B**, PSRAM free/largest **8,384,620/8,257,536 B**, and **0 LCD errors**. The capture contained no LCD transfer error, task-watchdog report, panic, or reboot. Its startup probe reported RTC and gauge `ESP_ERR_NOT_FOUND` under USB power; the early touch probe also NACKed before the driver reset and successful ID read. One telemetry sample and serial output cannot establish visual fields, spinner motion, touch/release, worst-case LCD timing, or a long-term heap trend. |

**Pending on the remediated image:** human inspection of LCD color fields,
spinner motion and GUI response, plus center/four-edge touch and release.
Manual ROM Download with read-only chip identification is pending with USB
power and, if available, battery power. RTC validity, battery-only behavior,
gauge calibration, ten controlled cold starts, PCB revision inspection, and the
separately authorized 30-minute visual/interaction criterion also remain
open. These checks require later physical access; Milestone 1 is not closed,
and this record does not authorize starting Milestone 2.

### Battery-connected RTC and gauge check, 2026-10-18

**Power condition:** The user reported a battery connected while USB stayed
connected. The USB-Serial/JTAG device remained `/dev/ttyACM0`. Two short
`idf.py -p /dev/ttyACM0 monitor` captures each performed a normal software
reset and recorded one boot of the same `roundie` 0.1.0 image (ELF SHA-256
prefix `afdcf3ed2`). No firmware rebuild, flash write, I²C configuration
write, storage operation, or endurance test was performed.

| Check | Earlier USB-only observation | USB plus battery, two fresh boots |
|---|---|---|
| PCF85063ATL RTC at `0x51` | ACKed once with the flagged time, then NACKed on several starts; the latest 2026-10-17 boot logged `ESP_ERR_NOT_FOUND`. | Probe and seven-byte time read returned `ESP_OK` twice. The firmware logged `01:52:10` then `01:53:40`, both with `voltage_low=1`. Communication and an advancing displayed value are verified; wall-clock validity and retention are not. |
| BQ27220 at `0x55` | Did not ACK without a battery; no voltage/status value was available. | Probe and read-word commands returned `ESP_OK` twice. Both boots reported `3970 mV` and `BatteryStatus() = 0x5028`. This is a device-reported voltage, not an independently metered or calibrated value. |
| Shared-bus context | The early CST816S probe may NACK while the controller is still in reset. | The early `0x15` probe again NACKed, then the touch driver read ID 181 and initialized with `ESP_OK` on both boots. No bus error, panic, or watchdog report appeared in either short log. No touch interaction was performed. |

The firmware field `voltage_low` reads bit 7 of the RTC seconds register.
For the schematic's PCF85063ATL, [NXP's PCF85063A datasheet](https://www.nxp.com/docs/en/data-sheet/PCF85063A.pdf)
defines that bit as **OS (oscillator stop)**: when set, clock integrity is
not guaranteed. The observed time must not be used as a valid date/time;
the flag was neither cleared nor was the clock set during this test.

Per [TI's BQ27220 reference manual](https://www.ti.com/lit/ug/sluubd4a/sluubd4a.pdf),
`Voltage()` at `0x08` reports measured millivolts. `0x5028` in
`BatteryStatus()` at `0x0A` has `OCVCOMP`, `SLEEP`, `OCVGD`, and `BATTPRES`
set. `BATTPRES` corroborates the reported battery attachment, and the
voltage is plausible, but these bits do not prove fuel-gauge configuration,
capacity calibration, or state-of-charge accuracy. The present firmware
does **not** read `StateOfCharge()` at `0x2C` or `OperationStatus()` at
`0x3A`; neither a percentage nor the gauge's `INITCOMP` state was captured.
Battery-only operation, external voltage comparison, RTC retention/setting,
and gauge configuration/SoC validation remain pending.

### Milestone 1 device checklist status

Updated for the user-performed retention and ROM recovery tests on
2026-10-18. Earlier visual and touch passes remain evidence for their tested
image. The recovery test confirms return of the latest diagnostic screen;
its full color/orientation/touch checklist remains open.

| Check | Status | Evidence and limit |
|---|---|---|
| Diagnostic LCD, orientation, color fields, spinner | **PASSED on earlier image; latest image PENDING** | User observed the page, correct red/green/blue/white fields, and moving spinner; LCD ID `00 02 7f 7f` selected variant 2. Latest synchronization image still needs visual confirmation. |
| Touch center and four reachable edge regions, release, controls after idle | **PASSED on earlier image; latest image PENDING** | User explicitly confirmed all five regions and release, and controls after at least five seconds idle. The 65-second log recorded 52 press/release pairs without touch-read errors. Latest-image recheck remains open. |
| Flash and PSRAM detection | **PASSED for detection** | Device reported 16,777,216 B Flash and 8,388,608 B PSRAM; startup heap values were logged. Executable PSRAM and memory stress are outside this gate. |
| Shared I²C devices | **PARTIAL** | Touch driver read ID 181; RTC/gauge ACKs and data reads passed. RTC now has valid time and OS=0, including after the physical retention test. Gauge accuracy/calibration and full active battery-only behavior remain unverified. |
| RTC battery-backed hardware PWR-off retention | **PASSED — approximately 90 minutes, one physical test** | User disconnected USB, left the battery connected, used PWR off, then powered on/reconnected and ran `status` without resync. Valid RTC, OS=0 and restored system UTC. [Exact evidence and limits](rtc-time-sync.md#test-c--physical-retention-passed). |
| Manual ROM Download and normal return, battery connected | **PASSED — one physical recovery cycle** | PWR hold with USB attached left the display running; USB removal followed by BOOT held during USB reconnection entered ROM. Esptool 5.6.0 `chip-id` (MAC fallback) and `flash-id` exited 0 with `--no-stub --before no-reset --after no-reset`. Unplug/replug without BOOT restored Roundie, diagnostics and valid RTC/OS=0. [Exact sequence and evidence](rom-recovery.md). |
| Ten cold starts; active battery-only operation | **PENDING** | The retention and ROM recovery cycles do not complete ten cold starts or active battery-only diagnostics. PCB revision inspection remains open. |
| 30-minute interaction and heap trend | **PENDING** | A separate five-minute unattended run logged one health sample and zero LCD errors; the 30-minute visual/interaction run and a heap trend remain untested. |

### Device installation and data preservation

The user explicitly identified the connected USB device as the 1.85B and
authorized backup and flashing. `esptool flash-id` reported an ESP32-S3
revision 0.2, 16 MB Flash, and 8 MB embedded PSRAM. Security information
reported Secure Boot and Flash Encryption disabled; no eFuse was changed.
The complete 16,777,216-byte pre-flash readback is
`/mnt/Dane/Development/roundie-backups/preflash-2026-10-17-pMVcHN/flash-16MiB.bin`,
SHA-256 `46d2d36244a9e303cc61a65fb29d9a71b9708d41d181e9e5b5befb9261039de7`.
The read log and flash logs are beside it. The first installation wrote
only offsets `0x0` (bootloader) and `0x20000` (factory app); later updates
wrote only the factory app. Each esptool write verified its image hash.

A full 16 MB post-flash readback after the interrupt-driven touch image
matched the pre-flash backup byte-for-byte in the partition table, NVS,
OTA data, PHY data, the gap before the factory app, factory tail, OTA app
slot, assets, and flash tail. Changed 4 KiB sectors were confined to
`0x00000–0x05fff` and `0x20000–0xaafff`, the bootloader and factory app
image ranges. Its SHA-256 was
`7e3b02df62c1b6ac66fa31fdfe64488ce02304967ca564e4bdaddd51a13ab9f9`.
The later five-minute-telemetry update wrote only the factory app at
`0x20000`, and esptool verified the written app hash. A full readback of that
final update was not compared with the original backup; the byte-for-byte
preservation statement above applies to the earlier readback.

The first flashed build reached the LCD but stalled inside LVGL's flush
wait and triggered the task watchdog. Adding `lv_display_set_flush_wait_cb`
fixed that measured failure. The next run showed repeated CST816S I²C
errors during idle polling; the controller's IRQ now starts reads, and
polling continues only while a press is active. The 65-second run of this
image logged no touch errors and the user confirmed controls work after idle.
All these tests used USB power without a battery.

### Final command record

On 2026-10-17, `idf.py set-target esp32s3 build` exited **0**; subsequent
incremental builds, including the final telemetry change, also exited **0**.
A separate fresh-directory build was started but stopped on the user's
instruction, so it is not counted as a completed final-source clean build.
The generated `sdkconfig` confirmed the 16 MB flash setting, Octal PSRAM
at 80 MHz, 8 KiB main-task stack, and PSRAM XIP disabled. Build
`compile_commands.json` retained `-Werror`; the build log contained
zero warnings. The component manager generated the versions above in
`dependencies.lock`. `python -m esptool image-info build/roundie.bin`
reported ESP32-S3, 16 MB flash, project `roundie`, version `0.1.0`,
and ESP-IDF `v6.1`. This inspects the built image, not the physical chip.

| Generated artifact | Size | SHA-256 |
|---|---:|---|
| `build/roundie.bin` | 569,568 bytes | `888691831e3eb4e8612ab7061b2a3da926f7186ea44b7880f131439751ac3585` |
| `build/bootloader/bootloader.bin` | 21,120 bytes | `c7a70b9af9d599095d524cac280838544d074bbd088f9a2e88d483aed95df26d` |
| `build/partition_table/partition-table.bin` | 3,072 bytes; generated, not flashed | `7f00b6c042a89b15b0cac534f82ed988caf29278ff5700b0c511eb1b5bb7c820` |

The host check compared both complete C table declarations with the
manufacturer's inspected commit and found exact matches. The legacy-driver
scan covered `main/` and `components/` C/H sources; it is a source check,
not a live I²C-bus measurement.

## RTC and battery diagnostic services, 2026-10-18

**Implementation decision (Milestone 1, spec §23):** The board component keeps
the sole new-driver I²C master and owns the RTC/gauge handles. It now exposes
typed RTC and battery samples with monotonic observation timestamps, validity
states, and underlying `esp_err_t` values. A missing device is retried on
later samples. One sensor worker reads both devices every five seconds; the
GUI task copies completed samples and updates LVGL labels once per second.
Each I²C transaction has the existing 100 ms bound. No gauge write command
is issued. The worker and UI do not allocate a new sample per poll.

**RTC behavior at the initial sensor-service checkpoint (superseded by the
host synchronization addendum below):** The PCF85063 interface reads seconds through years in one
seven-byte transaction, decodes BCD, checks date, leap year, weekday and
24-hour mode, and rejects the oscillator-stop bit as invalid time. Valid UTC
time initializes the system wall clock at startup; `esp_timer` monotonic time
remains independent. A later valid RTC sample can synchronize a previously
unsynchronized wall clock. The explicit `roundie_board_rtc_set_trusted()`
interface accepts a validated UTC calendar (2000–2099, Sunday = 0), writes
all seven time registers together, and requires a valid readback within one
second of the requested time. An unsupported 12-hour or stopped mode is
rejected rather than changed implicitly. No host-time transport exists yet,
so the invalid RTC is not set from build time or the computer clock. Setting,
retention and synchronization after a set remain untested on the device.
[NXP PCF85063A datasheet, Rev. 7.3](https://www.nxp.com/docs/en/data-sheet/PCF85063A.pdf),
§§7.2–7.4, supplies register and snapshot behavior.

**Gauge behavior:** The read-only BQ27220 interface uses `BatteryStatus()`
`0x0A`, `OperationStatus()` `0x3A`, `Voltage()` `0x08` in mV,
`FullChargeCapacity()` `0x12` and `RemainingCapacity()` `0x10` in mAh,
`StateOfCharge()` `0x2C` in percent, `Current()` `0x0C` as signed mA
(positive charge, negative discharge), and `Temperature()` `0x06` in 0.1 K
(converted to 0.1 °C). It exposes `BATTPRES`, `INITCOMP`, `SLEEP`, and `DSG`
where available. A missing `BATTPRES` is battery-absent; communication errors
and invalid fields are distinct. SoC is unavailable until initialization,
capacity and range checks pass. Its displayed percentage is explicitly a
gauge estimate: chemistry/profile, calibration and accuracy have not been
verified. A clear `DSG` means charging **or** relaxation, so the UI says
“Charge/idle.” [TI BQ27220 Technical Reference Manual, Rev. A](https://www.ti.com/lit/ug/sluubd4a/sluubd4a.pdf),
§2, is the command and flag source.

| Evidence tier | Result and limit |
|---|---|
| Host logic | `cc -std=c11 -Wall -Wextra -Werror -Icomponents/roundie_board/include -Icomponents/roundie_board components/roundie_board/sensor_codec.c tests/test_sensor_codec.c -o /tmp/roundie-sensor-tests && /tmp/roundie-sensor-tests` exited 0. It covers BCD, leap day, invalid date and weekday, oscillator flag decoding, little-endian gauge words, signed current, and temperature conversion. It does not simulate I²C absence or transient failure. |
| ESP32-S3 target compilation | Under pinned ESP-IDF v6.1, `idf.py build` exited 0. Two incremental rebuilds for final UI corrections also exited 0. Final `build/roundie.bin` is 574,528 bytes, SHA-256 `fb358ab2647446c70f5edd1d6c646fa8fe1482ffca663a63cfb651a6fce2c917`. This proves compilation, not board behavior. |
| Deployment guard | `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_28:84:85:87:98:7C-if00` resolved to `/dev/ttyACM0`. Read-only `esptool flash-id` identified ESP32-S3 revision 0.2, MAC `28:84:85:87:98:7c`, 16 MB Flash and embedded 8 MB PSRAM. `tools/flash_bringup.py` read and accepted the live partition table, erased OTA metadata, installed bootloader compatibility, image header and slot size. The app's rounded erase length `0x8d000` is below factory slot `0x692000`. Both app-only writes reran preflight; the final one wrote only 574,528 bytes at factory-app offset `0x20000`, and esptool verified the image hash. No bootloader, partition table, NVS, OTA or asset write was requested. |
| Physical device, unattended serial | A 125-second capture of the first image and a 35-second capture of the final image each recorded one boot, successful touch driver and LCD variant-2 startup, RTC `invalid data` with OS=1, and an explicitly unsynchronized wall clock. The final image (ELF prefix `46ee99ad6`) reported battery present, 3969 mV, SoC 84%, flags `0x5028`, operation flags `0x0024` (`INITCOMP` set). Five seconds later it logged the same voltage/SoC. Earlier USB-plus-battery boots reported 3970 mV and the same RTC OS flag; this is consistent but not meter or clock validation. No panic, watchdog or LCD-transfer error appeared in either capture. Unchanged sensor values are not logged repeatedly, so serial does not measure every five-second poll or confirm display appearance. |

**Pending experiments:** A trusted host-time transport and physical RTC
set/readback test; RTC retention with controlled power removal; independent
battery voltage/temperature/current comparison; gauge profile and SoC
accuracy; battery-only operation; touch and visual inspection of the new
labels; repeated unplug/replug and transient I²C fault recovery; and a
longer GUI-responsiveness run. These short serial captures do not close
Milestone 1's ten-cold-start, manual-recovery, or 30-minute interaction gate.

### Gauge capacity (development command RG1)

On 2026-11-21 the 1.85B gauge reported a steady SoC of 20% while on USB.
JTAG reads of the firmware's sensor sample (0.3.73) showed 4124 mV, 0 mA,
`RemainingCapacity()` 593 mAh, `FullChargeCapacity()` 3000 mAh,
`BatteryStatus()` `0x5028` and `OperationStatus()` `0x00A4` (INITCOMP,
UNSEALED, VDQ clear). The 20% comes from the chip (593/3000): the gauge still
holds TI's default 3000 mAh profile. The fitted cell is not Waveshare's
500 mAh cell: the owner replaced it with a cell of 600 mAh nominal capacity
(the cell maker's figure; not measured here).

`!RG1` (`main/gauge_diagnostics.c`, enabled with `CONFIG_ROUNDIE_DEV_POWER`;
client `tools/roundie_gauge.py`, reply format 2) has two operations, both in
`bq27220_config.c` (board directory, host-testable):

- `STATUS` reads `BatteryStatus()`, `OperationStatus()`, `DesignCapacity()`
  `0x3C` and three Data Memory blocks: `0x929B` (Gauging Configuration, Full
  Charge Capacity `0x929D`, Design Capacity `0x929F`, Design Voltage
  `0x92A3`), `0x91FB` (Charging Voltage `0x91FD`, Taper Current `0x9201`) and
  `0x9268` (Near Full `0x926B`). Each block read selects the address, waits
  for its echo at `0x3E` and refuses a block whose `MACDataSum()`/
  `MACDataLen()` it cannot reproduce. No subcommand is sent.
- `CAPACITY <mAh>` (100–6000) initializes Design Capacity and Full Charge
  Capacity once (below). It writes nothing when Design Capacity already has
  the value.

#### Corrected procedure (2026-11-21)

The first procedure (0.3.74/0.3.75) sent Full Access keys `0xFFFF 0xFFFF`
before `ENTER_CFG_UPDATE`. **Device result (0.3.74 and 0.3.75, one board):**
`STATUS` reproduced the block checksum and length and read 3000/3000 mAh,
matching `DesignCapacity()`; `CAPACITY 600` stopped at step `full-access`
twice because SEC stayed `10` (UNSEALED), also when polled for 1 s. Nothing
was written; the gauge kept 3000 mAh and `0x00A4`. The Full Access
requirement was wrong:

- **Verified (documentation):** TRM SLUUBD4A §2.2.21: `ENTER_CFG_UPDATE`
  (`0x0090`) "is only available when the fuel gauge is UNSEALED"; the host
  reads the CFGUPDATE flag at least 2 s after it. §8.1.4: the gauge leaves
  CONFIG UPDATE by itself after about 240 s. TRM Table 2-2 marks `0x0090`–
  `0x0092` as available sealed; this contradicts §2.2.21, and the procedure
  follows §2.2.21 (refuses SEALED).
- **Verified (documentation):** TI E2E thread 719878 (answer by a TI
  engineer): a Data Memory write by address without Full Access, as
  `ENTER_CFG_UPDATE`; address (little-endian) and data (big-endian) to `0x3E`
  in one transaction; checksum and length to `0x60`; exit. Checksum is
  `0xFF` minus the low byte of the sum of address and data bytes; length
  counts address, data, checksum and length. Example: `0x929F` ← `0F CB`
  gives `F4 06` (host test `test_bq27220_config.c`). Partial blocks up to 32
  bytes are allowed.
- **Decision:** no access keys are sent; the gauge is never sealed, reset
  (`0x0041`) or written in OTP. The earlier hypothesis of a non-default Full
  Access key is not pursued: Full Access is not needed, and keys are not
  guessed.

Sequence (`roundie_bq27220_initialize`; at most about 8 s, system worker,
task watchdog fed in every delay):

1. Read flags, `DesignCapacity()` and the blocks. Refuse (`INVALID_STATE`,
   step `precheck`, no subcommand sent) when SEALED, no battery (BATTPRES),
   INITCOMP clear, CFGUPDATE already set (an earlier procedure was
   interrupted: it is not resumed) or the target is out of range. Refuse
   (`INVALID_RESPONSE`) when the block's Design Capacity differs from
   `DesignCapacity()`, or is neither the target nor the 3000 mAh reset
   default (a profile nobody here configured).
2. Design Capacity equal to the target: done, nothing written (the learned
   Full Charge Capacity is kept).
3. `ENTER_CFG_UPDATE`; first `OperationStatus()` read 2 s later, then every
   500 ms (at most two reads per second) until CFGUPDATE, up to 4 s.
4. One write of `9D 92 hi lo hi lo` to `0x3E` (FCC and Design Capacity =
   target), then checksum and length 8 to `0x60`.
5. Read back `0x929B` inside CFGUPDATE. A mismatch leaves with
   `EXIT_CFG_UPDATE` (`0x0092`, no reinitialization on unconfirmed values).
6. `EXIT_CFG_UPDATE_REINIT` (`0x0091`); wait for CFGUPDATE clear and INITCOMP
   set, polls 500 ms apart, up to 3 s.
7. Read everything again: `DesignCapacity()`, Design Capacity and FCC must
   equal the target. SoC is not forced: the gauge re-estimates RM and SoC
   itself after the reinitialization.

On any failure after `ENTER_CFG_UPDATE` the procedure reads
`OperationStatus()` (2 s after a failed `ENTER_CFG_UPDATE` write, since the
gauge may have taken it) and sends `EXIT_CFG_UPDATE` only if CFGUPDATE is set.
The reply's `written` says whether the commit was sent; `step` says where it
stopped.

Host tests (`tests/test_bq27220.py`, a model of the documented behavior, not
of the device): UNSEALED and FULL ACCESS success with only `0x0090`/`0x0091`
sent and every other Data Memory byte unchanged; SEALED, no battery, not
initialized, open CFGUPDATE, out-of-range target, 500 mAh profile and a
`DesignCapacity()` mismatch refused with nothing sent; 600 mAh with a learned
FCC of 575 kept; bad block checksum and a missing address echo; CFGUPDATE
never or late confirmed; commit not applied; exit never confirmed (reported
with CFGUPDATE set); one failed I²C transfer at every position, lost or
applied, leaving Data Memory all-old or all-new and the gauge out of
CFGUPDATE. The tests found and fixed two faults before any device run: a
failed `ENTER_CFG_UPDATE` write was followed by a flag read after 0.5 s
instead of 2 s, and a commit that reported an error was not marked
`written`.

#### Profile review for the 600 mAh cell

Only Design Capacity and Full Charge Capacity change. TRM §1.1.10: learned
Full Charge Capacity starts from Design Capacity, so it is set once to 600
with Design Capacity and then left to learning; a configured gauge is never
written again. Everything else stays as the gauge holds it:

- **Chemistry and cell characterization** (EMF, C0, R0, T0, R1, TC, C1, fixed
  EDV0–EDV2, the DOD table): no data for this cell. Waveshare's 500 mAh demo
  profile describes another cell and TI's defaults a generic one; neither is
  copied.
- **Charging Voltage 4200 mV, Design Voltage 3700 mV:** match a common
  LiCoO₂/LiPo cell; unchanged. **Assumption:** the cell is a 4.2 V cell.
- **Taper Current (TI default 100 mA):** C/6 for 600 mAh. FC needs current
  below Taper Current for two 40 s windows; the charger's termination
  current decides whether FC sets. The charger and its termination current
  are not documented here. **Pending:** measure the charge current at
  termination, then decide Taper Current.
- **Near Full (default 200 mAh), Reserve Capacity, Gauging Configuration:**
  capacity-related or policy values; unchanged until the device values are
  read with `STATUS` (this unit's OTP may differ from TI's defaults).

#### Restoring after a gauge reset

The values live in the gauge's RAM: a gauge power-on reset (battery
disconnected) restores 3000 mAh from OTP. With
`CONFIG_ROUNDIE_GAUGE_DESIGN_CAPACITY_MAH` (1.85B board Kconfig, default 0 =
off) the system worker reads `DesignCapacity()` with every 5 s battery sample
and, when it differs from the setting, runs the procedure once
(`gauge_restore.c`): only with a present, initialized gauge, no OTA transfer
in progress and at least 60 s after boot. A failed attempt is not repeated
until the gauge reported the target or the battery was absent; a gauge at the
target is never written, so learned FCC survives firmware restarts. The
procedure itself writes only over the 3000 mAh reset default. OTP is never
programmed. The FCC written with Design Capacity is the learned value kept in
NVS when its record is valid, otherwise the design value (see "Capacity
learning" below).

**Device result (2026-11-21, firmware 0.3.76, one board, battery on USB):**
OTA to `ota_1` PASS (preflight PASS, unexpected resets 0); 0.3.75 stayed
VALID in `ota_0` as the rollback image. The restore setting was 0 (off).
`STATUS` before the write: OperationStatus `0x00A4` (SEC `10` UNSEALED,
INITCOMP 1, CFGUPDATE 0), BatteryStatus `0x5028` (BATTPRES), Design Capacity
3000 in `0x3C` and Data Memory, FCC 3000, Gauging Configuration `0x102A`,
Design Voltage 3700 mV, Charging Voltage 4200 mV, Taper Current 100 mA, Near
Full 200 mAh (the TI defaults), RM 593 mAh, SoC 20%, 4124 mV, 0 mA. One
`CAPACITY 600` (no access keys) returned OK at step `none` with
`written` 1 in about 3 s: the readback inside CFGUPDATE and after
`EXIT_CFG_UPDATE_REINIT` matched. After it: Design Capacity 600 in `0x3C`
and Data Memory, FCC 600, RM 577 mAh, SoC 97%, 4124 mV, 0 mA, BatteryStatus
`0x5028`, OperationStatus `0x00A4` (CFGUPDATE 0, INITCOMP 1); the other
profile values were unchanged. BootGuard: safe 0, failures 0, no latch; OTA
state VALID, `passed` 1. MCU uptime showed no reset after the write.

The SoC is the gauge's own estimate after reinitialization (RM from the
open-circuit voltage); it was not forced. 97% at 4124 mV with no current is
plausible for a 4.2 V cell resting after charge termination, but SoC
accuracy is not verified: the chemistry and EDV values are TI's generic ones.

**Pending:** enabling the restore setting at 600 and one battery
reconnection; a charge/discharge cycle to check SoC, FC detection (Taper
Current) and FCC learning. Each needs approval.

#### Capacity learning (TRM §1.1.3)

Source: TI SLUUBD4A §1.1.3–1.1.4, §1.1.10, §4.4.1, §4.9.12–4.9.14,
§4.9.35–4.9.38. Firmware: `components/roundie_board/gauge_learning.c`
(pure, host-tested in `tests/test_gauge_learning.c`),
`main/gauge_learning_service.c` (NVS, load hold, RG1 `LEARN`).

**How the gauge learns (TRM, verified text).** A qualified discharge starts
with RemainingCapacity() within Near Full of FCC and sets
OperationStatus()[VDQ]. The gauge counts the discharge (DCR, initialized to
FCC − RM) down to EDV2 and then sets FCC = DCR + FCC × Battery Low %, saved
within 4 s; one update may lower FCC by at most 256 mAh or raise it by 512.
VDQ clears on any disqualifying condition: a charge of 10 mAh or more,
temperature below Learning Low Temp, more than 256 mAh of self-discharge
estimate, at EDV2 a voltage below EDV2 − 256 mV, a current below 3C/32, or an
overload. EDV is not detected at all below C/32 or above OverLoad Current.

**Audit of the 600 mAh configuration.** Values from `STATUS` on 0.3.76
(verified) unless marked as TI defaults not yet read from this unit (the new
`STATUS` format 3 reads them; check before the cycle):

| Parameter | Value | Effect on learning with 600 mAh |
|---|---|---|
| Design Capacity / FCC | 600 / 600 (RAM) | learning starts from 600 (TRM §1.1.10) |
| Gauging Configuration | `0x102A` | CSYNC 1 (RM = FCC at charge termination), EDV_CMP 1 (EDVs computed from TI's generic CEDV coefficients, not this cell's), FIXED_EDV0 1 (3031 mV), SME0 1, SC 0 (learning optimized for a smart charger), FCC_LIMIT 0, FC_FOR_VDQ 0 (FC not needed for VDQ), IGNORE_SD 0 |
| Near Full | 200 mAh | qualification starts at RM ≥ 400 mAh (67 %): permissive. A start below full counts FCC − RM from the old FCC, so the cycle starts from charge termination |
| Battery Low % | 700 (TI default) | EDV2 at 7 %, 42 mAh of 600 added to the count |
| Learning Low Temp | 119 = 11.9 °C (TI default) | cycle at room temperature |
| OverLoad Current | 1500 mA (TI default) | not reached by this device |
| 3C/32, C/32 | 57 mA, 19 mA (computed) | discharge current at EDV2 must be ≥ 57 mA |
| Taper Current | 100 mA | termination needs AverageCurrent() < 100 mA twice in 40 s with V > 4200 − Taper Voltage; FC/TCA set by SOC Flag Config A (TI default `0x0C8C`: FCSETVCT, TCSETVCT) |
| Reserve Capacity | 0 (TI default) | FCC() = Learned FCC |
| DOD at EDV2, Qmax, Cycle Count | not in the TRM's Data Memory table | cannot be read or restored by address; see below |

**Findings.**

1. *Load at EDV2 (measured near full charge, see the device result below).*
   The earlier hypothesis that the screen-off system light-sleeps was wrong:
   M8-B turns the screen off after 30 s but makes no automatic light-sleep
   attempt (`attempts` 0), so the CPU stays active. On battery at about
   4.08 V the held load (screen on, CPU awake) drew 105–107 mA and the
   screen-off system 62–63 mA. 3C/32 is 57 mA, so the hold has about 85 %
   margin; screen off alone has under 10 %. The current at lower voltage
   near EDV2 is not measured. The tracker records the discharge current of
   every awake sample (`dsg_avg_ma`, `dsg_small_ma`, `dsg_large_ma`) and the
   current at EDV2.
2. *Compensated EDV with generic coefficients (assumption).* EDV2 comes from
   TI's default CEDV coefficients. The learned FCC is relative to that EDV2
   voltage; it can be off by the error of the generic model. Cell-specific
   coefficients need TI's characterization flow (not available).
3. *Near Full 200 mAh (proposal, not changed).* 40 mAh (the TI default ratio
   200/3000 scaled) would require the discharge to start near full. Changing
   it is a Data Memory write and needs its own decision; the procedure below
   gets the same effect by starting at charge termination.
4. *SC = 0 (open question).* The bit selects learning for a smart or an
   independent charger. The board's charger is not identified; if it is an
   independent charger, SC = 1 would apply. Not changed.
5. *Taper Current 100 mA (pending).* Whether the charger ends below 100 mA,
   and so whether FC/CSYNC happen, is a measurement (current during
   termination, FC in BatteryStatus).

**What must survive a battery disconnection.** The gauge keeps everything in
RAM, restored from OTP at its power-on reset. MCU restarts do not touch it.
Of the learned state, only Learned Full Charge Capacity (`0x929D`) has a
documented Data Memory address and a documented role (it sets
FullChargeCapacity(), TRM §4.9.36): it is kept. DOD at EDV2 (§4.9.37) only
improves the initial RM estimate until the next EDV2 and returns to the
firmware default consistent with Battery Low 7 %; Cycle Count (§4.9.38)
scales impedance through Age Factor, which is 0 by default; Qmax (§4.9.35)
is a configuration value, not learned by CEDV. None of the three has an
address in the TRM table, and addresses are never guessed, so they are not
stored.

**Learned FCC record (NVS).** With `CONFIG_ROUNDIE_GAUGE_DESIGN_CAPACITY_MAH`
> 0, namespace `gauge`, key `learned`, one 24-byte blob: magic `RGL1`,
version 1, design capacity, profile id (CRC-32 of the learning parameters:
Gauging Configuration, Design and Charging Voltage, Taper Current, Near Full,
Battery Low %, Learning Low Temp, OverLoad Current, Reserve Capacity, SOC
Flag Config A/B), learned FCC, sequence and CRC-32. A record is used only
when all of these match and the value lies in design/2 … design × 5/4. The
gauge cannot identify a cell: a different cell of the same capacity is not
detected and is corrected by its next learning cycle. A write happens only
after a tracked FCC update, when the gauge's Data Memory FCC belongs to the
configured Design Capacity and differs from a valid record by at least 1 % of
design; at most 4 attempts per boot. That bounds the writes to about one per
full discharge. A malformed record is ignored and replaced by the next valid
save; NVS is never erased.

**Restore after a gauge reset.** Unchanged trigger (`gauge_restore.c`,
Design Capacity back at 3000): the worker reads the gauge profile, takes the
record's FCC if valid for that profile, else 600, and writes FCC and Design
Capacity once. An MCU restart with Design Capacity 600 writes nothing, so a
learned FCC in the gauge is never replaced by 600.

**Monitoring (`tools/roundie_gauge.py learn`, read-only).** Phase
(`idle`, `qualified`, `edv2`), the last result (`updated`, `unchanged`,
`aborted`) with FCC before and after, the inferred cause (`charge`,
`low-temp`, `low-current`, `overload`, `gauge-reset`, `config-update`,
`not-qualified`, `unknown`), the start RM and FCC, the minimum temperature,
voltage/current/temperature/RM at EDV2, discharge current statistics,
3C/32, the hold state, the stored record, and the last sample (VDQ, EDV2
and FC are bits of `op` and `flags`). The cause is an inference from 5 s
samples: a short charge between samples, self-discharge accounting or EDV2
voltage margin are reported as `unknown`. After an MCU restart a discharge
already in progress is followed (`resumed` 1); one restarted after EDV2 is
not reported.

**Learning load hold (`learn-hold MINUTES`, `learn-stop`).** Holds a
diagnostics power inhibitor (CPU awake, display awake) renewed every 60 s
with a 120 s timeout, so it lapses within 2 min if the worker stops. It ends
by itself, and stays ended, at: an FCC update or `unchanged` result; an
abort; voltage below 3300 mV while discharging; BatteryStatus SYSDWN or FD;
temperature ≥ 45.0 °C; 30 s without a valid sample; the armed time (at most
24 h). Afterwards the normal screen timeout applies again (screen off after
30 s, CPU active, about 62 mA measured). It does not switch the battery off:
the device keeps drawing that current until USB is connected. The
`DISPLAY_AWAKE` kind only prevents the timeout; it does not turn on a screen
that is already off, so the procedure sends `screen-on` (RW1) right before
`learn-hold`.

**Battery-only telemetry (`learn-log`).** Every gauge sample (5 s while the
worker is awake) also goes to a 128-entry RAM ring (about 10 min awake, lost
on reset): time, voltage, current (`Current()` 0x0C, instantaneous),
temperature, RM, FCC, OperationStatus, BatteryStatus, hold state, whether
the hold held its inhibitor, and whether USB Serial/JTAG saw host SOF frames
(`usb_serial_jtag_is_connected()`). `learn-log` reads it after USB is
reconnected (RG1 `LEARN LOG n`, 10 entries per reply) and summarizes the
samples without a USB host, split by hold. The USB flag shows the host link,
not VBUS: it is evidence of battery operation together with the physical
unplugging and a negative current, not on its own.

#### Controlled learning cycle (procedure, not yet run)

Prerequisites, each separately approved: OTA of a firmware with this code
(and, for keeping the result across a battery disconnection, the restore
setting at 600), and the cycle itself. Room temperature 18–30 °C, the watch
on a non-flammable surface, observed at the start and the end.

**GO/NO-GO before unplugging USB (all must hold):**

1. OTA image VALID, BootGuard safe 0, no unexpected reset.
2. `status` format 3: Design Capacity 600 in `0x3C` and Data Memory, FCC
   plausible (300–750), Gauging Configuration `0x102A`, Near Full 200,
   Battery Low 700, Learning Low Temp 119, OverLoad 1500, Reserve 0, SOC Flag
   Config A `0x0C8C` / B `0x8C`, Taper 100, Charging Voltage 4200;
   OperationStatus UNSEALED, INITCOMP 1, CFGUPDATE 0. Any other value: NO-GO
   and review (the audit above assumed TI defaults).
3. Charge: on USB until the charge current falls to its termination level;
   aim for RM close to FCC. FC is **not** a GO condition: with FC_FOR_VDQ 0
   the gauge does not need FC to qualify a discharge, and FC never setting is
   itself a finding (Taper Current, charger termination), not a reason to
   force SoC. A start at lower RM still qualifies but leaves more of FCC to
   the gauge's own RM estimate (DCR starts at FCC − RM).
4. Temperature 15–40 °C (Learning Low Temp is 11.9 °C).
5. Load measurement on battery only: a short `learn-hold`, USB unplugged,
   then `learn-log` after reconnecting. Only samples with the USB flag 0
   count; a reading taken on USB power is not evidence of the battery
   current. Required: every held battery-only sample ≥ 70 mA (57 mA needed at
   EDV2 plus margin for the falling current at lower voltage). Below that:
   NO-GO, the load has to be raised first (for example brightness 100 %, a
   test load screen) and measured again. Reconnecting USB disqualifies a
   running discharge (`charge`).

**The key evidence that a discharge qualifies is VDQ itself**, not FC:
after unplugging, `learn-log`/`learn` must show OperationStatus VDQ (`op`
bit `0x0010`) set and phase `qualified`. VDQ sets only when the discharge
starts with RM ≥ FCC − Near Full (400 mAh with Near Full 200 and FCC 600).
It then stays valid only while TI's conditions hold (TRM §1.1.3): no charge
of 10 mAh or more, temperature above Learning Low Temp, estimated
self-discharge ≤ 256 mAh, voltage not below EDV2 − 256 mV, and at EDV2 a
discharge current of at least 3C/32 (57 mA) and below OverLoad (1500 mA).
VDQ not set within one minute of unplugging: stop, reconnect, NO-GO.

**Cycle.** `roundie_power.py screen-on`, then `learn-hold 600` (10 h), check
`hold` `running` and `inhibitor` 1, unplug USB. Expected duration: about
5–5.5 h to EDV2 at the measured 105 mA (estimate; the current at lower
voltage is not measured). Do not plug USB in until the screen turns dark by
itself (hold ended) unless aborting.

**Abort at any time** by connecting USB (the gauge disqualifies on charge; the
hold ends with `aborted`/`charge`): heat, swelling, smell, temperature above
45 °C, a reset or Safe Mode, or a hold running past 10 h.

**Over-discharge protection.** The hold ends at the FCC update right after
EDV2 (about 7 %, 42 mAh left by the gauge's estimate), at the latest at
3300 mV, SYSDWN or FD; then the screen times out but the system keeps
drawing about 62 mA (no automatic light sleep). No deep sleep or power-off
is used. About 42 mAh left at EDV2 lasts roughly 40 min at that current, so
the end of the cycle must be attended: connect USB within 15 min after the
screen goes dark. The cell's own protection circuit is not verified and is
not relied on.

**After reconnecting USB:** `learn` and `status`. Success: result `updated`
with cause `none`, new FCC within 300–750 and within the TRM's per-update
limits; with the restore setting, `stored` 1 with that value. `unchanged` with
cause `none` is a valid outcome (learned value equal). `unchanged` or
`aborted` with `low-current` means the load was too low: NO-GO for a repeat
until the load is raised. Record the EDV2 voltage/current/temperature and the
discharge current statistics.

**Then (separate approval):** one battery disconnection and reconnection to
verify the restore writes Design Capacity 600 and the stored FCC.

**Device result, short LEARN HOLD validation (2026-11-21, firmware
0.3.77-dev, one board).** OTA to `ota_0` PASS; 0.3.76 stayed VALID in
`ota_1` as the rollback image. Before the test `STATUS` format 3 showed the
values of the audit table, Design Capacity and FCC 600 in the registers and
Data Memory, `written` 0 (no restore write at boot), no NVS record. On USB:
`learn-stop` → hold `cancelled`, M8-B inhibitors 1 → 0; `learn-hold 1` →
`expired` after 60 s, inhibitors 0. On battery: `screen-on`, `learn-hold 3`,
USB unplugged 32 s later for 5 min 48 s; `learn-log` afterwards (samples
with the USB flag 0 only):

| Phase | Samples | Discharge current | Voltage | Temperature | RM |
|---|---|---|---|---|---|
| Hold (screen on, CPU awake) | 29 over 140 s | 105–107 mA, mean 106 | 4097 → 4073 mV | 25.7–25.9 °C | 577 → 573 |
| After expiry, before screen-off | 7 | 106–107 mA | | | |
| Screen off (normal M8-B) | 33 | 62–63 mA | 4087–4088 mV | 25.8–25.9 °C | 573 → 569 |

VDQ set in the first battery sample (OperationStatus `0x00A4` → `0x00B4`,
RM 577 ≥ FCC − Near Full = 400) without FC. The hold expired on time and
released its inhibitor; the screen turned off about 35 s later
(`screen_offs` 1 → 2). After reconnecting, charging at 220–400 mA cleared
VDQ after about 115 s (RM +10 mAh): the tracker reported `aborted`/`charge`.
No MCU reset (continuous sample times, BootGuard reset counters unchanged,
safe 0, failures 0), OTA VALID, no gauge write, Design Capacity and FCC
still 600, profile unchanged, NVS record absent and saves 0 (no FCC update
occurred). Not measured: the current at lower voltage and at EDV2.

## Manual board checklist

Run these only on an identified 1.85B unit after flashing is separately
authorized. Preserve the existing image and any user data first. Record
revision/markings, supply and battery state, date, firmware hash, monitor
port, logs, and each pass/failure. Do not erase partitions or change eFuses.

1. Photograph both PCB sides. Compare the printed revision and LCD/TE R5
   population with Rev1.1. Stop pin validation if the revision differs and
   obtain its schematic.
2. Perform **10 cold starts** with USB power; repeat with battery where
   available. Check boot completion, no panics, and stable heap diagnostics.
   Record detected 16 MB flash and 8 MB Octal PSRAM, free and largest blocks.
3. Confirm the three 7-bit I²C probes at 0x15, 0x51, and 0x55, with no bus
   lockups. Record other expected device addresses separately with a safe
   scan if needed; do not configure the audio codecs in this milestone.
4. Check RTC time reads at boot, voltage-low flag, and behavior after power
   removal under USB-only, battery-only, and combined power. Time retention
   is a separate outcome from a successful read.
5. Check gauge voltage and status reads under USB-only and battery power.
   Compare voltage with an external meter when safe. Do not infer state of
   charge or calibration accuracy from an ACK or voltage value.
6. Inspect LCD orientation and touch coordinate mapping. Tap center and
   four reachable edge regions, then release each touch. The screen and log
   must show coherent press/release and no inverted or stuck coordinates.
7. Tap each color swatch and inspect the full red, green, blue, and white
   fields for channel order, missing rows, artifacts, and rotation. Return
   by tapping the field. Observe spinner motion and GUI response.
8. Run at least **30 minutes** of interaction and changes through the
   backlight button. Record
   LCD/I²C transfer errors, GUI stalls, heap trend, and any reboot. The gate
   requires zero logged transfer errors and stable visual output.
9. With the image running, exercise manual ROM Download using the
   [physically tested procedure](rom-recovery.md#physically-tested-entry-procedure).
   On this battery-connected unit, hold PWR for about two seconds with USB
   attached, disconnect USB, confirm the screen stays dark, then hold BOOT
   during USB reconnection and release it after about two seconds. PWR alone
   with USB attached was not an established power cycle. Use a read-only
   chip-identity command such as the following, replacing `PORT` with the
   newly enumerated serial port:

   ```sh
   python -m esptool --chip esp32s3 --no-stub --before no-reset --after no-reset -p PORT chip-id
   ```

   Record enumeration and console/debug path. Repeat
   with battery if that use is intended. Do not write or erase flash.

## Known blockers and pending experiments

- **Device gate open:** USB-only testing established boot, LCD variant 2,
  touch, and memory detection. Subsequent tests established valid RTC time,
  software restart retention and one approximately 90-minute battery-backed
  hardware PWR-off retention pass, plus one manual ROM recovery/normal-return
  cycle with the battery connected. Fuel-gauge accuracy, active battery-only
  operation, ten cold starts, latest-image LCD/touch
  inspection and the 30-minute visual/interaction criterion remain open.
  Printed PCB revision and R5/TE population could not be inspected.
- **LCD identity:** the tested unit returned `00 02 7f 7f`. The other
  documented ID `00 7f 7f 7f` remains source-supported but untested here.
- **LCD timing/buffering:** measure 3 MHz flush latency, GUI response, DMA
  safety, and tearing during the 30-minute test; only then choose clock,
  strip height, and whether to move to two buffers. Check R5/TE electrically
  before considering synchronization.
- **Power/recovery:** the [battery-connected manual ROM route](rom-recovery.md)
  and normal return passed on 2026-10-18 through USB Serial/JTAG. PWR alone
  with USB attached did not visibly switch the unit off; USB removal was
  needed in the tested sequence. Other supply/latch states and later
  TinyUSB/USB-OTG takeover remain untested; no power-rail measurement was made.
- **RTC/gauge:** The USB-only RTC intermittently NACKed and the gauge never
  ACKed without a battery in earlier tests. Later USB-plus-battery tests
  established valid RTC time with OS=0, and the user confirmed battery-backed
  PWR-off retention. Gauge SoC remains an uncalibrated estimate. Active
  battery-only operation and independent gauge voltage/temperature/current/
  SoC accuracy checks remain open. If intermittent RTC behavior persists
  under controlled power, measure I²C levels rather than assuming a
  software-only cause.

## RTC host synchronization and retention addendum, 2026-10-18

This addendum supersedes the earlier statements that no trusted-time transport
exists and RTC setting/OS clearing are untested. It does not close the broader
Milestone 1 physical interaction, recovery, power, or endurance gate.

**Implementation decision:** PCF85063 stores UTC, restricted to calendar
years 2000–2099. A versioned line protocol uses the existing secondary USB
Serial/JTAG console through the standard IDF interrupt driver. One existing
sensor worker serializes RTC operations; LVGL remains on the GUI task.
`CONFIG_ROUNDIE_DEV_TIME_SYNC` is explicitly enabled for this diagnostic image
and must be disabled for production. It permits unauthenticated physical-host
time setting and an explicit software restart for retention testing.

**Source-verified:** Complete calendar writes include OS=0 in Seconds and do
not modify control/alarm/gauge configuration. Successful SET requires valid
immediate readback, a measured 1.2-second progression check with OS clear,
then system-clock initialization. Failed SET stays explicitly unsynchronized
until another successful SET during that boot. RTC/system tolerance is one
second; host verification allows three seconds and bounds serial latency.
No timezone changes or build-time synchronization are used. An old conversion
bug affecting December's day count was fixed and covered across all supported
dates. See [RTC protocol, dependencies, CLI, validation rules and manual test
steps](rtc-time-sync.md) for the full focused record and NXP/IDF sources.

On CachyOS, install `python-pyserial` (`sudo pacman -S python python-pyserial`).
Use the actual device path, preferably its stable by-id symlink:

```sh
PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_28:84:85:87:98:7C-if00
python tools/roundie_time.py --port "$PORT" status
python tools/roundie_time.py --port "$PORT" sync
python tools/roundie_time.py --port "$PORT" verify
python tools/roundie_time.py --port "$PORT" verify --restart
```

`status` reports invalid time without writing RTC; `sync` explicitly writes
host UTC and confirms an independent GET; plain `verify` is read-only for RTC.
`verify --restart` explicitly asks firmware to call `esp_restart()` and
reconnects within a bounded retry budget. DTR/RTS both asserted avoided
reset-on-open on this Linux USB Serial/JTAG device; forcing them low caused
resets in an earlier failed candidate. Logs cannot match a tool request unless
they contain its strict protocol prefix and identifier. Exit 2 indicates a
transport/permission/deadline failure, exit 3 a time-validation failure.

| Evidence tier | Result and limitation |
|---|---|
| Host tests | C11 and C23 codec/parser suites exited 0, including four seconds on every supported date and negative framing/calendar cases. All 15 Python protocol/timeout/error/reconnect tests passed. Simulated I²C error replies do not establish hardware fault recovery. |
| Target compilation | Pinned ESP-IDF v6.1 build exited 0. App 587,088 B, SHA-256 `f9fa9d6b8584fd1b1fb3e95d7aea48c29ff3655a774d786a4e0f308c80ee40cf`; ELF SHA-256 `27c13741c6ffc7e6e44be7948eda1068c7b495d88f009e95b110e924a89823f9`. Compilation alone is not device evidence. |
| Guarded app deployment | Verified the connected ESP32-S3 rev 0.2, MAC `28:84:85:87:98:7c`, 16 MB Flash and 8 MB PSRAM. `tools/flash_bringup.py --write` repeated live layout/bootloader/backup checks before each candidate write; only factory application offset `0x20000` was written, and esptool image-hash verification passed. No bootloader, table, NVS, OTA metadata, gauge calibration or other data write was requested. |
| Physical device: synchronization | Initial RTC invalid with OS=1 and no wall synchronization. Explicit SET cleared OS and verified progression/readback. Final host sync at 09:02:07 UTC returned RTC/system `1792314127`, RTT 1.221 s, approximate host difference -0.07 s. Independent GET and plain `verify` passed. |
| Physical device: Test A | RTC advanced `1792314258` → `1792314263` (+5 s) over 5.0105 s monotonic elapsed; OS remained 0. GET RTT 10.3 ms; system time agreed within one second. |
| Physical device: Test B | CLI `verify --restart` passed (+2 s RTC over approximately 1.87 s). Final same-connection capture separately recorded `RTC_SW_CPU_RST`, valid RTC startup/OS=0 and system synchronization; RTC `1792314266` → `1792314270`, wall `1792314270` afterward. This proves software restart retention only. |
| Physical device: rejection paths | Extra argument, negative timestamp, both range boundaries, oversized integer, overlong frame and expired partial frame returned expected `BAD_COMMAND`/`RANGE`/`FRAME`. A fresh GET afterward was valid with OS=0. No malformed command changed time. |
| Physical device: existing diagnostics | Loaded ELF prefix `27c13741c`, LCD variant 2, CST816S driver OK, RTC/gauge ACK, battery present, 3971 mV and estimated SoC 84%. No panic/watchdog failure in short restart captures. New GUI labels, spinner/touch appearance and battery accuracy were not physically inspected. |
| Physical device: Test C | **PASSED — approximately 90 minutes, one user-performed physical test.** USB disconnected, battery retained, hardware PWR off; after restoration, `status` without resync showed valid RTC/OS=0 and restored system time. [Exact output and limits](rtc-time-sync.md#test-c--physical-retention-passed). Offset -1.27 s at observation is not calibrated drift. |

Earlier checkpoint descriptions of OS=1 remain historical evidence. The RTC
is now valid on the tested USB-plus-battery unit. Brownout/complete RTC power
loss, physical I²C failure injection, long-term drift, electrical battery
comparison and the existing 30-minute visual gate remain pending. No later
milestone was implemented.

## Remaining Milestone 1 validation gates

Milestone 1 is **OPEN**. The user-performed retention test closes the
battery-backed RTC hardware PWR-off check under its recorded conditions.
The 2026-10-18 manual ROM test also closes the basic ROM recovery gate:
BOOT held at USB reconnection after PWR/USB removal, direct ROM identification
without stub or automatic reset, and normal Roundie/RTC restoration.
See the [recovery procedure and command results](rom-recovery.md). Other
power conditions and TinyUSB takeover remain outside this result.

The remaining acceptance work includes:

- **Cold-start reliability:** the documented ten controlled cold starts with
  boot/error/memory records; one retention cycle is not this series.
- **Latest-image LCD/touch:** UTC/RTC and battery labels, orientation, color
  fields, spinner, center/four-edge press/release and controls after idle.
  Earlier-image passes remain recorded above.
- **GUI endurance:** at least 30 minutes of interaction/backlight changes,
  stable visual output and heap trend, with zero logged transfer errors.
- **Battery/gauge:** active battery-only diagnostics and independent checks
  of voltage, temperature, signed current, profile/calibration and SoC
  accuracy. RTC retention does not validate these measurements.

PCB revision/markings remain unconfirmed. Brownout, depleted/disconnected
battery retention, long-term RTC accuracy and other pending experiments keep
their existing separate scope; this result does not resolve them or authorize
Milestone 2.

The [manual board checklist](#manual-board-checklist) remains the procedure
for the separate outstanding checks. The ROM result does not complete those
checks or authorize an endurance run or another milestone.

## Milestone 3E.4 — bounded dual-buffer/40 MHz acceptance, 2026-10-20

One genuine RO1 OTA replaced inactive B 0.3.7 with experimental 0.3.9;
known-good A 0.3.8 was preserved. B passed NEW/PENDING_VERIFY, core self-test,
VALID and BootGuard STABLE. Programmed LCD telemetry was source 80 MHz,
pre=1/n=2/high=1/direct=0: **40 MHz actual**. Slow ID read, panel initialization,
two internal DMA-capable 20-line LVGL PARTIAL buffers and all other functional
settings were unchanged. All 82 host regressions passed; target compilation,
image/layout checks and physical evidence remain distinct.

Five Hello/UI cycles per firmware, fifteen injected counter updates per
firmware, timer/Exit cleanup and settled idle checks passed without observed
DMA errors/timeouts, resource/heap growth or idle redraws. Handler medians
**82.282/82.105 ms** gave only **0.215%** gain. Back, partial updates and
readiness did not demonstrate a consistent gain. The user reported a modest
visible improvement, separately from pending detailed pixel/tearing/finger
checks and unmeasured electrical margins. Engineering selection therefore
remains **A 0.3.8 / 26.667 MHz / dual buffers**; 0.3.9 is experimental.

The [3E.4 report](display-3e4-acceptance.md) records guarded selection and the
independent final slot status, exact artifact hashes, all timing samples and
remaining limitations. Original 0.3.7 remains archived externally; it is no
longer an installed OTA fallback. No bootloader/table change, format, raw
update, full-Flash readback, endurance test or Milestone 3F work occurred.
