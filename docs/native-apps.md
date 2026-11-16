# Native REP applications on Linux x86-64 (M5) — design note

**Status: decided and implemented on the desktop (M5a, `1ea8d2f`..`14df47b`).**
It answers D6 in [roundie-core.md](roundie-core.md): the Roundie Desktop runs
real REP applications, built from the same sources as the ESP32-S3 packages,
through the same launcher, runtime services and UI bridge. The audit and
experiment below describe the state before the work; the decisions record what
was built, including where it differs from the proposal.

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

## Decisions

**N1 Loading: `ET_DYN` shared object, `dlopen` after validation.**
`components/roundie_elf/linux/loader.c` writes the ELF to a `memfd`, seals it
(`F_SEAL_SHRINK | GROW | WRITE | SEAL`), maps the sealed copy, compares it with
the bytes the package manager validated and validates the mapping again, then
loads it with `dlopen("/proc/<pid>/fd/N", RTLD_NOW | RTLD_LOCAL)` on the
executor thread (the process id rather than `self`, so that a debugger, which
resolves the path in its own process, opens the same file; see N9). The bytes validated are the bytes loaded. The entry is
`base + e_entry` and must equal `dlsym` of the ABI's entry name. No relocation
code of our own: `ld.so` relocates. After `dlclose` the loader checks with
`dladdr` that the image is no longer mapped and closes the descriptor.

`ld.so` binds an undefined symbol to the first definition in the process, and
a module's own definitions can be preempted the same way. Two measures follow:
the desktop exports exactly the REP service tables
(`platform/linux/rep_exports.list`, `--dynamic-list`), and after `dlopen` the
loader reads every relocation slot of the image and compares it with the
binding the validator expected: `RELATIVE` = base + addend, an import = the
desktop's service + addend, a defined symbol = base + its value. A mismatch
unloads the image and reports `binding at +offset is X, expected Y`
(`loader-relocation`). Not chosen: a custom x86-64 loader; one process per
application (see N7).

**N2 x86-64 validation with the same policy.** `validate_x86_64.c` checks
ELF64/`EM_X86_64`/`ET_DYN` (System V OS ABI): segment set, order, alignment and
bounds, RELRO and note bounds; no writable code, no executable stack, no
`PT_INTERP`/`PT_TLS`; dynamic tags from an allowlist (no `DT_NEEDED`, no
initializers or finalizers, only `BIND_NOW`/`NOW` flags, no `TEXTREL`/`RPATH`);
GNU hash bounds; the shared ABI import allowlist; relocations `RELATIVE`,
`GLOB_DAT`, `64` and `JUMP_SLOT` only, into writable image memory, with
`DT_RELACOUNT` consistent. An application may not define a REP service name
(`service-symbol-defined`): `ld.so` would bind references to the desktop's
copy instead. Every rejection names its rule. Xtensa validation is unchanged.

**N3 Package target `linux-x64.rep1`.** RPK1 is unchanged; only the platform
string differs, chosen per build (`ROUNDIE_PACKAGE_TARGET`). The firmware keeps
`esp32s3.rep1`. Each build reports the other's packages as
`package-incompatible` before any ELF parsing. `roundie_package.py --target`
selects it (default `esp32s3.rep1`); `tools/roundie_native_app.py` builds the
unchanged example sources as self-contained x86-64 shared objects.

**N4 Native ABI.** The same REP ABI 1/2/3 source API. Tables and descriptors
have LP64 System V layout on `linux-x64.rep1`; the value structures are
byte-identical to Xtensa. The public headers assert both layouts; nothing
changed for Xtensa.

**N5 Shared runtime.** `manager.c` (lifecycle, services, mailbox, timers, UI
ownership, storage, audio ownership, clock events) is one file compiled by
both builds; it reaches the platform only through the D1 seam and the loader
port (`loader.h`), implemented by `idf/loader.c` (`esp_elf`, `heap_caps`) and
`linux/loader.c` (N1). The executor stays per platform: a FreeRTOS task with
the task watchdog (`executor.c`), a pthread with the same one-slot queue
(`linux/executor.c`). LVGL keeps one owner: callbacks run on the executor, UI
requests go through the bridge queue to the GUI thread.

**N6 Audio, as built.** Both platforms implement the `roundie_audio.h` stream
contract that `manager.c` and `rep_audio_v1` use: the firmware with the ES8311
(`audio.c`, unchanged), the desktop with an SDL3 playback stream
(`platform/linux/audio.c`). They share `stream.c` (ring, playback policy,
PCM conversion); the stream bookkeeping around it (open, write, close, owner
release, status) is written twice, because moving it out of the firmware's
`audio.c` would change board audio without a device test. One thread owns the
SDL output and keeps about two 256-frame chunks queued, in place of the
codec's blocking write. Tones and the melody remain firmware diagnostics.
Tests run with SDL's `dummy` driver and capture PCM with the `disk` driver,
never the session's output.

**N7 Security model: opt-in, trusted developer code.** A native application
is arbitrary code with the user's rights inside the desktop process. The
validation and import allowlist are API and compatibility controls, not
isolation: the application can read and write any memory of the process and
make system calls directly. Native execution is off by default and enabled per
run with `--native-apps`; without it every launch is refused
(`native applications are disabled`). Only `linux-x64.rep1` packages installed
into the desktop's data directory are run; nothing is fetched or started
automatically. Separate processes and sandboxing are outside M5a and M5b.

**N8 Application faults (M5b).** A fault in native code cannot be survived in
one process, so the desktop names it and ends, and the next session behaves
like the firmware's boot after a fault. `app_runtime.c` installs a handler for
`SIGSEGV`, `SIGBUS`, `SIGILL` and `SIGFPE` (`SA_SIGINFO | SA_ONSTACK`; the
executor thread gets a 64 KiB alternate stack unless a sanitizer installed
one). For a fault on the executor it writes, async-signal-safely, the package
id, the signal and the program counter as `image+0xOFFSET` (or "outside its
image" with the absolute pc), e.g.
`E elf: application dev.roundie.fault faulted: SIGSEGV at image+0x1044, address 0x0; ...`.
It then restores the previous action (a sanitizer's or the default) and
returns, so the fault recurs and the sanitizer or the kernel reports and ends
the process as before; a sent signal is re-raised. `addr2line -e app.so 0xOFFSET`
maps the offset to source when the object has debug information (N9).

Before each launch the desktop writes `app-loaded` (the package id) to its data
directory and removes it when the executor is idle again or the desktop exits
normally; it is the analogue of BootGuard's `fault_app` record. If the next
session finds it, it logs `Previous session failed with package <id> loaded`
and the launcher starts on the list instead of the home application, as the
firmware's trial boot after Safe Mode does. The desktop cannot tell a fault
from a killed process; both leave the marker.

**N9 Developer builds and debugging (M5b).** `tools/roundie_native_app.py
--debug` builds with `-g -Og -fno-omit-frame-pointer` instead of `-Os`;
`--sanitize-undefined` adds `-fsanitize=undefined -fsanitize-trap=undefined`,
which turns undefined behaviour into a trap instruction (reported by N8 as
`SIGILL`) and needs no runtime, so the object has no new imports, initializers
or sections and passes the unchanged production validator. Debug sections are
not loaded and are not checked by the validator. AddressSanitizer is not
supported for applications: an instrumented object needs `__asan_*` imports and
an `.init_array` constructor, both rejected by policy (N2); accepting them
would need a separate developer validation profile, which is not built. The
desktop itself is instrumented with ASan/UBSan in the test builds.

GDB: the image is a normal `dlopen` object, so GDB's shared-library support
finds it as `/proc/<pid>/fd/N` with its symbols:

```
ASAN_OPTIONS=detect_leaks=0 gdb --args build/roundie-desktop --headless \
    --native-apps --install event.rpk --script s.txt
(gdb) set breakpoint pending on
(gdb) break event.c:event
(gdb) run
(gdb) bt        # app frames, then roundie_elf_step / executor frames
```

LeakSanitizer does not run under ptrace, hence `detect_leaks=0` with an ASan
desktop. Breakpoints in an application persist only while it is loaded; a
symbol from a stopped application disappears with the image.

**N10 Virtual time (M5b).** `--virtual-time` freezes the desktop's monotonic
clock (starting at 1 s) and moves it only while the script waits, in steps of
at most 5 ms. A step is taken only when the executor has no request, control
or event left and has seen the current time, and when the system worker has
parked in `roundie_sleep_ms` beyond it; the GUI loop and the worker take turns.
Application timers, clock events, the launcher, storage and the emulated RTC
therefore see the same times on every run, and the tests compare whole stdout
and screenshots between two runs. Limits: the executor's own short waits and
the audio thread run in real time (audio content is deterministic, its
consumption timing and the ring level are not); an application that does not
return stops time and the script with it; the wall-clock fields of `load_us`
and similar measurements are real time.

## Platform differences (checked in M5b)

Same source (`manager.c`) on both platforms: lifecycle order, event queue
capacity and coalescing, timers, UI ownership and stale-handle rejection,
storage blob format and its write-at-unload, audio stream ownership, clock
events. Differences that remain, by design or as known limits:

| Area | ESP32-S3 firmware | Linux desktop |
|---|---|---|
| Fault in an app | panic, reset; BootGuard records `fault_app`; Safe Mode, then a trial boot on the list | N8 report, process ends; `app-loaded` marker; next session starts on the list (no Safe Mode) |
| App that does not return | task watchdog reset (same recovery path) | stall reported after 5 s; GUI continues; new launches refused until restart; under `--virtual-time` time stops |
| Kill vs fault | distinguished by reset reason | indistinguishable (both leave the marker) |
| Closing while an app runs | reset: storage changes since the last unload are lost | the same: quitting the desktop does not unload, the blob is not written |
| Callback budget | measured on the board | measured on the host CPU; not comparable |
| Memory report (`before/during/after`) | heap_caps figures | zeros |
| Tones and melody diagnostics | ES8311 | absent |
| Storage/audio bookkeeping glue | `idf/` | written a second time in `platform/linux` (shared core in `manager.c`/`stream.c`) |
| UI example label | "Native Xtensa app" | same string (source unchanged), although x86-64 |

## Limitations

- An application that does not return cannot be stopped: Linux offers no safe
  way to end one thread of a process. The executor publishes since when its
  current call runs; after 5 s the desktop reports the instance once, keeps the
  GUI running, and refuses new launches until it is restarted. Nothing cancels
  or kills the thread. The firmware's watchdog reset has no desktop equivalent.
- Application code can be built with trapping UBSan (N9) but not with ASan;
  the sanitizers otherwise cover the runtime, bridge, loader and audio.
- A fault ends the desktop (N8); recovery is a restart, not an in-process
  unload.
- Firmware behaviour of the shared runtime is checked by builds and host tests
  only; a device run of Hello, UI, Sound, Watch and repeated start/stop is
  PENDING until flashing is authorized.

## Verification (M5a)

| Check | Result |
|---|---|
| Four firmware builds (1.85B and 1.75C, rep and legacy) | exit 0, no warnings |
| Host suite with the IDF environment | 122 OK, 1 skipped |
| Validator: every example in C11 and C2x; forbidden constructs; every truncation; 20000 mutations (ASan/UBSan) | rejected by rule, no sanitizer report |
| Launcher: Watch (ABI 3, UI bridge) closed to the list, then Hello (ABI 1) returns 42 (ASan/UBSan, leak check) | pass |
| 100 Hello and 36 Stopwatch cycles | no image mapping, descriptor or runtime resource left |
| Truncated package, ESP32 package, `puts`-interposing object, desktop without `--native-apps` | rejected with the named reason |
| Hung application | GUI responsive, stall reported, desktop exits cleanly |
| Sound through SDL3 (`disk` driver) | 6.3 s tune captured, first note 784 Hz, mono on both channels; closing mid-tune releases the stream |
| ThreadSanitizer: launcher and Sound scenarios (manual run) | no report |

## Verification (M5b)

| Check | Result |
|---|---|
| Four firmware builds (1.85B and 1.75C, rep and legacy) | exit 0, no warnings |
| Host suite with the IDF environment | 131 OK, 1 skipped |
| Desktop suite (ASan/UBSan desktop) | 32 OK; GDB test passes where `gdb` is installed |
| Every example through the launcher in `--virtual-time`, two runs compared | identical stdout and screenshots (Sound: identical PCM content, ring level masked) |
| Storage: write, close, reopen, restart the process | blob `RPD1` + SHA-256 kept; quitting while running loses changes since the last unload |
| Faults: load, store, fetch, stack overflow, trapping UBSan | named with image offset; next session logs the package and starts on the list; `addr2line` maps the UBSan offset to its source line |
| ThreadSanitizer: events, UI, round, stopwatch, watch, sound in virtual time | no report |
| Device run of the firmware changes since M5a | PENDING (no flashing authorized) |
