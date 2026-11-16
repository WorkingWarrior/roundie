# Native REP applications on Linux x86-64 (M5) — design note

**Status: proposal, awaiting decision.** Nothing below is implemented. It
answers D6 in [roundie-core.md](roundie-core.md): the Roundie Desktop runs real
REP applications, built from the same sources as the ESP32-S3 packages, through
the same launcher, runtime services and UI bridge.

## Audit (verified at `c3933d8`)

- **Runtime and loader are one file.** `components/roundie_elf/manager.c`
  (1231 lines) holds the lifecycle, the service tables (`rep_abi_v1/v2/v3`,
  `rep_storage_v1`, `rep_audio_v1`), the event mailbox, timers, UI ownership,
  the audio stream owner and clock events, and loads the image with
  `espressif/elf_loader` 1.3.5 (`esp_elf_init`, `esp_elf_relocate`, a strict
  symbol resolver). 67 lines use ESP-IDF, FreeRTOS, `heap_caps` or `esp_elf`.
  `executor.c` is a FreeRTOS task with a one-slot queue and the task watchdog.
- **Validation is Xtensa only.** `validate.c` accepts ELF32, `EM_XTENSA`,
  `ET_DYN`; rejects TLS, `PT_INTERP`, init/fini arrays and `SHT_REL`; checks
  the `.rep.abi` metadata, the entry point and the import allowlist per ABI
  (`rep_abi_vN`, plus `rep_storage_v1` and `rep_audio_v1` for ABI 3). The
  package manager and the executor call it.
- **Package format.** RPK1 already has a 16-byte platform field;
  `ROUNDIE_PACKAGE_TARGET` is `esp32s3.rep1` (`roundie_package_format.h`) and
  `tools/roundie_package.py` writes it as a constant. ELF payload ≤ 65536 bytes.
- **ABI.** Applications import nothing but the functions above; every service
  is reached through their tables. Value structures (widgets, updates, events,
  results) use fixed-width fields. Tables and descriptors hold pointers; their
  layout is asserted only when `UINTPTR_MAX == UINT32_MAX`.
- **Desktop today.** The launcher, UI bridge and package manager are the
  firmware's code; `platform/linux/app_runtime.c` refuses every launch.
- **Audio.** `components/roundie_audio/stream.c` (ring, playback policy, PCM
  conversion) is portable; `audio.c` (codec, I²S, audio task, stream owner)
  is ESP-IDF.

**Experiment (host, GCC 15, this machine):** all seven example applications
(`hello`, `event`, `ui`, `round`, `sound`, `stopwatch`, `watch`) compile
unchanged as x86-64 shared objects with `-nostdlib -fPIC -shared`, in C11 and
C23 with `-Werror`. Each imports exactly the allowed `rep_*` symbols, has no
`DT_NEEDED`, constructors or TLS, uses only `R_X86_64_RELATIVE` and
`R_X86_64_JUMP_SLOT` relocations, and is 14–16 KB.

**Prototype (scratch, not committed):** a host exporting only `rep_abi_v1`
(`--dynamic-list`) wrote the unmodified Hello into a sealed `memfd`, loaded it
with `dlopen`, ran START and unloaded it, 200 times under ASan/UBSan: START
returned 42 every time, the mapping was present while loaded and gone after
`dlclose`, no sanitizer report. A shared object importing `puts` loaded and
ran as well: `ld.so` binds undefined symbols from every library in the
process. Import validation before `dlopen` is therefore mandatory; limiting
the exported symbols does not restrict imports.

## Proposed decisions

**N1 Loading: `ET_DYN` shared object, `dlopen` after validation.**
The ELF is written to a `memfd`, sealed against writes and size changes,
validated from that sealed copy, and loaded with `dlopen("/proc/self/fd/N",
RTLD_NOW | RTLD_LOCAL)` on the executor thread; the entry is found with
`dlsym`. The bytes validated are the bytes loaded. The desktop exports exactly
the allowlisted `rep_*` functions. Reasons: the same build model as the Xtensa
target (`ET_DYN`, `-shared`, `-nostdlib`), no relocation code of our own, and
gdb and sanitizer reports symbolize application frames.
Not chosen: a custom x86-64 loader (full control of binding, but relocation
code to maintain and no debugger symbols); one process per application (real
isolation, but every API call becomes IPC — a different runtime architecture
from the firmware's).

**N2 x86-64 validation with the same policy.** A second validator for
ELF64/`EM_X86_64`/`ET_DYN` with the same rules and the shared ABI allowlist:
only `PT_LOAD`, `PT_DYNAMIC`, `PT_GNU_STACK` (not executable) and
`PT_GNU_RELRO`; no writable and executable segment; no `PT_INTERP`/`PT_TLS`;
dynamic tags from an allowlist (no `DT_NEEDED`, `DT_INIT*`, `DT_FINI*`,
`DT_PREINIT_ARRAY`, `DT_TEXTREL`, `DT_RPATH`, `DT_RUNPATH`); undefined
symbols exactly within the ABI's imports; relocations `RELATIVE`,
`GLOB_DAT`, `JUMP_SLOT`, `64` only; `.rep.abi` and entry as on Xtensa. Every
rejection names its rule, as today.

**N3 Package target `linux-x64.rep1`.** RPK1 is unchanged; only the platform
string differs. The firmware keeps `esp32s3.rep1`. Each build accepts only its
own target, so an ESP32 package on the desktop (and the reverse) is rejected
as `platform` before any ELF parsing and reported through the launcher's
error path. `roundie_package.py` gains `--target`; the default stays
`esp32s3.rep1`.

**N4 Native ABI.** The same REP ABI 1/2/3 source API. For `linux-x64.rep1`
the tables and descriptors have LP64 System V layout; the value structures
are byte-identical to Xtensa (already asserted). LP64 layout assertions are
added to the SDK headers next to the 32-bit ones; nothing changes for Xtensa.

**N5 Shared runtime.** `manager.c` is split into a portable runtime
(lifecycle, services, mailbox, timers, UI, storage, audio ownership, clock
events), using the D1 seam for locks, time and thread identity, and a small
loader port: load a validated image, find the entry, tell whether a pointer
lies in the image's code or data, unload, report memory. ESP-IDF implements
it with `esp_elf`, the desktop with N1. The executor loop stays per platform
(FreeRTOS task with the task watchdog; a pthread on the desktop). LVGL keeps
one owner: callbacks run on the executor, UI requests go through the existing
bridge queue to the GUI thread.

**N6 Audio.** The stream owner and service move out of `audio.c` beside
`stream.c`; the output (I²S and codec, or an SDL3 audio stream) becomes a
platform part. Headless tests capture the PCM with SDL's `disk` driver.

**N7 Security model (decision needed).** A native application is arbitrary
code running with the user's rights inside the desktop process. The import
allowlist is an API control, not isolation: the application can read and
write any memory of the process and make system calls directly. Proposal:
native execution is off by default and enabled per run with
`--native-apps`; only `linux-x64.rep1` packages from the desktop's data
directory are considered; without the flag a launch is refused as today.

## Consequences

- N5 is the largest firmware change since M2. Disassembly will not be
  identical, so the firmware is checked by the four builds and host tests of
  the shared runtime; a device run of Hello, UI, Sound, Watch and repeated
  start/stop is PENDING until flashing is authorized.
- An application that never returns blocks the executor. The GUI stays
  responsive, a stop request cannot complete, and the desktop must be
  restarted; the firmware's watchdog reset has no desktop equivalent.
- Application code is not instrumented by ASan (it links no runtime); the
  sanitizers cover the runtime, bridge, loader and leak checks.

## Stages

| Stage | Content | Check |
|---|---|---|
| M5a | N5 split; desktop still refuses | Four firmware builds without warnings; host tests; runtime host tests on Linux |
| M5b | N2, N3, N4; example applications built for both targets | Validator tests with malformed and forbidden ELFs; package tests |
| M5c | Desktop executor and loader; launcher integration; N7 | Hello through the real launcher; 100 start/stop cycles under ASan/LSan with mappings and descriptors checked; load and architecture errors reported |
| M5d | N6 | Sound plays through the desktop backend; captured PCM checked |
