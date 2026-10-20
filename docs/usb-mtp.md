# MTP inbox — feasibility status, 2026-10-20

**Milestone 3F — USB MTP: NO-GO / OPEN.** The current browser package-delivery
frontend is [Roundie Manager (3F-W)](roundie-manager.md), using Web Serial/RP1
and the existing PackageManager without firmware changes. It does not resolve
the MTP blockers below. Future MTP remains a separate restricted-staging
ingestion frontend to PackageManager and need not tunnel RP1.

**Not implemented or deployed.** [The USB architecture gate](usb-architecture.md)
is NO-GO. [The compile-only probe](../examples/usb_mtp_probe/README.md) proves
CDC+MTP component compilation and configuration descriptor structure, without
claiming an MTP device, LittleFS backend or install dialog exists.

## What the released backend actually does

**Verified source at esp_tinyusb 2.4.0 and 3.0.0:**

| Question | Evidence and limit |
|---|---|
| MTP enabled? | Kconfig `TINYUSB_MTP_ENABLED`; CMake compiles TinyUSB class and nine Espressif MTP translation units |
| CDC coexistence? | Both classes compile together and descriptor allocation fits; actual simultaneous traffic is untested |
| Actual upload code? | `SendObjectInfo`/`SendObject` dispatch into real file creation and streaming writes, not only stubs; no Roundie upload was run |
| Read/session/storage? | Real GetObject, session, storage and object handlers are present; Linux interoperability is pending |
| Cancellation/reset? | Control cancel, device reset and data-transfer failure paths retire pending state. Physical cable removal/reset behavior is untested |
| Backend? | Already mounted POSIX VFS paths, **with FATFS-specific registration and capacity queries** |
| LittleFS? | `tinyusb_mtp_register_storage()` calls `esp_vfs_fat_info()` and returns `ESP_ERR_NOT_FOUND` on failure; `GetStorageInfo` uses it too. Registering `/storage` is not supported by this implementation |
| Restricted adapter hooks? | Public storage config contains `base_path`, `display_name`, `removable`; no admission/read/write/commit callback table |
| Mutation policy? | Upload, delete, move, rename/property-set, edit, partial write and truncate all exist. An Inbox directory name does not restrict them |
| Work context? | MTP request/data callbacks dispatch synchronous filesystem operations in the TinyUSB service path, including cleanup and scans; there is no Roundie worker handoff |

Concrete sites: `mtp/tinyusb_mtp.c:tinyusb_mtp_register_storage`,
`tinyusb_mtp_operations.c:mtp_op_get_storage_info`,
`tinyusb_mtp_write.c` (`fopen(...,"wb")`, edit/truncate paths),
`tinyusb_mtp_transfer.c` (write/rename/cleanup),
`tinyusb_mtp_object_store.c` (delete/move/rename), and
`tinyusb_mtp_transport.c` (operation table and control cancel).
All refer to the [pinned source](https://github.com/espressif/esp-usb/tree/68bf7f9613dbb1a31094fdb903209ae3e4bf388d/device/esp_tinyusb).

**Decision:** changing only the capacity function or exposing a writable
LittleFS directory would bypass PackageManager admission and allow mutations
while native ELF memory is owned. Do not register `/storage`, package artifact
directories or a raw partition. Do not add FATFS/MSC or change the partition map.
These are integration blockers, not observed corruption of the current device.

## Smallest maintainable adaptation investigated

**Proposed, not scaffolded:** retain TinyUSB's MTP protocol implementation and
codecs, replace only the storage/operation bridge for one fixed logical inbox.
An upstreamable restricted backend needs capacity/object metadata plus bounded
begin/write/finish/abort and read operations. Its interface must distinguish
accepted asynchronous work from completed USB data; blindly acknowledging an
enqueue before durable write/error handling is insufficient. Simply adding a
function pointer around `fwrite` does not move long validation off TinyUSB.

Use one storage named `Roundie`, fixed logical `Applications/Inbox` parents,
at most one candidate and fresh session/object identities. Host filenames are
display metadata, never paths. Allow only bounded regular `.rpk` objects;
validate UTF-16 input, filename grammar/length, parent, declared/actual sizes,
quota/reserve and duplicate state before creation. Reject delete, move,
rename, edit, partial-object writes and directory creation predictably;
do not advertise writable operations that have no safe handler. Reads must
never reach active package artifacts or system files. Linux's required object
properties/events remain a measured compatibility question.

PackageManager currently expects a **full-package SHA-256 at BEGIN**, whereas
ordinary MTP ObjectInfo does not provide it. RPK1 includes an ELF digest, not
an out-of-band trusted full-package hash. A narrow internal inbox entry would
stream a full-package digest while receiving, then hand the fixed staging
object to the **existing** RPK1/embedded-ELF validator, computing/rechecking
the package digest before READY and again for installation. Do not pass a
guessed zero hash, invent host RP1 traffic, duplicate the ELF validator, or
call this publisher authentication. No such API was added at this failed gate.

Lifecycle design: IDLE → RECEIVING → RECEIVED → VALIDATING →
READY_FOR_CONFIRMATION → INSTALLING → INSTALLED, with explicit failure/cancel
states. USB completion cannot create an `active` selection. Validated metadata
alone may reach a small system-owned GUI dialog; GUI enqueues Install/Cancel,
the existing worker commits, then refreshes registry/launcher without autostart.
The existing 60-second READY timeout must be deliberately reconciled with
human confirmation; it must not unexpectedly free a worker-owned candidate.

## Flash admission and ownership design

**Verified existing contract:** PackageManager's admission checks
`roundie_packages_allowed()` plus runtime/RE1 ownership. RE1 `roundie_elf_busy()`
includes a still-loaded instance, not just currently running callbacks.
RP1/RE1/RO1 are serialized through the existing system worker. PackageManager
state is not a public thread-safe API for arbitrary TinyUSB task calls.

**Design:** TinyUSB callbacks copy bounded requests/data into a worker mailbox;
the same existing worker must own package state, admission and all filesystem
mutation. Before accepting any staging create/write/abort-unlink/commit, reject
Safe Mode, pending OTA, RO1/RE1/RP1 activity and **any loaded ELF**, including
SUSPENDED and STOPPED-but-loaded. Reserve transfer ownership so ELF loading
cannot race an admitted write. Use monotonically scoped session/operation
identity and bounded queues; return busy on overflow. Never hold USB/GUI locks
while doing filesystem work or waiting for the other task. The worker must
not wait for a USB callback that is synchronously waiting for the worker.

Disconnect/cancel invalidates further admission, requests cancellation, waits
for any worker access to retire, then releases staging and handles. Timeout
alone cannot free a buffer still owned by a worker. Cleanup itself mutates
Flash and must obey exclusion. Stale completions cannot install or revive a
retired session. Previous committed packages remain untouched on cancellation.
Power-loss durability and long endurance tests are outside this milestone.

## Resource budget: compilation evidence only

Probe configuration: one CDC (2048 B RX + 2048 B TX, 64 B endpoints), one
MTP storage, **32 objects**, 4096 B MTP endpoint buffer, MSC and packet tracing
disabled. A 4096 B MTP buffer is the released S3 Kconfig minimum, not a 64 B
buffer just because Full-Speed packets are 64 B.

Target archive measurement with `xtensa-esp32s3-elf-size`:

| Object/archive | text | data | bss |
|---|---:|---:|---:|
| Complete esp_tinyusb archive, pre-link sum | 88,156 | 4,479 | 5,488 |
| `cdc_device.c.obj` | 2,698 | 128 | 4,340 |
| `mtp_device.c.obj` | 3,257 | 4,120 | 80 |
| `tinyusb_mtp.c.obj` | 3,717 | 8 | 244 |

These are **object-section sizes**, not final linked firmware cost or live
heap use. Archive sums include code potentially discarded by the final link
and exclude dependent libraries and allocator overhead. The nonzero-initialized
MTP endpoint storage is in `.data`, so `.bss` alone understates RAM cost.

Xtensa DWARF type sizes (not 64-bit host guesses): object record **48 B**,
storage record **20 B**, context type **240 B**. The selected dynamic object
and storage arrays require **32 × 48 + 20 = 1,556 B**, before path/display
strings, VFS FILE/buffers, locks and allocator overhead. Default TinyUSB task
stack is **4096 B** plus TCB/task context; the current system worker remains
12 KiB. Default 512 objects would cost 24,576 B for records alone.

**Pending:** integrated link delta, peak/internal/largest heap, TinyUSB/GUI/
worker stack watermarks, queue depth, validation/commit time and Flash latency
during real transfer. No stack was started and no buffer was DMA-tested.
No conclusion about available application reserve follows from this table.

## Host and recovery acceptance plan

Available locally: `lsusb`, `mtp-detect`; those do not establish GVfs/KIO or
graphical file-manager acceptance. After all architecture blockers are resolved,
freeze an authorized candidate and validate the exact inactive-B RO1 path
with A untouched, as described in the task and [OTA documentation](ota.md).

Then inspect real descriptors/identity, query all five protocols on the new
CDC port, enumerate/list MTP with libmtp, transfer a valid RPK and compare size/
digest, prove it is absent from registry before physical confirmation, confirm,
launch and unload to zero resources. Exercise corrupt/truncated/oversized and
duplicate objects, invalid parents/handles, forbidden mutation operations,
cancel/session-close, cable removal at receiving/received/validation stages,
USB reset and reconnect, CDC close while MTP remains active, low-space and
loaded-ELF refusal. No power-cut test is authorized.

Record host payload duration/bytes, device receive/validation/commit timings,
peak queue depth/RAM/stack and error/retry counters. Test ROM recovery and
Safe Mode independently; require no formatting, stuck admission, automatic
execution or changed committed digest. GUI drag-and-drop and finger taps are
separate human evidence and remain PENDING. No part of this plan was run on
OTG firmware during this feasibility stop.
