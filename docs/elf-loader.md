# Native ELF proof — Milestone 3A

**Milestone 3D:** [PackageManager](package-manager.md) reuses this exact preflight
at install and every launch. It reopens committed RPK1, checks full/payload
digests and ABI/platform, then submits separately owned ELF bytes to the existing
RUN/START executor. Loader 1.3.5 and executable PSRAM mapping are unchanged.
RE1 raw ELF development staging remains available. No package file is unloaded
or deleted when runtime ownership retires.

**Milestone 3C:** loader preflight/resolution additionally allow the exact ABI 3
pair `rep_abi_v3` / `rep_app_entry_v3`, matching metadata version 3. No broad
exports, relocation changes or relaxed memory/alignment limits were added.
ABI 1/2 binaries keep their original entry/import contracts. The new
[UI bridge](app-ui.md) never retains pointers into an application image.

**Milestone 3B extension:** [application lifecycle](app-lifecycle.md) retains the
same pinned 1.3.5 loader, executable-memory/cache path and strict relocation
preflight. ABI metadata now selects exactly one versioned entry/import pair;
ABI 1 RUN remains one-shot, ABI 2 START retains the relocated image across
callbacks. Loaded/suspended/stopped instances exclude RO1/RE1 writes until
unload. The historical 3A acceptance below is preserved.

**Bounded 3A gate passed on 2026-10-19.** Installed B 0.3.1 is VALID/STABLE;
preserved A 0.2.5 is the compatible VALID fallback. This is a trusted native
development proof, not the full REP runtime or a production trust boundary.

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

Firmware XIP/instruction/rodata relocation was disabled for this gate; REP has
used XIP since the [2026-11-02 decision](display-o2-240-candidate.md). No permanent ELF heap
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
idf.py -B build-rep -DROUNDIE_PROFILE=rep -DPROJECT_VER=0.3.1 build
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
Focused compatibility probes specifically reject `__udivdi3`, `__errno` and
TLS symbols, rather than passing because of an unrelated format failure.

**Target verified:** REP 0.3.1 builds with unchanged pinned IDF v6.1, loader
1.3.5, protection retained, broad exports disabled, XIP off. Standalone hello:
1692 file bytes, 264 code bytes, 115 data-allocation bytes, 10 relocations,
one import. File SHA-256:
`5edbfbf49c72890dd80f33263d2e52ffeeba9bfb0c5ef577047428a7360f0c88`.

### First physical execution and alignment correction

**Verified physical, 0.3.0:** genuine OTA to B confirmed PENDING_VERIFY→VALID
at 1160/3290 ms, BootGuard normal/STABLE, NVS OK, LittleFS mounted and RTC
OS=0; original A 0.2.5 stayed VALID. One independently staged ELF actually ran
from PSRAM, resolved the REP import, logged hello/uptime and returned 42.
Internal free was 247159 before/after; PSRAM was 8382780→8382400→8382780.
Loader text/data requested 256/115 bytes; SDK allocation peak was 64 bytes.
Load/execution/unload measured 9427/9852/120 microseconds.

**Acceptance failure, not a complete pass:** the last log was rejected, giving
five successful service calls instead of six. Investigation found upstream's
`esp_elf_map_sym()` uses rounded text size: the original 253-byte text at
0x1cc absorbed rodata beginning at 0x2c9 into its three padding bytes. Thus the
first rodata pointer mapped to text; REP's range check refused that log before
dereferencing its pointer. No panic was observed. Tests stopped at this failure.

**Correction:** standalone linking aligns `.rodata` to four bytes using a small
linker augmentation, every sample log result is checked, and production preflight
rejects any loaded section overlapping the rounded text range (including its
address overflow). The original physical artifact is now rejected on the host
as `rounded-text-overlap`; the regression test covers this case. This preserves
the strict service-call assertion and does not widen pointer ranges or relax
comparison criteria. The correction is built as 0.3.1 for another bounded B
update through the preserved A. Historical evidence above is retained.

### Final physical acceptance — 0.3.1

**Verified physical:** guarded selection of original A and its required
reconfirmation passed. Exact USB/chip/Flash/table/bootloader identity and ROM
recovery were established by the original bounded read-only preflight (64 KiB
prefix plus two 288-byte headers). For the correction, running A independently
reported exact `rep-v1` and mounted storage; the frozen candidate table matched
that readback, both slots were VALID with expected full ELF identities, and
no intervening operation wrote bootloader/table/security settings. No second
prefix/full-Flash readback was needed. All writes used genuine RO1 to inactive
B and OTA metadata; running A's application bytes were preserved.

The corrected upload/activation took 71.234 seconds host monotonic. B logged
PENDING_VERIFY at 1160 ms and VALID at 3290 ms after its actual core self-test.
BootGuard reached normal/STABLE, test injection remained disabled, NVS
initialized ESP_OK, LittleFS mounted (9240576 total / 20480 used bytes after
development staging), and RTC was valid/OS=0. Gauge access remained available
(3975 mV, reported SoC 84%; accuracy remains unverified).

| Final artifact | Bytes | SHA-256 |
|---|---:|---|
| REP 0.3.1 image | 695840 | `aab70ebe6d56abbc269dd50b04a2f82df229ac3de6859342101aa6cac1d16365` |
| REP embedded ELF identity | — | `64e17722a3d8665399ca049eb1277e423e6bd6c3775b3057c008b0f6510a4298` |
| Standalone hello ELF | 1692 | `5edbfbf49c72890dd80f33263d2e52ffeeba9bfb0c5ef577047428a7360f0c88` |
| Preserved original A 0.2.5 ELF | — | `1fcfa1092406928118bbe28971415fca99419770eb47ca2a71f7b05a99e754c7` |

Three separately triggered load/run/unload cycles each completed ten actual
Xtensa relocations, six successful ABI calls, result **42**, no over-budget
flag and no outstanding SDK allocations. The real serial output included:

```text
elf: instance=1 text_write=0x3c0a16c8 text_execute=0x420a1794 data=0x3c0a17d4 code=264 data_allocation=115 relocations=10
rep_app: instance=1 Hello from a native Roundie ELF application!
rep_app: instance=1 uptime_ms=55304
rep_app: instance=1 REP ABI 1: 40 + 2 = 42; returning 42
```

`text_execute` above is the entry address within the instruction alias, not
the start of the text allocation. All three cycles had identical memory
accounting (bytes):

| Snapshot | Internal free | Largest internal | PSRAM free | Largest code-heap block |
|---|---:|---:|---:|---:|
| Before load | 247159 | 172032 | 8382780 | 8257536 |
| After relocation / callback admission | 247159 | 172032 | 8382392 | 8257536 |
| After unload | 247159 | 172032 | 8382780 | 8257536 |

Requested code/data sizes were 264/115 bytes; actual PSRAM free-space delta
was 388 bytes including allocator rounding/overhead. SDK allocation peak was
64 bytes, returned to zero. Infrastructure and input-buffer scope is as
documented above. Before/after counts matched exactly each cycle and across
these three cycles; this is bounded cleanup evidence, not an endurance result.

| Cycle | Load incl. relocation/cache (µs) | Entry + callback (µs) | Unload (µs) |
|---|---:|---:|---:|
| 1 | 9457 | 16095 | 167 |
| 2 | 9768 | 15626 | 137 |
| 3 | 9915 | 15505 | 166 |

Two bounded negative transfers had correct SHA-256 but respectively an unknown
`bad_abi_v1` import and ABI 99. END rejected each with INVALID_ARG and exact
`import:bad_abi_v1` / `ABI-version` diagnostics, before submission. Instance and
completed counters stayed at 3. No invalid candidate executed. The previous
staged valid file survived both failures and an explicit fourth positive run
returned 42 with six calls and zero outstanding allocation bytes. RE1 run/result
fields describe the last admitted instance; a staging error does not erase its
historical `executed` flag. Unchanged counters establish refusal here.

Explicit RS1 REQUEST booted Safe Mode without mounting LittleFS. RE1 STATUS
reported allowed/busy/completed=0, and RUN of the known-good digest was refused.
CLEAR/REBOOT returned to normal/STABLE with failures/latch/trial/fault remainder
all zero. B remained 0.3.1 VALID, A remained compatible VALID, OTA was IDLE,
NVS/LittleFS/gauge/RTC diagnostics worked, and RTC OS=0 with system time equal
to RTC at that sample. This does not establish host-UTC accuracy or resolve the
historical clock discrepancy. After reboot completed=0 with the staged file
still present: no application autostart occurred. No panic/brownout markers
appeared in the bounded acceptance logs; no fault injection was performed.

Raw logs, frozen binaries/configs, scripts and JSON evidence are outside Git:
`/mnt/Dane/Development/roundie-backups/elf-3a-2026-10-19/`; final corrected
acceptance is in `fix-0.3.1/`. Implementation commits: `43682c9`; measured
alignment correction: `e1b47bb`. No remote push was performed.

**Pending beyond this bounded gate:** cache/Flash concurrency and XIP comparison,
large/long-lived apps and fragmentation, non-returning/faulting applications,
power interruption/endurance, human display/touch responsiveness measurements,
allocator behavior beyond the documented limits, and full libc/TLS support.
No hardware isolation, production trust policy or full runtime is claimed.
Historical OTA/recovery/RTC evidence stays in its original documents.

Full runtime/events, GUI/hardware resources, packages/signatures/registry/MTP,
multiple apps, helper/TLS support and Milestone 3B are deferred.
