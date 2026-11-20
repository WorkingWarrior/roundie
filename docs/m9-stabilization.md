# M9: Stabilization and runtime safety

M9 stabilizes the MVP runtime without new user features, REP ABI changes or
Flash layout changes. This document records what was measured and decided,
what is PENDING, and the MVP definition-of-done status. Component details are
in [app-lifecycle.md](app-lifecycle.md) (budget counters) and
[crash-diagnostics.md](crash-diagnostics.md) (crash record).

Labels: **verified** (source, version and test or measurement given),
**decision** (recorded here), **assumption**, **PENDING** (question, method,
acceptance).

## 1. Stack and memory budgets

### Measured (verified)

Source: `RS1 HEALTH` on the 1.85B board (serial `28:84:85:87:98:7C`), firmware
0.3.55. "Free" is the FreeRTOS high-water mark: the least free stack ever seen
in that boot, in bytes. The lowest values are from the M8.1 stress and soak
runs ([hardware-endurance-m8-1.md](hardware-endurance-m8-1.md)). The 0.3.55
values from 2026-11-20 are from the same boot after 11.2 h of uptime.

| Task | Stack (B) | Lowest free, M8.1 | Free 0.3.55, 2026-11-20 | Peak use | Source of size |
|---|---|---|---|---|---|
| ipc0 | 1280 | 412 | 428 | 868 (68 %) | `CONFIG_ESP_IPC_TASK_STACK_SIZE` |
| ipc1 | 1280 | 608 | 608 | 672 (53 %) | same |
| IDLE0 / IDLE1 | 1536 | 788 / 792 | 788 / 792 | 748 (49 %) | `CONFIG_FREERTOS_IDLE_TASK_STACKSIZE` |
| main | 8192 | 2336 | 2336 | 5856 (71 %) | `CONFIG_ESP_MAIN_TASK_STACK_SIZE` |
| audio | 4096 | 3364 | 3364 | 732 (18 %) | `components/roundie_audio/audio.c` |
| system_worker | 12288 | 3704 | 4024 | 8584 (70 %) | `main/main.c` |
| rep_executor | 12288 | 10120 | 10120 | 2168 (18 %), firmware and applications | `components/roundie_elf/executor.c` |

- 8 tasks exist. `esp_timer` reports −1: no task of that name was found.
- Heap: the minimum-ever internal free heap is 123,136 B. The largest internal
  block is 81,920 B. The minimum-ever PSRAM free is 7,325,156 B. There were
  0 failed allocations over the M8.1 runs and in this boot.

### Budgets (decision)

- **Firmware tasks.** A task's budget is its measured peak plus at least
  25 % of its stack size, or 256 B if that is larger.
  - Every task meets this. The smallest margin is ipc0 at 412 B (32 %).
  - A low high-water mark is a review trigger, not proof of overflow.
  - No stack size changes in M9.
- **`rep_executor`.** This is also the application stack. The peak of the
  firmware path and all six example applications together was about 2.2 KiB
  during the M8.1 runs. The application budget is set at 8 KiB, which leaves
  about 4 KiB for the firmware path and reserve. It is recorded in
  [app-lifecycle.md](app-lifecycle.md). Unbounded recursion is an application
  fault (see the Kconfig section below).
- **Internal heap.** Keep at least 64 KiB of internal free heap as an
  emergency reserve under every M8 workload. Measured minimum: 123,136 B.
- **PSRAM.** No budget pressure is measured: the minimum is above 7 MB free.

### Kconfig (decision): no change

M9 tried one change and withdrew it after the device test:
`CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y` in the REP profile (0.3.56 only).

- **Motivation.** The 2026-11-02 device test ([boot-guard.md](boot-guard.md))
  found that unbounded recursion in a native application (`fault_elf`
  `stack`) ends in a silent reset. The watchpoint panics on a write to the
  last 32 bytes of the running task's stack. It has no RAM cost and costs up
  to 60 B of usable stack per task.
- **Device result (verified, 0.3.56, 2026-11-20).** The watchpoint did not
  fire.
  - `recurse()` has a 544-byte frame (`entry a1, 0x220`) and writes only
    `page[0]` at its bottom, so every write skips the watched 32 bytes. This is
    the limit the Kconfig help documents.
  - The overflow went past the `rep_executor` stack. The board then panicked
    on core 1 with LoadProhibited in `SysTickIsrHandler`
    (EXCVADDR `0x0006008f`, reset class 5, raw 4) and entered Safe Mode
    (reason=core).
  - One sample does not show that the watchpoint changed this outcome.
- **Decision.** Withdrawn: no Kconfig change in M9. No stack sizes, WDT
  timeouts or stack-check options change.
- **Remaining risk.** Native stack overflow with large frames corrupts the
  memory below the executor stack and is reported as an unrelated fault.
  - The crash record still names the loaded application by digest.
  - Options for a later milestone, as a decision to take:
    - an SDK stack-depth rule enforced at validation time (frame size from
      `entry`);
    - a guard region below the executor stack via the PMS/World controller;
    - `-fstack-check`-style probes in the SDK build.
  - Each needs its own measurement.

## 2. Executor budgets and queue overrun (verified, host)

See [app-lifecycle.md](app-lifecycle.md). Eight deterministic host cases in
`tests/test_elf.c` (`budget_tests`) cover:

- the five callback-time buckets at their exact boundaries (1 ms, 10 ms,
  80 ms and 100 ms);
- callback and queue latency maxima;
- mailbox peak, overflow drop (newest event, `ESP_ERR_NO_MEM`), coalescing of
  TIMER, VALUE and CLOCK events, rejection after stop, and discard on clear.

A mutation of the 100 ms boundary is detected. The soft budget is measured
after the callback returns. A native callback cannot be interrupted; an
endless callback is ended by the 10 s task watchdog panic, as before.

## 3. Telemetry (verified, host)

`RE1 BUDGET` (read-only) returns the per-instance and per-boot counters. It is
read by `tools/roundie_budget.py`, `run_m8.py` (`budget` step) and every
endurance snapshot. Firmware without the command is asked once and recorded
as unsupported.

## 4. Device validation

Board 1.85B, serial `28:84:85:87:98:7C`. OTA through `run_m8.py --ota`
(preflight, VALID rollback image in the other slot, `--no-rtc-sync`). No
Flash erase, bootloader, partition table or RTC write.

| Check | Result |
|---|---|
| OTA 0.3.55 (ota_0) → 0.3.56 (ota_1) | PASS. VALID after self-test; 0.3.55 VALID as rollback |
| `RS1 CRASH` first boot of 0.3.56 | `{"valid":0}` (0.3.55 had no record), as designed |
| `fault_elf` `store` started via RE1 | Panic StoreProhibited, Safe Mode reason=core, both images VALID. `RS1 CRASH` in Safe Mode: valid record. `roundie_crash.py` matched the digest and named `pc` `event+0x12` (ELF `0x252`), called from `roundie_elf_step` and `execute`. Objdump of `0x252`: `s32i.n a9, a8, 0` (store through null). RS1 REBOOT returned to normal; the record was still readable |
| `fault_elf` `stack` | Panic (see the Kconfig section): the record named the loaded application by digest, with a firmware PC on the other core. RS1 REBOOT returned to normal |
| OTA 0.3.56 (ota_1) → 0.3.57 (ota_0), the final tree: sdkconfig identical to 0.3.55 | PASS. VALID; 0.3.56 VALID as rollback |
| `endurance.py --level smoke` on 0.3.57 | PASS: all 8 criteria (resets, faults, integrity, delivery, allocation, tasks, memory, communication). Steps: baseline, lifecycle 40 s, UI stress 10 s, recovery 21 s, soak 180 s. Home application restored |

`RE1 BUDGET` boot counters at the end of the smoke run (verified, device):

| Callbacks | <1 ms | <10 ms | <80 ms | 80–100 ms | >100 ms | Max callback | Max queue latency | Queue peak | Dropped | Coalesced | Rejected |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 399 | 356 | 40 | 3 | 0 | 0 | 16.06 ms | 9.92 ms | 1 of 8 | 0 | 0 | 1 |

- The one rejection is the expected refusal of a stale-instance event in the
  recovery step.
- No callback came near the 100 ms soft budget.

Health at the end of the smoke run, 0.3.57:

- Internal heap: 155,455 B free; minimum 122,240 B; largest block 81,920 B.
- `alloc_failed` 0; 8 tasks.
- Stack free (B): main 2176, system_worker 3688, rep_executor 10136, audio
  3364, ipc0 428, ipc1 608, IDLE0 788, IDLE1 904. All are within the budgets
  in section 1.

### Memory, 0.3.55 → 0.3.57 (verified, `esp_idf_size`, 1.85B rep)

| | 0.3.55 | 0.3.57 | Change |
|---|---|---|---|
| Image (`roundie_rep.bin`) | 1,136,688 | 1,139,920 | +3,232 B (+0.28 %) |
| Flash code | 713,268 | 715,612 | +2,344 |
| Flash data | 329,608 | 330,056 | +448 |
| DIRAM static | 207,862 | 208,686 | +824 |
| RTC slow | 224 | 404 | +180 (crash record) |
| Minimum internal free at run time | 123,136 (M8.1) | 122,240 (smoke) | −896 |

The run-time difference matches the static DIRAM growth: budget counters and
crash context. The OTA slot is 3,670,016 B, so the image uses 31 %.


## 5. RTC recommendation (analysis only, no RTC writes)

**Verified (M8.1):** over 35,699 s at room temperature, the PCF85063 ran
−38.35 ppm against the NTP-disciplined host clock. Rigorous bounds are −38.93
to −37.83 ppm. That is about 3.3 s/day, or about 20 min/year.

Options (none implemented in M9):

| Option | Effect | Cost and risk |
|---|---|---|
| A. Accept drift | ±3.3 s/day | None. Visible within weeks on a watch face without sync |
| B. Resynchronize from host or NTP | Error bounded by the sync interval | Needs a connected host or Wi-Fi. Sync policy change. The dev `RT1 SET` is not a production path |
| C. Per-board PCF85063 offset register | Residual about ±2 ppm at the calibration temperature | Writes an RTC register, so a per-board measurement and an explicit decision are required. Does not track temperature |
| D. C plus occasional B | Best accuracy | Both costs |

**Recommendation: D, staged.**

1. Keep A until a time-sync policy exists.
2. Repeat the read-only second-edge measurement (`endurance.py --level soak`,
   RTC edges) at a second temperature and on a second board, to tell board
   tolerance from temperature drift.
3. If the rate is stable within ±5 ppm across those runs, calibrate per board
   with the offset register.
   - Arithmetic, from the datasheet LSBs: 38.35 ppm is 8.8 steps of 4.34 ppm
     (normal mode, correction every 2 h), or 9.4 steps of 4.069 ppm (course
     mode, every 4 min, higher current).
   - The sign convention and the mode choice must be checked against NXP
     PCF85063A Rev. 7.3 §7.2.3 before any write. This is not verified here.
   - Verify by the same edge measurement after the write; acceptance is
     |rate| < 3 ppm.
4. Add network resynchronization when Wi-Fi time is in scope.

## 6. Endurance as a regression procedure

The levels are in [tools/hardware_validation/README.md](../tools/hardware_validation/README.md#endurance-m81).
They are not part of GitHub CI: each needs the board, and soak takes hours.

| When | Level | Acceptance |
|---|---|---|
| Every firmware candidate that is OTA-installed on the dev board | `smoke` (about 10 min) | All criteria PASS; RS1 HEALTH and RE1 BUDGET sampled |
| Changes to executor, loader, UI bridge, storage or OTA | `stress --cycles 100 --ui-rounds 30` | PASS. Minimum internal heap and stack marks no worse than the M8.1 table by more than 5 % |
| A release candidate, or a change to timing or power | `soak --hours 8` or more | PASS. No reset. Heap and stack trends flat |

- Compare reports with the previous run.
- A regression in `alloc_failed`, resets, rejected or dropped budget counters,
  or the stack table is a FAIL to investigate, not a threshold to raise.
- Keep raw logs outside Git (`roundie-backups/`). Commit only the summary.

## 7. Build reproducibility (verified)

- The firmware CI job compares `dependencies.lock` before and after the build.
- `tests/test_ci_gate.py` requires one ESP-IDF image digest in the workflow,
  the Containerfile and the documented local command. Changing one copy fails
  the test.
- Example ELFs: adding `X.app.debug.elf` left all 14 stripped images
  byte-identical (`sha256`). `-g` was rejected because it changed two.

## 8. MVP definition of done

| Item | State |
|---|---|
| Host unit tests and validators (`run_tests.py --strict host`) | PASS: 208 pass, 1 expected skip; coverage check 253 tests, 0 problems |
| REP x86-64 native applications, ASan/UBSan | PASS (8) |
| Desktop runtime, virtual time, ASan/UBSan | PASS (7) |
| Desktop runtime, real time, ASan/UBSan | PASS (10) |
| Desktop under ThreadSanitizer | PASS (11) |
| Virtual Hardware Lab | PASS (14) |
| Graphics (gamescope, nested KWin) | Not run locally, to leave the active KDE session alone; runs in GitHub CI |
| Firmware, 4 configurations (`build_firmware.sh` in the pinned IDF image) | PASS, 0 warnings each; `dependencies.lock` unchanged |
| Device: OTA, rollback image kept, crash record, endurance smoke | PASS (section 4) |
| Device: stress and soak endurance on 0.3.57 | Not run in M9. The M8.1 results (0.3.55) stand; run them per section 6 before a release |
| Physical checks: touch, display, audio, buttons, power, battery | PENDING; need a person |
| MVP limits accepted for now | Native stack overflow with large frames is not reported as such (section 1). RTC drift of −38 ppm (section 5). No coredump (crash-diagnostics.md) |

All local test groups ran in the CI container on the M9 branch.
