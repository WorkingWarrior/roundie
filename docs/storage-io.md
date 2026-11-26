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
