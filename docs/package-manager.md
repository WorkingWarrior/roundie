# PackageManager — Milestone 3D

**3F feasibility checkpoint, 2026-10-20:** MTP is not installed or connected
to PackageManager. The [USB gate](usb-architecture.md) is NO-GO; released
MTP direct VFS writes cannot bypass existing Flash admission. The proposed
[restricted backend](usb-mtp.md) must also reconcile MTP uploads without an
out-of-band full-package digest with the existing BEGIN digest contract.
RP1 behavior and transactional activation below remain unchanged.

Milestone 3E adds [launcher selection](app-launcher.md) through the existing
launch-by-ID function and [system navigation diagnostics](system-navigation.md).
RPK1/RPA1, artifact selection, storage/Flash exclusion and validation are
unchanged. Successful cache scans publish a volatile revision for UI refresh;
no revision is written to Flash. The existing 12 KiB worker stack is retained.

`components/roundie_packages` owns persisted RPK1 artifacts and bounded
installation state. ELFManager/executor still own executable instances; the
GUI bridge still owns LVGL objects. No new task, thread, filesystem mount,
partition, application loop or global transaction framework was introduced.
The existing `system_worker` is the sole RP1/RE1/RO1/RS1 mutation owner.

## Persistence and transaction

Only `/storage/packages` is managed. It shares the existing LittleFS mount,
with automatic format/grow disabled. Each validated ID owns a directory:

```text
/storage/packages/.transfer           one bounded, unselected candidate
/storage/packages/<id>/0.rpk          artifact slot 0
/storage/packages/<id>/1.rpk          artifact slot 1
/storage/packages/<id>/active.tmp     uncommitted selection
/storage/packages/<id>/active         sole committed selection
```

Limits: 8 application directories, 65760 bytes/package, 160-byte manifest,
one transfer, two artifacts/ID, a 256 KiB free reserve plus 16 KiB filesystem
metadata allowance at admission/commit. Maximum package byte envelope is
`8 * 2 * 65760 + 65760 = 1117920` bytes, excluding filesystem metadata and
the reserve. This leaves most of the approximately 8.8 MiB filesystem available
for later work. No complete package cache is retained. Full validation owns
one at-most-64 KiB ELF input in PSRAM, then frees it or transfers it to the
existing executor. Discovery hashes in 512-byte blocks and reads 224 metadata
bytes; its fixed registry holds at most eight records.

The package-enabled system worker has a 12 KiB internal stack (4 KiB above the
3C budget), and the sole-worker reply scratch is 1900 static bytes. RP1 reports
`stack_min` in bytes. Compiler stack-usage analysis and physical high-water
measurements are recorded in acceptance; host process stacks are not evidence
of sufficient Xtensa stack margin.

`BEGIN → RECEIVING → STAGED → VALIDATING → READY → COMMITTING → IDLE` is the
production state machine. STAGED/VALIDATING/COMMITTING are synchronous bounded
phases on the worker, not independently queued callbacks. DATA uses exact
session, consecutive offset and 1..512 copied bytes. Idle transfer timeout is
10 seconds; READY expires after 60 seconds. END checks exact length, fsync and
close, then reopens persisted bytes, checks transfer SHA-256 and full validation.
READY is not installed. INSTALL requires the exact package SHA-256 and repeats
validation before filesystem commit.

First install uses artifact slot 0. Replacement uses the **inactive** artifact
slot. Rename the closed, synchronized transfer onto that inactive slot, write
and fsync/close `active.tmp`, then rename it onto `active`. The existing active
artifact is never overwritten during replacement. The 40-byte selection is
`RPA1`, one slot byte (0/1), three zero bytes, and full-package SHA-256 (32 bytes).
The last successful active-record rename is the activation point. Only then
can a successful installation be reported. A post-commit discovery failure is
reported as failure even though selection may have committed; inspect state
instead of blindly repeating a mutation after a missing/error acknowledgement.

**Verified source contract:** joltwallet/littlefs 1.22.2's VFS holds its semaphore,
rejects rename of open source/destination files, calls `lfs_rename()`, and maps
negative errors. File fsync calls `lfs_file_sync()`. Pinned littlefs documents
atomic rename and synchronized file writes. This uses a single active-record
rename, not an assumed atomic transaction across multiple files. All files are
closed before rename. POSIX host fault tests verify program ordering and
preservation; they do not establish board power-loss durability.

One former active artifact may remain in the alternate slot; the next update
reuses that slot. Inactive candidates and `active.tmp` are never selected after
restart. A failed commit preserves the previous `active` and active artifact.
A missing/corrupt selection fails closed, with no guessing from filenames or
version order. Explicit BEGIN cleans only `.transfer`. Explicit removal cleans
only `active`, `active.tmp`, `0.rpk`, `1.rpk` for a validated ID, then its empty
directory. No boot cleanup, formatting, NVS erase or arbitrary repair exists.
An orphan directory consumes the eight-directory admission budget until scoped
explicit removal; unknown files are preserved and can prevent directory removal.

Replacement requires a strictly higher numeric version. Equal versions and
downgrades are rejected. A corrupt current installation must be explicitly
removed before replacement; it is not silently repaired. An incompatible but
structurally valid older active version can be superseded by a compatible newer
one. Uninstall removes both retained artifacts, not application user data.
Removal unlinks `active` first (logical removal), then known artifacts; cleanup
errors are reported and may leave unselected files for explicit retry. Repeated
removal of an already absent ID is successful.

## USB and ownership

`CONFIG_ROUNDIE_DEV_PACKAGES` depends on development ELF support and must be
disabled for production. RP1 uses the existing sole USB RX owner, framing,
strict eight-hex-digit request/session IDs, 1100-byte line bound and 512-byte
stop-and-wait chunks. Replies fit below 1900 bytes. No arbitrary host paths.

```text
!RP1 <request> STATUS
!RP1 <request> BEGIN <session> <size> <full-sha256>
!RP1 <request> DATA <session> <offset> <hex-bytes>
!RP1 <request> END <session>
!RP1 <request> ABORT <session>
!RP1 <request> INSTALL <full-sha256>
!RP1 <request> LIST
!RP1 <request> INFO <id>
!RP1 <request> LAUNCH <id>
!RP1 <request> REMOVE <id>
```

```sh
python tools/roundie_package.py --port "$PORT" upload /tmp/hello.rpk --install
python tools/roundie_package.py --port "$PORT" list
python tools/roundie_package.py --port "$PORT" info org.roundie.hello
python tools/roundie_package.py --port "$PORT" launch org.roundie.hello
# ABI 1 completes/unloads itself. ABI 2/3 use the existing generation controls:
python tools/roundie_elf.py --port "$PORT" status
python tools/roundie_elf.py --port "$PORT" stop --instance <generation>
python tools/roundie_elf.py --port "$PORT" unload --instance <generation>
python tools/roundie_package.py --port "$PORT" remove org.roundie.hello
```

Upload without `--install` leaves READY; explicit `install --sha256 <digest>`
must arrive within 60 seconds. Legacy RE1 raw ELF staging/RUN/START is retained.
The seven `us` diagnostic fields are latest upload elapsed, validation, commit,
enumeration, lookup, launch submission and removal. Upload elapsed is measured
on-device **including USB/host waiting**, not device CPU time. Host CLI separately
reports end-to-end upload seconds. Launch submission includes discovery,
integrity/preflight and runtime admission; existing RE1 `load_us`, `init_us`,
`execute_us`, `shutdown_us` and `unload_us` describe later executor work.
These are bounded samples, not worst-case guarantees.

## Flash, Safe Mode and rollback

All package writes require normal/STABLE BootGuard, self-tested VALID current
firmware, unchanged boot selection, IDLE OTA, mounted storage, and no RE1 transfer
or loaded ELF. This includes SUSPENDED and STOPPED instances. RP1 sessions also
block RE1 staging/execution, RO1 mutations/restart and BootGuard NVS persistence.
There is no concurrent Flash programming with native PSRAM execution.

Lock order is the existing single worker serialization followed by the bounded
StorageManager capacity mutex or LittleFS VFS semaphore, never both together.
The worker does not wait for callbacks. Package launch transfers separately
owned ELF input to the existing atomic executor reservation. Runtime teardown
finishes callbacks/UI/resource retirement before releasing that reservation;
package writes remain excluded until release. Persistent package files outlive
runtime unload; PackageManager stores no pointer into native code.

Safe Mode requests during RECEIVING or READY cancel the transfer, then follow
existing deferred NVS/restart semantics. A request arriving during synchronous
validation/commit/removal is processed after that bounded operation returns;
there is no reset in the active-record critical operation. A completed commit
may therefore precede a later acknowledged request. An interrupted/uncommitted
transfer never reports install success. Active native code receives existing
cooperative teardown and deferred persistence. No package operation changes
OTA metadata. Safe Mode starts without mounting/discovering packages or starting
the executor. Core OTA confirmation and BootGuard STABLE do not depend on the
package registry. Older 0.3.3/0.3.2 firmware ignores this new private directory
and retains its original mount/recovery behavior.

Physical results and remaining experiments: [acceptance](packages-milestone-3d-acceptance.md).
