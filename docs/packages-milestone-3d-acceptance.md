# Milestone 3D acceptance — 2026-10-19

Milestone 3D's implementation, host/target and bounded physical acceptance gates
are complete on the corrected 0.3.4 image. A is VALID/normal/STABLE; unchanged
B 0.3.3 is VALID/fallback. Two healthy development packages remain installed,
with no active runtime, application UI, transfer or application-owned resources.
The first candidate's unexpected panic is preserved below, separately from the
successful corrected-image acceptance. No power-cut/endurance campaign was run.

Architecture: [format](package-format.md), [manager](package-manager.md),
[registry](app-registry.md). The previous [3C user acceptance](ui-milestone-3c-acceptance.md#user-verified-physical-touch-and-visual-acceptance)
has been recorded without repeating the finger test.

## Host and target gates

The real RPK decoder, POSIX PackageManager/registry and existing ELF preflight
are exercised under ASan/UBSan with mocked IDF admission/allocator/executor,
real files and injected write/fsync/rename failures. Host tests do not execute
Xtensa instructions or establish physical Flash atomicity.

The clean REP target build uses exact IDF v6.1 commit
`fff9895c82d744c7237be8847347bdd1b07c6643` and loader 1.3.5. One clean REP build
passed, followed by focused incremental corrections. The final full Python
unittest discovery passed **72 tests in 13.025 s**, including the production C
tests with ASan/UBSan, existing runtime/UI/OTA/BootGuard tests and **28 new
package scenario groups**. This count is unittest cases plus separately named
C scenario groups, not 100 independent physical tests.

Package groups cover canonical IDs/traversal, envelope/manifest/padding/version,
truncation/overflow, size/free-space limits, session/offset/length rejection,
abort/timeout/Safe Mode admission, write/fsync/allocation/rename failures,
digest/target/ABI/import/relocation rejection, successful installation,
replacement/downgrade/duplicate policy, preservation after failed update,
unselected orphans, missing selection, restart reconstruction, corruption,
incompatible rollback metadata, bounded enumeration and scoped removal.
Host executor submission verifies exact independently owned ELF bytes for ABI
1/2/3; actual Xtensa execution is recorded separately below. RE1 production
controller tests now also cover its exclusion while RP1 owns a transfer.

Standalone hello/event/UI examples were compiled with the pinned GCC 15.2.0
toolchain in host-test temporary builds. Building each package twice produced
identical bytes. Hardware used the exact previously accepted example binaries,
without changing/rebuilding them to bypass validation. The same C decoder and
existing ELF preflight are compiled into offline package inspection.

Final REP partition/layout validation and independent esptool image-info passed;
the table is unchanged. The bootloader built as an IDF artifact was **not
deployed**. Memory protection, ABI 1/2/3 allowlists, loader PSRAM mapping,
rollback, RTC policy and development fault-disable configuration were preserved.

| Final firmware artifact | Verified value |
|---|---|
| Version / project | 0.3.4 / roundie_rep |
| Source implementation | `3d236e1`; stack-budget correction `09ce84a` |
| Image length | 719472 bytes |
| Image SHA-256 | `346b5d102f3df3205fe4a88890dfbeb1cc78b74f49afad808cbfd213c2892daf` |
| Embedded ELF / full ELF SHA-256 | `69c27122a2321a3f8fb41afdadf28e301f479646719c782e5386def38ce9200c` |
| Destination | inactive A / ota_0, starting 0x10000 |
| Preserved B ELF identity | `5c0d5eb2ed9b4fe4918620f2833298cd9c2a3274bfbb229cff96ae90db270142` |


## First candidate: preserved physical failure

The first 0.3.4 candidate (source `3d236e1`, image SHA-256
`2ad8cd9d3cd3b8379de6a7e5e45bed73b4f92a9045ae4d42395b11814b1f2af1`,
ELF `011318b843354df665b34045cbd4550e53059b6d22fc31d21b16ba8d9b4a4e89`)
passed genuine inactive-A RO1 OTA and NEW/PENDING_VERIFY/VALID, preserving B
0.3.3 VALID. First package END then triggered an unexpected LittleFS
`lfs_mlist_isopen` assertion in `lfs_file_read`, reached through `inspect_file`.
There was no successful install/active selection and no native ELF execution.
BootGuard recovered into core Safe Mode, with A/B still VALID and USB working.
Writes stopped; the original candidate and complete panic/transfer logs were
preserved outside Git. No formatting, erasure or repair was attempted.

Compiler `-fstack-usage` analysis of that exact candidate reported 3296 bytes
for `roundie_package_command`, 1680 for the worker, 304 for its frame dispatcher,
1088 for inspection, plus validation/commit/VFS/littlefs/crypto frames on an
8192-byte worker stack. **Inference:** inadequate nested stack margin is a
plausible cause of the in-memory file-list failure; no hardware stack-overflow
watchpoint report was captured, so the assertion alone does not prove it.

The correction moves the 1900-byte sole-worker reply scratch out of its call
stack, budgets 12 KiB for that worker, and exposes its observed minimum remaining
stack in RP1. The compiled RP1 frame fell to **1424 bytes**. Validation/assertions
remain enabled. Existing guarded RS1 CLEAR/SELECT/REBOOT returned to unchanged B
to update inactive A through genuine RO1. B reached normal/STABLE, LittleFS
mounted without repair, and a fresh read-only gate checked idle/confirmed
firmware, exact identities, RTC and free capacity. There was no repeat full-Flash
read or recovery experiment, and no write to B's application image bytes.

After this correction, package acceptance completed with a measured **4576-byte
minimum remaining worker stack**. This supports the stack-margin correction;
it is not retrospective proof that an overflow watchpoint fired in the first
candidate. No repeated assertion, panic, watchdog or brownout appears in the
corrected candidate's OTA, package or restart captures.

## Physical preflight and corrected OTA

The initial read-only gate independently checked unique physical ESP32-S3,
16 MiB Flash, 8 MiB PSRAM, expected security state, exact REP table and installed
rollback-enabled bootloader. Reads were limited to 65536 bytes at the Flash
prefix and two 288-byte image headers (**66112 bytes**), with CRC-valid A/B
VALID OTA metadata. The retained factory backup and previously verified manual
ROM Download procedure were available. No full-Flash readback, erase, format,
repartition, security/eFuse operation or new recovery experiment was performed.

Initial B 0.3.3 was running/selected VALID, BootGuard normal/STABLE; A 0.3.2 was
VALID/compatible. OTA was IDLE, NVS initialized ESP_OK, RTC valid/OS=0 and
LittleFS mounted with total 9240576 / used 20480 / free 9220096 bytes. After
the necessary correction recovery, B again passed the normal/STABLE gate and
LittleFS remounted with the known uncommitted transfer preserved, without repair.

Corrected genuine RO1 streamed the frozen final image to **inactive A**.
END independently verified its image digest and embedded ELF identity; ACTIVATE
reported NEW. Boot logs recorded A **PENDING_VERIFY at 1174 ms**, then successful
critical self-test and **VALID at 3314 ms**. BootGuard subsequently reached STABLE.
Host upload/activation/restart-ACK elapsed was **74.469 s**; this is USB workflow
time, not package processing time. Independent RO1/RS1/RT1/RE1/RP1 queries after
startup confirmed A identity, B VALID/eligible, NVS ESP_OK, healthy mounted
LittleFS and RTC OS=0. Package tests began only after these checks.

## Physical packages, execution and transactions

RPK **format 1**, platform `esp32s3.rep1`, platform API 1, no capabilities beyond
the declared ABI. Each install used bounded RP1 transfer, END persisted-byte
validation, then a separate INSTALL with the exact package digest. LIST before
INSTALL proved READY candidates were not installed. All three IDs were then
enumerated simultaneously with distinct metadata, correct versions and health 0.

| Application | Initial version | ABI | Package / ELF bytes | Physical result |
|---|---|---:|---:|---|
| org.roundie.hello | 1.0.0 | 1 | 1916 / 1692 | Generation 1, real ELF returned 42 and unloaded |
| org.roundie.events | 1.0.0 | 2 | 2340 / 2116 | Generation 2, init/start, one USER and one timer event, suspend/resume/stop/destroy |
| org.roundie.ui | 1.0.0 | 3 | 3892 / 3668 | Generation 3, native init/start, 6 logical widgets / 7 realized LVGL objects, foreground view |

RP1 launch resolved committed package records and supplied the actual persisted
ELF through the existing ELFManager/executor. Full/payload hashes and preflight
were checked again. An attempt to launch another package while the event ELF
was loaded was rejected. REMOVE was rejected while RUNNING, BEGIN while
SUSPENDED, and REMOVE while STOPPED-but-loaded. No Flash-writing operation was
admitted in those cases. Unload kept all installed packages. ABI 2/3 resources
returned to zero outstanding; the UI view/widgets/commands/realized objects were
zero after teardown. Existing raw RE1 upload/RUN also returned 42 using the same
hello ELF. No package-specific executor or callback path was used.

The controlled hello replacement used the **same valid ELF** with version
**1.1.0** and display name `Roundie Hello Update`, producing a distinct package
digest. It was fully validated before activation, INFO selected 1.1.0, and a
subsequent installed-ID launch returned 42. The other packages stayed intact.

The failed replacement candidate deliberately had invalid ELF magic, with a
correctly recomputed ELF digest and full transfer digest. END rejected it with
`magic` before execution and returned transfer IDLE. INFO still selected the
exact 1.1.0 package digest, and launching that installed package again returned
42. There was no deliberately malformed native execution or filesystem damage.

Explicit removal of **org.roundie.events** succeeded while no ELF was loaded.
It disappeared from LIST, subsequent launch was rejected, hello/UI stayed
available, and reported used capacity fell from **69632 to 57344 bytes** around
that removal. There was no deletion of the whole package area. The previous
hello artifact remains unselected by design; the registry exposes the active
1.1.0 version only.

One normal controlled RO1 restart then reconstructed the same two healthy
records and exact package digests, without app execution. Fresh runtime
generation/executed/busy/resource counters were zero. This is normal-restart
persistence evidence, not power-loss transaction durability.

| Final selected package | Version | Full package SHA-256 |
|---|---|---|
| org.roundie.hello | 1.1.0 | `9a36506f21136344e557235aaa8caa532a52551dd9eb28936ffa3a8a6d054cd1` |
| org.roundie.ui | 1.0.0 | `76ab4c5be328397900de02784f0d5f3f8660a56ba82e974bf5b4c8e17d5bfcd7` |

The ELF digests remain hello
`5edbfbf49c72890dd80f33263d2e52ffeeba9bfb0c5ef577047428a7360f0c88`,
event `8aaee0a856ad590a34ca5931404c8c62cd7b6fc499135b9a1bd397f9cbc9aa6b`,
and UI `340c9146c8d354ea88e367cce4c6831ccee99d54bb0125a18fe537545215cb64`.

## Bounded timing and memory samples

The upload interval reported by the device includes host/USB idle time; it is
not CPU-only processing. Validation and commit columns below are on-device
operation durations. Host measurements include stop-and-wait replies and END.
They are individual samples, not worst-case or percentile guarantees.

| Package | Host upload seconds | Device upload elapsed us | END validation us | Commit us |
|---|---:|---:|---:|---:|
| hello 1.0.0 | 0.27949 | 237981 | 9851 | 43046 |
| events 1.0.0 | 0.34048 | 290747 | 10128 | 38074 |
| UI 1.0.0 | 0.52819 | 481341 | 11799 | 47561 |
| hello 1.1.0 | 0.32991 | 270507 | 11084 | 50522 |

Three-package enumeration took **83927 us**, cached lookup **11 us**; the final
two-package post-restart enumeration took **75543 us**. Removal took **65318 us**.
Launch submission includes registry scan, package validation and runtime
admission; ELF relocation/callback work occurs asynchronously afterward.

| Installed launch | Submission us | Host to observed state seconds | ELF load/relocation us | Init us | Start callback us | Unload us |
|---|---:|---:|---:|---:|---:|---:|
| ABI 1 | 102127 | 0.13887 (completed) | 11890 | n/a | 14107 | 180 |
| ABI 2 | 102077 | 0.14000 (RUNNING) | 11686 | 2240 | 4045 | 1284 |
| ABI 3 | 101740 | 0.16942 (RUNNING/view ready) | 11767 | 5777 | 44 | 1151 |

ABI 2/3 shutdown measurements were **1241/1686 us**; GUI teardown **1727 us**.
No new GUI/finger latency benchmark was run. ABI 3 had zero physical/injected
events in this package-loading test; the earlier USER-VERIFIED 3C touch and
visual gate remains its separate evidence tier.

LittleFS total remained **9240576 bytes**. The first corrected pre-install
sample used **32768 bytes**, including the previous candidate's known transfer;
the clean pre-goal sample had used 20480. Three installed packages used 65536;
update/staging activity reached 69632 before removal; final and post-restart
used **57344**, free **9183232** bytes. These are block-accounting snapshots,
including development staging, retained inactive artifacts and filesystem
metadata, not the sum of payload lengths.

| Runtime sample | Internal free before / after | PSRAM free before / relocated / after |
|---|---:|---:|
| ABI 1 | 235459 / 235459 | 8382780 / 8382392 / 8382780 |
| ABI 2 | 235459 / 235459 | 8382300 / 8381628 / 8382300 |
| ABI 3 | 235459 / 235459 | 8380764 / 8378612 / 8380764 |

These executor snapshots bracket relocated code/data and SDK cleanup, with its
input buffer already owned at `before` and not yet freed when `after` is
published. They do not represent the entire system before first executor-task
creation; completed `busy=0` additionally proves input retirement. The persistent
executor/queue are system resources, not a per-app leak. ABI 2 acquired/released/
reclaimed/outstanding was **8/7/1/0**; ABI 3 **18/17/1/0**. UI teardown balanced
**13 acquired / 13 released / 0 outstanding**, with zero LVGL objects afterward.

After the final restart, live RS1 internal free was **248135 bytes** with no
executor autostart. Boot's earlier memory sample was internal free/largest
265419/188416 and PSRAM free/largest 8386120/8257536. That boot sample is not a
later live final-PSRAM measurement. Fixed package registry storage, the 1900-byte
reply scratch and 12 KiB worker stack remain system overhead. The minimum
observed worker stack during acceptance was **4576 bytes remaining**; this is
a measured margin for this bounded sample, not a proof for every maximum-size
package or future call path.

The final Xtensa map gives **1856 bytes** for the eight-record registry,
**132** for manager status, **1900** for reply scratch and **52** for digest,
clocks and transfer descriptor: **3940 named writable bytes before linker
alignment**, plus the worker's 4096-byte stack increase relative to 3C. Complete
package bodies are not cached; validation uses one transient ELF input buffer.

## Independently verified final state

| Item | Final observation |
|---|---|
| Firmware | A / ota_0, 0.3.4, selected/running VALID, exact corrected ELF |
| Fallback | B / ota_1, unchanged 0.3.3 ELF, VALID/compatible, rollback=1 |
| BootGuard | normal/STABLE, safe/latch/failures/trial/fault_remaining=0 |
| OTA | IDLE, checked=passed=1, last_error=0 |
| Runtime | UNLOADED, instance=executed=busy=0, no events/timers/subscriptions/resources |
| App UI | generation/widgets/pending/outstanding/realized_objects=0; LCD errors=0 |
| Packages | IDLE, session/received/total=0, two healthy selected IDs, orphans=0 |
| LittleFS / NVS | mounted/capacity-valid; 9240576 total / 57344 used; NVS ESP_OK, no erase |
| RTC | GET OK, valid, OS=0, error=0; no RTC write/discipline change |
| Diagnostics | actual RO1/RS1/RT1/RE1/RP1 replies and UI STATUS |
| Recovery | actual first-candidate panic entered Safe Mode without package startup; USB remained usable |
| Final image faults | no further assertion/panic/WDT/brownout in corrected OTA/package/restart captures |

## Evidence archive, reproduction and limitations

Raw logs, local device identifiers, frozen firmware/ELF/packages and backups are
outside Git under
`/mnt/Dane/Development/roundie-backups/packages-3d-2026-10-19/`.
The root holds the failed candidate and original evidence; `corrected/` holds
the final candidate, recovery/OTA acknowledgements, `a-first-boot.log`,
`package-traffic.log`, `package-results.json`, `normal-restart-boot.log` and
`final-device-state.json`. `run.py`, `corrected_run.py` and `stack_measure.py`
preserve the exact bounded procedures and one-shot mutation guards. Test/build,
layout and independent image-info logs are retained alongside them. No generated
images, ELF/RPK files, logs, backups or private device IDs were committed.

Repeatable software commands (physical commands in the manager document require
the explicitly selected device):

```sh
source /home/solve/.espressif/v6.1/esp-idf/export.sh
python -m unittest discover -s tests -p 'test_*.py' -v
idf.py -B build-rep -DROUNDIE_PROFILE=rep -DPROJECT_VER=0.3.4 build
python tools/validate_storage_layout.py --build build-rep
python tools/roundie_package.py --help
```

**Development security:** packages are unsigned trusted native artifacts.
USB development access is unauthenticated; SHA-256 is integrity, not publisher
identity, sandboxing or production trust. REP production signing remains open.

**PENDING:** physical power-loss atomicity at each transaction point; actual
full-LittleFS/write-failure/commit-failure injection on the board; maximum-size
eight-app latency/stack bounds; long-term heap/Flash behavior; concurrent Flash
programming/native PSRAM execution (deliberately prohibited); production signing
and distribution. Receiving cancellation/admission have host evidence;
protected commit/removal ordering is established by the sole-worker source
review, not a campaign of physical mid-commit resets. The first panic's exact
memory-corruption mechanism remains inferred as described above. Earlier RTC
accuracy, battery calibration, wake/power and endurance questions remain open.
MTP, launcher UI, package signing and Milestone 3E were not started.
