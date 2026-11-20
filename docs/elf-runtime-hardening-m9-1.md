# M9.1 — Native ELF runtime hardening and fault containment

Status: implemented on branch `m9.1-hardening`. Device results are from the 1.85B
board (serial `28:84:85:87:98:7C`) running firmware 0.3.58–0.3.60 and, for the
review, 0.3.67 plus uncommitted diagnostic images 0.3.61–0.3.66 and 0.3.68. Each statement
is marked **verified** (with its evidence), **decision**, **assumption** or
**PENDING**.

This is not memory isolation. A native ELF still runs unisolated in the firmware
address space (spec §§4–10, [elf-loader.md](elf-loader.md)). M9.1 limits one
failure class, application stack overflow, and makes crash attribution reliable.

## 1. The M9 failure

**Verified (M9, 0.3.56; [m9-stabilization.md](m9-stabilization.md)):**
`fault_elf stack` recursed past the 12 KiB internal-RAM `rep_executor` stack.
The board then panicked with LoadProhibited in `SysTickIsrHandler` on core 1
(EXCVADDR `0x0006008f`) and entered Safe Mode with reason=core. The crash was
recorded as a firmware fault.

Mechanism:

- **Verified (disassembly).** Each `recurse()` frame is 544 B (`entry a1, 0x220`).
  The function writes only `page[0]`.
- **Assumption.** Window-overflow spills write the 16-byte base save area below
  each frame's SP.
- The stack is a heap block in internal DRAM. Below it lie other heap blocks:
  TCBs, queues, timer lists and driver state. The descent overwrote them.
  - The first victim used was a SysTick/timer structure (the fault address
    `0x6008f` looks like a corrupted pointer).
  - Which object was hit is not established.
- The FreeRTOS canary is checked only at a context switch, after the damage.
- The end-of-stack watchpoint (`CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK`) watches
  32 bytes. A 544-B stride with about 33 written bytes per frame jumps over it for
  480 of 544 stack-end alignments. The device did not fire it (M9).
- Host model: `tests/test_stack_overflow_model.py`. It replays this write pattern
  deterministically; it is a model, not a hardware result. It shows:
  - the watchpoint is skipped for 480 of 544 stack-end alignments;
  - a 64 KiB guard page is hit for every alignment;
  - the red zone holds the largest single ENTRY frame plus the interrupt frame;
  - `recurse()` crosses the monitor limit more than 70 frames before reaching
    the guard.

## 2. Protections compared

| Mechanism | Class | Stops `recurse()` before Core memory? | Cost | Decision |
|---|---|---|---|---|
| FreeRTOS canary (existing) | after the fact | No: checked at a context switch | none | kept |
| End-of-stack watchpoint | early detection | No: 32 B are jumped over (M9 device, model) | 1 debug watchpoint per core | rejected in M9 |
| High-water-mark check | after the fact | No: reads the damage | none | diagnostics only (`usable_min`) |
| Callback time budget | detection | No: it cannot interrupt native code | none | detection only, unchanged |
| Bigger DRAM stack | none | No: delays the same corruption | internal RAM | rejected |
| Separate FreeRTOS task | none | No: tasks share one address space | — | not a protection by itself |
| Unmapped PSRAM guard page (MMU) | prevention | Yes: a write or read there faults instead of reaching other memory | 64 KiB PSRAM | **adopted** |
| Assist-debug SP monitor (core 1) | early detection | Yes: SP below the red-zone top raises a level-3 interrupt | 1 interrupt, 40 KiB red zone | **adopted, with verification in the ISR** |
| MPU / PMS / World Controller isolation | prevention for all writes | Not evaluated on this board | unknown | out of scope; not assumed |

## 3. Design (decision)

Source: `components/roundie_elf/idf/executor_stack.c`.

One 128 KiB PSRAM block, aligned to the 64 KiB MMU page, never freed. Ascending:

| Range | Size | Mapping | Role |
|---|---|---|---|
| guard | 64 KiB | unmapped | last line: MMU fault |
| red zone | 40 KiB | mapped | room for the detection to run |
| usable stack | 24 KiB | mapped | executor and application frames |

- The `rep_executor` task is static, pinned to core 1, priority 1, with a
  64 KiB stack covering the red zone and usable part. Before M9.1 it had 12 KiB
  of internal RAM.
- The usable 24 KiB is 3× the 8 KiB application budget
  ([app-lifecycle.md](app-lifecycle.md)).
- The red zone is at least the largest single ENTRY frame (32,760 B) plus 4 KiB
  for the saved interrupt frame. Asserted at compile time and in the host model.
- Unmapping follows `esp_mmu_unmap()`:
  1. write back and invalidate the page;
  2. freeze the caches with interrupts off;
  3. `mmu_hal_unmap_region`;
  4. invalidate the address range;
  5. unfreeze.
  - It is checked afterwards: the guard must be unmapped and the stack mapped.
- **Verified (code review, IDF v6.1).** On S3, the elf_loader flush uses
  `esp_cache_msync` and never disables the cache. `CONFIG_ELF_LOADER_SET_MMU` is
  off. No executor path runs with the cache disabled, so a PSRAM stack is
  usable. SHA DMA handles PSRAM buffers.

### PSRAM stack and cache-disabling operations (review, IDF v6.1)

Source review of the pinned IDF v6.1 image and of this sdkconfig
(`SPIRAM_FETCH_INSTRUCTIONS`, `SPIRAM_RODATA`, `SPIRAM_XIP_FROM_PSRAM`,
`FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`, `SPI_FLASH_YIELD_DURING_ERASE` set;
`SPI_FLASH_AUTO_SUSPEND`, `LITTLEFS_MMAP_PARTITION` unset). Not a device
measurement.

- **Flash reads** never disable the cache here. `spi_flash_os_func_app.c`
  defines `MMAP_EXECUTABLES_FROM_FLASH` as 0 when code and rodata run from
  PSRAM.
- **Flash writes and erases** (NVS, LittleFS, OTA) disable the cache only while
  a Flash mapping exists (`flash_mmap_remain()`), for example during an image
  check. The project itself creates no Flash mapping.
- **While the cache is off**, `spi_flash_disable_interrupts_caches_and_other_cpu()`
  stalls the other core through `esp_ipc`. The IPC task has an internal stack and
  preempts the executor on core 1, so the executor's PSRAM stack is not touched.
  Non-IRAM interrupts, including the stack monitor, are masked, but the executor
  does not run in that window.
- **On the executor itself**, the same function asserts
  `esp_task_stack_is_sane_cache_disabled()`, as does light sleep
  (`sleep_modes.c`). A cache-disabling call from the executor is therefore an
  abort (controlled panic), not silent corruption. No such call path exists:
  `roundie_elf` and `roundie_app_ui` make no Flash, NVS or LittleFS call, and the
  SDK services reachable from callbacks are the UI queue, time, the audio ring,
  logging and `app_alloc`.
- **Concurrency.** RO1 refuses every OTA mutation while an application is loaded
  (verified on the device in M9.1). Storage, package and OTA work runs in system
  workers.
- **Rule.** Future SDK services must not write Flash, NVS or LittleFS on the
  executor; they hand the work to a system worker (AGENTS.md).
- **Stack monitor.**
  - Mechanism: the ESP32-S3 assist-debug `SP_SPILL_MIN` interrupt of core 1,
    with `SP_MIN` set to the red-zone top. It is allocated at level 3.
  - Self-test on every executor start, with the abort suppressed:
    1. one frame below `SP_MIN` must fire, and the latched PC must lie in the
       probe function;
    2. a 24-deep recursion through window overflows must fire;
    3. at the real limit, nothing may fire within the window.
- **Fail closed.** If the block, the unmapping, the interrupt or the self-test
  fails, the executor refuses every load (`ESP_ERR_NOT_SUPPORTED`; host test
  in `tests/test_elf.c`).
- The panic hook (`idf/crash_target.c`) never reads the guard page. It does no
  allocation, logging or Flash access.

### Device finding: the monitor fires spuriously (verified)

| Firmware | Observation |
|---|---|
| 0.3.58: monitor aborts on any hit | Self-test passed. Then the monitor fired in `rep_executor` during `ESP_LOGI`, blocking on the log mutex. Latched PC: `vPortEnterCritical` (inlined in `pvTaskIncrementMutexHeldCount`). No stack pointer was below the limit. Result: panic, Safe Mode reason=core, and `origin executor_stack_overflow` (false positive) |
| 0.3.59: ISR checks the interrupted SP | No false abort. 35–40 ignored hits/s while `org.roundie.watch` ran |
| 0.3.60: stronger self-test | Self-test passed on every boot observed. Ignored hits: about 6/s with the watch running, 0 in 30 s with no application loaded |

- **Hypothesis (unverified).** The comparator sees transient `a1` values of
  rotated register windows during window exceptions or context switches.

**Spurious hits and the saved SP (verified, diagnostic image 0.3.68, 30 s, watch
running, not committed).**

- 1,483 hits; in 1,120 of them the executor was the interrupted task.
- Every SP read from its interrupt frame lay between `0x3c13fa10` and
  `0x3c13fbc0`, the top 1.5 KiB of the stack. That matches the measured executor
  peak (3,200 B). None was below the limit (`0x3c13a000`).
- Latched PCs are ordinary code:
  - `printf` and USB-JTAG writes;
  - FreeRTOS mutex and critical sections (`pvTaskIncrementMutexHeldCount`,
    `xTaskPriorityDisinherit`, `xQueueSemaphoreTake`);
  - `_WindowUnderflow8`;
  - the task watchdog feed in `IDLE1`. The remaining 363 hits had another task
    interrupted.
- So the monitor fires while the real SP is about 22 KiB above `SP_MIN`. The SP
  read is consistent with the executor's real stack use but was not checked
  against a second, independent SP source.
- Code reading, not measured: if the level-3 interrupt nests in another
  interrupt, `pxTopOfStack` holds the SP saved at the outer entry. It is still
  the executor's SP, because the executor does not run in between.

**`ASSIST_DEBUG_CORE_1_SP_UNSTABLE` (verified on the 1.85B, M9.1 review).**

- The register map gives R/W, bits 7:0, default 0, and no description.
- **It survives a software restart.** 0.3.62 wrote 255; after `RTC_SW_CPU_RST`,
  0.3.63, which did not write it, read 255. The IDF bootloader enables the
  assist-debug clock and releases its reset (`bootloader_esp32s3.c`), but never
  resets the block.
- **Self-test by value** (0.3.65 and 0.3.66, 20 runs for each value 4–16):

| `SP_UNSTABLE` | Self-test | Failing check |
|---|---|---|
| 0 | 0 of 1 | nothing fires |
| 1–3 | 0 of 1 | also fires at the real limit |
| 4 | 3/20 | also fires at the real limit |
| 5 / 6 | 14/20 / 17/20 | as 4, less often |
| 7–12 | 20/20 each | — |
| 13 / 14 / 15 / 16 | 16/20 / 5/20 / 3/20 / 1/20 | the recursion is missed |
| 32, 64, 128, 255 | 0 of 1 | nothing fires |

- **Hypothesis.** The value is a filter: the number of cycles SP must stay below
  `SP_MIN` before the monitor latches.
- **Consequence, a bug in the first M9.1 commit.** The code relied on the value
  left by reset. After an image wrote a value outside 7–12, the committed code
  (0.3.64) no longer passed the self-test after a software restart. It failed
  closed: native applications were disabled. 0.3.58–0.3.60 passed with the
  register never written, so its power-on value lies inside the window. That
  contradicts the documented default of 0; the reason is unknown.
- **Decision.** `roundie_elf_stack_arm()` writes 10, the middle of 7–12, before
  every self-test.
- **Ignored hits by value** (60 s with the watch): about 28/s at 8 and about
  42/s at 10 (0.3.67). The rate is not monotonic and does not reach zero, so
  `SP_UNSTABLE` does not explain the spurious hits. The SP check in the ISR stays
  required.

**Decision.** A monitor hit alone is not trusted. The ISR aborts only when both
hold:

- the executor is the interrupted task on core 1;
- the SP saved in its interrupt frame is below the limit. `_frxt_int_enter`
  stores that SP in `pxTopOfStack`; it is the frame's `a1`.

Other hits are counted in `RS1 HEALTH` `executor_guard.spurious` and the monitor
is re-enabled.

Consequence: suppose a real crossing is masked (interrupt level ≥ 3) and SP
rises back above the limit before the interrupt is taken. Then the hit is
ignored. The writes stayed in the mapped red zone, so no other memory was
touched. A descent that continues reaches the guard page.

## 4. Crash attribution (record v2)

See [crash-diagnostics.md](crash-diagnostics.md) for the fields and the origin
table.

- The executor phase (`idle`, `load`, `callback`, `unload`) is set by
  `manager.c` around relocation, every callback and unload.
- The panic hook copies the phase, the trigger and the layout.

| Device case | Result |
|---|---|
| 0.3.58 false positive (verified) | `origin executor_stack_overflow`, phase idle, trigger 1, `monitor_pc` decoded by `roundie_elf_crash.py` (`sp<` frame) |
| M9 `store` crash | Same record format. Host-tested as `app_code` |

## 5. Faulty callbacks

- **Unchanged (verified, M9).** An infinite loop in a callback trips the task
  watchdog. The callback time budget is detection only: it cannot interrupt
  native code.
- A crash inside a callback is now classified as one of:
  - `app_code`: PC in the application;
  - `app_callback`: SDK or libc on the executor stack;
  - `app_stack_overflow`.

## 6. Tests

| Kind | Result |
|---|---|
| Host: `test_elf` | PASS. Covers the classification table (17 cases), phases, the spurious counter, rejection of out-of-range enums and of v1 records, fail closed, the creation mock, and one request through the real `execute()` loop with the guard armed (loaded) and unarmed (cancelled, never loaded) |
| Host: `test_runtime_policy` (in `test_elf`) | PASS. RO1 refuses `BEGIN` and `ACTIVATE` in a Safe Mode whose next image could not confirm |
| Host: `test_boot_guard` | PASS. `RS1 HEALTH` `executor_guard` and the `RS1 CRASH` v2 fields |
| Host: `test_roundie_crash` | PASS. 11 tests, including M9.1 origin fields |
| Host: `test_stack_overflow_model` | PASS. 4 model tests (section 1) |
| Mutation, `crash.c` | 11 of 11 killed |
| Mutation, `executor.c` | 6 of 6 killed: `guarded` check removed, `guarded` forced true, guard state not stored, guard state inverted, cancel not called, submit ignores a failed guard |
| Mutation, `ota_diagnostics.c` | Safe Mode gate removed: killed |

## 7. Measurements (1.85B rep, verified)

| | 0.3.57 (M9) | 0.3.60 (M9.1) | Change |
|---|---|---|---|
| Image | 1,139,920 | 1,143,824 | +3,904 B |
| Flash code | 715,612 | 717,920 | +2,308 |
| Flash data | 330,056 | 331,112 | +1,056 |
| DIRAM static | 208,686 | 209,434 | +748 |
| RTC slow | 404 | 440 | +36 (record v2) |
| Executor stack | 12,288 internal | 65,536 PSRAM, plus a 64 KiB unmapped guard | −12 KiB internal, −128 KiB PSRAM |

Run time, 20 load/unload cycles of `run_m8.py` (M8 baseline from
[hardware-validation-m8.md](hardware-validation-m8.md) and
[hardware-endurance-m8-1.md](hardware-endurance-m8-1.md)):

| | M8 / M8.1 | 0.3.60 | Change |
|---|---|---|---|
| Internal free in cycles (B) | 156,311 | 167,035 | +10,724 |
| Largest internal block (B) | 81,920 | 94,208 | +12,288 |
| PSRAM free in cycles (B) | 7,329,228 | 7,195,336 | −133,892 (the 128 KiB block) |
| ELF load, median (ms) | 9.65 (M8.1 soak) | 9.72 (max 9.77) | within noise |
| ELF unload, median (ms) | 0.175 (M8.1 soak batches, a different workload) | 1.31 (max 1.33) | not comparable; no regression claim |
| `rep_executor` peak in the usable 24 KiB | 2,168 (M9, 12 KiB stack) | 3,200 (24,576 − `usable_min` 21,376) | the block covers the smoke apps |
| Callback max (µs), RE1 BUDGET, watch | — | 233, all 53 under 1 ms | 0 over budget |
| Monitor ISR hits ignored | — | about 6/s with the watch, 0 idle (register never written); about 42/s at `SP_UNSTABLE` 10 (0.3.67) | at most about 40 short level-3 ISRs/s |

## 8. Device validation

- OTA 0.3.57 → 0.3.58 → 0.3.59 → 0.3.60 used the safe path: preflight, then
  confirmation, with the previous image kept VALID as rollback. See section 3
  for the 0.3.58 false positive.
- 0.3.59 activated from latched Safe Mode was not confirmed and ended INVALID.
  The board stayed on VALID 0.3.58. `RS1 REBOOT` (one normal trial) then cleared
  the latch, and the OTA was repeated from normal mode.
- **Why INVALID (code review).**
  1. The 0.3.58 false positive latched Safe Mode (reason core).
  2. RO1 still accepted `BEGIN` and `ACTIVATE`: the running image was a
     confirmed VALID image.
  3. 0.3.59 booted with the inherited latch, in Safe Mode, as `PENDING_VERIFY`.
  4. `roundie_recovery_confirmation_allowed()` refuses that case ("an inherited
     latch is no pass"). The GUI health result was therefore false, and
     `roundie_ota_boot_result(false)` called `roundie_ota_rollback()`, which
     marked the image INVALID and rebooted into 0.3.58.

  The `run_m8.py` preflight refuses Safe Mode, but the direct
  `roundie_ota.py upload`/`activate` path used for 0.3.59 had no such check.
- **Fix.** RO1 now refuses `BEGIN` and `ACTIVATE` with `ESP_ERR_INVALID_STATE`
  whenever an image started from the current mode could not confirm. It uses the
  same predicate as confirmation. The host test and mutation are in section 6.
  Leave Safe Mode first (`RS1 REBOOT`). Selecting the other valid slot
  (spec §19.7) is a separate command and is unaffected. Device check: PENDING,
  because it needs a latched Safe Mode.

`run_m8.py` on 0.3.60 (unattended, injected input only, report in
`build-ci/hardware-validation/m91-smoke-0.3.60`, not committed):

| Step | Result |
|---|---|
| core_status, launcher | PASS |
| elf_hello, elf_event, elf_ui, elf_watch, elf_stopwatch, round_dialog | PASS |
| bad_package | PASS (refused) |
| cycles (20) | PASS, memory stable |
| screen, restart | PASS; after the software restart, 0.3.60 VALID, guard re-armed |
| rtc | FAIL: before the sync the clock differed from host UTC by −3.85 s (limit 3 s). This is the known RTC drift of −38 ppm, unrelated to M9.1. SET corrected it |
| light_sleep | BLOCKED, as in M8: timer wake not authorised |

After the run:

- `RS1 HEALTH`:
  - `executor_guard` armed=1, `usable_min` 21,376;
  - `alloc_failed` 0;
  - 8 tasks;
  - every firmware stack within the M9 budgets.
- `RS1 CRASH`: no panic since the last load.
- `RE1 BUDGET`: 0 near or over budget, 0 dropped.

## 9. PENDING

- **Destructive overflow on the device (`fault_elf stack` on 0.3.60).** Expected
  result: an abort by the monitor, or an MMU fault in the guard, with
  `origin app_stack_overflow`. It is not run: the task rules forbid a
  deliberate overflow without a verified automatic recovery path and a clear
  need. Evidence so far:
  - the per-boot self-test (positive, recursion and negative);
  - the guard map check;
  - the host model.
- **Cause of the spurious hits.** `SP_UNSTABLE` does not remove them (section 3).
  Also open: the register's power-on value, measured by reading it once after a
  power cycle, which needs a physical cable change.
- **Cache autoload (resolved, verified).** `EXTMEM_DCACHE_AUTOLOAD_CTRL` reads
  `0x00000008` at every arm. Only the read-only `AUTOLOAD_DONE` bit is set;
  `AUTOLOAD_ENA` and both section enables are 0. The D-cache does not prefetch, so
  no autoload can touch the guard. Not covered: a future configuration that
  enables autoload.
- **Guard mapping across light sleep.** At creation the MMU check proves the
  guard is unmapped and the stack mapped (logged on every boot). IDF v6.1 has no
  S3 MMU-table save or restore in its sleep code, so nothing should remap the
  guard. Not measured: `light_sleep` is BLOCKED. Method: after a timer-woken
  light sleep, read the MMU entry of `guard_low` (for example, a HEALTH field).
- **Allocations larger than the red zone (`alloca`/VLA), not protected.**
  - One SP drop beyond the red zone plus guard (about 104 KiB minus the stack in
    use) skips the guard. It lands in other PSRAM below `guard_low`, as the
    watchpoint was skipped.
  - The monitor still sees SP below `SP_MIN`, but only after `SP_UNSTABLE`
    cycles and interrupt latency, or after a masked section ends. The first
    writes into the allocation may precede it.
  - Drops between the usable stack and 104 KiB write the red zone or the guard.
  - The SDK documents this as a limit; native code is trusted. Not
    device-tested: it needs a destructive test.
- **Writes through pointers** anywhere in memory. Not prevented. Isolation would
  need PMS/World Controller work that is not evaluated.
