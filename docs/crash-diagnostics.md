# Native application crash diagnostics (M9)

Native ELF applications run unisolated in the firmware address space
(spec §§4–10). A fault in one of them is a firmware panic; Boot Guard then
counts the failed start and may enter Safe Mode (`docs/boot-guard.md`). Before
M9 the panic printed a backtrace on the console only, and the run-time
addresses could not be tied to the application that was loaded. M9 adds a
small retained record and an offline symbolizer. Neither changes the REP ABI,
the Flash layout or the panic behaviour.

## Retained crash record

Implementation: `components/roundie_elf/crash.c` (logic, host-tested) and
`components/roundie_elf/idf/crash_target.c` (SHA-256 and panic hook).
Header: `components/roundie_elf/include/roundie_elf_crash.h`. The record is
firmware-internal and versioned (`RCR1`, version 2 since M9.1; a version 1
record from older firmware reads as absent); it is not part of the REP ABI.

| Moment | What is written |
| --- | --- |
| Application relocated (`idf/loader.c`) | Record reset; instance, SHA-256 of the ELF bytes (as package `elf_digest`), five-section map `{ELF vaddr, load address, size}` (text, bss, data, rodata, drlro) and the instruction-bus address of text; `loaded = 1` |
| Application unloaded | `loaded = 0`; an earlier fault is kept |
| Panic (`__wrap_esp_panic_handler`) | PC, EXCCAUSE (or panic pseudo-cause), EXCVADDR, SP, panic exception kind, core, and up to 8 backtrace PCs; `fault = 1`; then the normal IDF panic handler runs |
| Panic, since M9.1 | Also: the exception-frame address, the executor phase (`idle`, `load`, `callback`, `unload`), the trigger (0 exception, 1 executor stack monitor, 2 foreign), the PC latched by the stack monitor, and the executor stack layout `[guard, stack_low, limit, top, armed]` ([elf-runtime-hardening-m9-1.md](elf-runtime-hardening-m9-1.md)). A frame or stack pointer inside the unmapped guard page is not read: frame fields stay 0 |
| Boot (`main.c`, before any load) | A valid record is copied for reading; an invalid one is reset |

Properties:

- Lives in `RTC_NOINIT` memory: survives software and panic resets, not power
  loss. It is readable after the panic reset into normal boot or Safe Mode and
  stays until the next application load.
- A CRC-32 covers the record. A torn update (panic during a write) fails the
  check and the record reads as absent. The panic path uses byte loops in
  IRAM, no libc and no locks. If the record is not intact at panic time, the
  fault details are still kept without an application context.
- The panic hook is linked with `-Wl,--wrap=esp_panic_handler`;
  `esp_panic_handler` is called from a separate IDF translation unit, so the
  wrap applies. The backtrace walk stops at the first insane stack pointer.
- `roundie_elf_crash_locate()` maps a run-time address to `(section, ELF
  vaddr)`. Text matches either its data-bus or its instruction-bus address.

At boot, a recorded fault is logged once as a warning
(`Previous panic: pc=… application=… elf_pc=…`).

## Reading it: `RS1 CRASH`

`RS1 CRASH` is a read-only operation of the recovery protocol
(`main/recovery_diagnostics.c`). It works in normal boot and in Safe Mode and
changes no state. Reply:

```text
!RS1 <id> 0 {"crash":{"valid":0}}
!RS1 <id> 0 {"crash":{"valid":1,"loaded":1,"fault":1,"instance":7,"digest":"<64 hex>",
  "text_exec":…,"sections":[[vaddr,addr,size]×5],"pc":…,"cause":…,"vaddr":…,"sp":…,
  "exception":…,"core":…,"frames":[…],"frame":…,"phase":"callback","trigger":0,
  "monitor_pc":…,"stack":[guard,stack_low,limit,top,armed],"origin":"app_code"}}
```

The reply is one line (wrapped here) of at most 1024 bytes (768 before M9.1).

`origin` is computed when the reply is formatted (`roundie_elf_crash_classify()`,
host-tested), first match wins:

| Origin | Condition |
| --- | --- |
| `app_stack_overflow` / `executor_stack_overflow` | Monitor trigger, SP below the monitor limit, or EXCVADDR in the guard page; `app_` when an image was loaded and the phase was `load` or `callback` |
| `app_code` | An image was loaded and PC lies in its text |
| `app_callback` | Phase `load`/`callback`, SP on the executor stack, PC in firmware (for example an SDK or libc call made by the application) |
| `core_while_app_active` | Phase `load`/`callback`, SP elsewhere |
| `executor_core` | SP on the executor stack, no application running |
| `core` | Anything else |

The classification is an attribution aid, not proof: an application can
corrupt firmware memory and leave the fault to be taken later in `core`. Firmware before
0.3.56 answers `CRASH` as an unknown operation, which the tool reports as
unsupported (exit code 3).

## Offline symbolization: `tools/roundie_crash.py`

The example builds keep the unstripped link output beside the installed file:
`X.app.elf` (stripped, installed, digest in the package) and
`X.app.debug.elf` (with `.symtab`). Producing the debug ELF does not change the
stripped bytes: all 14 example ELFs were byte-identical to the previous build
(verified with the target toolchain, GCC 15.2.0 xtensa). Adding `-g` did change
two stripped images, so the examples have function names, not line numbers.

```bash
python3 tools/roundie_crash.py --port /dev/serial/by-id/<board> --elf build/examples --firmware build/roundie.elf
```

```bash
python3 tools/roundie_crash.py --report crash.json --elf path/to/fault.app.elf
```

The tool:

1. reads the record (from the board, or a saved reply or JSON object);
2. finds the `*.app.elf` whose SHA-256 equals the recorded digest and reads
   function and object symbols from its `.app.debug.elf`;
3. maps PC, each backtrace frame and a faulting data address inside the image
   to `section + ELF address + symbol+offset`;
4. names addresses outside the image from `--firmware`, if given.

It parses ELF32 directly and needs no toolchain. When no file matches the
digest, addresses are still mapped to ELF offsets. Tests:
`tests/test_roundie_crash.py` (synthetic ELF32), `tests/test_elf.c`
(`crash_tests`, `crash_classify_tests`), `tests/test_boot_guard.c` (`RS1 CRASH`
formatting).

Limits:

- The backtrace is the walk available in the panic handler. A corrupted stack
  ends it early, and a fault in a leaf function may report the caller only
  through `a0`.
- Application data is not captured: only addresses.
- One record. A second panic before the next load overwrites the first fault.
- Power loss clears it.

## Coredump: proposal only (not enabled)

The partition table already reserves `coredump` at `0xfe0000`, 128 KiB
(`partitions/rep.csv`, `docs/flash-migration.md`). The 0.3.55 configuration
has `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y`. M9 does not change this.

Proposal for a later milestone, as a decision to take, not a decided change:

- **Configuration.** `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`, ELF format,
  SHA-256 integrity check. Capture all tasks first, and narrow the scope only
  if the size does not fit. No partition-table change is needed.
- **Benefit.** Full task stacks and registers of every task, decoded by
  `idf.py coredump-info` or `coredump-debug` with the firmware ELF.
- **Size: pending experiment.** The dump of about 20 task stacks
  (system_worker 12 KiB; rep_executor, since M9.1, a 64 KiB PSRAM stack of which
only the used part matters) must fit in 128 KiB.
  Acceptance: `coredump-info` decodes a dump from the fault_elf `store` case on
  the dev board.
- **Panic-path cost: pending experiment.** The panic writes Flash with the
  cache disabled, which lengthens the time to restart. Measure it, and check
  that the Task and Interrupt WDTs do not interfere with the write.
- **What a coredump does not hold.** Application code is in PSRAM-mapped,
  loader-owned memory, which is not part of the dump. The retained record
  (digest and section map) is still needed to tie PCs to the ELF.
- **Privacy.** A dump holds RAM contents, which may include Wi-Fi credentials
  and user data. Do not publish dumps; share only the decoded backtrace.
- **Lifecycle.** Each panic overwrites the dump. Reading and erasing it must
  be an explicit host operation; erasing is a Flash write in a data partition
  and needs the same authorization as other destructive Flash operations.
- **Recovery.** The panic-time Flash write must not run while an OTA write
  holds the Flash. Verify the write with a panic during a pending OTA
  confirmation.

## Device status

**Verified on the 1.85B board (0.3.56, 2026-11-20):** `fault_elf` `store`
gave a valid record readable in Safe Mode and after RS1 REBOOT. The tool named
the faulting instruction (`event+0x12`, a store through null, checked with
objdump) and the firmware callers. With `stack`, the record named the loaded
application by digest, while the PC was a firmware ISR on the other core.
Details: [m9-stabilization.md](m9-stabilization.md) §4.

**PENDING:**

- a panic while an OTA confirmation is pending;
- a torn record. Host-tested only: the record checksum is covered in
  `tests/test_elf.c`.
