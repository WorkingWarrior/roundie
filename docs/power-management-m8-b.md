# M8-B — Power management and suspend/resume lifecycle

Extends [Stage 8 PowerManager](power-manager.md) and the M9.1 executor stack
protection ([M9.1](elf-runtime-hardening-m9-1.md)). REP §17 is the baseline. M10
(WatchFaceHost, a watch-face ABI, installable faces) is **not** part of this
milestone; the [M10 contract](#contract-for-m10-watchfacehost) below only fixes what
M10 may rely on.

Labels: **verified** (source, test or device measurement named), **decision**
(recorded here), **assumption**, **PENDING** (missing evidence and how to get it).

## 1. Audit (before M8-B, `5e04c43`)

- **Verified, source:** screen off/on, light sleep and the power menu existed
  (`main/power_gui.c`, `main/power_manager.c`, `components/roundie_power`). The
  state was the screen policy (`ACTIVE`, `SCREEN_OFF`) plus a worker sleep phase;
  there was no single state model and no record of refused or invalid transitions.
- **Verified, source:** inhibition was conditions only (OTA, storage mode, runtime
  busy, benchmark). There were no handles, no owner and no timeout.
- **Verified, source:** sleep was refused while a non-home application was in the
  foreground (`park_home` required the home app).
- **Verified, source:** the M9.1 guard (MMU-unmapped guard pages plus the
  assist-debug SP monitor) was armed once, when the executor task started. Nothing
  rechecked it after light sleep. IDF v6.1 `sleep_modes.c` saves no ESP32-S3 MMU
  table and no assist-debug registers. Survival across sleep was therefore unknown.
- **Verified, VHL:** after a timer wake the first BOOT press was swallowed. The
  debounced level was stale because the GUI does not sample while parked. Fixed in
  `roundie_power_woke`, with a regression test in `tests/test_power.c`.
- **Verified, source:** BootGuard maps `ESP_RST_DEEPSLEEP` to `ROUNDIE_RESET_SLEEP`,
  which is not an abnormal reset. `tests/test_boot_guard.c` (the loop over every
  non-fault reset class) asserts that it does not count toward Safe Mode.

## 2. State model

`components/roundie_power/lifecycle.c` (`roundie_pm_t`, pure C, host-tested) is
the single record of the system power state. Every call is made under
`roundie_power_shared.lock`.

```
            screen timeout / RW1 / menu
   ACTIVE  <------------------------------>  SCREEN_OFF
     |  \                                    /  |
     |   `-- request (admission OK) --------'   |
     v                                          v
 LIGHT_SLEEP_PENDING --cancel(refusal)--> ACTIVE|SCREEN_OFF
     |  worker, right before esp_light_sleep_start()
     v
 LIGHT_SLEEP --sleep call returned--> RESUMING --GUI restored--> ACTIVE|SCREEN_OFF

 ACTIVE|SCREEN_OFF --request--> DEEP_SLEEP_PENDING --cancel(UNSUPPORTED)--> idle
                                   (DEEP_SLEEP is modelled, never entered in M8-B)
```

Each transition is a function: `roundie_pm_request_light_sleep`, `_cancel`,
`_entered`, `_woke`, `_resumed`, `_screen`. Any other transition is rejected and
counted in `rejected`:

- a second suspend while one is in progress (`REFUSED_BUSY`);
- a resume without a suspend;
- a repeated resume.

A screen change during a sleep sequence records only the flag. The idle state
after resume follows it. The record also holds:

- time in each state;
- last transition and its age;
- attempts and refusals, with the last refusal;
- completed light sleeps, timer and user wakes, and sleep errors;
- last and maximum suspend time (request accepted → sleep entered);
- last and maximum resume time (sleep call returned → GUI restored).

## 3. Lifecycle contract (internal Core API, no REP ABI change)

**Decision:** fixed ordered steps owned by the existing tasks, not a participant
registry. There are three participants, and their order is fixed:

| Step | Owner | Suspend | Resume |
|---|---|---|---|
| Worker | admission: stable normal mode, OTA idle, no storage mode, no CPU_AWAKE inhibitor | — |
| Launcher (worker) | foreground app ends through its lifecycle (`park_home`); the executor has no loaded app | Home relaunched (`home_due`) after wake |
| GUI task | menu closed, LCD transfers drained, touch released, screen off, LVGL parked | display restored (re-init ST77916), LVGL resumes, `roundie_pm_resumed` |
| Worker | re-check (wake source low, inhibitor taken while parking) → cancel | — |
| Executor | `roundie_elf_guard_recheck()` arms a recheck epoch | the first executor step after wake re-verifies the guard **before** any ELF code runs |
| Worker | wake arm, `roundie_pm_entered`, `esp_light_sleep_start()` | wake causes, `roundie_pm_woke`, touch IRQ restore |

**Behaviour change (decision):** any foreground application, not only Home, is
ended for sleep, restart, USB storage and Safe Mode. Before, a non-home app
blocked them. **A running app never blocks sleep by itself.** Only an explicit
inhibitor or a derived condition does. Home is launched again after resume. Other
apps are not restored: there is no state-preserving suspend for native ELFs in
M8-B, and REP has no suspend callback.

Applications see only their existing lifecycle (stop and unload). No new ABI
entry, callback or event was added.

## 4. Sleep inhibitors

`components/roundie_power/inhibit.c` holds a fixed table of 8 slots. It does no
allocation and is always used under the shared lock.

- **Kinds** (REP §17.3): `CPU_AWAKE` 1 (blocks light sleep), `DISPLAY_AWAKE` 2
  (postpones the screen timeout), `NO_DEEP_SLEEP` 4. Network Required has no
  client (no Wi-Fi) and is not implemented.
- **Owner:** SYSTEM, OTA, STORAGE, AUDIO, RUNTIME, GUI or DIAGNOSTICS.
  Applications cannot take one: there is no ABI entry (REP §25 item 25 remains
  open).
- **Handle:** `generation << 16 | (slot + 1)`. 0 is never valid. The generation
  skips 0 on wrap, so a stale or double release returns `STALE` and is counted
  without freeing another owner's slot.
- **Timeout:** mandatory, 1 µs to 600 s. Expired handles are released by the GUI
  poll and counted as `expired`; a warning is logged, as the expiry means a leak.
  `release_owner` drops all of an owner's handles (owner teardown).
- **Full table:** acquire fails with `FULL` (counted). It never evicts.
- **Diagnostics:** active count, kinds mask, owners mask, stale releases,
  expired, full, acquired, released and longest hold.
- **Derived inhibitors:** OTA not idle, storage mode, package transaction and
  staging remain conditions, reported live as `admission` in RW1 STATUS.

## 5. Light sleep and the M9.1 stack protection

**Requirement:** native ELF code must not run after a wake unless the stack
protection is confirmed.

- Before every light sleep the worker calls `roundie_elf_guard_recheck()`, which
  bumps an epoch.
- The executor, before it executes a request or steps a loaded app, compares
  epochs and runs `roundie_elf_stack_recheck()` on its own task. The recheck
  verifies:
  - the guard pages are still unmapped and the stack pages still mapped;
  - `SP_MIN` equals the limit and `SP_MAX` is `UINT32_MAX`;
  - monitor enable and release are set;
  - `SP_UNSTABLE` is 10.

  Any deviation sets a bit in `guard_lost`.
- **Pass:** the monitor is re-armed and execution continues.
- **Fail (fail closed):** the monitor is disabled, the guard state becomes
  `FAILED` permanently, and a loaded app stays frozen (it is never stepped). New
  submits are refused with `ESP_ERR_NOT_SUPPORTED`. No overflow is provoked to
  test this. Host tests drive the failure with a mocked recheck
  (`tests/test_elf.c`).
- **Mutation checks (verified):** deleting the recheck before execute, or the
  `!home_parked` launch guard, or the BOOT re-arm each made a host test fail.

Device result: see [§11](#11-hardware-results-185b).

## 6. Wake sources (1.85B)

| Source | Light sleep | Deep sleep | Evidence |
|---|---|---|---|
| RTC timer | used (diagnostic sleep, safety timer 300 s) | available on the chip | **verified** on device, light sleep (§11) |
| BOOT (GPIO0, RTC GPIO, low level) | used | possible as EXT0/EXT1 | light: verified 2026-11-03 ([power-manager](power-manager.md)); deep: **PENDING**. A low GPIO0 at reset selects ROM Download (strap), so it must not be a deep-sleep wake without a measurement |
| CST816S INT (GPIO4, RTC GPIO, low level) | used | possible | light: verified 2026-11-03; deep: **PENDING** (touch controller auto-sleep and power across deep sleep unmeasured) |
| PWR key | none | none | **verified**, hardware audit: separate latch, no MCU signal |
| Other GPIOs (IMU INT, RTC INT) | not used | not used | **assumption:** not wired as wake sources; schematic check PENDING |
| USB Serial/JTAG | none (pad off in light sleep) | none | **verified**, IDF source |

## 7. Deep sleep

**Decision:** modelled, refused. `DEEPSLEEP` (RW1) and any future caller pass
through `roundie_pm_request_deep_sleep` and are cancelled with `UNSUPPORTED`.

Physical deep sleep is **BLOCKED** until both of the following hold:

1. an autonomous wake (the RTC timer) is confirmed on this board;
2. USB recovery after that wake is confirmed, including the GPIO0 strap
   question above.

The planned path:

- Deep sleep is a new boot (`ESP_RST_DEEPSLEEP`), not a resume.
- Restore runs through normal boot. BootGuard counts it as a non-fault reset
  (verified, host test).
- The wake cause comes from `esp_sleep_get_wakeup_causes()` and is recorded.
- Retained RTC state is limited to what boot already preserves.

A restart is not evidence of deep sleep. Acceptance needs the wake cause
`TIMER` with reset reason `DEEPSLEEP`, read over USB after the wake.

## 8. Diagnostics

The existing protocols carry everything; no new transport was added.

- **RW1 `STATUS`** (1280 B frame since E0): pm state, from, age, transitions, rejected,
  attempts, refused, last refusal, live `admission`, light sleeps, user and timer
  wakes, sleep errors, time in each state, suspend and resume (last and maximum),
  the inhibitor counters, and the guard state with its checks, failures and lost
  mask; since E0 also `brightness`, the backlight percent the GUI applies
  while the screen is on (0 is reported before the GUI binds).
- **RW1 commands:** `INHIBIT <kinds> <s>` (DIAGNOSTICS owner), `RELEASE <HANDLE>`,
  `DEEPSLEEP` (always refused).
- **RS1 `HEALTH`:** heap, PSRAM, tasks, stack high-water marks and the guard.
- **Bounded:** transitions only update counters. Each warning is one fixed log
  line, and nothing allocates.

`tools/roundie_power.py` validates every field.

## 9. Energy methodology (no numbers in M8-B)

No current was measured. **PENDING**, needs a person with equipment. The method:

1. Use a USB power meter, or a shunt with a scope, on the 5 V input. Remove the
   battery, or measure on VBAT with the charger disconnected.
2. Measure each state for ≥60 s: ACTIVE, SCREEN_OFF and LIGHT_SLEEP (RW1
   `SLEEP 60`). Report the average, the peak and the instrument.
3. USB Serial/JTAG enumeration changes the draw. Report the host connection.

## 10. Deterministic tests

Test comments number scenarios after the M8-B request list; this table maps
scenarios to tests by name.

| Scenario | Test |
|---|---|
| ACTIVE ↔ SCREEN_OFF | `test_power_lifecycle.c` |
| full light-sleep cycle, timing | `test_power_lifecycle.c` |
| concurrent suspend rejected (BUSY) | `test_power_lifecycle.c` |
| resume without suspend, repeated resume | `test_power_lifecycle.c` |
| cancel only from PENDING | `test_power_lifecycle.c` |
| screen change during the sequence | `test_power_lifecycle.c` |
| inhibitor acquire, release, double release (STALE) | `test_power_lifecycle.c` |
| inhibitor blocks sleep, release allows it | `test_desktop.py::test_lab_power_lifecycle` |
| inhibitor timeout expiry | `test_power_lifecycle.c`, VHL |
| timer wake keeps screen off | `test_power.c`, VHL |
| BOOT after a timer wake turns the screen on | `test_power.c`, VHL |
| full table, no eviction | `test_power_lifecycle.c` |
| guard recheck after sleep, pass | `test_elf.c` |
| guard recheck failure freezes the app, refuses submit | `test_elf.c` |
| owner release on teardown | `test_power_lifecycle.c` |
| any foreground app parked, Home relaunched only after resume | `test_launcher.c` |
| no recheck without a sleep | `test_elf.c` |
| VHL run twice, identical output | `test_lab_power_lifecycle` |
| generation wrap never yields 0 | `test_power_lifecycle.c` |
| deep-sleep wake is not a BootGuard fault | `test_boot_guard.c` |
| deep sleep refused, state restored | `test_power_lifecycle.c`, device |
| 4 threads × 20,000 random operations, invariants (TSan) | `test_power.py::test_lifecycle_and_inhibitors_tsan` |

VHL and host results are not evidence about the hardware.

## 11. Hardware results (1.85B)

Board 1.85B (serial in the local report), image `0.3.70` (REP profile), built
with the pinned IDF v6.1 image. Its sdkconfig and partition table are identical to
the CI build. Nothing touched the panel or buttons: all commands went over USB
(RW1, RP1, RS1, RO1).

**Preflight, read only (verified):**

- `0.3.69` VALID on ota_0; diagnostic `0.3.68` in ota_1;
- no Safe Mode, 0 BootGuard failures;
- guard armed, `alloc_failed` 0.

**OTA (verified):**

- The first BEGIN was refused (`ESP_ERR_INVALID_STATE`): OTA requires an idle
  runtime, and Home was loaded. This is existing behaviour.
- After RP1 `EXIT`, the launcher showed LIST and the runtime was idle. The
  upload, activation and restart then succeeded.
- `0.3.70` VALID on ota_1. The rollback slot ota_0 holds `0.3.69` VALID: its ELF
  hash equals the image that ran before the OTA. `0.3.68` is no longer on the
  board.
- The boot log shows the M9.1 monitor self-test passed.

**Series** (`m8b_device.py` in the session scratchpad; one run, no retries of
failed steps):

| Step | Result |
|---|---|
| Screen off/on ×100, each step confirmed by the state model | PASS: 200 transitions, 100 screen-offs, 0 rejected; confirmation off ≤161 ms, on ≤371 ms (avg 316 ms, includes the ST77916 re-init) |
| `DEEPSLEEP` | PASS: refused `ESP_ERR_NOT_SUPPORTED`, last refusal `unsupported`, state back to ACTIVE |
| Inhibitor `CPU_AWAKE` 60 s, then `SLEEP 5` | PASS: sleep refused `inhibited`, sleep count unchanged; release OK; second release refused (stale, counted); 0 held |
| Timer light sleep 5 s ×20, Home loaded | PASS ×20: each wake by timer; guard recheck ran once per sleep; `guard=armed`, `guard_lost=0`, 0 failures; Home relaunched (new ELF instance, runtime busy, launcher ACTIVE); state after wake SCREEN_OFF |
| Totals after the series | 21 light sleeps (1 from the first run), 21 guard checks, 0 sleep errors, 0 rejected transitions, 0 expired inhibitors |
| Reset during the series | none: uptime monotonic, reset reason unchanged, 0 BootGuard failures, still `0.3.70` VALID |

**M9.1 question answered for this case (verified, device):** the MMU guard
unmapping and the assist-debug SP monitor configuration survived every
5-second light sleep: 21 of 21 checks with the lost mask 0. Longer sleeps and
touch or BOOT wakes are not covered (PENDING).

**First run, not counted as PASS (recorded as is):**

1. The 100 screen commands were sent 150 ms apart without waiting. RW1 `SCREEN`
   is a single latest-wins slot, and the screen-on re-init takes ~300 ms. Only
   99 of 200 transitions happened, and the run's check did not catch it. The
   procedure now waits for each state.
2. The first sleep "failed" on a wrong criterion: the launcher's `foreground`
   flag. That flag is 0 for an active Home, before and after a sleep, also on
   `0.3.69`. The sleep itself was correct (timer wake, recheck, armed, Home
   relaunched).

Both runs used the same firmware. No device code changed between them.

**Memory, before → after the series (RS1 HEALTH, Home loaded both times):**

- internal free 166,643 → 166,643 B; minimum 133,428 B; largest block
  90,112 B;
- PSRAM free 7,126,176 B; largest block 6,946,816 B;
- `alloc_failed` 0; 8 tasks;
- stack high-water marks (free bytes): main 2,272 → 2,208, system_worker
  4,824 → 4,552, rep_executor 62,208, audio 3,364, IDLE 788/792;
- guard usable minimum 21,248 B. Spurious monitor hits rise with run time
  (M9.1 known behaviour).

**Not measured on the device:**

- touch and BOOT wake on 0.3.70;
- panel image after resume (a person must look);
- current;
- deep sleep.

## 12. Budgets

**Verified, target build:**

- Same pinned IDF image for both builds: baseline `5e04c43` and M8-B, 1.85B, 0
  warnings each.

| | Baseline | M8-B | Δ |
|---|---|---|---|
| REP image | 1,143,778 B | 1,149,802 B | +6,024 B (slot 3,670,016 B) |
| REP Flash code | 717,980 B | 722,596 B | +4,616 B |
| REP static DIRAM | 209,434 B | 209,882 B | +448 B |
| Legacy image | 884,236 B | 884,236 B | 0 (no power manager) |

The static RAM increase is the state model and the 8-slot inhibitor table in
`roundie_power_shared`. Transitions allocate nothing.

**Device, runtime (verified, not a controlled A/B):**

- `0.3.69` before the OTA: internal free 167,091 B, largest block 94,208 B.
- `0.3.70` before the series: 166,643 B, largest block 90,112 B.

The 448 B difference in free memory matches the static increase. The largest
block depends on run history. The series itself changed no memory figure
(§11).

**Timing (device, 20 timer sleeps):**

- suspend (request accepted → sleep entered) ≤ 110 ms. It was not broken down
  by step (Home unload, LCD drain, parking);
- resume (sleep returned → GUI restored) ≤ 3 ms after a timer wake, which keeps
  the screen off. With a user wake the display re-init adds ~300 ms, as for
  screen on (§11);
- RW1 to SCREEN_OFF ≤ 161 ms.

**Stacks:** the worker's minimum free stack went from 4,824 to 4,552 B over the
series, through the recheck and the larger RW1 reply. The executor stack was
unchanged at 62,208 B free.

## 13. Pending

- Current measurements (§9).
- Physical deep sleep (§7).
- BOOT and touch wake after M8-B. The changed code path is the same as on
  2026-11-03, but it needs a person.
- Guard survival over long sleeps (> 60 s) and many cycles: only the series in §11
  was run.
- Application inhibitors (REP §25 item 25).

## Contract for M10 WatchFaceHost

What M10 may rely on. Nothing here is implemented as a WatchFaceHost.

1. **State source.** `roundie_pm_t` is the only power state. A face host reads it
   under `roundie_power_shared.lock`. It never infers sleep from the screen flag.
2. **Order.** On suspend the host is parked before the GUI parks (lifecycle step
   2). On resume it is relaunched after `RESUMING → idle`, i.e. after the display
   is restored. It must not draw before that.
3. **No private loop and no inhibitor by default.** A face that needs the screen
   lit (always-on) must hold `DISPLAY_AWAKE` with an owner and a timeout. A face
   never holds `CPU_AWAKE` to keep ticking. Light sleep stops all ticks, and the
   face redraws from the RTC on resume.
4. **Native code gate.** A native face runs only while the M9.1 guard is `ARMED`.
   After any sleep the executor rechecks it first. If it is `FAILED`, the host
   must fall back to a firmware-built face.
5. **Deep sleep.** A wake is a fresh boot, so a face cannot keep RAM state across
   it. Anything persistent goes through storage before `DEEP_SLEEP_PENDING`, and
   only once deep sleep is enabled.
6. **ABI.** Any face callback (tick, ambient, visibility) is a new versioned ABI
   decision for M10, not an extension of this internal contract.
