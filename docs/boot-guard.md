# Milestone 2C.2 — BootGuard

## Architecture and policy

**Architectural decision, 2026-10-19 (REP §§18–19, 23, 25):** BootGuard
recovers an already confirmed firmware through Safe Mode in that firmware.
It never invalidates a VALID image or automatically selects another slot.
The existing OTAManager remains authoritative for PENDING_VERIFY confirmation
and rejection. No bootloader, partition-table or security change is involved.

The actual C engine is `components/roundie_boot_guard/boot_guard.c`.
It runs before NVS, filesystem, board and GUI initialization. Its stages are
START, CORE, GUI, OPERATIONAL and STABLE. The OTA snapshot separately records
whether the actual core self-test ran and passed. Entering `app_main()` or
starting recovery USB does not confirm a new image.

Three consecutive incomplete boots with probable software failure enter
Safe Mode. An incomplete attempt ends before STABLE. The operational window
defaults to 10 seconds (`ROUNDIE_BOOT_STABILITY_SECONDS`, 5–30 seconds).
Normal operation becomes operational only after the core self-test passes;
recovery becomes operational when the worker has processed the core result,
including a failed GUI result. Stability clears normal failure history and
the latch after a successful explicit normal trial. Stable recovery retains
its reason/latch and does not restart to test whether a fault disappeared.

| Reset classification | Incomplete previous boot |
|---|---|
| Panic / CPU lockup | Increment |
| Interrupt/task/other watchdog | Increment |
| Software without a planned marker | Increment |
| Planned diagnostic/OTA/recovery software restart | Do not increment |
| Power-on | Discard temporary RTC history; consult NVS latch |
| Brownout / power glitch | Discard temporary history; never count as a crash |
| Deep-sleep wake | No increment; no wake-source support is claimed |
| USB/JTAG/external/SDIO | No increment |
| Unknown / eFuse reset | Ambiguous, no increment |

**Runtime recovery decision:** an unplanned panic/watchdog/software reset
after STABLE enters Safe Mode on the next boot with reason=core. It does not
increment the incomplete-startup counter or invalidate the image. Recovering
on the first clear runtime software fault prevents recurring late failures
from evading the short startup counter. Planned resets and ambiguous/power
resets retain the classification above. A core recovery reason identifies a
lost operational session, not proof of defective firmware or a future ELF
application; application-specific attribution is deferred to its milestone.

**Failed-session attribution, 2026-11-02:** BootGuard's retained record (now
version 2) holds the package ID from launch until the runtime has unloaded it. After
a panic, watchdog or unplanned software reset, that ID appears for one boot as
`fault_app` in RS1 and in a boot log warning. It names what was loaded, not the
cause, and does not change Safe Mode policy. Power loss and brownout discard it, and
RE1 development images have no ID.

**System worker supervision, 2026-11-02:** the worker that serves USB recovery,
OTA and packages is now on the task watchdog (10 s, panic), reset once per loop.
A stall resets into the runtime recovery rule above. Its largest measured
request is 294 ms (OTA END, an earlier 668 KB image); the worst case for a full
slot or a slow Flash erase is unmeasured.

**Device test, 2026-11-02, under XIP:** diagnostic `0.3.19-boot-test` adds RS1 `HANG
<exact-running-ELF>`, accepted only when normal and STABLE. It stops the worker's
watchdog feed while the scheduler keeps running. Two runs, one each:

| Case | Result |
|---|---|
| Roundie UI loaded | Task watchdog named `system_worker` after 10.0 s, reset class 4 (raw 6), Safe Mode reason=core, `fault_app=org.roundie.ui` |
| No package loaded | Same reset and Safe Mode, `fault_app` empty |

RS1 answered after each reset, both images stayed VALID, and REBOOT returned to
normal. This verifies one real task-watchdog panic path only. A hung native callback,
other panic types, the display during the stall and repeated faults remain PENDING.
Evidence is in `roundie-backups/hang-test-2026-11-02/`, outside Git. Normal 0.3.19
then replaced the test image; five application cycles passed with 0 display errors,
25.0 FPS on the UI and 20.0 on the launcher.

Failure after a completed stable normal boot is not itself a startup loop.
Failure during an explicit normal trial returns directly to Safe Mode on the
next boot without granting another automatic trial. A fault while already in
Safe Mode suppresses its optional display on the next retained boot, including
a fault after recovery stability; the USB worker remains independent.

## State and persistence

**Architectural decision:** use a private `RTC_NOINIT_ATTR` record for
same-image warm-reset history, plus one NVS blob for the Safe Mode latch.
The record has magic, version, CRC32, full ELF identity, previous stage,
failure count, reason, planned-restart marker, one-shot trial token,
bounded development-test remainder and GUI suppression flag. Invalid fields,
CRC, schema or a different ELF identity reset temporary history. There is no
external application yet, so there is no invented application identifier.

This deliberately does not use `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC`:
the installed bootloader is frozen and has no common reserved record ABI.
REP §19.4 describes that option as a candidate. A same-image record needs no
cross-build address promise; NVS carries the cross-build latch. The broader
RTC reservation and brownout retention questions in §25 remain open.

**Verified source facts:** pinned ESP-IDF v6.1 commit
`fff9895c82d744c7237be8847347bdd1b07c6643`,
`components/esp_system/ld/esp32s3/sections.ld.in` leaves `.rtc_noinit` NOLOAD.
With this configuration RTC data is in RTC SLOW memory. The reset classifier
uses the pinned `esp_system/port/soc/esp32s3/reset_reason.c` public enum.
[IDF memory documentation](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/memory-types.html)
documents RTC no-init/deep-sleep behavior. Physical software-reset retention
is a separate acceptance test. No retention after power removal is assumed.

The NVS namespace `boot_guard`, key `latch`, contains 16 bytes: magic,
version, reason and CRC32. An absent record is normal. A malformed record
enters recovery and can be explicitly cleared. Unavailable NVS enters
recovery without erasing/formatting anything; clearing/trial requests fail
until persistence is available. A latch commit failure stays in recovery
and blocks further automatic write attempts for that boot.

No failure counter or stage is written to Flash on normal boots or periodic
iterations. NVS may create its namespace once; latch changes are committed
once, and unchanged values produce no writes. Safe entry, explicit CLEAR,
and successful trial completion are the sparse write points. NVS retains
its own wear levelling; this is not an endurance measurement. CRC is
corruption detection, not authentication or power-fail immunity for RTC RAM.

## Ownership and startup

1. Early engine/reset/ELF check; initialize and inspect OTAManager.
2. Non-destructive NVS initialization and latch read.
3. Normal mode only: existing StorageManager mount/diagnostic. Failure enters
   recovery; future safe boots skip that filesystem entirely.
4. Start the existing serial/sensor worker before board/LVGL. The worker owns
   USB RX, OTAManager and latch commits. It performs no sensor polling in Safe
   Mode. Board samples are published only after complete setup.
5. GUI task owns the existing LCD/LVGL path and watchdog. Recovery uses the
   existing BSP's LCD-only initialization, with no I2C, touch, RTC or gauge
   setup. A suspected failing GUI can be skipped entirely.
6. Real memory/configuration/GUI/DMA evidence completes the OTA self-test.
   The separate stability window then completes BootGuard startup.

The engine serializes its bounded in-memory transitions with an IDF critical
section. NVS and OTA Flash calls occur outside it and outside GUI callbacks.
Before the worker starts the boot owner exclusively owns initialization.
Headless recovery keeps callback storage alive and never reuses an in-flight
DMA buffer. Existing DMA timeout resets remain unplanned failures; a retained
failure in GUI stage suppresses GUI after the threshold.

## Verification and limitations

Host tests execute the actual engine, NVS backend and RS1 controller with
mocked IDF APIs, in normal and diagnostic compilations under ASan/UBSan.
They cover classification, stable completion, planned restart, threshold,
latch/trial persistence and failed trial, corrupted/missing records, NVS
errors, optional storage/GUI decisions, headless recovery, PENDING_VERIFY
guards and disabled production faults. Existing actual OTAManager tests
separately check pending confirmation/rejection, VALID preservation and
alternate-slot verification/state rejection. They are not board tests.

**Verified host/target facts, 2026-10-19:** 50 Python unittest cases passed
in the exported pinned IDF environment; the C engine/backend/controller,
OTAManager, boot-gate and storage tests use ASan/UBSan where applicable.
The existing boot-gate matrix covers 112 normal/armed combinations.
Normal REP 0.2.2 and separate unarmed-by-default 0.2.2-boot-test both built
with the pinned v6.1 toolchain. Normal image size is 680,384 bytes; diagnostic
size is 680,704 bytes, both within 3,670,016-byte slots. Image checksum and
validation hash passed `esptool image-info`; the layout validator passed.
No compiler warning suppression or managed source change was added.
The later post-STABLE recovery correction passed the same 50-test suite with
additional late-fault assertions, then built as normal 0.2.3 (680,400 bytes).
Its exact identity and genuine OTA/confirmation are recorded in safe-mode.md;
the corrected late-fault branch itself has actual-engine host evidence.

**Verified physical:** genuine normal OTA/confirmation, explicit safe entry,
retained software-reset state, normal trial, guarded alternate selection and
one three-reset incomplete-startup experiment in an already VALID diagnostic
image passed. The count reached 3 and stable Safe Mode; both images remained
VALID and recovery returned to normal with cleared history. Exact logs,
image identities and restoration status are in [safe-mode.md](safe-mode.md).
These are controlled software resets. Brownout, power interruption, deep-sleep
retention and visual/touch checks remain PENDING; controlled software resets do
not prove those reset classes. One real task-watchdog panic is recorded above.
BootGuard cannot recover code that fails before its own early check, an
unbootable firmware, broken USB hardware, or inability to allocate its system
worker. Manually usable ROM Download remains the final recovery path.

The preceding two-second RTC/system-clock discrepancy at long uptime remains
open. No thresholds, periodic discipline or time-setting policy were changed.
