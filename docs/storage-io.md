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
