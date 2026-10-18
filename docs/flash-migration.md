# Manufacturer layout to REP v1 — migration and provisioning

**Current status (Milestone 2B, 2026-10-18):** the authorized physical migration
has been programmed and verified. REP boots from OTA A; LittleFS mounts and
the bounded filesystem test passes. The execution record below supersedes
the historical Milestone 2A proposal and its no-device-access statements.

## Milestone 2B physical execution — 2026-10-18

**Authorization:** the goal explicitly authorized replacing the Waveshare
layout, affected data loss, matched bootloader/table/application/OTA writes,
LittleFS provisioning, verification and short device tests. No eFuse,
security enablement or battery/gauge configuration operation was performed.
The layout in **Selected REP v1 layout** below was installed unchanged.

**Verified preflight:** the original backup at
`/mnt/Dane/Development/roundie-backups/preflash-2026-10-17-pMVcHN/flash-16MiB.bin`
was rehashed: 16,777,216 bytes, SHA-256
`46d2d36244a9e303cc61a65fb29d9a71b9708d41d181e9e5b5befb9261039de7`.
esptool 5.6.0 recognized its ESP32-S3 bootloader (original IDF v5.5.3), valid
checksum and validation hash. Its table matches all six documented
Waveshare entries. The backup was not modified.

The live ESP32-S3 rev. 0.2 was matched to the previously recorded ROM MAC
and USB serial identity through the expected `303a:1001` Serial/JTAG
interface. Direct ROM identification, JEDEC identification (16 MiB), and
security status passed. Secure Boot and Flash Encryption were disabled.
A bounded `0..0x20000` read matched the exact original partition-table
sector, erased OTA data and previously verified installed v6.1 bring-up
bootloader. No full-Flash readback or broad hardware audit was repeated.
The frozen prefix was checked again on the same programming connection
before any Flash write. Local device identities are kept in untracked
evidence, not added to this record.

**Build/toolchain facts:** pinned ESP-IDF v6.1 commit
`fff9895c82d744c7237be8847347bdd1b07c6643`, Xtensa
`esp-15.2.0_20251204`, esptool 5.6.0, joltwallet/littlefs 1.22.2 with the
existing capacity-error patch. REP target build and artifact validator
passed. The 644,256-byte `roundie_rep` 0.1.0 image fits either 3,670,016-byte
slot. Firmware source is commit `220caa3`; subsequent tool/documentation
changes do not alter the installed binary.

| Artifact | SHA-256 |
|---|---|
| Bootloader, 21,200 bytes | `f7c33fe831a564d16b8b00406c20ac2e79b8b7146f75e862263764656f349188` |
| Generated table, 3,072 bytes | `a273fbb4600b658c91775e2c7e85bcf4f49339afbd57f848820515912f1563b1` |
| Initial OTA data, 8,192 erased bytes | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| Application `.bin` | `8ed7ecd43afaf2ac31c70c29522c232d3ee68c7035fcbe2428617e1dea32d155` |
| Application ELF / embedded ELF hash | `f929fe67dda2fcc3aab482d9fe0e0c6ed3f5d01e3202f39645ac4f412f95a5cb` |
| Provisioned LittleFS, 9,240,576 bytes | `697b97b62c23ddfb8fbec589ef151776acdc7865e182daf8a88026503d34994f` |

**Provisioning decision:** use the component's pinned host tool
`littlefs-python==0.15.0` in a dedicated build-directory venv. The
[image generator](../tools/provision_littlefs.py) derives partition size
and parameters from the validated REP build. Explicit host formatting uses
4,096-byte blocks × 2,256, read/program granularity 128, cache 512,
lookahead 128, block cycles 512, name maximum 64; target page size is 256.
An independent no-format-fallback host mount verified on-disk version 2.1
and the sole diagnostic request file. A second generation using identical
inputs produced a byte-identical image. Normal target startup still has no
format or erase fallback.

**Programming and erasure:** a local, goal-specific installer froze all
artifact bytes and derived offsets from the validated table/configuration.
It programmed these sector-aligned envelopes in one ROM/stub connection,
without an intervening reset:

| Range (half-open) | Programmed contents |
|---|---|
| `0x000000..0x008000` | REP bootloader, reservation padded with `0xff` |
| `0x008000..0x009000` | REP table, sector padded with `0xff` |
| `0x00d000..0x00f000` | Two erased OTA metadata sectors |
| `0x010000..0x390000` | REP application in OTA A, entire slot tail erased |
| `0x390000..0x710000` | Entire OTA B erased |
| `0x710000..0xfe0000` | Valid provisioned LittleFS image |
| `0xfe0000..0x1000000` | Coredump reservation erased |

The unchanged NVS `0x9000..0xd000` and PHY `0xf000..0x10000` regions were
preserved and compared byte-for-byte before normal boot. Old application,
assets and unknown tail data in the listed envelopes were intentionally
overwritten. Clearing both app slots prevents legacy executable remnants
being selected; clearing coredump prevents stale tail data looking like a
new dump. This establishes deterministic contents without a whole-chip
erase. Every envelope passed write-time digest verification and a separate
esptool verification; a bounded prefix read confirmed the exact REP table
and preservation checks. All commands exited 0. No verification failure
or offset adjustment occurred.

**Initial selection:** source inspection of the pinned bootloader verified
that erased OTA metadata with no factory app selects OTA 0 and writes
sequence 1 with `ESP_OTA_IMG_VALID` after app validation. The actual startup
log reports `active=ota_0`, subtype `0x10`, offset `0x10000`, size `0x380000`
and exact `rep-v1`. OTA B has no previous firmware. This initial USB install
does not test application self-confirmation or A/B rollback.

**Reset/boot evidence:** automatic esptool entry into ROM succeeded without
buttons. A subsequent RTS reset remained in ROM: no application output,
no RT1 response, OTA metadata still erased. No reflash was attempted.
The operator disconnected USB and reconnected without BOOT, leaving the
battery connected; normal startup then succeeded. USB capture began at
about 752 ms, in application/PSRAM startup. Early bootloader console lines
were not captured on the secondary USB console. Bootloader integrity is
direct Flash verification; its successful handoff is established by the
matching running application and exact runtime partition diagnostics. The
subsequent software-restart capture includes the complete early console:
`ESP-IDF v6.1 2nd stage bootloader`, all seven exact REP table entries,
image segment validation/loading and `Loaded app from partition at offset
0x10000`. Thus early bootloader startup was also directly observed, on the
second normal boot.

**Actual device results:** NVS initialized `ESP_OK` with no erase fallback.
LittleFS mounted `ESP_OK`: total 9,240,576, used 8,192, free 9,232,384 bytes.
The request-gated 256-byte test passed create/write/fsync/close, unmount,
remount, exact-length read/content comparison, deletion and another remount
confirming deletion. Final capacity remained total 9,240,576 / used 8,192.
The request is claimed before writing, so failures/interruption leave
evidence and do not automatically retry. Successful completion removes all
three dedicated diagnostic files. No production package or ELF was created.

RTC startup was valid, OS=0, `ESP_OK`; RT1 GET returned equal RTC/system UTC
without an RTC SET. BQ27220 was accessible, battery present, approximately
3,973 mV, reported SoC 84% (accuracy remains unverified). LCD ID selected
ST77916 variant 2 and initialization succeeded at 3 MHz QSPI. The initial
CST816S address probe returned `ESP_ERR_NOT_FOUND`, followed by successful
driver initialization/IC ID 181, matching the existing diagnostic behavior;
no touch interaction is claimed.

**Bounded runtime validation:** a 185-second serial capture contained one
application startup, a successful live RT1 GET, and no panic, watchdog
failure, LCD transfer failure or unexpected reboot. A separate explicit
RT1 software restart then passed in a roughly 12-second focused check:
bootloader/OTA A startup, NVS initialization, LittleFS remount with unchanged
8,192-byte usage, RTC OS=0, battery and LCD initialization. No second
`M2B filesystem` test ran. RTC advanced 12 seconds over 12.09 seconds; system
UTC was restored from RTC without writing RTC hardware.

**Separate failed time-reference check:** the existing RTC verification
helper rejected host UTC agreement at -3.63 seconds (its unchanged limit is
3 seconds), before requesting a restart. This was preserved in
`restart-host-reference-failure.log`. The dedicated retention test checks
RTC validity/OS, RTC-to-system agreement and progression across restart;
it does not claim host synchronization passed. Independent later status
was approximately -3.77 seconds relative to the host. No tolerance was
changed and no RTC resynchronization was performed. Without a pre-migration
host-offset measurement, the source of this offset is unestablished.

**Offline validation:** 32 Python tests passed, including actual
StorageManager/mock-backend and pinned capacity-helper ASan/UBSan tests,
partition/artifact fault injection, legacy rejection and REP state guards.
The CMake-generated dependencies were inspected to confirm all five raw
flashing targets depend on the fail target. The new REP updater's full
physical app-write path was not exercised: migration used the frozen
goal-specific image set, and no redundant application flash was performed.

**Remaining limits:** visual LCD/touch interaction is pending, while LCD
initialization and short GUI watchdog operation are verified by logs.
Host UTC agreement failed as recorded above. Battery calibration/SoC
accuracy, long runs, power interruption, corruption/fault injection on
Flash, cache/PSRAM concurrency, OTA self-test/confirmation/rollback and
Safe Mode remain unverified or future work. No 30-minute test, networking,
package/runtime work or Milestone 2C implementation was started.

**Evidence archive (outside Git):**
`/mnt/Dane/Development/roundie-backups/rep-migration-2026-10-18/` contains
the frozen images and their exact padded-envelope hashes in
`image-manifest.json`, `migrate.py`, build/validator/provisioning/test logs,
pre/post-programming prefix reads and serial captures. These generated
files and device identifiers are not committed.

## REP development updates

For 2C.1 the installed 2B bootloader already supports rollback. Rebuilding it
changes timestamp metadata, so the optional
`--installed-m2b-bootloader /path/to/exact-21200-byte-readback.bin` selects the
**pinned known 2B hash** for live comparison instead of the newly built artifact.
It never writes the bootloader. Unknown bytes/hashes fail before serial access.
All original initial-VALID-A, table, device and security checks remain. This is
only for the separately authorized app-only bootstrap of the first OTA-capable
firmware; see [the exact sequence and evidence boundary](ota.md).
Subsequent A/B updates must use `tools/roundie_ota.py`; direct flashing is not
evidence of OTA, and this bootstrap guard rejects noninitial metadata.

Only use this procedure when the current task separately authorizes a
hardware write. It updates the **currently selected initial OTA A image in
place**, with no power-loss safety or previous-image fallback. It does not
implement an OTA updater or switch to OTA B.

```sh
cd /mnt/Dane/Development/roundie
. /home/solve/.espressif/v6.1/esp-idf/export.sh
idf.py -B build-rep -DROUNDIE_PROFILE=rep build
python tools/flash_rep.py --port /dev/ttyACM0
# Once the current task authorizes the application-only write:
python tools/flash_rep.py --port /dev/ttyACM0 --write
```

Preflight connects through ROM and may reset the running application even
without `--write`. It requires the REP build/config/image identity, exact
live REP table, matching bootloader, matching USB serial/ROM MAC, ESP32-S3,
16 MiB Flash, disabled security features and initial sequence-1 VALID OTA A
metadata. Any later, pending, alternative or unknown OTA state fails closed
and requires a reviewed procedure. Frozen app bytes are revalidated, the
destination derives from the verified table, and written app data is hash
verified. Table/OTA state are read again after a write. It never writes the
bootloader, table, NVS, filesystem or OTA metadata.

If the final RTS reset remains in ROM on this unit, disconnect and reconnect
USB **without BOOT**, leaving the battery connected, as in the tested normal
return above. Do not repeat a verified write to solve a reset issue.
If USB removal leaves the board powered, stop and reassess the latch state.

The legacy updater checks exact equality against the original Waveshare
table before writing anything, so a REP device is rejected even when given
a valid legacy app. Offline tests exercise this rejection and changed REP
table, bootloader, OTA sequence/state/CRC/second-record failures. CMake also
blocks raw `flash`, `app-flash`, `bootloader-flash`, `partition-table-flash`
and `otadata-flash` targets for both profiles. Reconfigure historical build
trees to install the block; never bypass the guard with printed esptool
commands. This cannot prevent an explicitly invoked low-level programmer.

For an unbootable firmware use the already physically verified
[ROM Download recovery procedure](rom-recovery.md): disconnect USB, hold
BOOT while reconnecting, wait about two seconds and release BOOT, then
rediscover/identify the port. If disconnecting USB does not stop the board,
follow the documented latch/PWR checks instead of assuming a reset.
ROM recovery does not depend on LittleFS or a working application.
Restoring the original 16 MiB backup would destroy current REP data and
requires separate authorization; ordinary development updates never restore
it. Safe Mode and tested OTA rollback remain future milestone work.

## Authorization and evidence boundary

**Historical Milestone 2A scope, before migration.** Milestone 2A authorized implementation and offline tests
only. No physical Flash, partition table, bootloader, OTA metadata, NVS,
application, filesystem, backup, or eFuse was modified. No serial connection
or full-Flash readback was made. Each future migration requires separate
explicit authorization covering the affected data and destructive operations.
This document does not authorize running IDF's generated flash commands.

**Verified offline facts (2026-10-18):** the preserved original image is
16,777,216 bytes at
`/mnt/Dane/Development/roundie-backups/preflash-2026-10-17-pMVcHN/flash-16MiB.bin`,
SHA-256 `46d2d36244a9e303cc61a65fb29d9a71b9708d41d181e9e5b5befb9261039de7`.
Its partition table and erased OTA sectors were parsed from that local file
using the selected IDF generator. It is an October 17 manufacturer snapshot,
not a current user-data export. The installed Roundie bootloader/app differ
from the original snapshot. Installed layout/erased OTA selection were
established in the earlier bring-up record; they were not re-read here.

**Architectural decision (REP §§15, 18, 20, 23):** standard bootloader, two
OTA apps, no factory or recovery firmware. Firmware Safe Mode and OTA
confirmation are future work. A hardware recovery route was physically
validated separately in [rom-recovery.md](rom-recovery.md); that fact does
not validate this proposed migration.

## Original manufacturer partition table

Ranges in this document are half-open: offset through end, excluding end.
All addresses are derived from the backup/table, not guessed.

| Label | Type/subtype | Offset | Size | End | Purpose |
|---|---|---:|---:|---:|---|
| nvs | data/nvs (`01/02`) | `0x009000` | `0x004000` (16 KiB) | `0x00d000` | Manufacturer system data |
| otadata | data/ota (`01/00`) | `0x00d000` | `0x002000` (8 KiB) | `0x00f000` | Both records erased in saved evidence |
| phy_init | data/phy (`01/01`) | `0x00f000` | `0x001000` (4 KiB) | `0x010000` | PHY reservation |
| factory | app/factory (`00/00`) | `0x020000` | `0x692000` (6728 KiB) | `0x6b2000` | Currently used bring-up app |
| ota_0 | app/ota_0 (`00/10`) | `0x6c0000` | `0x400000` (4 MiB) | `0xac0000` | Manufacturer alternate app |
| assets | data/spiffs (`01/82`) | `0xac0000` | `0x400000` (4 MiB) | `0xec0000` | Manufacturer assets; subtype is not proof of contents |

Outside the table entries: bootloader at `0`, table at `0x8000`, gaps at
`0x10000–0x20000`, `0x6b2000–0x6c0000`, and tail `0xec0000–0x1000000`.
An unassigned region must not be assumed erased or free of valuable bytes.
The **legacy build table** is different: NVS `0x9000/0x6000`, PHY
`0xf000/0x1000`, factory `0x10000/1 MiB`, with no OTA metadata. Its only use
is compiling the diagnostic image for guarded factory-app relocation;
flashing that generated table onto this device remains prohibited.

## Selected REP v1 layout

Source: [rep.csv](../partitions/rep.csv), generated/validated with ESP-IDF
v6.1 commit `fff9895c82d744c7237be8847347bdd1b07c6643`.

| Region/label | Type/subtype | Offset | Size | End | Purpose / alignment |
|---|---|---:|---:|---:|---|
| Bootloader reservation | outside CSV | `0x000000` | `0x008000` | `0x008000` | Standard bootloader; must fit before table |
| Partition-table sector | outside CSV | `0x008000` | `0x001000` | `0x009000` | Generated table + sector padding, 4 KiB |
| nvs | data/nvs (`01/02`) | `0x009000` | `0x004000` | `0x00d000` | System NVS, 4 KiB |
| otadata | data/ota (`01/00`) | `0x00d000` | `0x002000` | `0x00f000` | Two redundant metadata sectors, 4 KiB |
| phy_init | data/phy (`01/01`) | `0x00f000` | `0x001000` | `0x010000` | Reserved, currently unused, 4 KiB |
| ota_0 (A) | app/ota_0 (`00/10`) | `0x010000` | `0x380000` (3.5 MiB) | `0x390000` | First REP application, 64 KiB |
| ota_1 (B) | app/ota_1 (`00/11`) | `0x390000` | `0x380000` (3.5 MiB) | `0x710000` | Alternate application, 64 KiB |
| storage | data/littlefs (`01/83`) | `0x710000` | `0x8d0000` (8.8125 MiB) | `0xfe0000` | Applications/resources, 4 KiB |
| coredump | data/coredump (`01/03`) | `0xfe0000` | `0x020000` (128 KiB) | `0x1000000` | Provisional reservation, 4 KiB; capture disabled |

CSV partitions occupy `0xff7000` = 16,740,352 bytes. Bootloader/table
reservations occupy `0x9000` = 36,864 bytes. Total allocated address space:
`0x1000000` = 16 MiB; no partition overlap, alignment gaps, or unused tail.
Unused bytes inside the bootloader reservation and app slots remain their
own growth margins, not extra partitions. Final coredump sizing still needs
task/capture measurements; no claim of complete PSRAM crash capture is made.

The current REP application fits either slot with more than 80% headroom;
the exact build-size record is below. The 64 KiB move of the first app is
intentional. The old factory-app updater cannot install a REP image safely.

## Overwrite and preservation analysis

| Target region | Original regions intersected | Consequence |
|---|---|---|
| Bootloader and table | Existing bootloader/table | Must be replaced as one reviewed layout installation |
| NVS / OTA / PHY | Same physical ranges | Addresses match; ownership/format compatibility still needs review |
| OTA A `0x10000–0x390000` | Initial gap, old factory beginning | Destroys the installed factory app |
| OTA B `0x390000–0x710000` | Old factory tail, gap, old OTA beginning | Old app bytes cannot remain usable rollback images |
| LittleFS `0x710000–0xfe0000` | Old OTA tail, all assets, part of old unassigned tail | Cannot reinterpret SPIFFS or raw resources as LittleFS |
| Coredump `0xfe0000–0x1000000` | Old unassigned tail | Tail contents need assessment if preservation matters |

These are **target ownership/erase ranges**, not a claim that flashing a
small app automatically erases its entire slot. A short app write alone
would leave stale manufacturer bytes in other sectors. The fresh route
below removes ambiguity by explicitly requiring approval to erase all
16 MiB; the data-preserving route needs a separate selective erase plan.

Neither manufacturer apps nor assets can be preserved in place. Preserving
assets requires export, content/format validation, conversion/import to the
new filesystem, and verification. NVS occupies the same bytes, but using
foreign namespaces or an incompatible NVS format is not accepted merely
because offsets match. This foundation firmware does not initialize NVS.
The original backup cannot recover later user changes or newly set RTC
time; RTC is a separate battery-backed device.

## Historical fresh REP installation proposal (Milestone 2A)

**Precondition:** the user explicitly accepts replacing the layout and all
Flash contents, including NVS, assets, alternate apps, and any unknown tail
data. Authorization must cover the destructive erase separately from ordinary
app flashing. This is a proposed procedure, not a runnable migration script.

1. Freeze the source revision, IDF/toolchain, lockfile, build profile and
   artifacts. Build `rep` and run the offline validator. Record all image
   sizes/SHA-256 values and archive the artifacts outside Git. Check that
   the full bootloader erase envelope ends before `0x8000` and the app fits
   **both** slots. Review the generated flash manifest against this plan.
2. Verify the immutable original backup hash and readable recovery tools.
   Under future authorization, identify the actual board, Flash size,
   current layout, security state, and serial route using bounded checks.
   Any mismatch stops installation. Export any wanted current data before
   granting destructive-install approval. Maintain stable power throughout.
3. Enter the already tested ROM Download route. After the separately
   authorized full 16 MiB erase, install this **matched set** from one REP
   build, without resetting into a partial installation:

   | Address | Artifact | Rule |
   |---:|---|---|
   | `0x000000` | `build-rep/bootloader/bootloader.bin` | Standard v6.1 REP bootloader with software rollback |
   | `0x008000` | `build-rep/partition_table/partition-table.bin` | Exact REP v1 table; erase/pad the full table sector |
   | `0x00d000` | `build-rep/ota_data_initial.bin` | Exactly `0x2000` bytes of `0xff`; no stale OTA record |
   | `0x010000` | `build-rep/roundie_rep.bin` | First application in OTA A |

   The raw flash tool's image-hash checks must succeed. NVS, PHY reservation,
   OTA B, LittleFS, and coredump stay erased. Do not copy the manufacturer's
   OTA metadata or bootloader, and do not put a factory image at `0x20000`.
   IDF's ordinary flash command alone does not implement this approved erase
   plan, data export, identity checks, or recovery workflow.
4. Initialize first-slot selection with the two erased OTA sectors. In the
   pinned `bootloader_utility.c`, absent factory + initial metadata selects
   OTA 0. Once the bootloader verifies that app, it writes sequence 1 and
   `ESP_OTA_IMG_VALID`. This is IDF's initial USB-install behavior, **not** an
   application self-test or tested OTA rollback. Keep OTA B erased so no
   legacy content can be selected as an alternate valid image. If a future
   procedure sets `NEW` instead, the application must first implement its
   critical self-test and explicit confirmation; this image does not.
5. Reset normally and verify boot logs: correct firmware/profile/hash,
   active `ota_0` at `0x10000`, exact `rep-v1`, partition sizes, and no reset
   loop. Blank LittleFS must report `MOUNT_FAILED`, capacity unavailable,
   with diagnostics still reaching LCD/RTC/battery/touch operation. It must
   not silently format or erase NVS. Check boot again after an authorized
   restart; use only bounded table/metadata checks if needed, not an
   unrequested full-Flash readback.
6. LittleFS first-use provisioning remains a **separate explicit operation**.
   No filesystem image or formatting command is delivered in this milestone.
   Review a compatible image/tool and capacity/format parameters before
   authorizing that step. An existing compatible filesystem should mount
   with plausible total/used values; blank storage staying unavailable is
   expected for the foundation installation.

**Power interruption:** until the matched installation completes, assume
normal boot may fail. Re-enter ROM and repeat the reviewed matched set after
checking the interrupted state and authorization. There is no atomic layout
migration, secondary bootloader, or dedicated recovery firmware. Do not try
to repair the migration with the legacy app-only updater.

## Data-preserving migration — decision gate first

Preservation is **not established** and no executable preservation workflow
is supplied. Before any erase/write, determine data value, formats, ownership,
and exact occupied ranges. Obtain future authorization for exports and a
selective migration plan; do not perform another full-Flash readback by
default. Existing original backup and later saved readbacks are historical
evidence, not proof of current contents.

Export current NVS entries with types/namespaces through a reviewed compatible
reader, or preserve its exact raw region offline pending format analysis.
Identify whether assets actually contain SPIFFS, another filesystem, or raw
binary resources; use read-only extraction and validate exported content
with hashes. Inventory unknown gap/tail data too. Do not mount foreign
storage writable merely to inspect it. Test any conversion and LittleFS
image generation on copies. Define the REP namespace/schema and import
only compatible reviewed data; never auto-erase NVS on a version/full error.

Produce and review an exact range/image manifest, including which old
regions must be exported before they are overwritten, which system bytes
are kept, which sectors are erased, and how output/import hashes are checked.
Only then may the user authorize those concrete operations. If export or
format conversion is unavailable, stop before writing and report preservation
as blocked; offer fresh installation only after explicit loss acceptance.
Reusing NVS's same address or restoring raw SPIFFS into LittleFS is not a
preservation strategy. A preservation route must still install matching
bootloader/table/app and deterministically reinitialize OTA selection.

## ROM recovery and backup use

Follow the physically tested [BOOT/PWR procedure](rom-recovery.md), with
the actual stable serial path identified afresh. Briefly: starting with USB
connected, close serial users, hold PWR approximately two seconds and release
(the display remained running), then disconnect USB without touching the
battery connector (the display went dark). Hold BOOT, reconnect USB, wait
approximately two seconds and release BOOT; do not press PWR during this
step. Confirm ROM communication directly. The
same USB VID/PID in normal mode does not prove ROM entry; the documented
esptool identification does. No new eFuse/security/USB-recovery change is
required or authorized.

Preferred failed-REP recovery: re-enter ROM and reinstall the reviewed REP
matched set. For returning to the old system, restoring the preserved
original 16 MiB image at address 0 would restore manufacturer bootloader,
table/apps/data together **and erase all later Flash changes**. It requires
separate explicit full-restore/data-loss authorization, matching device and
security checks, backup size/hash verification, write verification and a
normal-boot check. Never restore only its table over a REP image or only an
old app under the REP table. Preserve the original file unchanged and do
not mistake it for the latest Roundie app or a current user-data backup.

**Successful layout migration** means the authorized erase/write plan and
artifact checks completed, exact REP table and initial OTA selection are
verified, OTA A boots normally across a restart, and basic diagnostics work
even with blank storage. **Successful storage provisioning** separately
requires a compatible LittleFS mount with valid capacity and preserved/
imported data checks where promised. Neither success proves future OTA
rollback, Safe Mode, power-loss recovery, or Flash/PSRAM concurrency.

## Milestone 2A validation record and pending gates

Final verification on 2026-10-18 used the pinned IDF v6.1 checkout and
`xtensa-esp-elf esp-15.2.0_20251204`. All commands below exited **0**:

| Evidence tier | Command/check | Result |
|---|---|---|
| Target compilation | `idf.py -B build-rep -DROUNDIE_PROFILE=rep fullclean build` | Clean REP build passed with the pinned port correction |
| Target compilation | `idf.py -B build-rep -DROUNDIE_PROFILE=rep build` | Final incremental build passed after capacity-invalidation adjustment |
| Target compilation | `idf.py -B build -DROUNDIE_PROFILE=legacy build` | Existing legacy tree rebuilt successfully; no duplicate clean legacy tree |
| Host/artifacts | `python tools/validate_storage_layout.py --legacy-build build --backup /mnt/Dane/Development/roundie-backups/preflash-2026-10-17-pMVcHN/flash-16MiB.bin` | CSV/generated table equality, alignment/overlap/16 MiB bounds, both slot sizes, config/manifest/erased metadata, legacy identity, original backup hash/table and overlap analysis passed |
| Host/mock | `python -m unittest discover -s tests -p 'test_*.py' -v` | 28 tests passed: 13 storage/layout/deployment/port-correction tests + 15 existing RTC CLI tests; RTC restart output in this suite is mocked |
| Host C | `cc -std=c11 -Wall -Wextra -Werror -Icomponents/roundie_board -Icomponents/roundie_board/include tests/test_sensor_codec.c components/roundie_board/sensor_codec.c -o /tmp/roundie-sensor-codec-test` then execute | Sensor codec tests passed |
| Host C | `cc -std=c11 -Wall -Wextra -Werror -Imain -Icomponents/roundie_board -Icomponents/roundie_board/include tests/test_time_protocol.c main/time_protocol.c components/roundie_board/sensor_codec.c -o /tmp/roundie-time-protocol-test` then execute | Protocol tests passed |
| Host/saved evidence | Existing `flash_bringup.preflight()` with original backup, each saved `postflash-16MiB.bin` / `postflash-final-16MiB.bin`, and current legacy artifacts | Accepted both saved snapshots; current legacy bootloader code matches the known installed bootloader. This does not verify today's live device |
| Host/review | `git diff --check` | Passed |

The storage C harness and the exact patched LittleFS capacity helper were
compiled with `-Wall -Wextra -Werror -fsanitize=address,undefined`. Tests cover
allocation/missing partition, generic/unsupported/absent mount failure,
duplicate ownership, capacity I/O failure and invalidation, saturation,
unmount failure with retained ownership, mutex contention, and cleanup.
Artifact mutations reject overlap, bounds/alignment, subtype/flags, wrong
profile, renamed REP images, oversized apps, non-erased metadata, unsafe
config, unexpected manifest/bootloader size, and invalid backup. Mocked
updater tests verify rejection before serial access, read-only default,
and writing only the checked snapshot at `0x20000` under simulated `--write`.

During development, the first validator used a nonexistent IDF `flags`
attribute; it was corrected to `encrypted`/`readonly`, and all negative
checks remain enabled. The first port-copy build missed the component-scoped
`LFS_CONFIG` source property; that propagation was corrected and the final
clean build passed. A first protocol-test link omitted `sensor_codec.c`;
the complete command above passed. These were host/build failures, not
hardware observations. No warning policies or acceptance bounds were relaxed.

Final REP application: `0x9a340` = **631,616 bytes**. Both `0x380000` slots
leave **3,038,400 bytes (82.8%)** before sector rounding; the erase envelope
is `0x9b000`, leaving `0x2e5000` in each slot. REP bootloader is `0x52d0`
= 21,200 bytes, fitting its `0x8000` reservation; legacy bootloader remains
`0x5280` = 21,120 bytes. The final legacy app also fits its generated 1 MiB
slot and the actual manufacturer factory slot.

| Final REP artifact | SHA-256 |
|---|---|
| `bootloader/bootloader.bin` | `f7c33fe831a564d16b8b00406c20ac2e79b8b7146f75e862263764656f349188` |
| `partition_table/partition-table.bin` (3072 bytes, one reserved 4096-byte sector) | `a273fbb4600b658c91775e2c7e85bcf4f49339afbd57f848820515912f1563b1` |
| `ota_data_initial.bin` (8192 erased bytes) | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| `roundie_rep.bin` | `658f9edd505366fd0dfc39c8967742596201111e0f760ff3679c4c7d8e84e3b9` |

These hashes identify this local verification build, not a permanently
reproducible timestamp-free image or an installed image. Future migration
must regenerate/review its own artifact manifest after any rebuild.

All results above are host or target-compilation evidence; no device result
is claimed. Remaining physical gates include the migration itself,
compatible LittleFS provisioning/mount, non-destructive blank/corrupt/I/O
failure behavior on real Flash, storage cleanup on hardware, GUI/cache/PSRAM
latency, and future OTA confirmation/rollback. Existing noncritical
Milestone 1 checklist entries remain unchanged.

Primary pinned implementation references:
[IDF partition generator](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/partition_table/gen_esp32part.py),
[initial OTA selection/valid record](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/bootloader_support/src/bootloader_utility.c),
[storage API, profiles, tests and limitations](storage.md).
