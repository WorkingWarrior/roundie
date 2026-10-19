# Native ELF proof — Milestone 3A

## Integration

**Architectural decision:** REP v0.4 §§8–11/23, official
`espressif/elf_loader` **1.3.5**, exact manifest and dependency lock. Registry
hash: `1c4ca126b277369585e75419f42d8aa3dee5b6b12a5fad6afd86a568fb6506f2`;
upstream source revision: `4962410c80a51cd5822da1d8d2041eb4285265f1`.
No managed source or IDF installation is modified/copied into REP.

Sources: [registry](https://components.espressif.com/components/espressif/elf_loader/versions/1.3.5/readme),
[pinned implementation](https://github.com/espressif/esp-iot-solution/tree/4962410c80a51cd5822da1d8d2041eb4285265f1/components/elf_loader),
[official examples](https://github.com/espressif/esp-iot-solution/tree/4962410c80a51cd5822da1d8d2041eb4285265f1/examples/elf_loader).
Target build verifies source integration with pinned IDF v6.1; physical evidence
is separate below.

## Format and validation

ELF32 little-endian Xtensa **ET_DYN**, `-fPIC -shared -mlongcalls -nostdlib
-nostartfiles`, hidden visibility, exported `rep_app_entry` as ELF entry.
ET_DYN is the upstream relocatable application container; shared libraries and
dlopen are disabled. REP invokes the relocated entry using its descriptor ABI.
Upstream `esp_elf_request()` takes argc/argv and discards the return, so it is
not used. See [ABI 1](app-abi.md).

Independent CMake uses the official build flow's link/strip options and only
public SDK headers. Removing `.dynamic` leaves an unused PT_DYNAMIC descriptor
(also in upstream's build flow); readelf can warn about the absent section.
Section-based loading does not use it. Program-file bounds are still checked.
The retained `.got` is not loaded; referenced/relocated addresses must map to
supported sections.

**Verified source limitation:** upstream accepts no input length and ignores
the arch-relocator return in its relocation loop. REP must preflight every
file. Production validation is allocation-free and checks magic/class/endian,
Xtensa/type/version, header sizes/counts/alignment, all table/file ranges,
overflow, bounded strings, symbol/string links, ABI metadata, entry name/address,
imports, declared memory and relocation types/indices/targets/alignment/values.
TLS, REL, constructor/destructor arrays and unsupported addends are rejected.

Loaded sections: text, BSS, data, rodata, data.rel.ro. Their virtual ranges may
not overlap. Text's rounded four-byte copy and data alignment padding are
bounded according to the pinned loader's actual allocation formula. Validation
uses subtraction checks before dereferencing. Upstream performs the relocations.

Accepted Xtensa types: RTLD (2, upstream no-op), GLOB_DAT (3), JMP_SLOT (4),
RELATIVE (5). External relocations name only the allowed import; relative values
must map into loaded sections. R_XTENSA_32 and all other types are rejected.

The supported `elf_set_symbol_resolver()` hook is permanently set to REP's
exact-name resolver with no fallback. Broad libc/ESP-IDF/customer tables and
shared loading are disabled and checked by CMake. Unknown imports fail preflight
with diagnostics, and the resolver independently returns zero for unknown names.

## Ownership and executor

One internal manager owns one opaque loader state, text/data and four bounded
SDK allocation slots. Each request gets a generation. The lifecycle is:

```text
UNLOADED → LOADING → READY → RUNNING → STOPPING → UNLOADING → UNLOADED
                 failures ───────────→ cleanup ────────────────┘
```

Last error/result remains after cleanup. Duplicate submissions/execution are
rejected. No forced unload exists: entry/callback returns first, SDK allocations
are released, loader deinit frees text/data and clears pointers, then the input
buffer is freed by the executor. No background worker retains application data.

`rep_executor`: lazy task, 12,288-byte stack, priority 1, no affinity, one-element
request queue. Its infrastructure persists after the first explicit RUN and is
not a per-run leak. It is never created on ordinary/Safe Mode startup. App code
does not execute in GUI/USB contexts. The task subscribes to the existing
10-second panic watchdog during requests and unsubscribes while idle. The
100 ms budget detects late return; it cannot safely preempt native instructions.
These settings do not establish GUI latency or fault isolation.

## Executable memory decision

**Verified build/source limitation:** existing S3 memory protection means pinned
IDF exposes no `MALLOC_CAP_EXEC` heap. Upstream's non-PSRAM fallback would be
ordinary data memory. REP retains protection and selects the official PSRAM
loader path. Text and data/BSS allocate SPIRAM/8BIT memory; text uses the
instruction alias `SOC_IROM_LOW - SOC_DROM_LOW`, relative text pointers are
remapped, and `esp_cache_msync()` writes back D-cache and invalidates aligned
I-cache lines. IDF's S3 MMU region specifies shared instruction/data buses and
the PSRAM target. This requires physical execution evidence; ordinary PSRAM
allocation alone proves no executable capability.

Firmware XIP/instruction/rodata relocation stays disabled. No permanent ELF heap
or new MMU allocator is introduced. Limits: 64 KiB file, 16 KiB code, 16 KiB
data including padding, 128 sections, 16 segments, 1024 symbols/table and 1024
relocations. Require 48 KiB internal free, 64 KiB PSRAM reserve beyond code/data
and a sufficient largest code-heap block. Allocation/cache/relocation errors
always enter cleanup.

Memory snapshots are `[internal_free, internal_largest, psram_free,
code_heap_free, code_heap_largest]`. Code heap means PSRAM allocation
capabilities here, not proof that every byte executes. Snapshots occur before
load, after relocation, during callback admission and after unload. Input buffer
and persistent executor are present in both before/after. SDK allocation peak
is separate; allocator metadata rounding can exceed requested sizes. Load time
includes validation/copy/allocation/relocation/cache work; execution and unload
are separate. Writable/executable text and data addresses are logged.

Flash/cache concurrency, XIP comparison, large apps/fragmentation, GUI latency
percentiles and endurance remain pending future experiments. OTA/development
file writes are serialized against application sessions in 3A.

## Development workflow

```sh
cd /mnt/Dane/Development/roundie
. /home/solve/.espressif/v6.1/esp-idf/export.sh
cmake -S examples/hello_elf -B examples/hello_elf/build
cmake --build examples/hello_elf/build
python tools/roundie_elf.py inspect examples/hello_elf/build/hello.app.elf
idf.py -B build-rep -DROUNDIE_PROFILE=rep -DPROJECT_VER=0.3.0 build
# After authorized genuine OTA deployment:
PORT=/dev/serial/by-id/<actual-device>
python tools/roundie_elf.py --port "$PORT" status
python tools/roundie_elf.py --port "$PORT" upload examples/hello_elf/build/hello.app.elf
python tools/roundie_elf.py --port "$PORT" run --sha256 <exact-file-SHA256>
```

Inspect compiles production preflight using host `cc` in a temporary directory.
Use the pinned `xtensa-esp32s3-elf-gcc` wrapper; the generic driver defaults to
a different Xtensa configuration. RUN waits four seconds, checks generation
and reports the actual result; timeout never kills/unloads code.

RE1 shares the existing sole USB RX owner, 1100-byte framer/two-second deadline,
512-byte stop-and-wait chunks, 2048-byte rings and bounded TX. It reuses RO1's
strict chunk parser without dispatching OTA commands:

```text
!RE1 <id> STATUS
!RE1 <id> BEGIN <nonzero-session> <size> <SHA256>
!RE1 <id> DATA <session> <offset> <1..512 bytes hex>
!RE1 <id> END <session>
!RE1 <id> ABORT <session>
!RE1 <id> RUN <SHA256>
!RE1 <id> <esp_err> <bounded JSON>
```

IDs/digests use lowercase hex. No arbitrary paths or RESTART. Transfers create
`/storage/dev/.hello.part`, hash persisted bytes, validate ELF and rename only
a valid complete file to `/storage/dev/hello.elf`. Failure preserves the previous
staged file. Errors/abort/Safe Mode/10-second inactivity remove only the partial.
An interrupted file across reset is removed on the next explicit BEGIN; startup
does not scan. RUN re-reads/hashes/validates the fixed file before submission.
No formatting, installer, partition access or MTP. SHA verifies integrity,
not origin; physical access to this development transport is trusted.

## OTA and recovery

Staging/RUN require normal/STABLE boot, passed core self-test, running VALID,
unchanged boot selection, idle OTA and mounted StorageManager. They are denied
in NEW/PENDING_VERIFY, all active OTA phases and Safe Mode. OTA mutations and
restarts are blocked during transfer/execution. Confirmation and BootGuard
progression never depend on applications. Boot never auto-executes ELF.

RS1 REQUEST inhibits new work/SDK calls, cancels staging, persists Safe Mode,
and defers restart until any in-flight native callback returns and unloads.
If it does not return, the established watchdog/system recovery path applies;
no code is freed while executing. Recovery starts without storage/executor and
retains USB diagnostics. Boot thresholds and RTC behavior are unchanged.

## Verification record — 2026-10-19

**Host verified:** 67 unittest cases pass under exported pinned IDF, including
production validation/manager/RE1 policy and real temporary-file staging under
ASan/UBSan. Existing 112-case OTA boot matrix is intact. ELF tests cover malformed
headers/ranges/overflow, architecture, relocation/import/ABI refusal, loader
init/allocation/relocation/descriptor errors, owner/generation checks, duplicate
loads/execution/free, five repeat cycles and automatic allocation cleanup.
A bounded 4000-case mutation corpus exercises preflight. RE1 tests cover Safe
Mode/OTA, hash, malformed commands, timeout and preservation of the last file.
Target C11/C23 layout assertions pass. Mock tests prove no Xtensa execution.

**Target verified:** REP 0.3.0 builds with unchanged pinned IDF v6.1, loader
1.3.5, protection retained, broad exports disabled, XIP off. Standalone hello:
1680 file bytes, 256 code bytes, 115 data-allocation bytes, 10 relocations,
one import. File SHA-256:
`9c6d3b87cd3e284745c5e3a64fddae9e4f1037ba9dcd4ee109bcfb4b26efa723`.

**Physical PENDING:** inactive-B OTA/confirmation, PSRAM instruction execution,
result and memory recovery, negative candidates, Safe Mode refusal and final
service health. Append measurements only after actual operations succeed.
Historical OTA/recovery/RTC evidence stays in its original documents.

Full runtime/events, GUI/hardware resources, packages/signatures/registry/MTP,
multiple apps, helper/TLS support and Milestone 3B are deferred.
