# Milestone 3D acceptance — 2026-10-19

Implementation and validation are in progress. This record separates production
source/host tests, target compilation and actual physical-device evidence.
Until the physical sections are populated, deployment and package acceptance
are **PENDING**. No power-cut or endurance campaign is authorized for this gate.

Architecture: [format](package-format.md), [manager](package-manager.md),
[registry](app-registry.md). The previous [3C user acceptance](ui-milestone-3c-acceptance.md#user-verified-physical-touch-and-visual-acceptance)
has been recorded without repeating the finger test.

## Host and target gates

The real RPK decoder, POSIX PackageManager/registry and existing ELF preflight
are exercised under ASan/UBSan with mocked IDF admission/allocator/executor,
real files and injected write/fsync/rename failures. Host tests do not execute
Xtensa instructions or establish physical Flash atomicity.

The clean REP target build uses exact IDF v6.1 commit
`fff9895c82d744c7237be8847347bdd1b07c6643` and loader 1.3.5. Image/layout validation,
full-suite totals, final artifact identity and physical results will be recorded
after the corresponding gates complete.

## Pending experiments

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
stack in RP1. Validation/assertions remain enabled. A narrowly scoped return to
unchanged B is necessary to update inactive A through genuine RO1 while keeping
B's image bytes intact. Corrected-build and physical evidence follows below.

## Pending physical acceptance

Physical acceptance: genuine inactive-A RO1 OTA, independently confirmed VALID
startup/fallback; two or three simultaneous packages, actual ABI 1/2/3 execution,
update/failure/removal, controlled restart and final health. Acceptance requires
normal/STABLE, idle OTA/runtime/transfer and at least one valid installed package.
Physical power-loss transaction durability, worst-case latency and concurrent
Flash/native execution remain PENDING and outside this bounded gate.
