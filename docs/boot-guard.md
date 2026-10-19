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

Physical results: **PENDING** until the bounded sequence in
[safe-mode.md](safe-mode.md) is recorded. Real panic/watchdog, brownout,
power interruption, deep-sleep retention and visual/touch checks remain
PENDING; controlled software resets do not prove those reset classes.
BootGuard cannot recover code that fails before its own early check, an
unbootable firmware, broken USB hardware, or inability to allocate its system
worker. Manually usable ROM Download remains the final recovery path.

The preceding two-second RTC/system-clock discrepancy at long uptime remains
open. No thresholds, periodic discipline or time-setting policy were changed.
