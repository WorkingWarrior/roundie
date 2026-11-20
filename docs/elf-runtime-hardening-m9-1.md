# M9.1 — Native ELF runtime hardening and fault containment

Status: implemented on branch `m9.1-hardening`. Device results are from the 1.85B
board (serial `28:84:85:87:98:7C`) running firmware 0.3.58–0.3.60. Each statement
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
- `ASSIST_DEBUG_CORE_1_SP_UNSTABLE` (register map: R/W, bits 7:0, default 0, no
  description) may relate to this. Not tried: PENDING.

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
| Host: `test_elf` | PASS. Covers the classification table (17 cases), phases, the spurious counter, rejection of out-of-range enums and of v1 records, fail closed, and the creation mock |
| Host: `test_boot_guard` | PASS. `RS1 HEALTH` `executor_guard` and the `RS1 CRASH` v2 fields |
| Host: `test_roundie_crash` | PASS. 11 tests, including M9.1 origin fields |
| Host: `test_stack_overflow_model` | PASS. 4 model tests (section 1) |
| Mutation, `crash.c` | 11 of 11 killed |
| Mutation, `executor.c` | 1 of 2 killed. The survivor is the `guarded` check inside the target-only `execute()` loop, which host tests do not run; submit-time refusal is killed |

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
| Monitor ISR hits ignored | — | about 6/s with the watch, 0 idle | about 6 short level-3 ISRs/s |

## 8. Device validation

- OTA 0.3.57 → 0.3.58 → 0.3.59 → 0.3.60 used the safe path: preflight, then
  confirmation, with the previous image kept VALID as rollback. See section 3
  for the 0.3.58 false positive.
- 0.3.59 activated from latched Safe Mode was not confirmed and ended INVALID.
  The board stayed on VALID 0.3.58. `RS1 REBOOT` (one normal trial) then cleared
  the latch, and the OTA was repeated from normal mode.

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
- **`SP_UNSTABLE` effect and the cause of the spurious hits.** Method:
  1. set the register on a development image;
  2. compare the `spurious` rate under the same workload;
  3. accept only if the self-test still passes.
- **Cache autoload across the guard boundary.** The value is logged at arm time.
  Whether a prefetch of an unmapped line raises a fault is not measured.
- **Allocations larger than the red zone (`alloca`/VLA).** With more than 104 KiB
  (red zone plus guard) between two touches, a frame can jump over the guard,
  as with the watchpoint. The SDK documents this as a limit; native code is
  trusted.
- **Writes through pointers** anywhere in memory. Not prevented. Isolation would
  need PMS/World Controller work that is not evaluated.
