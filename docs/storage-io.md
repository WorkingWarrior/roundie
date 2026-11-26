# Storage I/O ownership (EABI P1)

## Inventory of `/storage` access, 2026-11-26 (main 48ad0ba)

**Verified fact (source read, `grep` of `components/` and `main/`, tests excluded).**
Task priorities today: GUI loop in the main task (prio 1, `app_main`), `rep_executor` (prio 1,
pinned CPU1, `executor_stack.c:56`), `audio` (prio 3), `system_worker` (prio 4, `main.c:1238`).
There is no `io_worker` in production. The prio 0 `io_worker` of E0 §14 exists only inside
`main/storage_bench.c` (helper task `fsbench_x`).

`/storage` is LittleFS on partition `storage` (`partitions/rep.csv`, 0x710000, 8.8 MiB).
`/usbdisk` is a PSRAM block device with FAT for USB storage mode (`main/storage_usb.c:21`,
`esp_blockdev`), not `/storage`.

| # | Area | Code (file:function, lines of VFS calls) | Calling task | Trigger |
|---|---|---|---|---|
| 1 | Mount, capacity | `roundie_storage/storage.c`: `roundie_storage_mount` 93, `unmount_locked` 118, `snapshot` (`esp_littlefs_info`) 162 | main task at startup (`main.c:1227` `roundie_storage_diagnostics`, before `system_worker` starts at 1238) | boot |
| 2 | Capacity query | `main/storage_diagnostics.c:19` `roundie_storage_usage` → `roundie_storage_capacity` → `esp_littlefs_info` | `system_worker` (`app_runtime.c:133` package/face capacity, `package_diagnostics.c:211`, `storage_bench.c`) | install, RP1 status |
| 3 | Boot filesystem test | `storage_diagnostics.c` `migration_filesystem_test` 50–116 (open, stat, rename, fsync, unlink) | main task at startup, only with a provisioned request file | boot |
| 4 | Registry scan | `roundie_packages/manager.c` `roundie_packages_scan` 308–360 (opendir, readdir, stat) and `inspect_file` 90 (open, read, hash of each `.rpk`); `read_selection` 73 | `system_worker`: `launcher.c:529` (`roundie_launcher_poll`, `main.c:438`), after install/remove inside `manager.c`, `package_diagnostics.c:185` (RP1) | boot, launcher refresh, RP1 |
| 5 | ELF load | `manager.c` `roundie_packages_launch` → `validate_file` 137 (open, whole file read into PSRAM, hash) → `roundie_packages_execute` (`app_runtime.c:166`) | `system_worker`: `launcher.c:498`, `package_diagnostics.c:190` | app start |
| 6 | Package install / remove | `manager.c` `roundie_packages_begin` 215–222, `_write`, `_finish` 262, `_install` 438–460, `cancel` 185, `_remove` 520–526 | `system_worker`: RP1 (`package_diagnostics.c:25–29, 192`), USB importer (`storage_import.c:157–175`) | upload, import |
| 7 | App data | `manager.c` `roundie_packages_data_read` 545, `roundie_packages_data_write` 581–609 (stat, unlink, open, fsync, rename) | `system_worker`: read in `roundie_packages_execute` (`app_runtime.c:175`); write in `persist_storage` (`app_runtime.c:98`) from `roundie_app_runtime_poll`. The executor only touches the RAM blob (`roundie_elf_storage_set/take`) | app start, after unload |
| 8 | RWF faces | `roundie_watchface/store.c`: `scan` 297–344, `load_file` 228, `read_preference` 149, `read_settings` 185, `replace_file` 132–140, `start` 444, `cancel` 465, `begin` 487–494, `finish` 537, `install` 616, `remove` 694–698 | `system_worker`: `roundie_watchface_init` (`main.c:419`), `roundie_watchface_poll` (`app_runtime.c:112`), RW1 (`face_diagnostics.c:21–29, 71–73`) | boot, face change, upload |
| 9 | USB importer | `main/storage_import.c` `ops_list/open/close/remove` 37–97 read `/usbdisk/Inbox`; writes to `/storage` go through row 6 | `system_worker` (`roundie_storage_import_poll`, `main.c:437`) | after storage mode |
| 10 | Development ELF | `main/elf_diagnostics.c` `operate` 129–193, `read_candidate` 67, `abort_transfer` 48 (`/storage/dev`) | `system_worker` (RE1 via `poll_time_commands`) | development upload |
| 11 | Storage bench | `main/storage_bench.c` `prepare` 121–145, `measure` 157, `x_execute` 310–372, `x_command` 529, `roundie_storage_bench_command` 602 (`/storage/dev`) | `system_worker` and the `fsbench_x` helper (configurable prio/core) | RP1 bench, development |
| 12 | OTA | none: `roundie_ota` writes the app partition with `esp_ota_*`, not VFS | `system_worker` | RO1 |
| 13 | USB storage mode | `main/storage_usb.c` `write_text` 212, `roundie_storage_usb_start` 261 on `/usbdisk` only | main task (GUI loop, `main.c:1434, 1495`) | storage mode |

Paths: `/storage/packages` (`manager.c:14`), `/storage/packages/<id>/data` (app data),
`/storage/faces` (`store.c:17`), `/storage/dev` (`elf_diagnostics.c:22`, `storage_bench.c:24`),
`/storage/.roundie-m2b-*` (row 3).

**Facts that bound the design.**
- After startup every `/storage` access runs in `system_worker` (prio 4, above GUI). This is
  the case E0 §14 measured: one LittleFS call from `system_worker` during a write gave
  145–228 ms GUI gaps, and the whole package/RWF path is synchronous in that task.
- `system_worker` also owns serial RX/TX (one owner, `main.c:371`), OTA, recovery and power.
  `roundie_power_worker_poll` relies on "this worker owns every Flash writer"
  (`main.c:441`), so light sleep may only start with no storage job running.
- Startup rows 1 and 3 run before any worker exists, with exclusive ownership.
</content>
</invoke>

## Design for P1 (2026-11-26; implemented in P1 step 2, see docs/progress.md)

**Assumption to check while implementing:** no other code touches the state of the
packages manager, watch face store, launcher core and app runtime outside `system_worker`,
except the calls listed under "Cross-task points". The grep above found no others.

**Task split.** The storage-owning service loop moves out of `system_worker` into a new
`io_worker`. Module state stays owned by one task, as today; only the task changes.
- `io_worker` runs `roundie_app_runtime_faces_init` (now `main.c:419`), then a loop of
  `roundie_app_runtime_poll`, `roundie_storage_import_poll` and `roundie_launcher_poll`
  (now `main.c:436–438`), and executes forwarded `!RP1`, `!RF1`, `!RE1` lines
  (`main.c:335–343`). It waits on its line queue with a 10 ms timeout instead of
  `vTaskDelay(10)`, and writes each reply as one `usb_serial_jtag_write_bytes` call (whole
  line), as the handlers already do.
- `system_worker` keeps serial RX and demux, RT1, RG1, RW1, RO1, RA1, RS1, OTA, recovery,
  power and sensors. It copies an io-owned line and its `discard` flag into a 2-entry queue.
  When the queue is full it drops the line, logs it and counts it. The host times out as for
  a lost frame; the host protocols are stop-and-wait.
- Startup rows 1 and 3 stay in the main task. Ownership passes to `io_worker` when it starts.

**Cross-task points (today implicit through one task).**
1. OTA vs package transaction or launch: `ota_diagnostics.c:129,154` check
   `roundie_app_runtime_busy()`, and package admission checks `ota.phase == IDLE`
   (`app_runtime.c:43–46`). In two tasks this is a check-then-act race. Fix: one short
   FreeRTOS mutex held only while checking and claiming (OTA begin; package begin, install,
   remove, launch, import). No I/O under it.
2. `recovery_diagnostics.c:444` calls `roundie_app_runtime_poll` from `system_worker` after
   closing admission. Replace it with a wake of `io_worker`. The deferred restart
   (`recovery_diagnostics.c:155`) already waits for `!roundie_app_runtime_busy()`.
3. Power: light sleep relies on one owner of every Flash writer (`main.c:441`). Add a park
   request: `system_worker` asks `io_worker` to park between jobs and sleeps only after the
   acknowledgement, else it defers sleep. `roundie_launcher_park_home/resume_home`
   (`power_manager.c:183,192`) change launcher state, so `io_worker` runs them during park and
   resume. NVS writers (gauge learning, BootGuard persistence) stay in `system_worker`.
4. Read-only getters called across tasks (`roundie_app_runtime_busy`, `_allowed`,
   `roundie_launcher_status`): the flags they read must be atomic or read under the item 1
   mutex.
5. GUI side (`roundie_launcher_gui_*`, `roundie_watchface_gui_*`) already uses mailboxes
   between the GUI and the worker; check that they do not assume a specific task.

**4 KiB VFS calls.** Today some `read` calls cover a whole file: `read_exact` in
`manager.c:53` and `store.c:110` (whole ELF or RWF), `elf_diagnostics.c:78` (whole
candidate), `data_write` `manager.c:604–605` (header plus blob up to `REP_STORAGE_CAPACITY`).
Cap each call at 4096 B in the bounded read/write helpers.

**Owner assertion.** One check at the entry of each public storage-touching function
(table rows 2 and 4–11), in a development build: the current task is the registered owner
(main task until hand-over, then `io_worker`). The check is a pure function so a host test
covers it, plus a negative test: a call from another task is caught.

**Priority variants to measure (task criteria).**
- V0: GUI 1, executor 1 (CPU1), `io_worker` 0, `system_worker` 4 (E0 §14 model).
- V1: GUI 3, executor 2, `io_worker` 1, `system_worker` 4 (above GUI), audio above GUI.
- Criteria: GUI gap ≤ 33 ms, `system_worker` reply ≤ 33 ms, read and write throughput, and
  starvation: an `io_worker` read makes bounded progress while a development app loads the
  CPU. Tool: `run_e0_stall.py --only lowprio`, extended with the starvation case.

## P1 requirements added by the user (2026-11-26)

These refine the design above and are implemented in P1 steps 2–4.

1. **Full `io_worker` queue: answer BUSY at once.** `system_worker` never waits for room in
   the `io_worker` line queue (enqueue with zero timeout). On a full queue it replies to the
   forwarded line immediately with the protocol's BUSY reply and counts it, instead of
   dropping the line silently (this replaces "drops the line" in "Task split"). Host test:
   the forwarding decision with a full queue yields BUSY and no enqueue. (Step 2.)
2. **OTA ↔ package mutex guards only flags.** The mutex of cross-task point 1 is held only
   while checking and setting the claim flags; it is never held across a VFS call. A
   development-build assertion catches a VFS call while the calling task holds the mutex,
   plus a host negative test that triggers it. (Step 2 the mutex; step 3 the assertion and
   test, together with the owner assertion.)
3. **Bounded park before light sleep.** `io_worker` parks only at a 4 KiB chunk boundary
   (between VFS calls, never inside one transfer step). `PowerManager` waits for the park
   acknowledgement for a bounded time; on timeout it gives up this sleep attempt (stays
   awake and retries on a later idle decision) instead of waiting indefinitely. The step 4
   starvation test also covers parking while a development app loads the CPU. (Step 2 the
   mechanism; step 4 the measurement.)
4. **4 KiB cap under short reads.** A host test with a substituted VFS that returns short
   reads and EOF in the middle of a 4 KiB chunk covers every loop changed by the cap:
   `manager.c`, `store.c`, app data, development ELF. If `manager.c` cannot be built on the
   host, its read loops move into a host-testable module. Already in step 2, a short trial
   on the 1.85B: install a package larger than 4 KiB, read app data, load the development
   ELF. (Step 2 the trial; step 3 the host test.)

### Fixes before P1 step 4 (2026-11-26)
- **Safe Mode without io_worker (fix 1):** a REQUEST deferred because the runtime is busy restarts into Safe Mode at `ROUNDIE_SAFE_TEARDOWN_TIMEOUT_US` (3 s) even if io_worker never finishes the teardown (no NVS write in that case). Details and measurements: docs/progress.md.
- **Every /storage VFS call through the guarded helpers (fix 2):** `roundie_io_*` in `roundie_bounded_io.h` cover open, close, read, write, lseek, fstat, fsync, stat, rename, unlink, mkdir, rmdir, opendir, readdir and closedir. The owner check now applies to all of them, not only the bounded read/write calls. `tools/ci/check_storage_vfs.py` forbids direct libc VFS calls outside `components/roundie_storage/`.
- **Reply codes (fix 3):**

  | Code | Meaning | Host |
  |---|---|---|
  | 503 (`ROUNDIE_IO_BUSY_CODE`) | transient, the request was not executed: the io_worker queue is full (RP1/RE1/RF1, payload `{"format":1,"error":"busy"}`), or OTA BEGIN met an io_worker step (RO1 `refusal` "io") | `DeviceBusy`; `request()` sends it again, at most 3 attempts, 200 ms apart (Python tools and Manager) |
  | 259 (`ESP_ERR_INVALID_STATE`) | a precondition does not hold; RO1 names it in `refusal`: "app" (an ELF is loaded or reserved), "transfer", "package", "safe-mode", "not-ready" | `DeviceError` with an instruction; never retried |

  The BUSY payload under any code other than 503 is a malformed reply. Each RO1 reply carries `"refusal"` ("none" when nothing was refused). `roundie_ota.py` still accepts replies without it, so OTA from older images works.
  The BUSY reply addresses the request id from the line when it is 8 lowercase hex characters, the same alphabet as the protocol parsers. Before fix 3 it accepted decimal digits only, so a host id with a–f got a reply for `00000000` and the host timed out.

### Status after P1 step 3 (2026-11-26)
- Requirement 1 (BUSY): implemented and host-tested (step 2).
- Requirement 2 (flags only under the mutex): development-build guard in `bounded_io.c` and `io_worker.c`, with negative host tests (step 3). Only the bounded read/write calls are checked.
- Requirement 3 (bounded park): implemented (step 2). The measurement under CPU load is still pending (step 4).
- Requirement 4 (4 KiB, short reads, mid-chunk EOF): host tests for manager.c, store.c, app data and the development ELF (step 3). 1.85B trial done (step 2).

### P1 step 4: priority variants and the io_worker watchdog (2026-11-26)

**Decision: V1** (GUI 3, executor 2, `io_worker` 1, `system_worker` 4, audio 4).
`GUI_PRIORITY` in `main/main.c`, `EXECUTOR_PRIORITY` in
`components/roundie_elf/idf/executor_stack.c`, `TASK_PRIORITY` in
`components/roundie_audio/audio.c`. The `io_worker` watchdog stays as it is (subscribed all
the time, 10 s); see "Watchdog" below.

Images on the 1.85B (serial 28:84:85:87:98:7C). The two V0/partial images are diagnostic
builds and are not committed:
- 0.3.96-eabi: partial V1 (only `io_worker` 1; GUI 1, executor 1, audio 3).
- 0.3.98-eabi-v0: V0 (`io_worker` 0) with the executor start fix.
- 0.3.99-eabi: full V1, the committed tree.

**`run_e0_stall.py --only lowprio`.** The stopwatch runs during reads and is closed during
writes. Each variant has 2 repeats; the table gives the maximum over both repeats.

| variant | GUI gap ms | system_worker STATUS ms | read W / L / L0 KiB/s | write W / L / L0 KiB/s |
|---|---|---|---|---|
| partial V1 | **33.6** (W 16 KiB, yield 1) | 13.6 | 1585 / 2503 / 1740 | 120 / 77 / 91 |
| V0 | 30.0 | 13.0 | **800** / 2540 / 1840 | **81** / 82 / 97 |
| V1 | 30.0 | 22.1 | 1582 / 2482 / 1690 | 122 / 80 / 92 |

Synchronous reads on `io_worker` (W) run at half speed in V0, and writes drop by a third,
because `io_worker` time-slices with the IDLE tasks. In partial V1 the GUI gap goes over
33 ms: the GUI and `io_worker` share priority 1. The probe task `c0p1` (priority 1, core 0)
gaps up to 35 ms in V1. It is a measurement probe below the GUI, not a criterion.

**Starvation, `run_p1_prio.py`.** RS1 BURN (development) spins one task per core for 6 s at
the given priority. During the burn, 3× IOPARK, 3× RS1 STATUS and
`XREAD 1024 4096 0 W` run on `io_worker`. Each cell gives the maximum park wait / the read's
wall time.

| load (core, priority) | partial V1 | V0 | V1 |
|---|---|---|---|
| none | 9.8 ms / 0.33 s | 19.8 ms / 0.47 s | 9.8 ms / 0.31 s |
| core 1, 1 | 9.8 ms / 0.32 s | 19.8 ms / 0.65 s | 9.8 ms / 0.32 s |
| both, 1 (app + GUI busy) | 19.8 ms / 0.47 s | **timeout 261 ms / 5.31 s** | 19.8 ms / 0.47 s |
| core 0, 2 | 9.8 ms / 0.31 s | 19.8 ms / 0.61 s | 9.8 ms / 0.30 s |
| core 1, 2 (executor priority) | — | — | 9.8 ms / 0.32 s |
| both, 2 | timeout / 5.18 s | timeout / 5.32 s | timeout / 5.14 s |

- RS1 STATUS stays ≤ 12 ms in every case.
- A park timeout refuses sleep with "storage", as designed.
- V0 fails the starvation criterion. When both cores are busy at priority 1, `io_worker`
  makes no progress at all until the load ends.
- V1 makes bounded progress unless both cores are busy above priority 1 the whole time.

**Watchdog.** IOSTAT records the longest gap between two `io_worker` feeds. Hypothesis
"can the 10 s `io_worker` TWDT reset the board under reasonable load":
- **Measured, ordinary work:** each iteration is short.
  - Feed gap ≤ 0.50 s in every V1 case except "both, 2".
  - Package upload + install (6000 B) takes 0.43 s per iteration, launch 0.23 s, remove 0.26 s.
  - Packages are capped at `ROUNDIE_PACKAGE_MAX` (64 KiB ELF), and reads run at about
    1.5 MiB/s.
- **Measured, starvation:** the gap equals the load duration (6.3 s for a 6 s burn). It
  appears only when both cores are busy above `io_worker`.
- **Why the `io_worker` TWDT adds no reset path (derived, not reset-tested):**
  - Starving the unpinned `io_worker` needs both cores busy above priority 1.
  - That also keeps IDLE0 and IDLE1 (priority 0) off the CPU.
  - Both IDLE tasks are watched (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1=y`), so the
    TWDT would fire at the same 10 s without `io_worker`.
  - Neither "TWDT only while `io_worker` has work" nor progress supervision would avoid
    that reset. Both would only drop the detection of a hung `io_worker` iteration.
- The 7.8 s gap of the "workload" case is a measurement artefact. It is the development
  `XWRITE 1024 4096 0 W` bench, which feeds the TWDT directly every 64 KiB
  (`main/storage_bench.c:356`), bypassing the IOSTAT counter. With `chunk_max` 50 ms, the
  real gap is at most 16 × 50 ms = 0.8 s (derived).

**Bug found and fixed: executor start race.**
- Cause: `roundie_elf_stack_create` stored `executor_task` only after
  `xTaskCreateStaticPinnedToCore` returned. The new task can run first on CPU1, and
  `roundie_elf_stack_arm` compares the current task with `executor_task`.
- Evidence: on V0 the log showed `arm: not on the guarded executor stack (core 1 sp
  3c14fce0)` before the creator's `task=` line. After that, applications were disabled
  until the next reboot.
- Risk: in V1 the executor (2) outranks its creator `io_worker` (1), so on CPU1 the race
  would be the normal case.
- Fix: the executor task waits for a notification, which the creator sends once the handle
  is stored. Verified on 0.3.98-eabi-v0 and 0.3.99-eabi: "monitor self-test passed", and
  the stopwatch runs.

**Not verified:**
- An actual TWDT reset by a burn longer than 10 s (not run: it only reproduces the
  documented IDLE watchdog).
- A package close to the 64 KiB cap.
- The 1.75C board.
- Long-running load.
