# M8.1 unattended endurance and reliability validation

This is the record of the M8.1 runs on a Waveshare ESP32-S3 1.85B (USB serial
`28:84:85:87:98:7C`). No touch, button press, cable change or manual restart
was used. The runner is `tools/hardware_validation/endurance.py` (see
[its README](../tools/hardware_validation/README.md#endurance-m81)).

Evidence is outside Git in `roundie-backups/m8.1-2026-11-19/`. Each run has a
`report.json`, `report.md`, `telemetry.jsonl`, `serial.log` and the runner's
stdout.

**Status: smoke, stress and a 10 h soak PASS on 0.3.55. See "Not covered".**

## Method

### Firmware

- 0.3.54 (M8 baseline) was running at the start: `ota_1`, VALID.
- 0.3.55 adds read-only `RS1 HEALTH` ([safe-mode.md](safe-mode.md)) and
  nothing else. It was installed through RO1 by the M8 runner after a full
  preflight. 0.3.54 stays the VALID rollback image in `ota_1`.

### Telemetry

| Source | Metrics |
|---|---|
| RS1 HEALTH (new in 0.3.55) | free and minimum-ever internal heap, largest internal block, PSRAM free/minimum/largest, failed allocations, task count, stack high-water marks of nine named tasks |
| RE1 STATUS | instance, completion, events, user and UI events, executor rejections, drops, over-budget callbacks, application error, SDK resources, load/unload time, heap after unload |
| RE1 UI STATUS | widgets, outstanding objects, LVGL bytes, injected/accepted/rejected/dropped inputs, maximum input latency |
| RP1 NAV | launcher state, generation, timings |
| RT1 DIAG | RTC seconds, device wall and monotonic time around the RTC read, uptime |

Not available on the device:

- per-task create/destroy counts (`CONFIG_FREERTOS_USE_TRACE_FACILITY` is off;
  enabling it was not assessed);
- LVGL memory beyond the bridge's byte count.

### Criteria (fixed before the runs)

The criteria are `endurance.CRITERIA`. Every report carries a copy. A criterion
without data is UNTESTED, never PASS.

| Criterion | Rule |
|---|---|
| resets | 0 unexpected resets (uptime never drops outside a planned restart) |
| faults | 0 serial panics, watchdog reports or asserts |
| integrity | every cycle unloads cleanly; no application error, executor rejection, drop or over-budget callback outside deliberate invalid input |
| delivery | every spaced injected input is delivered; in bursts, accepted = delivered + stale-rejected |
| allocation | RS1 HEALTH `alloc_failed` does not grow |
| tasks | no irreversible task-count growth in the same device state |
| memory | no steady decline (`leak_check`) of free internal heap, largest block, PSRAM or LVGL bytes in the same state after the warm-up |
| communication | every communication loss ends in a reconnect |

A single drop in heap or a single slow sample is not a regression.

### RTC drift

The drift method is second-edge capture:

1. The runner polls `RT1 DIAG` back to back until the PCF85063 seconds counter
   changes.
2. The edge lies after the last read that showed the old second began, and
   before the first read that showed the new second ended.
3. That interval is recorded against four clocks:
   - host `CLOCK_MONOTONIC`, which systemd-timesyncd slews but never steps;
   - `CLOCK_MONOTONIC_RAW`, which is not disciplined;
   - `CLOCK_REALTIME`;
   - the device monotonic clock.
4. A least-squares fit over all edges gives the estimate.
5. The first and last edge intervals give rigorous bounds.
6. A device clock set or restart splits the series.
7. A host realtime step excludes only the realtime fit.

The RTC was never written during the measurement.

## Results

### Runs

| Run | Level | UTC | Outcome | Notes |
|---|---|---|---|---|
| `ota-0.3.55` | M8 runner install | 2026-11-19 | PASS | 0.3.55 in `ota_0`, VALID; 0 resets |
| `smoke1`, `smoke2` | smoke | 2026-11-19 | FAIL (runner) | runner bugs, fixed before `smoke3` (see below); no device fault |
| `smoke3` | smoke | 21:20–21:24 | PASS | all eight criteria PASS; 180 s soak segment |
| `stress` | stress, `--cycles 100 --ui-rounds 30 --restart` | 21:24–21:59 | PASS after re-analysis | see "Analysis defect" |
| `soak` | soak, 10 h | 22:02–08:02 (next day) | PASS | `delivery` not applicable to this level |

### Runner defects found on the device

These were runner defects, not firmware defects. Each was fixed and tested
on the host before the next run.

- The launcher relaunches home 190–250 ms after any non-home application
  ends, including one started through RE1. This is documented behaviour
  ([system-navigation.md](system-navigation.md), "Returning home") that the
  runner had not accounted for. Waiting for "our" application to
  be closed timed out. The runner now waits for `completed >= instance`.
- The bad-package upload and the restart loop raced the home relaunch: the
  loader refused them as busy (259). The runner now returns to the LIST
  screen, with the runtime idle for 2 s, before each.
- The first event start time included package staging. Staging now happens
  before the timer starts.
- The M8 runner stopped reset detection after one serial error. The
  endurance runner overrides it so a transport error propagates.
- **Analysis defect** (commit `0434848`): the live stress report read PASS
  while `memory` and `tasks` were UNTESTED. The planned restart at the end
  made the analysis keep only the last boot, which had one sample. The
  analysis now evaluates each boot separately, and an UNTESTED criterion
  makes the outcome INCOMPLETE. The recorded stress telemetry was
  re-analysed offline with the fixed code (`stress/reanalysis.json`): all
  eight criteria PASS.

### Stress (verified on the device, 0.3.55)

Duration 34 min 28 s. Lifecycle 1955 s, round UI 77 s, recovery 36 s.

**Counts**

- 100 lifecycle cycles; 600 application runs (100 each of hello, event,
  ui, round, watch and stopwatch).
- 100 user events.
- 30 round UI rounds; 570 injected inputs.
- 1 planned software restart (RS1/RO1); 20 serial reopens.

**Resets and faults**

- 0 unexpected resets; uptime increased monotonically until the planned
  restart.
- 0 panics, watchdog reports or asserts in `serial.log`.
- 0 application errors, executor rejections, drops or over-budget callbacks.

**Memory, measured in LIST after each of the 98 post-warm-up cycles in boot 0**

| Metric | First | Last | Range |
|---|---|---|---|
| internal free (B) | 156,351 | 156,351 | 0 |
| internal largest block (B) | 81,920 | 81,920 | 0 |
| PSRAM free (B) | 7,332,528 | 7,332,528 | 0 |
| LVGL bytes (bridge) | 0 | 0 | 0 |

- Minimum-ever internal free: 123,136 B. Minimum-ever PSRAM free:
  7,325,156 B.
- `alloc_failed`: 0 throughout.
- Internal heap after each unload (RE1 `after`): 156,331 B.

**Tasks**

- 8 in every sample: main, system_worker, rep_executor, audio, ipc0, ipc1,
  IDLE0 and IDLE1.
- Lowest stack high-water marks (bytes):

  | Task | Bytes |
  |---|---|
  | ipc0 | 412 |
  | ipc1 | 608 |
  | IDLE0 | 788 |
  | IDLE1 | 792 |
  | main | 2336 |
  | audio | 3364 |
  | system_worker | 3704 |
  | rep_executor | 10120 |

**Latency, host-measured over USB serial (ms; n = 100 per application)**

| | min | median | p95 | p99 | max |
|---|---|---|---|---|---|
| start hello | 236.6 | 237.9 | 238.2 | 238.4 | 238.6 |
| start event | 48.3 | 49.5 | 50.1 | 59.8 | 82.3 |
| start ui | 265.9 | 267.0 | 327.0 | 327.2 | 327.2 |
| start round | 325.9 | 326.9 | 327.2 | 337.0 | 343.2 |
| start watch | 386.3 | 396.8 | 397.8 | 398.1 | 402.6 |
| start stopwatch | 266.2 | 267.0 | 326.7 | 327.2 | 327.2 |
| close hello | 19.0 | 20.0 | 20.9 | 30.0 | 30.1 |
| close event | 39.7 | 40.3 | 40.6 | 40.7 | 51.5 |
| close ui | 237.5 | 247.9 | 248.5 | 249.0 | 257.6 |
| close round | 237.2 | 238.1 | 238.5 | 238.6 | 239.1 |
| close watch | 141.8 | 143.0 | 203.3 | 203.6 | 206.3 |
| close stopwatch | 82.8 | 83.2 | 238.0 | 257.1 | 257.9 |
| event round trip | 19.5 | 20.0 | 20.1 | 20.4 | 27.7 |
| UI input to delivery (n = 330) | 29.4 | 30.4 | 109.9 | 110.0 | 110.8 |

Host latencies include serial polling. They are not on-device latencies.

On-device figures:

- ELF load: 9.26 ms minimum, 9.65 ms median, 13.0 ms maximum (n = 390).
- ELF unload: 133 µs minimum, 175 µs median, 1.66 ms maximum.
- Maximum UI input latency per round (bridge): 9.9–10.0 ms.

**Delivery**

- Spaced inputs: 330 sent, 330 delivered.
- Invalid inputs (out-of-range slider, out-of-range list index, value to a
  dialog): 120 sent, 0 delivered, each rejected by the bridge.
- Bursts: 240 sent.
  - 29 were refused at the command level (backpressure, reported to the
    sender).
  - 211 were accepted, and all 211 were delivered: 0 stale-rejected, 0
    dropped, 0 lost.
- Neither the PR #4 round UI defect nor the `lv_atan2` panic recurred.

**Recovery**

All recovery checks passed:

- A package with a corrupted ELF magic was refused at upload END (258).
- Stale-instance EVENT and STOP were refused.
- Five consecutive application restarts each got a new instance.
- A stale UI generation was refused.
- 20 serial reopens succeeded.
- The planned restart through RS1/RO1 returned with the same image, still
  VALID, and the same heap: internal free 156,351 B, 8 tasks.

### Soak (verified on the device, 0.3.55)

The soak ran for 10 h 00 min, from 2026-11-19 22:02 to 2026-11-20 08:02 UTC.
The home application (`org.roundie.watch`) ran throughout. The runner:

- sampled telemetry every 60 s (597 samples with home in the foreground);
- captured an RTC edge every 300 s (120 captures, 0 failed);
- ran a lifecycle batch of 5 cycles every hour (9 batches: 45 cycles, 270
  application runs, 45 events).

**Results**

- Resets and faults: 0 unexpected resets; 0 panics, watchdog reports or
  asserts; 0 serial notices.
- Communication: 0 losses.
- Integrity: 0 application errors, executor rejections, drops or
  over-budget callbacks.
- Allocation: `alloc_failed` stayed 0.
- Tasks: 8 in all 639 samples (597 in home, 42 in LIST).
- Memory in home over 10 h, slope 0 B/h:

  | Metric | Value |
  |---|---|
  | internal free (B) | 156,351, constant |
  | internal largest block (B) | 81,920, constant |
  | PSRAM free (B) | 7,325,284, constant |
  | LVGL bytes | 1,952 → 1,920 |

- Memory in LIST after each batch cycle: internal free 156,351 B and PSRAM
  free 7,332,528 B, the same as in stress.
- Minimum-ever free over the whole boot: internal 123,136 B, PSRAM
  7,325,156 B. These minima were set before the soak and never lowered
  during it.
- Lowest stack high-water marks:

  | Task | Bytes |
  |---|---|
  | ipc0 | 428 |
  | ipc1 | 608 |
  | IDLE0 | 788 |
  | IDLE1 | 792 |
  | main | 2336 |
  | audio | 3364 |
  | system_worker | 4024 |
  | rep_executor | 10120 |

- Batch latencies matched stress within about 10 ms at p99. For example,
  watch start had a median of 387.6 ms and a maximum of 397.6 ms.

### RTC drift (verified on the device; interpretation noted)

- 120 edges over 35,699 s in one segment, with no clock set and no restart.
- The host realtime clock was not stepped. NTP was synchronized at the start
  and the end.
- The RTC was not written.

| Reference | Estimate (ppm) | Rigorous bounds (ppm) | Fit residual (ms) | Edge interval median (ms) |
|---|---|---|---|---|
| host `CLOCK_MONOTONIC` (NTP-slewed) | −38.35 | −38.93 … −37.83 | 0.66 | 19.97 |
| host `CLOCK_REALTIME` | −38.35 | −38.93 … −37.83 | 0.66 | 19.97 |
| host `CLOCK_MONOTONIC_RAW` | −19.82 | −20.35 … −19.25 | 0.44 | 19.99 |
| device monotonic clock | −33.33 | −33.66 … −33.00 | 0.02 | 11.70 |

A negative value means the RTC counts slower than the reference.

**Verified:** relative to the NTP-disciplined host clock, the RTC lost
about 38 ppm during this run, about 3.3 s per day.

**Interpretation, not separately verified:**

- The ~18.5 ppm gap between `CLOCK_MONOTONIC` and `CLOCK_MONOTONIC_RAW` is
  timesyncd's frequency correction of the host oscillator.
- The NTP-slewed clock is the better reference, assuming NTP held
  throughout. NTP was synchronized at both ends.
- By the same arithmetic, the ESP32-S3 clock was about 5 ppm slow.
- Drift at other temperatures, and the battery-backed RTC with USB
  disconnected, were not measured.

## Not covered

- **Physical interaction:** no touch, button, cable or battery test, by
  rule. Touch input was injected through the UI bridge, which bypasses the
  touch controller and gesture recognition.
- **Power:**
  - power interruption during Flash writes, OTA or package installation;
  - power loss and brown-out;
  - deep sleep and wake.
- **OTA:** only one OTA (0.3.54 → 0.3.55) ran in M8.1. Repeated OTA and
  rollback were covered in M8, not repeated here.
- **Soak length:** 10 h, not 24 h; the RTC was not measured beyond 10 h.
- **Missing telemetry:**
  - task create/destroy counts (trace facility off);
  - LVGL heap beyond the bridge count;
  - per-callback timing in the soak;
  - on-device load and unload times for soak batches (in the soak step,
    not counted in the latency table).
- **Executor stress:** callbacks that run near or over their budget were
  not exercised. Over-budget detection stayed at 0 because no test
  application is slow.

## Recommendations before M8-B and M9

1. **Core stability (verified).** Over 870 application runs (stress and soak) and 10 h of
   home, internal heap, PSRAM, largest block and task count did not move.
   No leak, fault or reset was seen. This supports starting M9 on the
   runtime as it is.
2. **Stack margins (verified; action recommended).** Before M9 adds load,
   review the stack margins of:
   - `ipc0` (412 B minimum) and `ipc1` (608 B), which are ESP-IDF tasks
     sized by `CONFIG_ESP_IPC_TASK_STACK_SIZE`;
   - `IDLE0` and `IDLE1` (788 and 792 B).

   Keep RS1 HEALTH in later runs to follow them.
3. **RTC (verified; decision needed).** A ~38 ppm slow RTC loses about
   20 min per year without network time. Options:
   - calibrate the PCF85063 offset register per board;
   - resynchronize from NTP or the host;
   - accept the drift.

   Any calibration writes RTC registers and needs its own decision and test.
   Repeat the measurement at more than one temperature first.
4. **Endurance in CI (proposal).** The runner and its host tests are ready
   for a scheduled hardware job. Use stress as a regression gate; a soak is
   too long for every change.
5. **For M8-B (power):** reuse `endurance.py` telemetry and RTC edge capture
   around sleep and wake. Measure wake reliability and RTC continuity with a
   person present, as the specification requires.
6. **Telemetry gaps (proposal).** Add an LVGL heap figure, and executor
   budget statistics per application, to RS1 HEALTH or RE1 STATUS before
   M9 applications grow. The reply has about 146 B of margin, so a new
   field may need a second command.
