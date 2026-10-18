# Milestone 2C.1 — local A/B firmware updates

**Implementation decision:** REP §§18, 20, 23; one internal OTAManager and
the standard ESP-IDF OTA/bootloader APIs. Local development USB delivery only.
No network transport, TinyUSB, MTP, Safe Mode, Boot Guard, ELF runtime, security
enablement or partition migration is part of this milestone.

**Evidence boundary:** offline tests and target builds are separate from device
tests. The read-only physical baseline below passed. Physical bootstrap, OTA
transfer, first-boot confirmation, rollback, GUI responsiveness during Flash
writes and data-retention checks are **PENDING separate authorization**.
No Flash writes were performed for 2C.1 during this offline implementation.

## Verified installed baseline — 2026-10-18

Pinned ESP-IDF v6.1 is commit `fff9895c82d744c7237be8847347bdd1b07c6643`.
Its `components/bootloader/Kconfig.app_rollback`,
`components/app_update/esp_ota_ops.c`, bootloader sources and bundled
`docs/en/api-reference/system/ota.rst` establish the contract used here.
The [v6.1 OTA reference](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/system/ota.html)
was also checked. The required option is
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`; there is no separate
`BOOTLOADER_APP_ROLLBACK_CONFIRM_BY_APP` option in this pinned version.

Read-only ROM inspection matched the recorded USB identity to the ROM MAC,
ESP32-S3 and 16 MiB Flash, with Secure Boot and Flash Encryption disabled.
It read 710,080 bytes total: the 64 KiB system prefix, exactly 644,256 app
bytes and the 288-byte OTA B header. No whole-Flash read was repeated.

| Item | Actual result |
|---|---|
| Installed bootloader | Exact 2B reservation match; executable image 21,200 bytes, SHA-256 `f7c33fe831a564d16b8b00406c20ac2e79b8b7146f75e862263764656f349188` |
| Rollback configuration provenance | This same artifact matched the rollback-enabled generated bootloader configuration before the clean rebuild; 2B records its installation and verification |
| Installed table | Exact REP v1 table/sector from 2B |
| Active selection | `ota_0`, `0x10000`, capacity `0x380000` |
| OTA A image | Exact 2B image; project `roundie_rep`, version `0.1.0`, IDF `v6.1`, SHA-256 `8ed7ecd43afaf2ac31c70c29522c232d3ee68c7035fcbe2428617e1dea32d155` |
| OTA A ELF identity | `f929fe67dda2fcc3aab482d9fe0e0c6ed3f5d01e3202f39645ac4f412f95a5cb` |
| otadata sector 0 | Sequence 1, state 2 (`VALID`), CRC `0x4743989a`, checked with pinned IDF's sequence-only CRC convention |
| otadata sector 1 | Erased, no valid record |
| Complete otadata SHA-256 | `8ba3b110139f45443d4f268d1a3373ef99a1718b71d51664531b83ee2d4b91a3` |
| OTA B | Header erased; cannot be a bootable image. No fallback |
| Return to firmware | RT1 GET succeeded after ROM reset; RTC valid/OS=0, RTC/system within one second. No RTC SET |

Evidence is outside Git at
`/mnt/Dane/Development/roundie-backups/ota-2c1-2026-10-18/`:
`inspect_baseline.py`, `baseline.log`, `baseline.json`, bounded readbacks.
The active-slot inference uses the valid highest sequence and matching physical
application, together with 2B's startup record; the old firmware cannot answer
RO1 status. The original full backup and 2B images remain untouched.

**No bootloader replacement is necessary for this device.** A newly rebuilt
bootloader has a different compile timestamp and digest. That is not evidence
that the installed bootloader lacks rollback. Never replace it merely to match
new build output. Replacement on another device requires an exact compatible
artifact, preflight, preserved ROM recovery and **separate explicit approval**.

## Manager and ownership

`components/roundie_ota` is an internal C system component, not an SDK ABI.
It checks the exact REP A/B addresses/capacities and OTA metadata location.
Initialization runs before the worker starts. Afterwards the existing sensor/USB
worker is the sole manager and serial-RX owner. The manager never retains caller
chunk buffers and never starts a background job or task.

The existing worker has priority 4, no new core affinity, firmware lifetime and
an 8,192-byte stack (formerly 4,096). It now owns bounded protocol processing,
OTA calls and boot confirmation as well as the existing sensor sampling. Its
frame buffer is 1,100 bytes; decoded payloads are at most 512 bytes. It yields
10 ms between RX polls; Flash work never runs from the GUI task. RT1 RTC SET
retains its existing bounded worker delay. No second USB driver, competing
reader, generic RPC dispatcher or persistent transfer allocation was added.
For OTA builds the same driver's RX/TX rings are each 2,048 bytes, rather than
the 256-byte defaults. The pinned RX ISR does not report a full-ring drop, and
TX cannot enqueue a response larger than its ring: each ring must hold a whole
maximum request/response. Stop-and-wait is mandatory; there is no pipelining
during Flash writes. Overrun/truncated input fails framing, progress, digest
or timeout checks instead of activating an incomplete image.

GUI/main publishes its self-test evidence and copies status through the existing
style of short critical sections; all LVGL calls stay in main/GUI. The existing
GUI watchdog remains active. `END` logs worker stack high-water evidence.
**Assumptions pending measurement:** the stack reserve, priority and Flash/cache
latency are adequate on the device; task separation alone does not establish
responsiveness. IDF image validation is synchronous over the bounded slot and
is not preemptible by an ABORT request. This must be timed on hardware.

```text
IDLE -> RECEIVING -> VERIFYING -> READY -> ACTIVATION_PENDING
          |              |        |
          +-- failure ---+        +-- abort / 5 min expiry -> IDLE
          +-- abort / 15 s inactivity ---------------------> IDLE
```

Only one session exists. BEGIN requires passed core self-test, a compatible,
structurally valid running image with metadata `VALID`, unchanged boot
selection, and IDF's next partition equal to the checked inactive slot.
Manual `UNDEFINED` or missing metadata is diagnosed separately and does not
authorize overwriting the other slot. Metadata repair is not implemented.

Each DATA includes the session and exact next byte offset. Duplicate, skipped,
oversized, empty or overflowing writes fail; a wrong session cannot cancel the
owner's work. A failed transfer aborts/frees the IDF handle and hash operation,
keeps the running image/selection, and never erases an image to perform cleanup.
END consumes the IDF handle on success **or failure**. READY retains only bounded
manager state, expires after five minutes, and requires explicit activation by
the exact digest. Activation selects the image; restart is another explicit
step. A failed metadata-selection write leaves ACTIVATION_PENDING locked until
reboot/reinspection because its persistent outcome may be uncertain.

**Important IDF v6.1 detail:** `esp_ota_begin()` invalidates inactive OTA metadata,
even when sequential erasure is selected. BEGIN can therefore modify `otadata`
before ACTIVATE. Starting another update replaces the old inactive fallback;
the current valid image is the fallback for the new image. Never assume both
old images survive a new upload. Sequential erasure bounds work per chunk
instead of erasing the whole slot in BEGIN. Flash writes may stall GUI/cache/
PSRAM access; the real latency remains a device-test gate.

## Validation and identity

The first chunk must contain the complete 288-byte IDF header/segment/descriptor
prefix. Firmware validates image magic, segment count, target/revision/Flash-mode
compatibility using `esp_ota_check_image_validity()`, descriptor magic, bounded
terminated version/IDF/project strings and project `roundie_rep`. Declared size
must fit the inactive slot. The complete transfer SHA-256 uses the pinned IDF's
PSA API, compared before `esp_ota_end()` performs IDF image validation.

The written descriptor is checked again; IDF verifies the Flash image and its
exact image length must equal the declared transfer length. Activation uses
`esp_ota_set_boot_partition()` with its normal validation, never a skip-validation
API. Running-image writes, factory images, arbitrary partitions and raw sector
commands are not exposed. Standard IDF descriptors supply version, project,
IDF, compile date/time, secure version and full ELF identity. RO1 reports both
slot identities, state, selection, progress, digest, self-test and last manager
result. Descriptor version text is sanitized for JSON display; ELF identity
remains exact. The GUI shows version, active slot, image/update state and result.

SHA-256 checks transfer integrity, **not firmware origin**. The USB development
interface has no authentication and is explicitly gated by
`CONFIG_ROUNDIE_DEV_OTA` (REP diagnostics default on, production must disable).
No signing policy is introduced. Security version is read from existing image
metadata; hardware anti-rollback is disabled, so no eFuse floor is enforced.
CMake rejects security enablement/anti-rollback in this milestone; no eFuse API
or update of a security counter is added.

## Development USB protocol and CLI

Uses existing secondary USB Serial/JTAG and the RT1 printable-line framer.
RT1 retains its 63-byte command limit. The shared buffer admits RO1 frames
up to 1,099 bytes before LF; overflow, nonprintable input or a frame taking
over two seconds is discarded through the next LF. Every new host command
starts with LF for resynchronization. Diagnostic logs cannot satisfy an
addressed RO1 response. Responses are single contiguous driver-mutex writes,
with a 100 ms TX deadline, and have their own versioned prefix.

```text
!RO1 <8-lowercase-hex request-id> STATUS
!RO1 <id> BEGIN <nonzero-8-hex-session> <decimal-size> <64-hex-sha256>
!RO1 <id> DATA <session> <decimal-offset> <1..512 bytes as lowercase hex>
!RO1 <id> END <session>
!RO1 <id> ABORT <session>
!RO1 <id> ACTIVATE <64-hex-sha256>
!RO1 <id> RESTART
!RO1 <id> <decimal-esp_err> <bounded JSON status>
```

Malformed messages return errors without executing a suffix. Incomplete sessions
expire even without another host message. Lost acknowledgements do not trigger
blind DATA retries: the host attempts ABORT and reports failure. READY's session
identifier is in status, so a later process can cancel it. Restart is blocked
during boot verification, RECEIVING and READY, including through RT1.
Activation and ordinary idle restarts require successful self-test.

Reuse the pyserial dependency and `open_serial()`/bounded line I/O from
`roundie_time.py`. Opening the port retains the verified asserted DTR/RTS pair;
there is no line-reset sequence or unbounded serial flush. The tool streams
512-byte chunks and hashes with an 8 KiB host buffer. Individual requests have
an eight-second deadline; abort cleanup has a one-second deadline. Exit 0 is
success, 2 transport/framing/deadline failure, 3 image/device/file validation
failure and 130 user interruption. No fixed tty number or automatic device
selection is used for writes.

```sh
# Choose the actual stable USB alias; do not assume ttyACM0.
PORT=/dev/serial/by-id/<actual-device>
python tools/roundie_ota.py --port "$PORT" status
python tools/roundie_ota.py --port "$PORT" upload build-rep/roundie_rep.bin
# Copy the exact digest from the successful upload response.
python tools/roundie_ota.py --port "$PORT" activate --sha256 <digest> --restart
# After the device has re-enumerated, use its current stable alias.
python tools/roundie_ota.py --port "$PORT" verify --image build-rep/roundie_rep.bin
python tools/roundie_ota.py --port "$PORT" abort --session <session-from-status>
```

Upload performs no activation. Plain `status` and `verify` do not write Flash.
`verify --image` compares the running ELF identity, VALID state, self-test result
and boot selection; it does not claim to read back the complete image or prove
physical touch/display behavior.

## First-boot confirmation and rollback

Initialization diagnoses `PENDING_VERIFY`, `VALID`, `UNDEFINED`, missing metadata,
INVALID/ABORTED and a missing or incompatible fallback separately. Core self-test
is also recorded for manually installed firmware, but only PENDING_VERIFY calls
`esp_ota_mark_app_valid_cancel_rollback()`. A successful boot of the initial USB
installation does not become evidence of OTA self-confirmation.

The current deterministic confirmation policy requires:

- Exact checked REP OTA layout, 16 MiB Flash and 8 MiB PSRAM.
- Successful shared-board/LCD startup, LVGL objects and DMA buffer allocation,
  GUI watchdog registration and system-worker creation.
- After one second of GUI operation, at least one completed LCD DMA transfer,
  no LCD errors/fault, at least 16 KiB free internal memory with an 8 KiB largest
  block, and 128 KiB free PSRAM. These thresholds are conservative **decisions**,
  not measured production budgets.
- The worker must receive that GUI evidence within five seconds of starting
  verification, then confirm from its own context. Confirmation errors remain
  unconfirmed, block new OTA and are reported; they are not silently ignored.

Optional LittleFS mounting/capacity, NVS recovery diagnostics, RTC validity,
battery calibration, touch interaction, networking, microSD and external apps
are not confirmation prerequisites. NVS/LittleFS errors keep the existing
limited diagnostics and never cause erase/format fallback.

Critical startup failure rejects a pending image only if a structurally valid,
compatible `roundie_rep` fallback with `VALID` metadata exists and
`esp_ota_check_rollback_is_possible()` agrees. If none exists, the firmware stays
unconfirmed with failed/limited diagnostics and no programmed retry loop.
Early unrecoverable board/GUI initialization failures can leave console-only
diagnostics; the final Safe Mode is deliberately not implemented.

**Automatic rollback:** activation sets NEW, bootloader sets PENDING_VERIFY,
and another reset before confirmation produces ABORTED and returns to the
previous eligible image. The one confirmation opportunity is why ordinary
restarts and new uploads are blocked before self-test. After VALID, automatic
rollback does not supervise future runtime failures.

**Explicit rollback:** the internal `roundie_ota_rollback()` checks the previous
compatible VALID image and IDF eligibility before requesting invalidation and
reboot. There is no public USB ROLLBACK command or Safe Mode UI. Logs expose a
previous INVALID/ABORTED image; the exact historical reset cause is not persisted
and must not be inferred solely from that state.

For an approved automatic-rollback experiment only,
`CONFIG_ROUNDIE_OTA_TEST_UNCONFIRMED_RESET=y` creates an explicitly armed test
image. On PENDING_VERIFY, after otherwise passing core checks, it resets only
with an eligible VALID fallback; it never confirms itself. Without those
prerequisites it fails self-test and does not intentionally reset. The option
is off in normal artifacts and cannot be armed by USB or stored data. Build and
freeze the test image, record its identity and destination, obtain separate
fault-test authorization, then expect ABORTED and return to the known-good slot.
No deliberate image corruption or crash loop is needed.

## Interruption and recovery

Disconnect, timeout, short upload, malformed frame, invalid prefix/digest/image,
user abort and IDF write failure leave the running image intact. A reboot before
activation leaves the current boot selection. READY is volatile: after reboot
an uploaded but unselected image must be uploaded/verified again before activation.
After activation, reset before confirmation may roll back. Flash-selection
errors require reinspection; no app erase or metadata repair is attempted.

Only the intended inactive app sector envelope and `otadata` may change during
an approved OTA sequence. LittleFS, NVS, PHY, partition table, bootloader and
coredump are not OTA targets. No `erase-flash`, auto-format, NVS erase or raw IDF
flash command is permitted. Keep the original full backup and the verified
[manual ROM Download procedure](rom-recovery.md) available. A recovery write is
a separately scoped action using the reviewed [REP updater](flash-migration.md).
Do not blindly replay a full-Flash migration or the generated IDF flash command.

## Validation gates and physical test sequence

**Verified offline:** one clean REP target build under the pinned IDF; subsequent
incremental builds cover reviewed corrections. The host suite passes 44 tests,
including actual manager/parser code with mocked IDF and ASan/UBSan, real host
SHA-256, repeated begin/abort, both running-slot directions, range/partition/
selection guards, timeout/truncation/cancellation, wrong target/digest/length,
IDF begin/write/end/activation/confirmation errors, manual/pending states,
missing/invalid fallback, and host protocol/schema/identity failures. Existing
RTC and storage tests are included. This does not emulate bootloader execution.
Host C tests require a compiler and OpenSSL development headers/library.
The common main changes also compile in an incremental legacy-profile build.
The armed-reset branch passes a separate pinned-Xtensa syntax check; it has
not been linked as an armed test image or run on the device.

```sh
. /home/solve/.espressif/v6.1/esp-idf/export.sh
idf.py -B build-rep -DROUNDIE_PROFILE=rep build
python -m unittest discover -s tests -v
python tools/validate_storage_layout.py --build build-rep
```

**Physical baseline PASSED; all update tests PENDING.** Installed 0.1.0 has no
RO1 protocol. A genuine OTA test therefore requires an authorized bootstrap:

1. Freeze the normal 0.2.1 app, its SHA-256/full ELF identity and exact sector
   envelope. Preserve the installed original bootloader and old app evidence.
2. Recheck the uniquely identified device, installed 2B bootloader, exact table
   and initial VALID A metadata. Use `flash_rep.py`'s exact-2B-bootloader option
   to bootstrap **only A** with the normal app. This direct installation is
   explicitly **not** an OTA test and temporarily has no physical fallback.
   ROM recovery and the archived original A image are the recovery path.
3. Boot A, require passed core self-test/VALID status and inspect normal RTC,
   LittleFS and user-data diagnostics. A user touch/visual check remains PENDING
   if no operator is available; do not wait for it to do read-only checks.
4. Through RO1 upload the same frozen normal image into **B**, validate END,
   activate and restart; require B's PENDING_VERIFY-to-VALID self-confirmation,
   exact ELF identity, A as known-good fallback and stable boot selection.
5. Check LittleFS capacity/mount, NVS initialization, RTC OS/progression and
   ordinary diagnostic services against the pre-update record without RTC SET,
   filesystem test writes or unrelated configuration changes.
6. Only after a separate approved, frozen armed-image plan, upload the automatic
   rollback test into A with VALID B available, select/restart once, and require
   B return plus A ABORTED. Preserve logs from both boots. No power-cut tests.

The bootstrap updater's `--installed-m2b-bootloader` accepts only the exact
21,200-byte artifact/hash above, compares it to the live device, and **never
writes it**. This resolves rebuild timestamps without weakening arbitrary
bootloader matching. All its initial-A/table/metadata/device/security checks
remain. After the first real A/B selection, it intentionally rejects the changed
state; use OTAManager thereafter. **Build/commit approval does not authorize any
of these writes.** Record exact candidate hashes, slots, modified Flash ranges
and rollback prerequisites in the authorization request before execution.

### Frozen normal candidate prepared for authorization

Firmware sources: commit `03d66af`. Generated artifacts and evidence remain
outside Git in `roundie-backups/ota-2c1-2026-10-18/` beside the repository.
`normal-candidate/` contains the frozen app and the build-validation inputs;
`installed-m2b-bootloader.bin` is the exact executable bootloader slice from
the bounded physical readback, used only for comparison. `candidate.json`
records the following identity. Do not silently replace the frozen image with
a later rebuild under an approval for these bytes.

| Candidate / Flash envelope (half-open) | Exact value |
|---|---|
| Project / version / IDF | `roundie_rep` / `0.2.1` / `v6.1` |
| Application bytes | 668,528 |
| Application SHA-256 | `198b1053405e85f4da660a728f4df8dd41f025a8f1432ecc6ab047198509d925` |
| Embedded full ELF SHA-256 | `1e55c7a475ee573d9f38139f14e5498a631df2eabfd959e00ef8cdcdcfc92a02` (also matches the built ELF file) |
| Development OTA / armed-reset option | Enabled / **disabled** |
| Bootstrap A sector envelope | `0x010000..0x0b4000` |
| Genuine OTA B sector envelope | `0x390000..0x434000` |
| OTA metadata during BEGIN/activation/confirmation | `0x00d000..0x00f000` |
| Bootloader / table / NVS / PHY / storage / coredump | No writes authorized or proposed |

After explicit authorization of bootstrap A and the normal A-to-B OTA test,
the prepared commands are:

```sh
. /home/solve/.espressif/v6.1/esp-idf/export.sh
EVIDENCE=/mnt/Dane/Development/roundie-backups/ota-2c1-2026-10-18
PORT=/dev/serial/by-id/<actual-verified-device>
python tools/flash_rep.py --build "$EVIDENCE/normal-candidate" --port "$PORT" \
  --installed-m2b-bootloader "$EVIDENCE/installed-m2b-bootloader.bin" --write
# Require successful startup, passed self-test, initial VALID A and RO1 STATUS.
python tools/roundie_ota.py --port "$PORT" status
python tools/roundie_ota.py --port "$PORT" upload "$EVIDENCE/normal-candidate/roundie_rep.bin"
python tools/roundie_ota.py --port "$PORT" activate \
  --sha256 198b1053405e85f4da660a728f4df8dd41f025a8f1432ecc6ab047198509d925 --restart
# After re-enumeration, require VALID B, exact identity and eligible A fallback.
python tools/roundie_ota.py --port "$PORT" verify --image "$EVIDENCE/normal-candidate/roundie_rep.bin"
```

Approval of that normal sequence does not authorize bootloader replacement,
the armed-reset image, corruption tests, power cuts, erasure or another milestone.
The automatic-rollback experiment still needs its own frozen-image review and
authorization after the first normal OTA establishes a known-good fallback.

Power interruption, battery-dependent behavior, endurance, physical callback
latencies, touch/visual operation and worker stack reserves during OTA remain
unverified. They cannot be inferred from compilation, mocks or the ROM readback.
