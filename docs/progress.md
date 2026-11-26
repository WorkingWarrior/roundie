# Progress

Rules (from the task, binding for every session):
- Autonomous mode. Decisions and their reasons go in the reports. PASS only with evidence from a test that was run.
- Closure rule: before a stage counts as finished, do everything that can be implemented and checked without the 1.75C and without the user present. The only allowed gaps are:
  - BLOCKED: items that need a connected 1.75C;
  - PENDING: physical tests (current, finger touch, image, audibility, wake moment, brownout).
- Hardware: flash/OTA and automatic tests on connected boards are allowed. Never erase all Flash, the storage partition or NVS. No eFuse, Secure Boot or Flash Encryption. At most one retry of a test on a board.
  - The one-retry limit applies to repeating the same test in the same configuration (image, runner, scenario, parameters). Diagnostic experiments in different configurations are allowed; record each in the report with its purpose and result.
- Git: commit after every subphase. Never commit AGENTS.md or `.log`.
- Publication after each stage:
  1. push the branch;
  2. `gh pr create` to main;
  3. `gh pr checks --watch`;
  4. merge commit (no squash), only when CI is green and every automatically checkable gate item is PASS;
  5. delete the remote branch.
  If push or `gh` fails, stop and report it. Don't work around it.

## Stage A: E0 closure (branch e0-baseline)

### A1: gap table (done)
- docs/baseline/e0-summary.md §10.1 classifies each gap as:
  - do now (all of these are done);
  - decision;
  - not to do (destructive, or changes the boot logic);
  - EABI;
  - PENDING (physical);
  - BLOCKED (1.75C).

### A2: 1.75C runner support (864fc93)
- RG1 BATTERY reports the board identity, RW1 STATUS the brightness, and run_e0.py v2 records the board, display and power context.
- `e0_board.py` runs the whole E0 baseline with one command.
- Verified: host tests, and all 6 firmware builds (0 warnings). The device run on the 1.75C is BLOCKED.

### A3: runner vs Manager (01d9ab7)
- Decision: the Python runner is the E0 executor, recorded in the roadmap.

### A4: Safe Mode §9.4 diagnostics (469472c, e5746f8)
- RTC session record holding:
  - the code of the unplanned restart site;
  - the last battery sample (mV, mA, SoC, flags, temperature, RTC time), at most once a minute.
- Cause record (`boot_guard/cause`):
  - written after the latch commit, in its own commit;
  - the host test proves the latch trace is identical when the cause write fails (no space, damaged entry, commit failure).
- Crash copy (`boot_guard/crash`, 224 B, limit 256 B), read with `RS1 CRASH STORED`. The cause is read with `RS1 CAUSE`.
- Bounded store: a blob is written only while 126 NVS entries (one page) stay available, in its own commit, and touches only its own key. The full-NVS test passes (NO_ROOM, settings untouched).
- Safe Mode/BootGuard logic is unchanged: the latch trace is the same with diagnostics on and off.
- 1.85B, `run_e0_diag.py`: PASS.
  - NVS used entries went 17 → 34 of 504 (+17), with 344 still available.
  - Decision: the copy stays in NVS (docs/safe-mode.md).

### A5: the other "do now" gaps
- RTC on 1.85B: valid. `roundie_time.py verify --restart` passes after fixing the race with board publication (8cfa79f).
- LIST alone under the stall probes: `RP1 STALL ARM/REPORT` plus the `listprobe` scenario (04834ab). LIST takes 122 ms and stalls the GUI for the same 122 ms. Details: e0-1.85b.md §14.
- e0-1.75c.md has the one-command instructions (c4c67df).
- The CI layout line appears in a clean rep-nodev configure.
- The console line-loss mechanism is confirmed from the IDF source (e0-summary §10.1 #8).

### A6: builds, host tests, gate on 1.85B (BLOCKED by a FAIL, under investigation)
- 6 configs × 0 warnings: PASS.
- Host tests: 333 tests, 0 coverage problems.
- Gate on 1.85B with image 0.3.86-e0 (`build-ci/e0/a6-185b-gate/`):
  - base (with light sleep): PASS, 0 unexpected resets;
  - stall (all scenarios): PASS;
  - **followup: FAIL in `worst_stack`**, twice (first run plus the one allowed retry, `followup` and `followup-2`). Every other followup step passed.
- The failure:
  - The stopwatch ends on its own with RESULT `[0,-1,-24]` before the runner sends EXIT. The launcher then refuses `EXIT <generation>` with 259 INVALID_STATE.
  - `-24` comes from `show_state()` in examples/stopwatch_elf/stopwatch.c: one of its four `set()` calls was refused.
  - Sequence: PRESS dial (start) → `bench_with_traces` (full-refresh bench) on the running stopwatch → HEALTH → PRESS dial (stop) → PRESS lap → settle 1 s.
  - This is **not** the STOP race (§8.6), because EXIT had not been sent yet.
  - The same scenario passed on 0.3.83/0.3.84-e0 (`build-ci/e0/followup-185b-3`).
- Diagnostic experiments (new configurations, not retries; `build-ci/e0/a6-matrix/`):

  | Report | Image | Runner | Purpose | Result |
  |---|---|---|---|---|
  | r1-i086 | 0.3.86-e0 | before v2 (c8aa780) | old runner on new image | PASS (max_depth 11, rejected 0) |
  | r2-i086 | 0.3.86-e0 | v2 | reproduce the gate | FAIL −24 (max_depth 12, rejected 1) |
  | r1-i084 | 0.3.84-e0 (OTA, 3ef9c14) | before v2 | regression check | FAIL −24, same sequence |
  | r2n-i084 | 0.3.84-e0 | v2 with RG1 BATTERY disabled (v2 as is: BLOCKED, no RG1 BATTERY on 0.3.84) | runner reads | FAIL −24 |
  | r2-i087 | 0.3.87-e0 (refusal counters, e021994) | v2 | refusal cause | FAIL −24, `reject_reason=FULL`, `reject_depth=12`, admission 0, request 0, GUI pumped 26.3 ms earlier |

- Verified facts:
  - Both images fail and the old runner passes once on the new image, so the failure follows timing, not the image. **Not a stage A regression.**
  - The cause is a full UI queue, not closed admission. PRESS stop and PRESS lap arrive 10 ms apart. Stop (about 6 commands) and reset (about 7) are queued before the next GUI pump, so the 13th command, inside `show_state`, is refused.
  - I²C hypothesis rejected:
    - the 26 ms pump age is one normal GUI iteration (stopwatch frames 15–62 ms, `LV_DEF_REFR_PERIOD=33`);
    - runner v2 reads RG1 BATTERY only before the scenario;
    - gauge sampling (system_worker, 5 s) is unchanged since 0.3.84;
    - the A4 battery record copies the existing sample.
- Decision: a contract defect that existed earlier and was exposed by timing.
  - The stopwatch (1.1.1, a431103) treats a refused update as a stale view and redraws everything on the next tick. Only a refusal lasting 50 ticks is an error.
  - Host regression test: "Backpressure" block in `tests/test_app_round.c`. It reproduced the failure before the fix and passes after it.
  - UI backpressure semantics ("busy" code, distinct from "stopping") are EABI P2 in docs/roadmap.md.
- Image 0.3.87-e0 = HEAD firmware (rebuild gives the same binary, 0 warnings). Stopwatch 1.1.1 installed: SHA-256 `2f54ed84…5f47f540`.
- Follow-up after the fix (`a6-185b-gate/followup-fix`, 0.3.87-e0, stopwatch 1.1.1, runner v2, defaults): **PASS**, all 4 steps (memprobe, cycles, worst_stack, fsbench_gui), 0 unexpected resets. In worst_stack the app ended through EXIT with result `[0,0,0]`.
  - Not verified on the device: whether the queue actually overflowed in this run. At the lap PRESS, 6 stop commands were still pending (the same state as in the failures). No UI snapshot was taken between the reset and EXIT. The host regression test is the evidence for the refused path.
- A7 (publication) waits for this: every automatically checkable gate item must be PASS.

### Next
- A6: resolve the worst_stack FAIL (above), record the gate in e0-summary §10 and commit.
- A7: publication.
- Then Stage B:
  - branch `eabi` from main;
  - P1–P8 from the task;
  - do not raise runtime limits;
  - no later-stage services.
