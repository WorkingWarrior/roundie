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
- Backup push (user, 2026-11-26): pushing the working branch (`eabi`) to origin as a backup is allowed and recommended after every subphase. PR and merge still come only after the stage is finished, per the publication procedure.

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

### A6: builds, host tests, gate on 1.85B (done)
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
- Gate repeated on the final image 0.3.87-e0 (new image, so a new configuration):
  - base with light sleep (`base-087`): PASS;
  - stall (`stall-087`): 6 of 7 PASS. `race` FAIL: "reproduced 0/10 with delay". The probe counted only RESULT views, and stopwatch 1.1.1 now tolerates the refused update, so it returns to Home.
  - Decision: the race probe measures the race at the bridge. An attempt reproduces it if the launcher shows RESULT **or** the bridge counted a refusal by closed admission (`rejected_admission`, kept from the unload until the next open). RESULT views are reported separately (`delayed_results`). Host test: `test_run_e0_stall` verdict cases.
  - `race` with the changed probe (`stall-087-race`): PASS. Reproduced 10/10 with a 300 ms delay (`rejected_admission=1` each time), 0/10 RESULT views; control without delay: 0 refusals, 0 RESULT.
  - Follow-up: PASS (above).
- Host tests: PASS 287/287; `--check-coverage`: 333 tests, 0 problems.
- A6 result: every automatically checkable gate item on 1.85B is PASS.

### A7: publication (done)
- Pushed `e0-baseline`, PR #12 to main. CI: 20/20 checks pass.
- Merge commit 48ad0ba (no squash). Remote branch deleted.
- Stage A is closed. Left: BLOCKED (E0 on the 1.75C) and PENDING (physical tests).

## Stage B: EABI (branch eabi from main 48ad0ba)

### P1: storage and io_worker (in progress)
- dd67c0b: rule and P2 additions from the user (backup push; UI update coalescing; `examples/lifecycle_probe`; runner reads refusal counters).
- 6f981d2: inventory of every `/storage` access with file/line and calling task, in docs/storage-io.md. **Fact:** after startup all of it runs in `system_worker` (prio 4); there is no production `io_worker`.
- Design proposal in docs/storage-io.md "Design for P1": task split (storage service loop and RP1/RF1/RE1 lines move to `io_worker`), four cross-task points (OTA vs packages mutex, recovery wake, power park, atomic getters), 4 KiB call cap, owner assertion, priority variants V0/V1.

- 4 KiB cap: `read_exact`/`write_exact` in `manager.c` and `store.c`, `data_write` header and blob, `read_candidate` in `elf_diagnostics.c`. Other writes are ≤ 512 B (RP1 data field `roundie_ota_protocol.h:18`, `ROUNDIE_IMPORT_CHUNK`). **Verified:** host 287/287 PASS (with IDF exported; without it `test_rep_deployment` errors on `IDF_PATH`, an environment issue), target build 1.85B rep 0 warnings. **Not verified:** `manager.c` has no host build, and no host test is known to read a file over 4 KiB through `store.c`; the device gate (step 4) covers ELF load and data write.

- Step 2, `io_worker` (this commit):
  - **Implemented:** `main/io_worker.c` (prio 1, 12 KiB stack, TWDT, queue of 2 lines) runs the package/face/launcher/import loop and executes RP1/RF1/RE1. `system_worker` forwards the lines without waiting (`roundie_io_forward`, `components/roundie_storage/io_forward.c`).
  - **Cross-task points:**
    - The OTA/packages claim is in `claim.c`. The mutex guards only the flags and the `runtime_busy` check, never VFS. OTA BEGIN is refused (259) during an io step or a package job; admission refuses while OTA holds the claim; the claim is released when the OTA phase is IDLE again.
    - Recovery: RS1 REQUEST no longer runs the teardown on `system_worker`; io_worker's next poll (≤ 10 ms) does.
    - Power: park at a 4 KiB chunk boundary (bounded I/O hook) or between jobs, with a 250 ms wait. On timeout PowerManager gives up the sleep with the new refusal `storage`. Restarts park with the same bound, then proceed (LittleFS is power-loss safe).
    - Busy getters read word-sized fields (**assumption**).
  - **Decisions:**
    - BUSY reply: `!XX1 <id> 259 {"format":1,"error":"busy"}`. The host tools raise `DeviceBusy`.
    - The 4 KiB helpers live in the portable `components/roundie_storage/bounded_io.c`. In `elf_diagnostics.c` `read_candidate`, a short read now continues instead of failing (behaviour change).
    - `tests/test_boot_guard.c`: the "teardown races REQUEST" case now expects the teardown on io_worker's poll, not inside REQUEST.
  - **Host:** 289/289 PASS, `--check-coverage` 335 tests and 0 problems. New `test_storage_io`:
    - a full queue makes exactly one non-blocking send attempt, then the exact BUSY line;
    - an invalid id answers `00000000`;
    - an oversized line is forwarded marked discard;
    - claim refusal during an io step and while busy, with `runtime_busy` called under the mutex;
    - grant, re-grant, release;
    - the three host parsers raise `DeviceBusy`.
  - **Target:** all 6 configurations (2 boards × rep, rep-nodev, legacy), 0 warnings.
  - **Device, 1.85B, image 0.3.88-eabi (`build-eabi`, SHA-256 `653cafb4…a348b0`):**
    - OTA upload/activate: running `ota_0`, VALID, verify exit 0;
    - package `org.roundie.eabiprobe` 1.0.0 (6000 B RPK, 5776 B ELF, the stopwatch ELF under a new id) installed and read back (SHA-256 `0ae81247…dfc85c`), count 7, `stack_min` 6408 B;
    - app data: the watch 1.3.0 theme changed by an injected press on widget 2 (`input_accepted` 1), then EXIT and relaunch: reply 0, no `app_storage` warning in the 4 s console capture. **Not verified:** that the theme read back matches (not visible from the host; the capture showed no log lines at all);
    - development ELF: the stopwatch ELF over RE1 (5776 B) loaded through START: `loader_result` 0, code 1932/data 997 match the artifact, running, then STOP. My first attempt used `run`, the one-shot command; ABI 3 refuses it with 258. That was an operator error, not a test retry.
  - **Not covered yet:** fake-VFS short reads / mid-chunk EOF (step 3); priority, starvation and parking under CPU load (step 4).

- Step 3, owner and mutex assertion plus fake-VFS tests (this commit):
  - **Implemented:**
    - `roundie_storage_owner_set` / `roundie_storage_access_check` in `claim.c`. The owner is the main task at startup, then io_worker from its first line. Before any owner is set, every caller passes.
    - `bounded_io.c` calls an access guard before every VFS call.
    - Development builds (`CONFIG_ROUNDIE_DEV_ELF`) install a guard in `io_worker.c` that logs and calls `abort()` when the caller is not the owner or holds the claim mutex.
    - **Scope:** only the bounded read/write calls are checked. open/stat/rename/unlink are not.
  - **Host:** 292/292 PASS, `--check-coverage` 338 tests and 0 problems.
    - `tests/test_storage_guard.c` over a fake VFS:
      - 4 KiB cap; hook only between calls; short reads; EOF in the middle of the second chunk and after a short read; error on call 2; short and zero writes;
      - negative cases: a call from another thread gives NOT_OWNER; a VFS call inside the claim mutex (from `runtime_busy`) gives UNDER_CLAIM.
      - Three mutants (no UNDER_CLAIM, no NOT_OWNER, no guard call) each fail the test (exit 134).
    - `tests/fake_vfs.c` (short limit; EOF / zero write after k bytes). The packages, watchface and ELF-development harnesses run a second time with 700 B short reads and pass unchanged.
    - Mid-chunk EOF cases with files over 4 KiB:
      - package install and launch (`manager.c:66/84/95/131`);
      - app data read (`:539/:545`, a 6000 B file with a test capacity of 8192 B; the runtime limit is 512 B) and data write (`:591`);
      - face load and discovery (`store.c:231/312/135`) and transfer write (`:500`);
      - development ELF `read_candidate` (`elf_diagnostics.c:82`).
    - Mutant "a short read fails" (`count != step`): the short-read variants of packages, watchface and ELF fail; the base runs pass. **Not covered by an EOF case:** `store.c:118/171` (56 B and 16 B, only in the short-read run).
  - **Target:** all 6 configurations, 0 warnings.
  - **Device, 1.85B, 0.3.89-eabi (guard active, SHA-256 `87a9ef69…e6008d606`):**
    - OTA: running `ota_1`, VALID, verify exit 0.
    - My first BEGIN got 259: the stopwatch development ELF from step 2 was stopped but not unloaded (state 11). This is the existing rule in `ota_diagnostics.c:154`, not a regression. Unload, then upload passed.
    - Then, all with exit 0: remove and reinstall of `eabiprobe`; watch launch, press, exit, relaunch, exit; development ELF upload, START, STOP, UNLOAD.
    - Boot status before and after: `safe` 0, `failures` 0, `reset` 2. So no abort from the guard on these paths.

- Fix 1 (user, before step 4): Safe Mode must not depend on io_worker.
  - **Verified fact (source):** a running application is torn down by `rep_executor` (`roundie_elf_set_allowed(false)` sets `mailbox.shutdown`, `roundie_elf_step` unloads), not by io_worker. The step 2 note "io_worker's next poll runs the teardown" holds only for what io_worker owns: aborting an open RE1/RP1 transfer and package jobs (`staging_busy`, `roundie_packages_busy`).
  - **Implemented:** `recovery_diagnostics.c`: a deferred REQUEST gets a deadline (`ROUNDIE_SAFE_TEARDOWN_TIMEOUT_US`, 3 s; a repeated REQUEST does not move it). At the deadline system_worker makes a planned restart without the NVS write. The request is already sealed in the RTC record by `roundie_boot_enter_safe`, so the next boot is Safe Mode (reason request).
  - **Decision / known limit:** no NVS write while an application may still run, as before. So in that boot `latch` is 0 and no cause record is written: a power loss in that Safe Mode boot returns to normal mode. BootGuard logic is unchanged.
  - Development hook `RS1 IOSTALL <1..30000 ms>` (`CONFIG_ROUNDIE_DEV_ELF` only): io_worker serves nothing until the deadline but keeps feeding its watchdog.
  - **Host:** `test_boot_guard` new case: busy runtime, no reset 1 µs before the deadline, reset at it with 0 NVS writes, the next software boot is Safe Mode/request with no fault_app. Mutant without the deadline branch fails. Host 292/292 PASS, coverage 338/0.
  - **Target:** 6 configurations, 0 warnings.
  - **Device, 1.85B, 0.3.90-eabi (ota_0, VALID, SHA-256 `89d93182…c0b1ff`), `run_p1_iostall.py` (`build-ci/eabi/p1-iostall/`): PASS.**
    - control (open RE1 transfer, io_worker serving): restart 1.01 s after REQUEST, latch 1;
    - stall (open transfer, IOSTALL 8000): restart 4.25 s after REQUEST (3 s timeout plus about 1.2 s boot, the same overhead as the control), latch 0, no fault_app;
    - app_stall (package running, IOSTALL 8000): restart 1.28 s, latch 1, which confirms the executor fact above;
    - each case ended with REBOOT and normal operation.
  - Runs that did not count (runner errors, new configurations): run 1 launched the app instead of opening a transfer, so the clean path ran (this is how the executor fact was found). Run 2 stopped because an app I had launched by hand was still loaded. Run 3 accepted a HEALTH reply from before the restart (REQUEST sets `safe` at once) and sent REBOOT to the old boot; the runner now requires a reply from a boot after REQUEST.
- Fix 2 (user, before step 4): the owner assertion covers every /storage VFS operation.
  - **Implemented:** guarded helpers in `roundie_bounded_io.h` (`roundie_io_open/close/read/write/lseek/fstat/fsync/stat/rename/unlink/mkdir/rmdir/opendir/readdir/closedir`). Each runs the owner check and then the libc call. The 126 direct calls in `roundie_packages/manager.c`, `roundie_watchface/store.c`, `main/elf_diagnostics.c`, `main/storage_bench.c` and `main/storage_diagnostics.c` now go through the helpers.
  - **CI rule:** `tools/ci/check_storage_vfs.py` (run by `tests/test_storage_io.py`) fails on a libc VFS call in `main/` or `components/` outside `components/roundie_storage/`, outside the files that only use the `/usbdisk` mount (`storage_import.c`, `storage_usb.c`), and outside lines marked `vfs-ok: <target>` (two `/dev/secondary` lines in `main.c`). Comments and strings are ignored.
  - **Decision:** the development storage bench runs its low-priority helper task (modes L/L0) as a registered guest (`roundie_storage_guest_set/clear`, a single slot) while io_worker waits for it. Without the slot the guard would abort the bench. The slot is only used by `storage_bench.c`.
  - **Host:** `test_storage_guard` makes all 15 operations as owner (OK), from another thread (NOT_OWNER), under the claim (UNDER_CLAIM), as guest (OK), and as guest after clear (NOT_OWNER). A mutant without the guard in rename fails. `StorageVfsRuleTests`: the repository is clean, and every listed call, helpers, comments, strings and markers are classified correctly. `test_packages` fault injection now redirects the helpers. Host 295/295 PASS, coverage 341/0.
  - **Target:** 6 configurations, 0 warnings; desktop build, 0 warnings.
  - **Device, 1.85B, 0.3.91-eabi (`ota_1`, VALID, SHA-256 `165a16c3…29dd7`):** with exit 0 for every step: package build, remove, install, list, launch, stop, unload; watch launch, stop, unload; face status; development ELF upload, start, stop, unload. Bench guest path: `XWRITE 64 4096 0 L`, `XREAD 64 4096 0 L`, `XWRITE/XREAD 64 16384 0 L0` all finished with code 0 and `low` 1. Afterwards `safe` 0, `failures` 0, `reset` 2 (software), so the guard did not abort.
  - Not verified: paths not exercised on the device (package import from USB uses `/usbdisk` and is outside the rule; package job failure paths are host-tested only).
- Fix 3 (user, before step 4): BUSY (transient) separated from precondition refusals.
  - **Decision:** new code 503 (`ROUNDIE_IO_BUSY_CODE`, not an `esp_err_t` value) means "not executed, send again". It is used for a full io_worker queue (RP1/RE1/RF1 BUSY payload) and for OTA BEGIN that meets an io_worker step (`roundie_storage_claim_ota_try` returns `IO_STEP`). 259 stays for preconditions. RO1 now names the precondition in `"refusal"`: "app", "transfer", "package", "safe-mode" or "not-ready" ("none" otherwise; "io" with 503). The table is in docs/storage-io.md.
  - **Hosts:** `roundie_time.check_busy` raises `DeviceBusy` only for code 503 with the BUSY payload; that payload under another code is malformed. `request()` in `roundie_package/elf/face/ota.py` retries only on `DeviceBusy`, at most 3 attempts, 200 ms apart. A 259 raises `DeviceError` with an instruction (for example `roundie_elf.py stop/unload --instance N`, `roundie_safe.py reboot`). `roundie_ota.py` also accepts RO1 replies without `refusal`, from older images. Manager: same retry rule; a 503 BUSY payload on RP1 is no longer rejected by `decodePackage`, which before this change would have closed the connection; new names for 259 and 503.
  - **Verified fact, pre-existing bug found by the device test:** `roundie_io_busy_reply` accepted only decimal digits in the request id. Hosts send lowercase hex (`secrets.token_hex`), so most BUSY replies were addressed to `00000000` and the host timed out instead of seeing BUSY. Fixed: same alphabet as `roundie_transfer_parse`, with a host test.
  - **Host:** `test_runtime_policy.c` (actual RO1 controller): 259 with "app"/"transfer"/"package"/"safe-mode", 503 with "io" while io_worker holds its step, and "none" on success. `test_storage_io.c`: 503 reply, hex id. Python: `test_busy_and_precondition_refusals`, `test_request_retries_only_busy`, `test_hosts_recognize_busy`, `test_hosts_retry_only_busy`, `test_precondition_hint_only_for_259`. Manager: 2 new tests, 108/108. Mutants (no code check in `check_busy`; `IO_STEP` answered 259; no hex letters in the BUSY id) all fail tests. Host 299/299 PASS, coverage 345/0; Manager typecheck clean.
  - **Target:** 6 configurations, 0 warnings; desktop build, 0 warnings.
  - **Device, 1.85B, 0.3.93-eabi (VALID, SHA-256 `13708850…392804`), `run_p1_refusal.py` (`build-ci/eabi/p1-refusal/`): PASS.** RO1 STATUS 0/"none"; with eabiprobe running, BEGIN 259/"app" and the host message carries the unload instruction; with an open RE1 transfer, 259/"transfer" with the abort instruction; io_worker stalled and 5 RE1 STATUS lines: codes `[0, 0, 503, 503, 503]` with the BUSY payload, then serving again; final `safe` 0, `failures` 0.
  - Run that did not count (0.3.92-eabi, before the id fix): lines 3–5 got no addressed reply. A diagnostic run with ids `0000000N` showed the 503 replies, which led to the id fix above.
  - Not verified on the device: the "io" refusal (the window between io_worker's check and claim cannot be produced on demand) and the host retry loop against a real BUSY (host-tested only).
- Fix 4 (user, before step 4): a development command replaces judging the watch theme by eye.
  - **Implemented:** `RP1 DATAHASH <id>` in `main/package_diagnostics.c` (whole file is `CONFIG_ROUNDIE_DEV_PACKAGES`, off in production). It replies `{"format":1,"datahash":<length>,"sha256":"<hex>"}` for the payload that `roundie_packages_data_read` returns, and NOT_FOUND for a package that is not installed. Read only; it runs on io_worker like every RP1 line, so it goes through the guarded helpers.
  - **Device, 1.85B, 0.3.94-eabi (VALID, SHA-256 `44e51c4a…dcd46a5`), `run_p1_datahash.py` (`build-ci/eabi/p1-datahash/`): PASS.** Stored theme t0 = 2. Launch, 1 press on the hands, exit: `{1, 0}`. Launch, no input, exit: the same hash. Launch, press, exit: `{1, 1}`. Launch, press, exit: `{1, 2}`. The app was stopped and unloaded before each hash, every press was accepted, an unknown id was refused, and the final state was `safe` 0, `failures` 0. The last step's value can only come from a watch that read the stored theme: one starting at theme 0 would write `{1, 1}`. The board is left with its original theme.
  - **Host / target:** host 299/299 PASS, coverage 345/0; 6 configurations, 0 warnings.
  - Not verified: persistence across a reboot (not part of the run); no host test of the C handler (development command, IDF-only; the runner decodes against hashes computed on the host).

### Next
- P1 implementation, in this order, each verified and committed:
  1. ~~4 KiB cap in the read/write helpers~~ done (see below);
  2. ~~`io_worker` task and line forwarding, with the four cross-task points; target build of all configurations~~ done (see above);
  3. ~~owner assertion plus host negative test~~ done (see above);
  4. ~~priority variants V0/V1 on 1.85B with `run_e0_stall.py --only lowprio` plus a starvation case~~ done (see below).
- P1 step 4 (2026-11-26): priority variants, starvation, park latency, io_worker watchdog.
  - **Decision: V1.**
    - Priorities: GUI 3, executor 2, io_worker 1, system_worker 4, audio 4.
    - io_worker TWDT unchanged (always subscribed, 10 s).
    - Reasons, tables and images: docs/storage-io.md "P1 step 4".
  - **Measured, `run_e0_stall.py --only lowprio` (PASS on all three images):**
    - GUI gap: V1 ≤ 30.0 ms. Partial V1 (io_worker 1 only) 33.6 ms.
    - system_worker STATUS ≤ 22.1 ms.
    - Synchronous read: 1582 KiB/s in V1 against 800 in V0. Write: 122 against 81.
  - **Measured, starvation (`run_p1_prio.py`, development RS1 BURN for 6 s; IOSTAT, IOPARK):**
    - V0: with both cores busy at priority 1, park times out (261 ms) and the read waits for the whole burn (5.3 s). V0 fails the criterion.
    - V1: park ≤ 19.8 ms and read ≤ 0.47 s for every load except both cores above priority 1. That case includes the executor at priority 2 on CPU1.
  - **Watchdog:**
    - Measured: io_worker feed gap ≤ 0.50 s under every load in V1, except both cores busy above priority 1. Package install, launch and remove each take ≤ 0.43 s per iteration.
    - Starving io_worker also starves IDLE0 and IDLE1, which the TWDT already watches. So the io_worker TWDT adds no reset path. Gating it or replacing it with progress supervision would change no outcome. This conclusion is derived; no reset was forced.
    - The 7.8 s "workload" gap is a measurement artefact: the development XWRITE bench feeds the TWDT directly every 64 KiB.
  - **Bug fixed:** executor start race.
    - Cause: the new executor task could arm its stack guard before its creator stored the handle.
    - Seen on V0: "arm: not on the guarded executor stack", then native apps were disabled until reboot.
    - Fix: the task waits for a notification from its creator.
  - **Diagnostic experiments:**
    - 0.3.96-eabi: partial V1, for the reference.
    - 0.3.97-eabi-v0: V0. It hit the race above; the run did not count.
    - 0.3.98-eabi-v0: V0 with the fix.
    - Neither V0 image is committed.
  - **Runs that did not count:**
    - The first runner attempts: wrong `install` CLI usage, then an upload left open (aborted).
    - One stale test package, removed.
  - **Board state:** the 1.85B runs 0.3.99-eabi (VALID).
  - **Not verified:** a forced TWDT reset (>10 s burn); a package near the 64 KiB cap; the 1.75C board; long-running load.

- Then Stage B (EABI). Scope: the EABI section of docs/roadmap.md. Branch `eabi` from main after stage A is merged. Commit after each subphase. Do not raise runtime limits. Do not implement later-stage services (random, deadline, sensor, ...) or the resource runtime.
  - P1, storage and io_worker:
    - inventory every `/storage` access (registry scan, ELF load, app data, RWF, USB importer, diagnostics, OTA) with file/line and calling task;
    - io_worker is the only owner of `/storage` access; VFS calls are limited to 4 KiB; system_worker does only short operations and keeps state in RAM;
    - a storage-layer assertion (development build) that the caller is io_worker, plus a negative test;
    - priorities: priority 0 only for idle. Measure io_worker above idle and below GUI (e.g. GUI 3, executor 2, io_worker 1, system_worker above GUI) against today's io_worker at prio 0. Criteria: GUI gap ≤ 33 ms, system_worker reply ≤ 33 ms, read/write throughput, and a starvation test (an io_worker read makes bounded progress while a development app loads the CPU). Choose the variant from the measurements.
  - P2, ABI 4:
    - shared types; handles (slot + generation); a 64 B event without an ABI field, with a 24 B payload;
    - lifecycle RUNNING → STOPPING → UNLOADED; core/UI/audio tables; modular service getters; event routing by import, subscription, owner and generation;
    - in STOPPING, UI operations return a non-critical code. A result after a system-requested STOP is not shown to the user as an error (except crash or timeout). The ELFDELAY regression for −23 (show_laps) and −24 (show_state) must return to Home;
    - UI backpressure "busy" code (roadmap "Backpressure UI (P2)");
    - UI update coalescing (user, 2026-11-26): a `ui_update` of the same widget and the same property overwrites the pending value instead of adding a queue entry. The queue holds only non-coalescable operations (create, delete, dialogs, ...); the "busy" code stays for those. A host test replays the stop+lap sequence within 10 ms (13 commands) and must show no refusals after the change;
    - test app `examples/lifecycle_probe` (development only): records every returned UI operation code and the lifecycle state; the runner reads them. The STOP (ELFDELAY) regression test uses it instead of the stopwatch;
    - after scenarios that use the UI queue, the runner reads the refusal counters by cause and writes them to the report;
    - both ELF validators (Xtensa, x86-64) and the Linux backend.
  - P3, package format per the roadmap:
    - header + manifest + bounded requirement list `(service_id, feature_mask)` + ELF;
    - a deterministic builder and a canonical parser shared by host and firmware;
    - host tests, including bad magic and version;
    - a new docs/package-format.md.
  - P4, PackageManager:
    - one registry `/storage/packages`; classify OBSOLETE vs CORRUPT vs I/O error; replace/REMOVE an OBSOLETE entry;
    - an activation record anchored to the metadata; selective verification at scan and start; a global space budget;
    - app data in `/storage/app-data/<id>`, independent of the registry;
    - firmware start erases and moves nothing; all I/O through the P1 io_worker.
  - P5: migrate the examples, tools, USB import and CI. `reinstall-all`: `--clean` removes every OBSOLETE entry after listing it; `--prune` handles IDs outside the set; app-data is always kept.
  - P6, Manager: format version, ABI, target, requirements, declared services, refusal reasons, free/required space, install progress.
  - P7, gate on 1.85B (1.75C BLOCKED unless connected). Items:
    - OTA, reinstall-all, stopwatch start/unload, sound PCM;
    - watch keeps its theme after removal and reinstall;
    - OBSOLETE entries kept at scan and start; replacement and REMOVE; interrupted commit; the STOP regression;
    - delta against E0: memory, stacks, GUI gaps during a 512 KiB install.
    Report in docs/eabi-report.md. Apply the closure rule before closing.
  - P8: update the roadmap with findings that change it, then publication.
- Publication details from the task:
  - PR description: scope, results, PENDING/BLOCKED, decisions.
  - If CI fails: fix, commit, push, repeat.
  - If a merge condition cannot be met, or the merge conflicts in a way that needs a design decision: leave the PR open, give the reason in the PR and here, and stop.
- Continuity: if the session context grows large, close the current subphase, update this file and stop, so a new session can continue from the repo alone.
