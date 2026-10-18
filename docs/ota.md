# Milestone 2C.1 — local A/B firmware updates

**Implementation decision:** REP §§18, 20, 23; one internal OTAManager and
the standard ESP-IDF OTA/bootloader APIs. Local development USB delivery only.
No network transport, TinyUSB, MTP, Safe Mode, Boot Guard, ELF runtime, security
enablement or partition migration is part of this milestone.

**Evidence boundary:** offline tests and target builds are separate from device
tests. The read-only baseline, explicitly authorized A bootstrap, genuine
streamed A-to-B OTA, activation and B's first-boot confirmation **PASSED**.
The separately authorized automatic-rollback experiment also **PASSED**:
the device now runs normal 0.2.1 in VALID B and the armed A is ABORTED.
**Only B is currently eligible; restoring two confirmed copies requires
separate authorization.** Mount/capacity, NVS initialization and RTC progression
checks passed after recovery. Manual rollback remains unverified.
Visual/touch inspection, GUI latency during Flash writes,
byte-for-byte user-data comparison, power cuts and endurance remain unverified.

## Verified pre-test installed baseline — 2026-10-18

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
For the original normal A-to-B run the armed branch had only a pinned-Xtensa
syntax check. The separate fully linked armed candidate and stronger focused
tests are recorded in the rollback preparation section below; its later
explicit approval and physical execution are also recorded below.

```sh
. /home/solve/.espressif/v6.1/esp-idf/export.sh
idf.py -B build-rep -DROUNDIE_PROFILE=rep build
python -m unittest discover -s tests -v
python tools/validate_storage_layout.py --build build-rep
```

**Physical baseline and normal A-to-B sequence PASSED**, as recorded below.
The following reviewed sequence was necessary because the original installed
0.1.0 had no RO1 protocol. The deliberate rollback experiment in step 6 was
outside that approval; its later separate authorization/execution follow below:

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

## Authorized physical A-to-B execution — 2026-10-18

**Authorization:** the user explicitly approved bootstrap A and the normal OTA
B test with the exact frozen image, sector envelopes and metadata range above.
The approval excluded the armed rejection/rollback experiment and bootloader
replacement. Both operations used exactly 668,528 bytes with SHA-256
`198b1053405e85f4da660a728f4df8dd41f025a8f1432ecc6ab047198509d925`.
No firmware rebuild, new image, partition migration, Flash-wide readback,
filesystem test write, RTC SET, eFuse/security operation or fault injection
was performed for this sequence.

**Bootstrap A:** the frozen config/image and unique recorded USB identity were
checked before serial programming. The reviewed updater matched USB identity
to ROM MAC, checked chip/Flash/security, compared the exact installed 2B
bootloader, REP table and initial VALID A metadata, and wrote only the approved
A application envelope. Write-time digest verification and separate application
verification passed. The post-write bounded prefix passed the same bootloader,
table and metadata checks. Updater exit was 0. RTS reset returned directly to
firmware; no physical cable/button action or second write was needed.

Captured startup and RO1 status established exact 0.2.1 ELF identity, VALID A,
passed core self-test, absent B and no rollback destination. This direct
bootstrap did **not** exercise OTA confirmation: existing A metadata was
already VALID. NVS initialized ESP_OK, LittleFS mounted with 9,240,576 total /
8,192 used bytes, and RTC was valid/OS=0. The old original image and bootloader
copies remain in the evidence archive.

**Genuine OTA B:** `normal_ota_test.py` used the actual `roundie_ota.py` request,
upload and verification functions with a serial-recording wrapper. It streamed
512-byte chunks through RO1/OTAManager into inactive B while A was running.
All progress responses succeeded. END reported READY, the exact declared
SHA-256, valid B image and expected full ELF identity, with A still selected.
ACTIVATE selected B and returned its NEW state. An explicitly acknowledged
RESTART then produced the captured first-boot sequence:

```text
I (1147) ota: slot=ota_1 version=0.2.1 state=PENDING_VERIFY other=ota_0/VALID image=1 rollback=1 self_test=0/0 result=ESP_OK
I (3257) ota: Core self-test result: ESP_OK
I (3257) ota: slot=ota_1 version=0.2.1 state=VALID other=ota_0/VALID image=1 rollback=1 self_test=1/1 result=ESP_OK
```

The final independent RO1 query/identity verification reported `running=ota_1`,
address `0x390000`, `boot=ota_1`, `phase=IDLE`, `ota_state=VALID`,
`checked=1`, `passed=1`, `last_error=0` and `rollback=1`. A's state was VALID,
its image was valid and both slots had the expected full ELF identity
`1e55c7a475ee573d9f38139f14e5498a631df2eabfd959e00ef8cdcdcfc92a02`.
This is evidence of real streamed OTA, activation and application confirmation,
not merely a successful ROM application flash. It establishes eligibility for
a future rollback test, **not execution of automatic or explicit rollback**.

| Measurement | One approved physical run |
|---|---|
| Host upload duration, including END validation | 65.826 s |
| Largest DATA request round trip | 60.797 ms |
| BEGIN round trip | 101.608 ms |
| END round trip / largest overall | 294.618 ms |
| ACTIVATE round trip | 293.484 ms |
| Instrumented successful RO1 requests | 1,313, including final status |
| Worker stack minimum remaining at END | 2,200 bytes of the configured 8,192 |
| Pending startup log to VALID log | 2.110 s; VALID log at boot timestamp 3.257 s |
| RTC progression across upload/restart/inspection | 81 seconds over 81.163 s monotonic elapsed |

These are combined host/USB/firmware request latencies, **not measured GUI
frame times or isolated Flash erase times**. No panic, task-watchdog event,
LCD transfer timeout or unexpected extra reboot appeared in the captured
transfer/boot records. LCD initialization and core DMA checks passed; human
visual/touch inspection remains PENDING.

**Data-retention evidence and limits:** after B boot, NVS again initialized
ESP_OK without erase fallback. LittleFS again mounted/capacity-queried ESP_OK
with exactly 9,240,576 total / 8,192 used bytes; the M2B write/remount diagnostic
did not rerun. RTC retained valid OS=0 data, agreed with system time within
one second and met the existing two-second progression criterion without an
RTC SET. This checks service continuity and filesystem capacity, plus the
reviewed OTA destination guards; it does not claim bytewise comparison of all
NVS/user files. No host-UTC synchronization pass is claimed (the older offset
question is separate). Bootloader/table were preserved by the app-only updater;
the OTA component has no write path to those or NVS/LittleFS/PHY/coredump.

**Evidence, outside Git:** in
`/mnt/Dane/Development/roundie-backups/ota-2c1-2026-10-18/`, see
`bootstrap_capture.py`, `bootstrap-write.log`, `bootstrap-result.log`,
`bootstrap-a-boot.log`, A status/RTC JSON, `normal_ota_test.py`,
`normal-ota-protocol.log`, `normal-ota-result.log`, `normal-ota-b-boot.log`,
the READY/activation/restart/final status and RTC JSON, and
`normal-ota-metrics.json`. The scripts reject replay through exclusive log
creation; no physical write was repeated after success. Firmware sources and
the frozen candidate remain those of commit `03d66af`.

**Historical stop boundary after the normal sequence:** deliberate
automatic/explicit rollback, physical fault injection, power interruption,
battery-dependent behavior, endurance, detailed callback/GUI timing and broader
stack-budget validation were PENDING at that checkpoint. No armed image was built or deployed
as part of that normal sequence, and no Safe Mode or subsequent milestone was
started. The later, separately scoped rollback preparation follows.

## Controlled automatic rollback — preparation, 2026-10-18

**Status: PHYSICAL AUTOMATIC ROLLBACK: PASSED.** Preparation initially stopped
at the separate authorization boundary. The user subsequently approved exactly
one frozen-image experiment, which passed as recorded below. Previous normal
A-to-B approval was not reused. Do not run a second experiment or repair A
automatically; redundancy restoration remains separately scoped.

**Fresh read-only preflight, before building:** RO1 reported normal 0.2.1 in
VALID B (`0x390000`), stable boot selection, passed core self-test and compatible
VALID A as inactive/fallback. Both ELF identities matched the original normal
image. A bounded ROM inspection matched the recorded USB serial to ROM MAC,
ESP32-S3, 16 MiB and disabled security features, compared the exact installed
2B rollback bootloader and REP table, and checked both OTA metadata CRCs:
A sequence 1 / VALID, B sequence 2 / VALID (selected). The 668,528 physical B
bytes matched the frozen normal image byte-for-byte and SHA-256
`198b1053405e85f4da660a728f4df8dd41f025a8f1432ecc6ab047198509d925`;
A's 288-byte descriptor prefix also matched. Total read: 734,352 bytes, not a
whole-Flash backup. The original 16 MiB backup and documented manual ROM
procedure remain available. A bounded normal return capture and fresh RO1/RT1
queries reconfirmed VALID B/A, self-test and valid RTC OS=0. No unexplained
mismatch was found and no physical Flash write was performed in preparation.

**Pinned source semantics:** IDF commit
`fff9895c82d744c7237be8847347bdd1b07c6643`,
`components/bootloader_support/src/bootloader_utility.c` lines 393–400 changes
PENDING_VERIFY to ABORTED on the next boot before normal slot selection;
lines 443–447 changes the selected NEW image to PENDING_VERIFY. Inactive
metadata can already change in `esp_ota_begin()`. The experiment uses these
mechanisms without raw metadata management or bootloader changes.

### Test-only control-flow correction and focused verification

Source commit `1f03b79` corrects a safety ambiguity in the earlier test hook:
its failed guard formerly fell through to `boot_result(false)`, whose ordinary
policy may explicitly mark INVALID/reboot. That is not the intended automatic
ABORTED experiment. The armed branch now has only two outcomes:

- Core self-test evidence received/passed, running PENDING_VERIFY, compatible
  VALID fallback and IDF rollback eligibility: log the passed conditions and
  suppressed confirmation, bound USB TX drain to 100 ms, delay 100 ms and call
  `esp_restart()` once without confirming or invalidating either image.
- Otherwise, including failed/absent GUI evidence, early critical startup
  failure, nonpending state or unavailable fallback: hold unconfirmed with a
  recorded error and uploads blocked; no confirmation, explicit invalidation
  or intentional restart. Even an unexpectedly returning reset call cannot
  fall through to normal confirmation/rejection. Timeout evidence is decided
  once, preventing a retry/log loop.

The internal hold method does not access Flash or expose a USB command. Normal
builds retain the ordinary confirmation/failure policy. The compile-time option
cannot be armed over RO1. CMake now accepts standard explicit `SDKCONFIG` and
`PROJECT_VER` overrides for a separate external build and rejects armed images
without a version ending in `-rollback-test`; defaults remain normal 0.2.1.

**Verified offline:** 12 focused OTA host tests PASS. They compile/run the actual
orchestration in both normal and armed modes with ASan/UBSan and exercise 112
combinations of image state, fallback eligibility and passed/failed/timed-out/
early-failed self-test. System and manager calls are spies; actual-manager
tests separately prove eligibility and hold semantics, preserved pending/
fallback metadata, zero confirmation/rejection calls and blocked new sessions.
Host framing/image/transfer tests remain included. These tests are not bootloader
or physical rollback evidence. No unrelated endurance/full-suite repeat ran.

### Frozen armed candidate

Artifacts and evidence are outside Git in
`/mnt/Dane/Development/roundie-backups/rollback-2c1-2026-10-18/`.
`frozen-test/` holds the exact app, ELF, generated config/header, explicit SDK
config and partition table; `candidate.json` records identities and scope.
`initial-preflight.log`, `initial-result.json`, bounded raw readbacks,
`initial-return-boot.log`, `focused-tests.log`, `build.log`, `layout.log` and
`image-info.log` preserve the preparation evidence. Local identifiers/raw logs
are not committed.

| Frozen test artifact | Verified value |
|---|---|
| Source commit | `1f03b79a1985f55550e74adbdcb3823cf4dfae7c` |
| Project / version / IDF | `roundie_rep` / `0.2.1-rollback-test` / pinned `v6.1` |
| Image bytes | 668,384 |
| Image SHA-256 | `126f41255befc470e215c18dec756056407c13a99b456afbde1bb17413ba5fbb` |
| Full ELF identity | `d4fccb9270e536d06102288db389962c2308c391cd6ef2a275c9e4ca8943afef` (matches ELF file hash) |
| Target / format | ESP32-S3, valid IDF descriptor, checksum and validation hash |
| Config difference from normal frozen JSON | Only `ROUNDIE_OTA_TEST_UNCONFIRMED_RESET: false -> true` |
| Other build identification | Explicit CMake `PROJECT_VER=0.2.1-rollback-test` |
| Normal image/config | Preserved unchanged; armed option disabled; original SHA-256 verified |
| Destination partition | Inactive A, `0x010000..0x390000` |
| Actual A erase/write sector envelope | `0x010000..0x0b4000` |
| Metadata envelope | `0x00d000..0x00f000`, potentially modified from BEGIN onwards |

The separate external build, exact REP layout validator and esptool image
checksum/hash validation passed. No security, partition, driver, storage or
other production configuration changed. The test image identity is distinct
from the normal firmware. Build command (already executed once; do not rebuild
or substitute bytes under an approval for this frozen artifact):

```sh
idf.py -B /mnt/Dane/Development/roundie-backups/rollback-2c1-2026-10-18/build \
  -DROUNDIE_PROFILE=rep \
  -DSDKCONFIG=/mnt/Dane/Development/roundie-backups/rollback-2c1-2026-10-18/sdkconfig.rollback \
  -DPROJECT_VER=0.2.1-rollback-test build
```

### Proposed single physical experiment — approval required

This was the reviewed plan at the authorization checkpoint. It was subsequently
approved and executed once; it is not permission to replay the experiment.

1. Immediately before BEGIN, repeat critical read-only device, normal B hash,
   bootloader/table, metadata and RO1/RTC preflight. An unexplained mismatch
   stops preparation; do not assume the earlier state is still current.
2. Start a bounded serial capture able to reconnect to the same verified USB
   identity across both software resets; retain host receipt timestamps as
   well as device boot timestamps. Upload **the frozen test binary** via
   existing RO1/OTAManager into A. Verify READY/digest/ELF, and B still selected
   and VALID before activation. Upload is already a Flash write, including
   possible metadata changes in BEGIN.
3. ACTIVATE A, verify NEW, and request one RO1 restart. A must log its distinct
   test identity and PENDING_VERIFY, then passed core checks and the explicitly
   armed unconfirmed reset. It must never log confirmation or VALID.
4. Observe the second bootloader selecting/loading normal B automatically.
   Require fresh RO1 status: B VALID/running/selected with its original full
   ELF identity; A ABORTED with the armed ELF identity; core self-test passed.
   IDF's installed INFO-level loader need not emit its DEBUG-only ABORTED line;
   actual resulting metadata plus loader selection and the captured preceding
   unconfirmed restart must establish the rollback.
5. Check LittleFS mount/capacity, NVS initialization, RTC OS/progression without
   SET, USB diagnostics and absence of unexpected panic/watchdog events.
   Preserve timings at their actual precision; host receipt time bounds are
   not exact Flash/GUI/reset execution times.
6. Stop after one attempt. Unexpected states or missing evidence mean no PASS;
   preserve all records and perform only read-only diagnostics. Do not
   reactivate the test or automatically reflash. If normal recovery fails,
   retain the [verified manual ROM route](rom-recovery.md); any Flash repair
   requires a separate reviewed authorization. Bootloader, table, NVS,
   LittleFS, PHY and coredump are outside the proposed writes.

**Expected final state:** A ABORTED, B VALID/running. This intentionally removes
A as an eligible fallback: `rollback_possible` is expected to become false.
The armed binary may remain structurally valid in A but is not confirmed
redundancy. Manual rollback remains separately unverified.

### Redundancy restoration — separate later authorization

Do not repair A as part of this experiment. To restore two confirmed copies,
first obtain separate approval for the preserved normal 0.2.1 binary (SHA-256
`198b1053405e85f4da660a728f4df8dd41f025a8f1432ecc6ab047198509d925`,
668,528 bytes), destination A `0x010000..0x0b4000` plus metadata
`0x00d000..0x00f000`. With known-good B running/VALID, upload that normal image
through RO1, verify it, activate A and boot it. Require A to pass its ordinary
self-test and become VALID, then independently query **both** A and B as VALID
with normal identities. Merely uploading a structurally valid A image does
not restore a confirmed fallback. This procedure ends with A running and B as
fallback; no return-to-B operation or extra update is implied or authorized.

## Physical automatic rollback — authorized execution, 2026-10-18

**Result: PASSED — automatic rollback, not manual rollback.** The user answered
“Zatwierdzam jeden test rollbacku” to the exact frozen-artifact/range/behavior
request before execution. Approval covered one upload to A, activation and
host restart, the guarded unconfirmed restart and observation of B recovery.
It excluded repeated experiments, A repair, unrelated Flash regions and
bootloader replacement. The approved image was not rebuilt or substituted:
SHA-256 `126f41255befc470e215c18dec756056407c13a99b456afbde1bb17413ba5fbb`,
668,384 bytes, ELF
`d4fccb9270e536d06102288db389962c2308c391cd6ef2a275c9e4ca8943afef`.

`execute_once.py` rechecked that artifact and its single-attempt guard, then
repeated the critical read-only preflight immediately before writing. Both
metadata CRCs/states, selected VALID B, normal B bytes/hash, A descriptor,
device identity, installed rollback bootloader, exact table and backup
availability still matched. It returned to normal B and reconfirmed RO1/RTC
before uploading. No mismatch or recovery interaction was required. No
whole-Flash dump or bootloader write was performed.

### Actual transaction and two boot attempts

The real RO1/OTAManager path streamed the test into inactive A. READY reported
all 668,384 bytes, the approved digest and test ELF, with B still VALID and
selected. ACTIVATE reported A NEW and selected; one RO1 RESTART was acknowledged.
The continuous serial capture recorded:

```text
I (259) boot: Loaded app from partition at offset 0x10000
I (1146) ota: slot=ota_0 version=0.2.1-rollback-test state=PENDING_VERIFY other=ota_1/VALID image=1 rollback=1 self_test=0/0 result=ESP_OK
I (2986) ota: ARMED core self-test PASS: PENDING_VERIFY with compatible VALID fallback; confirmation suppressed
W (2986) ota: ARMED DEVELOPMENT TEST: reset before confirmation; expect ABORTED + fallback
rst:0xc (RTC_SW_CPU_RST),boot:0xa (SPI_FAST_FLASH_BOOT)
I (263) boot: Loaded app from partition at offset 0x390000
I (1060) ota: slot=ota_1 version=0.2.1 state=VALID other=ota_0/ABORTED image=1 rollback=0 self_test=0/0 result=ESP_OK
I (3070) ota: Core self-test result: ESP_OK
I (3070) ota: slot=ota_1 version=0.2.1 state=VALID other=ota_0/ABORTED image=1 rollback=0 self_test=1/1 result=ESP_OK
```

Boot timestamps restart at each boot; the first host-requested reset also
reported `RTC_SW_CPU_RST`. There was one test A load, one guarded reset intent,
and one automatic normal B load in the experiment capture. A never reported
VALID or the refused/returned reset path. No host reactivation, explicit INVALID
rollback request, second experiment or A repair occurred.

The installed bootloader's INFO log does not emit its DEBUG-only “marking as
ABORTED” message. The pinned source transition, captured unconfirmed A reset,
automatic B loader selection and fresh actual A=ABORTED metadata together
establish that behavior; a DEBUG message is not claimed as observed.

The independent final RO1 query/normal-image verification passed:

| Final property | Observed value |
|---|---|
| Running / selected partition | `ota_1` / `ota_1`, address `0x390000` |
| B metadata | `VALID` |
| B full ELF | `1e55c7a475ee573d9f38139f14e5498a631df2eabfd959e00ef8cdcdcfc92a02` (original normal image) |
| A metadata | `ABORTED` |
| A full ELF | `d4fccb9270e536d06102288db389962c2308c391cd6ef2a275c9e4ca8943afef` (approved armed image) |
| A structural image validity | Valid image bytes, but **ineligible** ABORTED state |
| Core / manager state | `checked=1`, `passed=1`, `last_error=0`, `phase=IDLE` |
| Eligible rollback destination | None: `rollback=0` |

### Recovery services and measurements

In normal B's boot portion, NVS initialized ESP_OK with no erase fallback.
LittleFS mounted/capacity-queried ESP_OK at the unchanged 9,240,576 total /
8,192 used bytes; the M2B filesystem write test did not rerun. RTC remained
valid, OS=0, within one second of system time, and advanced 76 seconds over
75.388 s monotonic test time, passing the unchanged two-second progression
criterion. There was no RTC SET, formatting or storage erase. These are service
continuity/capacity checks, not bytewise comparison of all NVS/user files or
a host-UTC synchronization result.

Normal B core self-test and USB RO1/RT1 availability passed. The complete
transfer/two-boot capture contains no unexpected panic, task-watchdog trigger,
stack-canary failure, brownout or LCD transfer error. This is one controlled
experiment, not a GUI/endurance/fault-injection reliability campaign.

| Metric | Actual evidence / precision |
|---|---|
| Test upload through END | 68.475 s host monotonic |
| Activation | Success; A selected NEW, B still VALID |
| First A loader line received | 0.401 s after the host RESTART request |
| First A PENDING_VERIFY app log received | 1.304 s after that request |
| Observed pending app log to reset-intent log | 1.840 s (device boot timestamps 1146 to 2986 ms) |
| Reset-intent to next ROM reset-cause receipt | Approximately 0.100 s, host receipt interval |
| Reset-intent to B loader line receipt | 0.384 s, host receipt interval |
| B diagnostics independently received | By 6.605 s after host RESTART request |
| Final normal B self-test log | Device timestamp 3070 ms in B's second boot |

PENDING_VERIFY began in bootloader selection before A's application log, so
**1.840 s is an observed lower bound, not the full time spent in that state**.
The reset-intent log precedes bounded TX drain and a 100 ms delay; it is not
an instrumented timestamp of the reset instruction. Host receipt intervals
include USB buffering and 100 ms serial polling. The final RO1 query followed
a deliberate 300 ms settling interval after B's passed self-test log; 6.605 s
is an observed availability bound, not its exact first available instant.

### Evidence, stop state and remaining work

Generated evidence remains outside Git at
`/mnt/Dane/Development/roundie-backups/rollback-2c1-2026-10-18/`:
`authorization.md`, `execute_once.py`, `execution-start.json`,
`approved-result.json`, approved bounded readbacks/return log,
`execution-result.log`, `rollback-serial.log`, `capture-events.json`,
`before-write.json`, `ready.json`, `activation.json`, `restart-ack.json`,
`final-status.json`, `final-rtc.json`, `metrics.json` and `acceptance.json`.
Raw logs retain device identifiers locally. Acceptance was checked against
the actual serial messages and status/identity fields, not just the PASS file.

**Current state: A ABORTED, B VALID/running; only B is an eligible normal image.**
Two-copy redundancy has **not** been restored. The normal-image procedure above
requires separate explicit approval and must select, boot and confirm A before
claiming both slots VALID. No such update was performed. Manual rollback,
Safe Mode, Milestone 2C.2, power cuts, visual/touch testing and endurance remain
outside this result. No further hardware operation is needed for this goal.
