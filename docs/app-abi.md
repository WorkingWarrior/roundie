# REP application ABI 1 — Milestone 3A

**Milestone 3D:** [RPK1 packages](package-format.md) declare ABI 1/2/3 and must
match the embedded `.rep.abi` before install and launch. No application ABI or
allowlist was widened. Package metadata/selection remain private system formats;
installed artifacts confer no publisher identity, isolation or execution trust.

**Milestone 3C:** explicit [ABI 3 and asynchronous UI](app-ui.md) adds a separate
service table, descriptor capability declaration, entry and sole import. ABI
1/2 public headers and binary layouts remain unchanged. START accepts 2/3;
RUN remains 1. See `include/rep/rep_app_v3.h` and `examples/ui_elf`.

**Milestone 3B compatibility:** ABI 1 public header, descriptor, metadata,
entry and API table remain binary compatible. `RUN` keeps one-shot semantics.
The separate [ABI 2 lifecycle contract](app-lifecycle.md) uses
`include/rep/rep_app_v2.h`, `rep_app_entry_v2` and the sole import `rep_abi_v2`.
Its metadata version is 2; firmware rejects mixed-version imports and RUN/START
contract mismatches. No ABI 1 structure was resized. Both SDK headers have
C11/C23 layout assertions, including actual Xtensa 32-bit checks. ABI 2 adds
seven lifecycle/event callbacks, a 24-byte copied event and bounded timer
registration/cancellation. Ownership, return codes and cleanup are described in
[app-lifecycle.md](app-lifecycle.md) and [resource-manager.md](resource-manager.md).
The historical 3A contract and evidence below remain unchanged.

## Contract

**Architectural decision:** REP v0.4 §§5, 7–10, 21, 23; one trusted native
Xtensa LX7 application, one system executor, callbacks returning to the system.
Public header: [`rep/rep_app.h`](../include/rep/rep_app.h). ABI 1 freezes this
small contract, not a full future SDK. Extensions require explicit ABI agreement.

Target: Xtensa windowed ABI, little endian, 32-bit pointers, GCC
`esp-15.2.0_20251204` (15.2.0). C11/C23 target assertions verify table size
28, descriptor size 12, context size 16 and pointer-field offsets. Host pointer
layout is different and is never used as target layout evidence.

`rep_app_entry(const rep_api_t *api)` is the exported symbol and ELF entry
address. It returns a borrowed descriptor in loaded data, or NULL on refusal.
The descriptor's ABI/size and `on_start` executable-text range are checked
before calling it. The callback gets a borrowed START context with ABI/size,
instance generation and event type. No private loop, task, ISR, persistent
callback or background execution is supported. Both entry and callback must
return; the measured entry+callback budget is 100 ms.

The `.rep.abi` metadata contains three little-endian uint32 words: magic
`0x31504552`, ABI `1`, size `12`. Missing/incompatible metadata is rejected
before relocation or execution. Metadata is not authentication.

## Services

The only allowed named import is **`rep_abi_v1`**, matched exactly. For version
1 it returns the same table supplied to entry; unsupported versions or calls
outside the active executor return NULL. The example exercises this actual
import, including unsupported-version refusal. Zero-import modules are rejected
for this proof gate.

| Table field | Contract |
|---|---|
| ABI/size/capabilities | Version 1, 28 target bytes, LOG/MONOTONIC/ALLOC bits |
| `log(instance, text, length)` | Up to 160 UTF-8 bytes, borrowed for the call; 0 success, -1 rejected |
| `monotonic_ms(instance)` | uint64 monotonic milliseconds; 0 rejected; unrelated to UTC |
| `alloc(instance, bytes)` | 1..4096 bytes, 4096 aggregate, four live allocations; NULL on rejection/exhaustion |
| `release(instance, pointer)` | Exact tracked pointer; 0 success, -1 invalid owner/pointer/double release |

All services require the current instance and executor in READY/RUNNING.
Safe Mode inhibition rejects new calls. Log text must be in loaded data or an
SDK allocation; arbitrary pointers and stack strings are outside this initial
contract. API/descriptor pointers are borrowed until unload; context is borrowed
only for the callback. SDK buffers belong to the instance and remaining buffers
are reclaimed after callback return. Generations are never reused (exhaustion
rejects new sessions). Raw pointers retain ordinary C lifetime rules.

Callback return is an int32 application result, never a firmware crash
classification. Hello intentionally returns **42** after a calculation from
volatile input, BSS access, logging, uptime query and 64-byte alloc/release.
Returning is the only application exit mechanism in this slice.

No UTC capability is advertised. Existing clock discipline/discrepancy is
unchanged. There are no task, GUI, peripheral, filesystem, network, OTA,
physical-memory, Flash, eFuse, bootloader or arbitrary lookup services.

## Hidden ABI and trust

**Verified toolchain/source:** unchanged IDF v6.1 commit
`fff9895c82d744c7237be8847347bdd1b07c6643`, Picolibc firmware configuration.
Hello links neither libc nor libgcc. Compiler-generated imports face the same
allowlist. Target-built probes for 64-bit division, `errno` and TLS are rejected
by production preflight. This proves exclusion, not working libc/TLS support.
Constructors/destructors and unsupported relocations are rejected. Helper
support requires a separate measured compatibility decision.

Native code shares firmware memory. Symbol filtering controls API compatibility
and accidental exposure; it cannot prevent direct memory access, corruption or
non-returning code. A task/watchdog is not process isolation or safe preemption.
Only trusted development artifacts are permitted. Signatures, packages, full
events/lifecycle, persistent installation and SDK release remain deferred.

**Stack (M9.1, no ABI change).** Callbacks run on the executor stack: 24 KiB
usable above a 40 KiB red zone and an unmapped guard page. The application
budget stays 8 KiB ([app-lifecycle.md](app-lifecycle.md)). A descent into the
red zone aborts the firmware with `app_stack_overflow` in `RS1 CRASH`. A single
allocation larger than the red zone plus the guard (`alloca`, VLA) can jump over
both and is outside the contract. This limits stack overflow only; it is not
memory isolation ([elf-runtime-hardening-m9-1.md](elf-runtime-hardening-m9-1.md)).
