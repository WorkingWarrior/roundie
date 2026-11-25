# Progress

## Stage A: E0 closure (branch e0-baseline)

### A2: 1.75C runner support (committed)
- RG1 `BATTERY` reports board identity, display size and AXP2101/BQ27220 battery data (main/gauge_diagnostics.c, tools/roundie_gauge.py).
- RW1 STATUS reports `brightness` (frame 1280 B); the runner records the brightness it reads back.
- run_e0.py v2: the board identity check, display context (466x466 panel, 360x360 canvas upscaled) and power context.
- Single command: `python3 tools/hardware_validation/e0_board.py --board 1.75c`.
- CI coverage: the missing E0 test modules are registered in tools/ci/run_tests.py (coverage check: 0 problems, 327 tests).
- Verified: the host tests pass (test_roundie_gauge, test_run_e0, test_e0_board).
- NOT yet verified: the firmware builds with these changes. This check belongs to A6.

### A4: Safe Mode §9.4 diagnostics (in progress, uncommitted in working tree)
Done:
- components/roundie_boot_guard/diagnostics.c (new): an RTC session record holding the restart site and the battery, the previous session, and the cause.
- persistence.c writes the NVS `cause` blob in the same commit as the latch.
- The tests/test_boot_guard mocks and assertions are updated and pass.

Still to do:
- Wire the calls in main/main.c:
  - `roundie_boot_diag_begin(esp_reset_reason(), app_elf_sha256)` after `roundie_boot_begin`;
  - `roundie_boot_diag_restart_site` before the unplanned `esp_restart` calls (LCD timeout, flush, GUI watchdog);
  - `roundie_boot_diag_safe_site` before the `enter_safe` calls for the serial fd and for system_worker creation;
  - `roundie_boot_diag_battery` in the sensor loop.
- Copy the crash report to NVS after a panic or WDT, readable with `RS1 CRASH STORED`.
- Add `previous` and `cause` to the RS1 HISTORY JSON, and update tools/roundie_safe.py and its tests.
- Document it in docs/safe-mode.md.
- Not implemented, described only:
  - a coredump partition (destructive);
  - whether the latch should clear after power-on (this changes the boot logic).

### Remaining in Stage A
- A1: add a gap table to e0-summary.md §10. Planned classification:
  - do now: A2–A4, RT1 SET on 1.85B, an "RP1 LIST alone" stall scenario, the CI layout check;
  - PENDING (physical): current, touch, image, audibility, wake moment, brownout;
  - BLOCKED: 1.75C.
- A3: keep the Python runner and update the roadmap E0 text. Reasons: it runs unattended, it is testable on the host, it needs no Web Serial gesture, and it survives USB loss in light sleep.
- A5: RT1 SET on 1.85B; the LIST-alone scenario in run_e0_stall plus a device run; the one-command instructions in e0-1.75c.md.
- A6: six builds with 0 warnings; host tests (`python3 tools/ci/run_tests.py --check-coverage` in the IDF environment); OTA and run_e0 on 1.85B; update e0-summary.md.
- A7: publish the stage, then Stage B (P1–P8, branch `eabi`).
