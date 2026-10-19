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

Physical acceptance: genuine inactive-A RO1 OTA, independently confirmed VALID
startup/fallback; two or three simultaneous packages, actual ABI 1/2/3 execution,
update/failure/removal, controlled restart and final health. Acceptance requires
normal/STABLE, idle OTA/runtime/transfer and at least one valid installed package.
Physical power-loss transaction durability, worst-case latency and concurrent
Flash/native execution remain PENDING and outside this bounded gate.
