# Stage 8 — PowerManager

## Scope and decisions

**Architectural decision, 2026-11-03 (REP §17, §23 stage 8):** the first power
milestone provides automatic screen off, a power menu on BOOT and explicit light
sleep. Deep sleep, software power-off and application sleep inhibitors are not
part of it. `CONFIG_ROUNDIE_POWER` enables it; the REP profile turns it on, and
the bring-up profile keeps its earlier behavior.

| State | Entry | Exit |
|---|---|---|
| ACTIVE | Boot, touch, BOOT, wake from light sleep by touch or BOOT | Screen timeout, menu Screen off, RW1 |
| SCREEN_OFF | 30 s without touch or BOOT (`ROUNDIE_POWER_SCREEN_TIMEOUT_SECONDS`, 0 disables), menu, RW1, timer wake | Touch or BOOT |
| LIGHT_SLEEP | Menu Sleep, RW1 `SLEEP <s>` | Touch INT, BOOT, safety timer |

`POWER_OFF` is not represented. Rev1.1 places PWR in a separate latch with no MCU
signal ([hardware audit](hardware-audit.md)), so the menu has no Power off entry,
and nothing presents sleep as power-off. `DEEP_SLEEP` is deferred: a deep-sleep wake
is a full boot, and whether a low GPIO0 at that wake changes the boot mode is an
open question (REP §17.4, §25 item 22).

**Screen off** is DISPOFF plus backlight duty 0. Panel memory stays valid and LVGL
keeps rendering, so applications see no change. The touch that turns the screen
on, and every touch while it is dark, never reaches LVGL; delivery resumes after
that touch is released. After a sleep wake the waking press may be read late. The
hold therefore lasts until a press is seen and released, or for 500 ms when none
is seen, as after a BOOT wake.

**Power menu.** BOOT (GPIO0) is configured as an input with pull-up, never as an
output, so it stays the ROM Download strap. A debounced press (30 ms) opens or
closes the menu while the screen is on, and only turns the screen on while it is
off. The menu is a full-screen backdrop on LVGL's top layer, above applications
and the launcher's Exit control. Its five 200 × 42 targets (Sleep, Screen off,
Restart, Safe Mode, Cancel) lie between y=62 and y=304, where the narrowest chord
of the round panel is about 260 px. Restart and Safe Mode run in the system worker.
They are refused while an application, staging or package transaction owns the
runtime, while OTA is not idle, or outside a VALID image. The menu then shows
"Exit the app first" or "Busy". Safe Mode writes the same persistent request as
RS1 `REQUEST`. Safe Mode itself never binds the PowerManager: its display stays on
and BOOT has no function there.

**Light sleep** is executed by the system worker, the sole owner of OTA, packages,
the NVS latch and USB recovery. No Flash writer is therefore in progress when it
sleeps. The sequence is:

1. A menu tap or RW1 posts a request.
2. The worker refuses it outside stable normal mode, while OTA is not idle, or
   while the runtime is busy (an application loaded, staging or a package
   transaction).
3. The GUI closes the menu and waits until every submitted LCD transfer has
   completed (the ISR completion count equals the submission count) and the touch
   has been released for 300 ms. It then turns the screen off and parks: no LVGL
   or LCD work, watchdog fed every 10 ms.
4. The worker checks again. Touch INT or BOOT still low after one second aborts
   the sleep.
5. Otherwise it arms low-level GPIO wake on GPIO4 (touch INT, its edge interrupt
   disabled meanwhile) and GPIO0, plus the safety timer, and calls
   `esp_light_sleep_start()`.
6. On wake it restores the touch edge interrupt. It requests a touch read only if
   INT is still low: after a BOOT or timer wake, CST816S may be in auto-sleep, and a
   failed read would start the existing 5 s touch retry backoff.
7. A touch or BOOT wake turns the screen on. A timer-only wake leaves it off, with
   USB recovery back.

The safety timer (`ROUNDIE_POWER_SLEEP_SAFETY_SECONDS`, default 300 s) keeps a
development board reachable if GPIO wake does not work. It is a development choice
pending the wake tests below, not a product policy.

**Inhibitors.** REP §17.3 describes owned inhibitor handles with timeouts released
by ResourceManager. Their only required client is an application, which would need
a new ABI entry. This milestone therefore uses conditions instead of handles:

- the refusals above block sleep;
- an active GUI benchmark postpones the screen timeout.

The ABI is unchanged. Application inhibitors and the final policy remain open
(REP §25 item 25).

**Diagnostics.** `CONFIG_ROUNDIE_DEV_POWER` adds RW1 on the shared USB framer:
`STATUS`, `SCREEN OFF`, `SCREEN ON` and `SLEEP <1..3600>`. A diagnostic sleep
always carries a timer. The reply is sent before parking; USB then disconnects
until wake. `tools/roundie_power.py sleep <s>` reconnects and checks that the
sleep counter advanced. Status reports state, phase, last refusal, sleep count,
last duration, the wake-cause bitmap and the screen-off count.

## Verified facts

- **Source, pinned IDF v6.1 (`fff9895c`):** the ESP32-S3 lacks
  `SOC_USB_SERIAL_JTAG_SUPPORT_LIGHT_SLEEP`. `sleep_modes.c` therefore disables the
  USB Serial/JTAG pad and clock for every light sleep, and restores them on wake.
  RS1, RO1, RP1 and RW1 are unreachable while asleep.
- **Source, same IDF:** `gpio_wakeup_enable()` accepts level types only, switches
  the pin's interrupt type, and for RTC GPIOs (0 and 4 included) also enables RTC
  wake. The touch edge interrupt must be restored afterwards, as the board code
  does.
- **Source, same IDF:** `esp_light_sleep_start()` suspends the timer-group
  watchdogs through the sleep critical section. Their behavior on this board is
  not yet measured.
- **Source, `esp_lcd_st77916`:** the driver implements `disp_on_off` but not
  `disp_sleep`. Panel sleep-in (0x10) would need a raw command and is not used.
- **Host tests:** `tests/test_power.py` runs the actual policy under ASan/UBSan,
  covering:
  - timeout, postponement and disabled timeout;
  - the waking touch withheld until release;
  - BOOT debounce, with no repeat while held and no menu from the press that woke
    the device;
  - timer wake keeping the screen dark;
  - every sleep refusal.

  All 85 host tests passed.
- **Target build:** normal `0.3.21` (REP profile) built with the pinned toolchain,
  no new warnings, 784,080 B in a 3,670,016 B slot.

## Device test, 2026-11-03, under XIP

**Authorization:** the user approved the OTA installation of 0.3.21 and the
unattended tests. The tests ran over USB Serial/JTAG through RW1, RS1 and RP1.
Nobody touched the panel or BOOT. Evidence is in
`roundie-backups/power-0.3.21-2026-11-03/`, outside Git.

| Case | Result |
|---|---|
| OTA from 0.3.20 (`ota_1`) | 784,080 B, SHA-256 `35511f7d…cd607`; 0.3.21 runs from `ota_0`, VALID, self-test passed; 0.3.20 stays VALID |
| Automatic screen off | Happened once within the first 30 s after boot; RW1 `SCREEN ON/OFF` switched it on and off again |
| Timed light sleep, 10 s | 9,991 ms asleep, wake cause timer only (bitmap 16). RW1 answered again 14.5 s after the command, measured on the host |
| Timed light sleep, 60 s | 60,000 ms asleep, timer wake; no reset, so the 10 s watchdogs did not fire across the sleep. The host waited 68.7 s in total |
| Ten 3 s sleeps in a row | 10 of 10 entered and woke; no reset, 0 sleep errors, internal free 213,959 B afterwards |
| Sleep from an active screen | Timer wake left the screen off, as designed |
| Sleep with Roundie UI loaded | Refused with `app`; the sleep counter did not change |
| Safe Mode (RS1 `REQUEST`) | PowerManager not bound; RW1 `SLEEP` rejected; REBOOT returned to normal, VALID, bound |
| Regression after 13 sleeps | 5 hello and native-UI cycles PASS, 0 display errors; full frame 25.0 FPS on the UI and 20.0 on the launcher, both as on 0.3.20; minimum GUI stack 2,368 B |

**User-reported, 2026-11-03:** touch wake turned the screen on without the touch
acting on the UI. BOOT wake worked, including with BOOT held during the wake: no
ROM Download and no menu from that press. Wake latency was not measured, and
the number of attempts was not recorded.

The figures above are one run of each case. The display results show that LCD
transfers resume without errors after wake. They are counters, not a view of the
panel.

## Pending experiments

| Question | Method | Acceptance |
|---|---|---|
| Screen off and BOOT menu | Wait 30 s, touch; press BOOT, try each item | Dark panel, no stray input, Restart and Safe Mode as described |
| Current draw | USB power meter, then battery with gauge current | ACTIVE vs SCREEN_OFF vs LIGHT_SLEEP recorded |
| Resume time | Wake to first frame | Recorded, no budget assumed |

Deep sleep, BOOT as a deep-sleep wake source, panel sleep-in, RTC INT wake (GPIO6)
and application inhibitors remain open.
