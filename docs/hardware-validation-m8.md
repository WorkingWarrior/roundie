# M8 unattended hardware validation (variant A)

This is the record of the M8 unattended validation on 2026-11-19. It checked a
Waveshare ESP32-S3 1.85B over USB, with no touch, button press, cable change or
manual restart. The runner and its usage are in
[tools/hardware_validation/README.md](../tools/hardware_validation/README.md).

Evidence is outside Git in `roundie-backups/m8-2026-11-19/`. Each run has a
`report.json`, `report.md`, `serial.log` and the runner's stdout.

**Status: the unattended part passed on firmware 0.3.54. M8 hardware validation
is not complete.** Everything that needs a person or an instrument is PENDING
(see the PENDING section below), and the 1.75C kit has not been run.

## Device

- **Verified fact:** Waveshare 1.85B, USB Serial/JTAG serial `28:84:85:87:98:7C`.
  It was opened through `/dev/serial/by-id/…-28:84:85:87:98:7C-if00`.
- **Identity:** checked through protocols (board RTC presence → `1.85b`,
  profile `rep`), never through `ttyACM` order. Discovery did not reset the
  board or enter ROM Download.

## Firmware and OTA

| | Before | After |
|---|---|---|
| Start of day | 0.3.51 in `ota_1`, VALID | — |
| run3 | 0.3.51 in `ota_1` | 0.3.53 in `ota_0`, VALID, self-tested; 0.3.51 VALID in `ota_1` |
| run4 | 0.3.53 in `ota_0` | 0.3.54 in `ota_1`, VALID, self-tested; 0.3.53 VALID in `ota_0` |

**0.3.54 image:**

- `roundie_rep.bin`: 1,135,120 B, SHA-256 `22e77b99…ff09a8`.
- ELF: SHA-256 `fd3779e3…f02a5`.
- Built in the pinned IDF image with 0 warnings. Its partition table is
  byte-identical to the device's.

**OTA path:**

- Updates went through RO1 only, after the preflight passed:
  - self-tested VALID image;
  - OTA idle;
  - not in Safe Mode;
  - a VALID rollback image in the other slot.
- The bootloader, partition table, NVS and LittleFS were not written.
- Rollback stays enabled.

## Runs

| Run | Firmware | Outcome | Notes |
|---|---|---|---|
| run0 | 0.3.51 | PASS | read-only `core_status` |
| run1 | 0.3.51 | FAIL | Runner development. Steps raced the launcher and an early RT1 read before self-test. Runner fixed; no firmware finding. |
| run2 | 0.3.51 | FAIL | Device panic during `round_dialog`; the board entered Safe Mode (see the arc slider finding) |
| run3 | 0.3.53 | FAIL | Arc fix confirmed. `elf_event` firmware bug (clock subscription finding); two runner bugs |
| run4 | 0.3.54 | **PASS** | All 14 automated steps PASS; 0 faults, 0 unexpected resets |

## run4 results (0.3.54)

UI input marked INJECTED went through the RE1 UI bridge or RP1. It says nothing
about the CST816S touch panel.

| Step | Status | Evidence |
|---|---|---|
| preflight | PASS | VALID, self-tested, OTA idle, not safe, rollback image VALID |
| ota | PASS | 0.3.53 → 0.3.54, see above |
| core_status | PASS | protocols answer, no GUI/UI errors |
| rtc | PASS | See the rtc details below |
| launcher | PASS | INJECTED (RP1 EXIT/SELECT): list, refresh, home relaunch |
| elf_hello | PASS | ABI 1, result 42, memory before = after |
| elf_event | PASS | ABI 2, 1 event and 1 user event, app_result 0, no error, 0 dropped/rejected |
| elf_ui | PASS | ABI 2 UI example loaded, updated and unloaded |
| elf_watch | PASS | home watch application, clock events |
| elf_stopwatch | PASS | stopwatch application |
| round_dialog | PASS | See the round_dialog details below |
| bad_package | PASS | See the bad_package details below |
| cycles | PASS | See the cycles details below |
| screen | PASS | Screen off/on through RW1; previous state restored |
| light_sleep | PASS | See the light_sleep details below |
| restart | PASS | See the restart details below |

**rtc:**

- Before: offset −0.22 s vs the host, RTC−system 1 s.
- Set from host UTC; after: −0.44 s.
- Time zone `CET-1CEST,M3.5.0,M10.5.0/3` restored.

**round_dialog** (INJECTED):

- 10 dialog rounds, 32 inputs injected and 32 delivered to the executor.
- Each round changed the application's level.
- Cancel restored "Level: 40"; an out-of-range input was rejected (1).
- 0 rejected deliveries; maximum input latency 10.3 ms.
- No panic. Unloaded cleanly, LVGL bytes 0.

**bad_package:**

- A corrupt ELF was refused at upload END (code 258, magic).
- An unknown digest was refused.
- The good application ran afterwards (result 42).

**cycles:**

- 20 load/unload cycles. All memory figures stable:
  - internal free 156,311 B;
  - largest internal block 81,920 B;
  - PSRAM free 7,329,228 B (minimum 7,328,332 B);
  - LVGL 0 B.

**light_sleep** (timer wake only):

- One sleep of 4,991 ms, sleep_error 0.
- Touch and BOOT wake PENDING.

**restart:**

- Software restart through RS1. Uptime dropped, which was expected.
- Time was valid on the first read and advanced 3 s while the host measured
  3.78 s.
- Home watch ran again; 0.3.54 stayed VALID.

**Monitoring:**

- 0 unexpected resets, 0 serial faults (panic, watchdog, assert) and 0 BootGuard
  or Safe Mode notices.
- The task count is not exposed by any diagnostic protocol, so task leaks are
  not checked.

**Restore:** the time zone rule, screen state and home application were
restored. The user's time state now follows host UTC; the earlier offset is
recorded in `report.json`.

## Findings

### Arc slider hit-tested before layout (fixed in 0.3.53)

**Verified fact (run2, 0.3.51):** the device panicked with
`IntegerDivideByZero` in `lv_atan2` during the `round_dialog` step. The
backtrace was decoded with `addr2line` against the 0.3.51 ELF.

**Cause:**

- A new LVGL object has zero size until the next layout refresh.
- An input read before that refresh hit-tested the zero-radius arc.
- At the arc's centre, the advanced hit test calls `lv_atan2(0, 0)`, which
  divides by zero.

**Hypothesis:** the injected input point was (0, 0) after the software restart
with no touch since boot. This is consistent with the evidence but not proven.

**Fix:**

- `components/roundie_app_ui/render.c` calls `lv_obj_update_layout()` on the
  slider when it is created.
- `tests/test_app_round.c` hit-tests the root origin before layout. It fails
  with an FPE on the old code and passes with the fix.

**Recovery:**

- BootGuard put the board in Safe Mode.
- `roundie_safe.py reboot` (RS1 REBOOT) returned it to normal, with no user
  action.

### Clock subscription leaked into ABI 2 applications (fixed in 0.3.54)

**Verified fact (run3, 0.3.53):** the event example (ABI 2) failed with
app_result −4 and error `callback-event`, with 1 event and 0 user events. It ran
after the watch application (ABI 3, subscribed to clock events).

**Cause:**

- `components/roundie_elf/manager.c` reset the clock subscription flags only
  when an ABI 3 application started.
- An ABI 2 application therefore inherited it and received
  `REP3_EVENT_CLOCK`, a type it does not know.

**Fix:**

- The flags are now reset for every ABI.
- `tests/test_elf.c` (`clock_leak_tests`) fails without the fix and passes with
  it.
- run4 confirmed the fix on the device.

### Runner fixes (no firmware change)

- After boot, the launcher shows its list for about 14 s before it starts the
  home application. The runner now waits for that instead of racing it.
- A corrupt package can be refused at upload END, not only at START; both count
  as refused.

## VHL comparison

The desktop/VHL build compiles the same `render.c` and `manager.c`, so both
fixes apply there too. The desktop host tests pass with them.

**Confirmed discrepancies that need a model change:** none. The model was not
changed.

**Observations, not yet treated as discrepancies:**

- **Boot timing.** The board shows the launcher LIST for about 14 s before home
  autostarts. Whether VHL reproduces this delay was not compared.
- **Early time read.** In run1, an RT1 read before self-test returned
  `RTC_READ` (261). After a software restart on 0.3.54, time was valid on the
  first read.
- **RTC drift.** The device clock started the day 1,385 s off the host; this is
  not a measured drift rate. The VHL RTC has no drift model.
  - **Pending experiment:** measure drift over a known interval before adding
    one.

## 1.75C kit

The runner supports `--board 1.75c`:

- The board must report no RTC.
- The RTC step checks only the system clock.
- Light sleep stays off unless it is requested.

**Not run on a 1.75C. No 1.75C data exists.**

## PENDING (needs a person or instrument)

- Finger touch on the list, slider, edge button, dialog and Exit swipe.
- What the panel shows.
- Audio audibility.
- BOOT button.
- Touch and BOOT wake from light sleep.
- Current draw.
- Battery behaviour.
- A run on the 1.75C.

**Not exercised by design:**

- deep sleep;
- ROM Download;
- erasing NVS, LittleFS or Flash.
