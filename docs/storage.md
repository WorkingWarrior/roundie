# Milestones 2A–2B: storage foundation and physical provisioning

## Status and decisions

**Architectural decision, 2026-10-18 (REP §§14–15, 18, 23):** select the
16 MiB `rep-v1` layout in [partitions/rep.csv](../partitions/rep.csv). It has
two 3.5 MiB OTA slots, no factory/recovery firmware, and a dedicated LittleFS
partition. This resolves the layout for this milestone. Future resizing is
another migration decision. The 128 KiB coredump reservation is provisional;
capture remains disabled until task/data budgets are measured (§25).

**Verified host/source facts:** the IDF partition generator validates the
CSV and generated binary. LittleFS 1.22.2 resolves with IDF v6.1 and compiles
on Xtensa. Host tests exercise the actual manager against a mock VFS/partition
backend and pthread mutexes. These are not hardware filesystem tests.

**Historical Milestone 2A evidence:** offline only. **Milestone 2B device
evidence:** REP is installed; LittleFS mount, capacity query, 256-byte file
write/sync/remount/read/integrity/delete/remount passed. NVS and PHY were
preserved during migration. See the execution record in
[flash-migration.md](flash-migration.md). Existing Milestone 1 open checks,
fault injection, power interruption, concurrency budgets and OTA rollback
remain outside this result.

## Build profiles

The top-level CMake selector is intentionally just `ROUNDIE_PROFILE`:

| Profile | Configuration inputs | Generated config | Suggested build directory | Application |
|---|---|---|---|---|
| `legacy` (default) | `sdkconfig.defaults` | `sdkconfig` | `build` | `roundie.bin`, embedded project `roundie` |
| `rep` | common defaults + `sdkconfig.rep.defaults` | `sdkconfig.rep` | `build-rep` | `roundie_rep.bin`, embedded project `roundie_rep` |

Commands are in the [README](../README.md). Both use the same managed
components and lockfile; only the two necessary firmware build directories
exist. An incremental legacy build reuses the existing tree. For a clean
REP rebuild remove only its generated tree/config with IDF's `fullclean`
and regeneration as appropriate; never remove backups. Generated SDK config
can override defaults, so CMake rejects a layout/rollback mismatch rather
than silently accepting it. Unknown profiles, non-16 MiB Flash, or a table
offset other than `0x8000` are rejected. `build*/roundie-profile.txt`, image
names, embedded descriptors, and serial diagnostics identify the profile.

Legacy retains the pre-existing generated IDF single-app table (app
`0x10000`), not the actual manufacturer's table. The guarded updater still
relocates only this diagnostic app into the inspected factory slot at
`0x20000`. It now checks the build marker, the exact legacy generated table
hash, and embedded app project **before any serial access**, then retains
all existing backup/live-table/erased-OTA/known-bootloader/manifest/size
checks. Copying or renaming a REP binary does not bypass its identity check.
The actual write uses a temporary snapshot of the checked app bytes, so a
concurrent build cannot replace the image between preflight and deployment.
Missing markers also fail closed: rebuild old artifacts before deployment.
The unchanged Milestone 1 source baseline is Git commit `7b253aa`, with its
original defaults and dependency lock still available in history. Rebuilding
that revision reproduces its source/configuration (image timestamps preclude
a blanket bit-for-bit claim); today's legacy build includes the new serial
storage diagnostics while preserving the board/RTC/battery/GUI sources.

REP enables `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` with the standard IDF
bootloader. There is no OTA downloading, activation, self-test confirmation,
or Safe Mode implementation yet. Do not set OTA metadata to `NEW` for this
foundation firmware: it does not confirm `PENDING_VERIFY` images. The fresh
installation route uses initial erased metadata, described in the migration
plan. Coredump writing, security features, and XIP from PSRAM stay disabled.
`phy_init` reserves 4 KiB; PHY data-in-partition is disabled and no PHY image
is required. NVS is not initialized or erased by this milestone.

## StorageManager ownership and errors

[roundie_storage.h](../components/roundie_storage/include/roundie_storage.h)
is an internal firmware API, not the public ELF SDK. The boot owner creates
an opaque manager with `roundie_storage_init(&storage)` before sharing it.
An absent `storage` data/littlefs partition is a valid initialized-but-
unavailable state. No arbitrary partition, raw partition handle, or filesystem
implementation type is exported.

`mount`, `unmount`, `status`, and `capacity` serialize on a manager mutex;
waiting for that mutex is bounded to 100 ms. Backend Flash operations can
take longer; the mutex deadline is not an I/O deadline. Init/deinit require
exclusive lifecycle ownership, and deinit requires all callers to be
quiescent. No external code may register/unregister this mount. Before
unmount, callers must close all VFS files and stop users. There is no worker
or general file-access API in this milestone. The port's global mount lock
rejects a second manager claiming an already mounted partition; the failed
manager never unregisters someone else's mount.

Mount uses only label `storage`, subtype `littlefs`, at `/storage` through
the pinned ESP-IDF VFS port. Both `format_if_mount_failed` and
`grow_on_mount` are explicitly false. A blank, incompatible, corrupt, or
missing filesystem stays unavailable. There is no format API, retry loop,
NVS erase fallback, or provisioning action in normal boot. A failed mount
can be explicitly retried. Successful mount does not itself establish
capacity; request a refresh separately. A writable LittleFS mount may perform
ordinary consistency work; the no-format policy is not a read-only forensic
mount guarantee.

| Result | Meaning |
|---|---|
| `NOT_INITIALIZED` | NULL manager / already cleaned up |
| `NOT_MOUNTED` | Valid manager, no owned mount |
| `MOUNT_FAILED` | Port returned a generic mount failure; blank/corrupt/I/O causes may be indistinguishable |
| `UNAVAILABLE` | Designated partition missing or port cannot find it |
| `INVALID_STATE` | Duplicate init/mount, mount already registered, or encrypted/unsupported port state |
| `NO_SPACE` | Capacity scan reports used == total; not a guarantee about a future write |
| `IO_ERROR` | Capacity traversal or unmount failed, or capacity data was invalid |
| `NO_MEMORY` | Manager/mutex/port allocation failed |
| `BUSY` | Manager mutex wait expired; output snapshot must not be treated as authoritative |
| `INVALID_ARGUMENT` | Missing output pointer |

The snapshot includes initialization, partition availability, mount ownership,
capacity validity, last operation result, and the underlying `esp_err_t`.
`status()` returns whether snapshot retrieval succeeded; `out.result` is the
last operation result, so retrieving status does not erase a mount failure.
`capacity()` refreshes total/used and returns its operation result. A failed
refresh invalidates and zeros previous capacity. Capacity is a snapshot, not
a reservation. LittleFS copy-on-write may need metadata space even when
reported free space is nonzero. Saturated usage may include the port's
documented shared-block overcount; it is not proof of `ENOSPC` on a write.
The port collapses mount errors into `ESP_FAIL`, so no invented diagnosis of
physical I/O vs corruption is made. Raw error details remain in port logs.

Unmount failure retains the manager and mount ownership for retry. Deinit
unmounts first and frees the mutex/state only on success. Startup retains
the mounted manager for firmware lifetime; failed startup cleans it up.

## Pinned port correction

**Verified source defect:** joltwallet/littlefs 1.22.2, upstream commit
`76107e7c300665df3d971081f70d3ae2a1a2249b`, multiplies a negative
`lfs_fs_size()` result into an unsigned size and clamps it to total capacity,
then returns `ESP_OK`. This can turn an I/O/corruption failure into a false
successful full-disk result.

`tools/patch_littlefs.py` verifies SHA-256 of that exact translation unit,
creates a corrected **build-directory copy**, and CMake replaces only that
source in the existing component target. Managed sources and their registry
hash stay intact. Negative traversal results now clear capacity and return
`ESP_FAIL` through all four public info entry points; the manager reports
`IO_ERROR`. Existing nonnegative COW overcount saturation stays unchanged.
The exact corrected helper is host-compiled with injected negative traversal
results. Remove this narrow workaround only after an upstream fix is pinned
and verified. Source drift fails the build; warnings are not suppressed.

Sources: [pinned port source](https://github.com/joltwallet/esp_littlefs/blob/v1.22.2/src/esp_littlefs.c),
[port registration API](https://github.com/joltwallet/esp_littlefs/blob/v1.22.2/include/esp_littlefs.h).

## Boot diagnostics and concurrency limits

Before board/GUI setup, concise serial output identifies the running
partition (label/type/subtype/address/size), embedded build profile, exact
REP table match or legacy/unknown, LittleFS presence, mount outcome,
underlying error, and total/used validity. Mounting requires both the REP
image identity and the complete REP table, including flags, and a running
OTA partition. A mismatch skips storage and basic diagnostics continue.
No GUI layout, RTC, battery, touch, or display code was redesigned.

REP §§11.2–11.6 still apply: SPI1 writes/erases can disable cache, stall both
cores/tasks and PSRAM access, rendering, DMA use, and future ELF execution.
A StorageManager mutex coordinates its own lifecycle, not all system Flash
users. GUI must not perform storage I/O; future package, loader, MTP, and
OTA workers must arrange lifecycle quiescence and mapping ownership. Never
wait for conflicting Flash writes while holding mapped Flash resources.
No XIP, asynchronous worker framework, latency budget, or isolation claim
is introduced here. **Pending experiment:** after separately authorized
migration/provisioning, measure Flash/PSRAM concurrency and GUI latency under
the actual workload before choosing these policies.

## Repeatable offline validation

After both builds, in the exported IDF environment:

```sh
python tools/validate_storage_layout.py --legacy-build build \
  --backup /mnt/Dane/Development/roundie-backups/preflash-2026-10-17-pMVcHN/flash-16MiB.bin
python -m unittest discover -s tests -p 'test_*.py' -v
```

The validator only reads local files. It verifies IDF CSV/binary equality,
alignment/overlap/bounds, both image slots, bootloader bounds, config,
manifest, initial OTA bytes, optional original-backup hash/table, overlap
mapping, and legacy build eligibility. It has no device/write/erase mode.
Storage tests use ASan/UBSan and real pthread synchronization around a mock
VFS; artifact tests inject profile, metadata, bounds, subtype, flags,
manifest, configuration, image-identity, size, and backup errors.

Actual results are recorded in [flash-migration.md](flash-migration.md).
Milestone 2B closes physical migration, provisioning, mount and the bounded
filesystem/remount test. Hardware corruption/I/O fault injection, power
interruption, concurrency and OTA rollback remain PENDING.

## Milestone 2B provisioning and one-shot diagnostic

The component's host dependency is pinned to `littlefs-python==0.15.0`.
Provisioning creates a new host buffer explicitly; it never opens a serial
port or reformats an existing image. To reproduce the installed initial
filesystem, after exporting the pinned IDF environment and building REP:

```sh
python -m venv build-rep/littlefs_py_venv
build-rep/littlefs_py_venv/bin/pip install \
  -r managed_components/joltwallet__littlefs/image-building-requirements.txt
build-rep/littlefs_py_venv/bin/python tools/provision_littlefs.py \
  --diagnostic --output build-rep/storage-initial.bin
```

Output creation is exclusive: choose a new output path if it already exists.
Without `--diagnostic`, the image is empty and contains no test request.
Neither output is an ordinary deployment payload; programming storage would
replace its contents and requires explicit provisioning authorization.
The guarded application updater never writes it.

The generator takes geometry from the validated partition/configuration:
2,256 × 4,096-byte blocks, read/program 128, cache 512, lookahead 128,
block cycles 512, name maximum 64, on-disk 2.1. Target page size is 256.
Two host generations were byte-identical; independent host mount/read and
actual target mount passed. Physical capacity was 9,240,576 total, 8,192
used and 9,232,384 free bytes, both initially and after cleanup.

The only provisioned file was `.roundie-m2b-request` with an exact versioned
token. Boot diagnostics run before GUI ownership and before other storage
users exist. They claim the request by renaming it to `.roundie-m2b-running`,
create `.roundie-m2b-data` with exclusive creation, write/fsync/close 256
known binary bytes, remount, compare exact content/length, delete data and
claimed request, remount again and confirm absence. Every descriptor is
closed before unmount. Errors preserve evidence and do not trigger format,
retries or NVS erasure. Routine startup only checks for the request; a
successful test is not repeated. This is a boot diagnostic, not a generic
command framework or storage benchmark.

Actual first boot logged filesystem PASS; an explicit software restart
remounted with unchanged usage and no second test. A 185-second smoke run
had no panic, watchdog failure or unexpected reboot. NVS initialization
returned `ESP_OK` without an erase fallback. RTC remained valid/OS=0 and
restored system UTC on restart; the separate 3-second host UTC check failed
at approximately -3.6 seconds and was not hidden by resynchronization.

The entire hardware gate for Milestone 2 is not closed: OTA manager,
application confirmation/rollback, power interruption and Safe Mode are
outside Milestone 2B. See [development flashing](flash-migration.md#rep-development-updates)
for the initial-OTA-A-only procedure and ROM recovery limits.
