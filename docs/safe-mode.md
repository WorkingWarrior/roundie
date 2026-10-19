# Milestone 2C.2 — Safe Mode

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
reset reason, stage/previous stage, RTC validity, NVS availability, trial,
test remainder/build, firmware version/ELF, running/alternate OTA state,
alternate image/compatibility/ELF and internal free memory. `retained` means
BootGuard's RTC RAM record, not PCF85063 calendar validity. Existing RT1 GET
and RO1 STATUS remain separate calendar and full OTA/session diagnostics.

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

Implementation and host tests are separate from physical proof.
Target build, physical OTA/confirmation, explicit Safe Mode entry/exit,
same-image RTC warm-reset retention, bounded fault test and final normal
state: **PENDING** until execution is recorded below. Real WDT/panic,
power interruption/brownout, LCD disconnect/failure injection and human
visual/touch evidence remain PENDING; no endurance campaign is authorized.
