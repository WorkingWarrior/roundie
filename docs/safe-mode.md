# Milestone 2C.2 — Safe Mode

**Later 3A integration, 2026-10-19:** explicit Safe Mode inhibits native starts
and SDK calls, cancels development staging, and defers restart while a native
callback is still executing. No forced unload is provided. Physical refusal and
normal/STABLE restoration passed in 0.3.1; see [3A evidence](elf-loader.md).
The historical 2C.2 validation record below is preserved.

## Recovery services and OTA contract

Safe Mode is part of the installed REP application. It starts the existing
system USB worker, OTAManager diagnostics and optional existing LCD/LVGL GUI.
Its LCD-only BSP path avoids shared I2C, touch, RTC and gauge initialization;
sensor polling is suspended. It does not mount LittleFS or start applications,
packages, schedulers, MTP, Wi-Fi, automatic updates or repair/format actions.
No second USB reader, service framework or recovery partition is introduced.

The GUI owner displays ROUNDIE / SAFE MODE, reason, firmware version,
slot/state and a USB diagnostic hint. No touch navigation is needed. A GUI
setup failure keeps USB running. If a recovery GUI fails across a retained
reset, the next boot skips it. This is source behavior; visual appearance and
physical LCD failure injection require separate evidence.

The original first-boot core checks still require real GUI/DMA progress,
expected Flash/PSRAM and memory reserve. LittleFS, RTC and battery health
remain optional. A LittleFS-only failure may enter Safe Mode while a real
successful core self-test confirms the image. An inherited recovery latch
does not itself pass the new image's self-test. Missing/broken GUI cannot
confirm PENDING_VERIFY; existing IDF rollback behavior remains authoritative.
For already VALID images, failed GUI evidence enters recovery without an
INVALID/ABORTED write. Recovery operational/stable markers are separate from
OTA confirmation.

## Commands

`ROUNDIE_RECOVERY_USB` is required in REP and independent of development RTC
and OTA upload switches. It shares the existing RT1 printable-line framer
and driver; response writes use the driver's TX mutex and a 100 ms deadline.
The versioned RS1 interface is physically accessible and unauthenticated.
Physical USB access is the trust boundary. Production excludes fault injection
and may exclude RTC writes/uploads without losing recovery status/control.

```text
!RS1 <8-lowercase-hex-id> STATUS
!RS1 <id> REQUEST
!RS1 <id> CLEAR
!RS1 <id> REBOOT
!RS1 <id> SLOT STATUS
!RS1 <id> SELECT ota_0|ota_1 <exact-64-hex-target-ELF>
!RS1 <id> <decimal-esp_err> <bounded-JSON-status>
```

RS1 commands are shorter than 128 bytes. The shared framer discards overflow,
nonprintable or expired input through LF; RT1 keeps its original 63-byte
limit, RO1 its existing limit. Operations are exact and reject extra suffixes.
The response includes current Safe Mode/latch/reason, count, classified/raw
reset reason, stage/previous stage, retained-record validity, NVS availability, trial,
test remainder/build, firmware version/ELF, running/alternate OTA state,
alternate image/compatibility/ELF and internal free memory. `retained` means
BootGuard's RTC RAM record, not PCF85063 calendar validity. Existing RT1 GET
and RO1 STATUS remain separate calendar and full OTA/session diagnostics.

`reset` class values are 0 unknown, 1 power, 2 software, 3 sleep, 4 watchdog,
5 panic, 6 brownout and 7 external; `raw_reset` is the pinned IDF
`esp_reset_reason_t`. Stage values are 0 START, 1 CORE, 2 GUI,
3 OPERATIONAL and 4 STABLE. Malformed framing uses INVALID_ARG; denied
state/operation uses INVALID_STATE, persistence failure uses FAIL, and
ineligible slot selection uses OTA_VALIDATE_FAILED (numeric IDF error values
on the wire). No error invokes a destructive fallback.

REQUEST persists the latch and acknowledges before a planned software restart.
It is refused during PENDING_VERIFY or an OTA transaction. CLEAR explicitly
clears history/latch while retaining the active recovery session. REBOOT
grants one normal startup trial while keeping a durable latch until stable.
If that trial fails, the next boot returns to recovery. Unrelated restarts
do not consume a fresh trial or clear a latch. No periodic retry exists.
NVS failures are reported, without erase or successful-clear claims.

SELECT requires active Safe Mode, an idle manager, VALID running image and
the other slot's immediately revalidated compatible image, VALID metadata,
exact label and ELF identity. Unknown, NEW, PENDING_VERIFY, INVALID, ABORTED,
missing and incompatible targets are refused. It uses the standard validating
`esp_ota_set_boot_partition()` and never invalidates the running slot.
The pinned IDF rewrites the target as NEW; boot makes it PENDING_VERIFY and
its firmware must confirm again. A selection write failure blocks further
updates for that boot because metadata may be uncertain. No raw sectors or
arbitrary partitions are exposed. Anti-rollback/encryption remain disabled
by the existing build guards.

Selecting a previous firmware does not migrate its data. The operator must
confirm data compatibility and the exact ELF; no compatibility with arbitrary
future storage schemas is promised. CLEAR before SELECT if a normal boot of
the other firmware is intended; then REBOOT performs the selected boot.
Only explicit requests select/restart, never Safe Mode entry by itself.

```sh
python tools/roundie_safe.py --port /dev/serial/by-id/<actual-device> status
python tools/roundie_safe.py --port /dev/serial/by-id/<actual-device> request
python tools/roundie_safe.py --port /dev/serial/by-id/<actual-device> slots
python tools/roundie_safe.py --port /dev/serial/by-id/<actual-device> clear
python tools/roundie_safe.py --port /dev/serial/by-id/<actual-device> reboot
# Explicit alternate selection: first inspect slots and confirm data compatibility.
python tools/roundie_safe.py --port /dev/serial/by-id/<actual-device> select ota_0 --elf <exact-ELF>
```

The host reuses `roundie_time.py`'s bounded serial writes, addressed reads and
verified DTR/RTS idle configuration. Exit 0 means an accepted response, 2 a
transport/framing deadline failure, 3 rejected operation/device state. It
never blindly retries a mutation after a lost response. A reboot response is
acceptance, not proof of the next boot; reconnect and inspect fresh status.

## Bounded diagnostic and deployment plan

`ROUNDIE_BOOT_GUARD_TEST` defaults off. CMake requires an explicit version
ending in `-boot-test` if enabled. Such an image is unarmed on startup.
Development-only `FAULT <exact-running-ELF>` requires an idle, already VALID,
stable normal firmware and healthy persistence. It arms exactly three
incomplete-startup software resets. Each remaining reset is consumed in RTC
before restart; early boot counts those unplanned incomplete attempts.
Safe Mode cancels all remaining faults. There are no invalid accesses,
intentional hangs, power cuts, manual OTA metadata writes or automatic
invalidation of the current/fallback firmware. Normal firmware rejects FAULT.

Authorized plan: preserve original normal A's bytes throughout. First install
normal 0.2.2 into inactive B from A through RO1; observe PENDING_VERIFY→VALID,
normal services, explicit REQUEST/status and one normal trial. From recovery
explicitly select original verified VALID A, which must confirm again. Then
install the unarmed diagnostic into B, confirm it normally, and only then arm
one bounded three-reset experiment. Inspect stable recovery and return to
normal trial. Explicitly select A again; replace diagnostic B with the exact
frozen normal 0.2.2 through RO1, activate, confirm and inspect final status.
This uses three uploads and keeps A's original image available. It does not
assume selecting A restores B or that selecting a VALID slot leaves it VALID.

Before the first write: read-only live USB/chip/Flash/table/bootloader/OTA
metadata and both image identities, image integrity/configuration/size,
session/fallback eligibility, backup and ROM procedure checks. Freeze exact
image hashes, destination B and expected slot/metadata transitions outside
Git. Only B's application range (starting 0x390000, slot size 0x380000), IDF
otadata sectors 0xd000–0xefff and the small BootGuard NVS record may change.
Do not write bootloader/table/eFuses, erase user storage, perform whole-Flash
dumps or repeat fault injection after a failed test.

## Validation record

### Host, target and image gates — 2026-10-19

**Verified:** 50 host unittest cases passed under the exported pinned IDF
environment, including actual C logic with ASan/UBSan. This includes the
existing 112-case boot-gate matrix. The complete REP layout validator passed;
normal and diagnostic target builds passed without new warning suppression.
`esptool image-info` independently accepted each image checksum/hash.

| Frozen image | Bytes | Full file SHA-256 | ELF SHA-256 |
|---|---:|---|---|
| Normal 0.2.2, test disabled | 680384 | `9b0d05ecfe3353f504b364abc463d1fecf4835bf9b4bda9f03f6c427f824f00d` | `436db7c5a3ef9b39fcede486802b883b8a80c59dc48adb754b121688d2a15c8e` |
| Diagnostic 0.2.2-boot-test, initially unarmed | 680704 | `8ca17e11e44ea4d681a22627dff233117dd43bb2bfdb5b21364d27f531e16215` | `c80de259eff3da2f82f4ab03c0b29a2eda46a79987a34126773fc649b1193791` |
| Final normal 0.2.3, test disabled | 680400 | `5b3495d28a5adb028a116484d76de0a3b1b382d80c777a16cef9623b296f0937` | `9bedb988fe27a4655c26622fe23a048df24f8830b6728bb9fd22bc21f7b8acac` |

Source implementation commit: `30609ab`; late-runtime recovery correction:
`3e017a1`. Images/configs and raw evidence are
outside Git in
`/mnt/Dane/Development/roundie-backups/boot-guard-2c2-2026-10-19/`.
The generated build bootloader is not a deployment payload; the already
installed verified rollback-enabled bootloader is preserved.

### First physical OTA and explicit recovery — 2026-10-19

**Verified physical:** full read-only preflight passed immediately before the
first OTA. It checked USB/chip identity, 16 MiB Flash, exact REP table and
installed bootloader, CRC-valid VALID A/B metadata and expected 0.2.1 headers,
runtime verified fallback/idle state, available original backup and documented
ROM recovery. Reads were limited to the 64 KiB prefix and two 288-byte image
headers; no whole-Flash dump was performed. The exact destination, image
identity, metadata sectors and final-state plan were frozen before writing.

Genuine RO1 upload from A to inactive B, READY/digest/ELF check, activation
and acknowledged restart passed. B logged PENDING_VERIFY at 1150 ms and
VALID at 3290 ms after its actual core self-test. Transfer through activation
took 69.681 seconds host monotonic. A remained the original compatible VALID
fallback. Independent RS1 reported normal/STABLE, failures=0, latch=0,
test_build=0 and fault_remaining=0. NVS initialized ESP_OK; LittleFS mounted
with 9,240,576 total / 8,192 used bytes. RTC was valid/OS=0; gauge reads
remained available. These are service observations, not calibration evidence.

REQUEST entered Safe Mode with reason=request, retained=1, failures=0,
latch=1 and both slots still VALID. RS1 STATUS/SLOT STATUS worked. Recovery
startup had no LittleFS mount, I2C probe or touch-driver initialization;
LCD/core DMA checks passed. REBOOT granted one normal trial; after stability,
independent status had safe=0, retained=1, trial=0, failures=0 and latch=0.
This physically verifies same-image software-reset retention, explicit safe
entry and a successful bounded exit, not retention across power loss.

The first explicit guarded SELECT of original A also passed: the immediately
verified alternate was VALID, selection made it NEW while B stayed VALID,
and A's captured PENDING_VERIFY self-test confirmed it VALID again. A's full
ELF identity remained the original normal 0.2.1. No A application bytes were
written. This separately verifies manual selection and reconfirmation; it is
not the automatic-rollback experiment already documented for 2C.1.

### Controlled failure in confirmed firmware — 2026-10-19

**Verified physical:** after selecting/reconfirming original A, genuine RO1
installed the initially unarmed diagnostic into inactive B. Its ordinary
first boot passed the core self-test and confirmed PENDING_VERIFY→VALID;
independent status established normal/STABLE with no armed remainder before
the one FAULT request. A remained compatible VALID throughout the experiment.

FAULT was explicitly armed once against the exact running diagnostic ELF.
The command consumed the first of three resets; the next two early boots
consumed the remaining resets before optional initialization. Captured logs:

```text
BootGuard reset=2 retained=1 failures=1 safe=0 reason=none trial=0 fault_remaining=2
BOUNDED BOOT TEST: VALID incomplete startup; remaining=1; software restart
BootGuard reset=2 retained=1 failures=2 safe=0 reason=none trial=0 fault_remaining=1
BOUNDED BOOT TEST: VALID incomplete startup; remaining=0; software restart
BootGuard reset=2 retained=1 failures=3 safe=1 reason=boot_loop trial=0 fault_remaining=0
```

All three running-slot observations were VALID, with original A VALID as
the alternate. No automatic slot selection, INVALID or ABORTED transition
occurred. No LittleFS mount occurred in the incomplete-startup/recovery log.
The final recovery start completed its core checks. A bounded 20-second
capture and addressed status established stable Safe Mode, failures=3,
latch=2, retained=1, fault_remaining=0 and no further automatic reset. This
observation proves cessation of this bounded software-reset sequence, not
indefinite endurance or response to an actual panic/watchdog.

An explicit REBOOT normal trial returned to normal/STABLE, both images VALID,
failures=0, latch=0, trial=0 and fault_remaining=0. This is a BootGuard test
of confirmed firmware; it does not repeat PENDING_VERIFY rollback. The
diagnostic was then scheduled for replacement with the exact frozen normal
image through original A, with no second fault experiment.

### Final restoration and remaining evidence

The planned restoration of frozen normal 0.2.2 passed: original A was selected
and reconfirmed, RO1 replaced diagnostic B, and B confirmed PENDING_VERIFY to
VALID. Independent status established normal/STABLE, test_build=0,
fault_remaining=0 and a compatible VALID original A fallback.

**Concrete review finding and correction:** the initial policy ignored an
unplanned panic/watchdog/software reset after STABLE. A focused assertion
against the actual engine failed for a late watchdog reset. Firmware 0.2.3
now enters Safe Mode with reason=core on the next boot after such an
operational-session failure, without incrementing the startup counter or
invalidating the image. Added actual-engine cases cover late watchdog, panic,
unplanned software reset and a planned post-STABLE reset. The three-incomplete-
boot threshold and RTC record layout are unchanged. This correction requires
one further normal OTA into B through preserved A; it does not repeat fault
injection. The corrected engine passed the full 50-test host suite again,
including late-fault cases in normal/diagnostic ASan/UBSan compilations.
Pinned REP build, layout/size validation and image-info checks passed again.

**Verified physical final installation:** preserved A was explicitly selected
and reconfirmed, with normal 0.2.2 B still VALID. The exact frozen 0.2.3 image
was installed only into inactive B via RO1. READY matched its full digest and
ELF; activation selected NEW B. B logged PENDING_VERIFY at 1153 ms and VALID
at 3283 ms after the actual self-test. Upload through activation took
69.730 seconds host monotonic. NVS initialized ESP_OK, LittleFS mounted with
unchanged 9,240,576 total / 8,192 used, and RTC was valid/OS=0.
No second fault experiment occurred. Four OTA uploads in total were necessary:
the three planned images plus this concrete policy correction. A's original
application bytes were never overwritten; its standard IDF selection metadata
was explicitly rewritten/reconfirmed during the guarded switches.

**Verified physical final controls/state:** one explicit REQUEST and one
normal trial on 0.2.3 passed after the correction, with no fault command.
The recovery boot retained reason=request and skipped LittleFS/I2C. The
normal boot logged retained=1, trial=1, passed core checks and completed
stability. Fresh independent RO1/RS1/RT1 queries then established:

| Property | Final observed state |
|---|---|
| Running / selected | B (`ota_1`), normal 0.2.3, VALID |
| Running ELF | `9bedb988fe27a4655c26622fe23a048df24f8830b6728bb9fd22bc21f7b8acac` |
| Fallback | A (`ota_0`), original normal 0.2.1, compatible/verified VALID |
| Fallback ELF | `1e55c7a475ee573d9f38139f14e5498a631df2eabfd959e00ef8cdcdcfc92a02` |
| OTA manager | IDLE, checked=1, passed=1, last_error=0, rollback=1 |
| BootGuard | STABLE, safe=0, reason=none, failures=0, latch=0, trial=0 |
| Injection | test_build=0, fault_remaining=0; diagnostic image replaced |
| Persistent-state backend | persistence=1; no erase/format fallback |
| LittleFS / NVS | mounted with unchanged capacity/usage; initialization ESP_OK |
| RTC / USB | fresh GET OK, valid/OS=0, error=0; addressed diagnostics responsive |

No more hardware work is needed for this bounded goal. A's older firmware
does not contain BootGuard; it is the preserved known-good recovery/update
fallback. Active B contains the new Safe Mode. Mount/capacity continuity is
not a bytewise audit of every user file or NVS entry.

Evidence includes `selected-plan.json`, bounded preflight files,
`normal-install-*`, `explicit-safe-*`, `normal-trial-*`, guarded selection
records under `before-diagnostic-*` / `before-restoration-*`,
`diagnostic-install-*`, `fault-before.json`, `fault-ack.json`,
`fault-boots-and-safe.log`, `fault-safe-status.json`, `fault-safe-ota.json`,
`fault-recovered-normal.json`, `normal-restoration-*`, `before-core-policy-fix-*`,
`core-policy-install-*`, `final-safe-boot.log`, `final-normal-boot.log`,
`final-device-state.json` and `acceptance.json`. `validate_once.py` and
`final_acceptance.py` preserve the exact bounded host procedures. Generated
images, device identifiers, backups and raw logs remain outside Git. Actual
transition logs and final fields were inspected separately from PASS markers.

Real WDT/panic, power interruption/brownout, NVS persistence across physical
power loss, deep sleep, actual LCD disconnect/GUI failure injection and human
visual/touch evidence remain **PENDING**. The GUI/DMA checks establish live
initialization and transfer progress, not human inspection of the displayed
screen. Host NVS/GUI/storage failure tests do not establish physical fault
behavior. No endurance campaign was run. The preceding long-uptime
RTC/system-clock discrepancy is unchanged; host-UTC accuracy was not a gate.
