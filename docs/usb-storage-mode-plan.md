# USB storage mode — integration plan

**Status: D0 approved by the user on 2026-11-04** (spec §16 and §23 stage 7
amended). The user did not answer D1–D4 separately, so the recommendations in
section 5 are in force as defaults and can be changed. Gates G1 onward are
implemented one at a time; the text below was written as a proposal and is kept
as written.

**Plan only until a gate says otherwise.** No firmware, partition, ABI or protocol changes are made
by this page. It builds on the [MSC probe results](usb-msc.md), the
[USB architecture gate](usb-architecture.md) and the [PackageManager](package-manager.md).
Labels follow AGENTS.md: **fact** (source or device evidence), **proposal**
(an architectural decision that needs the user's approval), **assumption**,
**experiment** (question, method, acceptance).

Replacing MTP by MSC changes REP §16 and §23 stage 7. That is an explicit
architectural decision, so it is listed as decision D0 below and takes effect
only when approved and recorded in the specification.

## 1. Goal and non-goals

Goal: install `.rpk` packages by copying them to a drive that the watch shows
over USB, with the host never touching LittleFS or system data, and every
package going through PackageManager.

Not in scope: always-on USB-OTG, migrating RP1/RO1/RS1/RT1/RE1/RW1 to CDC, MTP,
an SD card, exporting files from the watch beyond a short listing, Wi-Fi,
partition changes, eFuses, Secure Boot, flash encryption.

## 2. Facts this plan rests on

| Fact | Evidence |
|---|---|
| Software restart from an OTG image returns to USB Serial/JTAG (2 of 2) | usb-architecture.md, OTG probe |
| Manual ROM Download works from OTG ownership (1 run) | same |
| Cable pull gives suspend, never detach; re-plug gives attach | same, cable test |
| An LVGL-free FAT volume in PSRAM mounts on Linux, accepts files, and after eject the firmware sees them at exact size | usb-msc.md, probe and real-OS test |
| esp_tinyusb 3.0.0 mounts to the host on attach and to the application on eject or detach; `auto_mount_off` disables both | `tinyusb.c:62-89`, `tinyusb_msc.c:1147-1175,1757` |
| PackageManager needs the full-package SHA-256 at BEGIN, accepts 65,760 B per package, 8 packages, 512-byte chunks, READY expires after 60 s | package-manager.md |
| Packages are admitted only when no ELF is loaded and OTA/Safe Mode are idle | `roundie_packages_allowed()` |
| BootGuard does not count planned restarts as failed boots; retained state lives in RTC memory | boot-guard.md, `boot_guard.c` |

## 3. Proposed architecture (D0)

**USB storage mode is a separate, one-shot boot mode, not a runtime feature of
normal mode.** Normal mode keeps USB Serial/JTAG, so diagnostics, esptool
auto-reset, Safe Mode and recovery stay as they are.

1. A user action requests the mode. The firmware stores a one-shot flag in the
   BootGuard retained record and restarts as a *planned* restart.
2. Early in `app_main`, before USB or any application start, the flag is read
   and **cleared first**. Any crash, watchdog reset or power loss therefore
   returns to normal mode on the next boot.
3. Storage mode brings up the display and touch, mounts LittleFS for
   PackageManager, creates a fresh FAT volume in PSRAM, and starts TinyUSB with
   mass storage only. It starts no launcher, home, ELF runtime or applications,
   so nothing else can touch Flash.
4. The host owns the volume until the user ejects it, taps "Zakończ", or a
   timeout passes. The firmware then owns it and imports `Inbox/*.rpk`.
5. After the import, a planned restart returns to normal mode.

Why this over always-on OTG: the exclusive ownership MSC needs is free here (no
applications run), and nothing about the working USB path changes. Cost:
two restarts per use (to be measured) and no live access to the drive.

## 4. Design details

**Volume (assumption).** 1 MiB FAT12, created at each start, discarded at exit;
holds `Inbox/`, a short `README.TXT` and a listing of installed packages. 1 MiB
is eight maximum-size packages plus metadata; a larger copy fails on the host
with "disk full", which is a useful signal. The probe used 4 MB; nothing else
competes for PSRAM in this mode.

**Ownership policy.** With auto-mount on, a re-attach during an import would
unmount the volume under the importer. Required behaviour: every file operation
is checked, a vanished volume aborts the current file and its PackageManager
session, and the state machine decides what next. Alternative: set
`auto_mount_off` and drive ownership ourselves, which needs another way to see
the host's eject command; pending the source check in G2. A suspend without
eject has no detach event, so the volume is taken back after a suspend timeout
(value measured in G2, not assumed).

**Importer.** Runs in the storage-mode loop, never in a TinyUSB callback.
TinyUSB callbacks only copy a small event into a bounded mailbox; the block-device
callbacks are `memcpy` on RAM only.
For each file in `Inbox` (at most 8, regular files, extension `.rpk` in any
case, name grammar bounded, hidden and system entries from the host ignored,
size ≤ 65,760 B):

1. Hash the file (streamed SHA-256). This resolves the "digest at BEGIN" blocker
   from usb-mtp.md: the file is on the device before BEGIN.
2. `begin`, `write` in 512-byte chunks, `finish`: the existing validator runs.
3. Show the validated name and version and ask the user (D1 below).
4. `install` with the same digest, or `abort`. The 60 s READY window bounds the
   dialog; on expiry the file is skipped with a message.
5. Record the result: installed, updated, or rejected with the reason from
   PackageManager (not higher version, invalid, incompatible, too large).

**BootGuard and OTA.** Entry is refused when Safe Mode is active or latched,
when an OTA session is open, when the running image is not yet confirmed, or
while recovery is running. A running home application is parked as for light
sleep (`roundie_launcher_park_home`). Storage mode never confirms an OTA image.
Failure counting follows the existing rules; a crash in storage mode counts
like any other incomplete boot. Whether it marks STABLE is an experiment (G1).

**USB identity (D3).** The probe used Espressif's default descriptors, which
are a lab convenience. A product needs its own strings and a VID/PID the
project is entitled to use. Whether Espressif issues PIDs under 0x303A to
third parties is not verified here.

**Language.** On-screen text Polish; code, logs and docs English.

## 5. Decisions needed from the user

| # | Decision | Recommendation |
|---|---|---|
| D0 | Replace MTP by this MSC storage mode in REP §16/§23 | Yes, amend the spec when G0 is accepted |
| D1 | Confirm each package on the watch, or install automatically | Confirm each: the host is untrusted; the earlier MTP design also did |
| D2 | Where the mode is started | BOOT power menu, after re-spacing it (six items overflow the round panel), plus a development command in RS1 |
| D3 | USB identity | Find an entitled PID before any release; a lab ID is acceptable during development |
| D4 | Volume size | 1 MiB; revisit after the first measurement |

## 6. Gates

Each gate has host tests, target build and device tests reported separately;
physical steps the user must do are recorded as PENDING and do not block
independent work.

| Gate | Content | Acceptance |
|---|---|---|
| G0 | This plan, D0–D4 answered, spec amended | User approval recorded |
| G1 | One-shot flag, planned restart, storage-mode branch showing a screen and returning to normal; no USB | Host tests of the flag logic and refusals. Device: 10 enter/exit cycles, BootGuard failure counter unchanged, OTA state unchanged. Experiment: does storage mode mark STABLE |
| G2 | MSC on the RAM volume (the probe, productised), ownership policy, screen states, development-only CDC status | Linux mounts, copies, ejects; re-attach during idle; suspend timeout measured; ROM entry from storage mode repeated; return to normal |
| G3 | Importer, confirmation dialog, results, mocks for PackageManager | Host tests: name and size rules, host junk, 8-file bound, vanished volume, duplicate and lower versions, READY expiry. Device: install and update the watch and stopwatch packages; every rejection class once |
| G4 | Entry points (power menu, RS1 command), home parking, documentation, spec amendment | Full host suite; normal-mode regression (RP1 flows, sleep, restart); no change to diagnostics |
| G5 | Stabilisation | 20 enter/import/exit cycles; re-attach mid-import; cable pull without eject; Windows, macOS and an Android phone as PENDING for the user |

## 7. Risks

| Risk | Mitigation |
|---|---|
| Re-attach unmounts the volume mid-import | Check every operation; abort session; host test with fault injection |
| Host sends commands the library does not handle (`SYNCHRONIZE CACHE` returns an error today) | Measure on Windows and macOS in G5; the library is the place to fix if needed |
| Host writes unexpected data | Size, count and name limits; PackageManager is the only admission point |
| Storage mode hangs | One-shot flag already cleared; watchdog plus an idle timeout restart into normal mode |
| Entry while a package transaction or OTA is open | Refused by precondition |
| Boot round trip is slow | Measure in G1; accept or reduce |
| Lab USB identity reaching users | D3 blocks release |

## 8. Gate G1 results, 2026-11-04 (firmware 0.3.35)

**Implemented.** Component `components/roundie_storage_mode` (flag and
admission rule, host-tested), RS1 `STORAGE` (`tools/roundie_safe.py storage`),
`main/storage_mode_screen.c` (Polish screen, "Zakończ" button, countdown), a
`storage` field in the RS1 status, and `roundie_app_runtime_allowed()` returning
false while the mode is active. No USB, no PackageManager use.

**Staging choices.** G1 keeps the normal system worker and USB Serial/JTAG, so
the mode can be observed over RS1; G2 replaces that path. The idle timeout is
30 s for quick cycles; the plan's working value (a few minutes) is set in G2.
The display is kept on through the existing power-policy `keep_display` input.

**Verified.**

| Evidence | Result |
|---|---|
| Host | Flag: one request survives exactly one software restart, any other reset class, a different image, a corrupt or uninitialised record, or a second boot never starts it. All six refusals. RS1 harness: STORAGE is refused while an application is loaded, while OTA is open, for an unconfirmed image and when discarded; accepted once, then refused while active. Full suite: 86 tests OK |
| Target build | 0.3.35, no new warnings |
| Device, 10 cycles (RS1 over USB Serial/JTAG, no touch) | Each: request accepted with the restart flag, RS1 answered 1.6 s later with `storage=1`, `safe=0`, `failures=0`, the launcher in WAITING with no generation, the mode ended by itself after about 32 s, and the next RS1 reply showed `storage=0`, `failures=0`, `safe=0`, image VALID |
| Refusal on device | A second STORAGE request inside the mode was rejected (code 3) |
| BootGuard stage | Inside the mode, 14 s after the request: stage 4 (STABLE), failure counter 0, no trial. After leaving it: stage 4 again, home running |

**Experiment answered:** in G1 the mode marks STABLE because the normal boot
machinery runs unchanged. G2 must keep that property or define it explicitly
when the system worker no longer starts USB Serial/JTAG.

**Not verified.** How the screen looks and whether "Zakończ" works by finger
(PENDING, needs the user); a restart refusal path with a real OTA session;
behaviour with a loaded application is refused on the admission rule only (the
device tests entered with the home application closed).

## 9. Gate G2 results, 2026-11-04 (firmware 0.3.36)

**Implemented.** `main/storage_usb.c`: 1 MiB FAT volume in PSRAM behind an
`esp_blockdev`, esp_tinyusb 3.0.0 mass storage with the library's automatic
mount switching, a bounded queue of bus events from the TinyUSB task, and the
development CDC status port (`ROUNDIE_DEV_STORAGE_CDC`, word `EXITNOW`).
`components/roundie_storage_mode/session.c`: the ownership and timeout policy.
`main/storage_mode_screen.c`: states WAITING, HOST_OWNS, EJECTED and a USB
failure, with a countdown. The system worker skips the USB Serial/JTAG driver in
the mode. Light sleep is refused in the mode. The USB identity is still the
component's default (D3 open). The build adds the esp_tinyusb dependency
(`dependencies.lock`), two Kconfig options and about 78 KB.

**Policy values (decisions, adjustable):** no host for 120 s ends the mode; a
host that owns the drive and does no I/O for 600 s ends it; after an eject the
mode ends after 20 s (G3 replaces this with the import and a short countdown);
a host that owns the drive and has been suspended for 20 s loses it.

**Verified**

| Evidence | Result |
|---|---|
| Host | Session policy: no-host timeout; busy host postpones the idle timeout; eject starts the short timeout; re-attach returns the drive; suspend without detach takes the drive back after the limit; resume cancels it; suspend while the firmware owns the drive takes nothing. Full suite: 87 tests OK |
| Target build | 0.3.36, no new warnings, 1,063,136 B |
| Device, enter | RS1 `storage` accepted; USB Serial/JTAG left the bus and `303a:4003` appeared 2.9 s after the command; the status port reported `usb=ok`, `phase=1` (host owns), `att=1`, `det=0` |
| Device, Linux 7.3.1 | `sdc` 1 MB with a 992.5 KB vfat partition; udisks mounted it; `Inbox` and `README.TXT` visible; a 100 KiB file copied into `Inbox` with a matching SHA-256 after `sync` |
| Device, eject | udisks `Drive.Eject`: the status port showed `owner=APP`, `phase=2`, `left=16`; 18.2 s after the eject the mode ended by itself and USB Serial/JTAG (`303a:1001`) returned |
| Device, back in normal mode | RS1: `storage=0`, `failures=0`, `safe=0`, image VALID, stage reached 4 |
| EXITNOW | Stray bytes before the word did not end the mode; the word did; USB Serial/JTAG returned 2.5 s later |
| Host owns after attach | With the cable plugged in, the library mounts the drive to the host at attach, so the "no host" timeout does not apply to a plugged cable: an idle host keeps the mode for the 600 s idle limit (390 s were left when observed) |

**Not verified (PENDING, need the user or a decision).**

- The screen by eye and "Zakończ" by finger.
- A cable pull while the host owns the drive (take-back after 20 s) and a
  re-attach during the ejected window: the logic is host-tested, the device
  path is not.
- ROM Download entry from storage mode (the same procedure as the OTG probe,
  repeated with this firmware).
- A restart refusal with a real OTA session.
- Windows, macOS and Android.
- Power cost of the mode and its boot round trip: only the 2.9 s and 2.5 s
  above were observed, not measured as a budget.

### G2 physical results, 2026-11-04 (firmware 0.3.37)

0.3.37 adds two counters to the development status line: `tb` (drive taken back
from a silent host) and `rel` (drive passed from the host to the firmware).
Each result below is one run with the user at the board.

| Case | Observation |
|---|---|
| Screen and button | User-reported: "looks good". The mode had ended and normal mode was back (`storage=0`, `failures=0`, stage 4) when checked, which is consistent with the "Zakończ" tap; the tap itself was not observed |
| Cable pulled while the host owned the drive, replugged after 25-30 s | Counters before: `att=1 susp=0 tb=0 rel=0`; after: `att=2 det=0 susp=1 tb=1 rel=1`. The drive was taken back, then returned to the host at the re-attach (`phase=1`, `owner=USB`, `io` rose from 113 to 205 as the host rescanned). The delay between the pull and the take-back was not recorded |
| ROM Download entry from storage mode | PWR held 2 s with USB attached (the mode kept running), cable removed (host lost the device), BOOT held while reconnecting: esptool `--no-stub chip-id` connected, ESP32-S3, MAC `28:84:85:87:98:7c`, "Staying in bootloader"; read-only |
| Return from ROM | Cable removed and replugged without BOOT or PWR: 0.3.37 on `ota_1` VALID, reset class power-on, `storage=0`, `failures=0`, home running, USB Serial/JTAG answering |

**Observation, not new:** the PWR hold leaves the power latch off, so after
this procedure the board needs one short PWR press to run on battery alone
(usb-architecture.md).

**Open for G2:** the restart refusal during a real OTA session is unreachable in
this mode, because OTA runs only over USB Serial/JTAG, which is off; it stays
covered by the admission rule and host tests. Windows, macOS and Android remain
PENDING. G2's device gate is otherwise met.

## 10. Gate G3 results, 2026-11-04 (firmware 0.3.39)

**Implemented.** The importer is `components/roundie_storage_mode/import.c`, a
non-blocking state machine with injected file system, SHA-256 and PackageManager
(so it is host-tested with mocks). `main/storage_import.c` binds it to the VFS,
PSA hashing and `roundie_packages_*`, and is stepped from the system worker loop,
the only caller of PackageManager. The GUI reads a published copy of the status
and posts requests, answers and cancels through atomics; it never waits for
storage work. `main/storage_mode_screen.c` shows the confirmation dialog
("Zainstalować?", name, version, "Instaluj" and "Pomiń") and the result summary.

**Decisions applied (D1 default, adjustable).** Each package is confirmed on the
watch. Candidates are `.rpk` files directly in `Inbox`, at most 8 per run, each
at most 65,760 B (PackageManager's maximum), ASCII names under 64 characters,
not hidden. Everything else is ignored and counted; empty and oversized `.rpk`
files are reported as failures. The answer window is 45 s, inside
PackageManager's 60 s READY window; no answer is a timeout and the file stays.
A file is deleted from `Inbox` only after it was installed. The run waits up to
30 s for the boot to be stable (PackageManager admits nothing earlier) instead of
failing; a host that re-attaches during a run cancels it. Admission for
packages is allowed in storage mode while applications stay inhibited.

**Verified**

| Evidence | Result |
|---|---|
| Host | Classification table; valid install and an update; skip, timeout (before the READY window), empty, oversized, not a package; refusal at begin, write, finish and install, aborting exactly when a session is open; a volume that vanishes while hashing and while staging; list failure; the 8-file bound; cancel mid-transfer and while waiting for the answer; readiness wait then success, and refusal after 30 s; session numbers increase across runs; at most 8 chunks are read per step. Full suite: 88 tests OK |
| Target build | 0.3.39, no new warnings |
| Device, real host stack (Linux 7.3.1, `cp`, udisks eject; answers sent through the development words) | Run A: `MSCTEST.RPK` installed; `BAD.RPK` (one flipped byte in the ELF) `bad_package`/`package-ELF-digest`; `JUNK.RPK` (400 random bytes) `bad_package`/`package-header`; `BIG.RPK` (70,000 B) `too_large`; `EMPTY.RPK` `empty`; `._MAC.RPK` and `NOTES.TXT` ignored. Run B: no answer, `timeout` after 45 s, file kept. Run C: the same version again `refused` (`replace-requires-healthy-older-version`), then 1.1.0 `updated`. Run D: `skip`, file kept. After each run: `storage=0`, `failures=0`, `safe=0`, VALID. The installed package was listed by RP1, launched (accepted), then removed to restore the device |
| First attempt | A run started 8-9 s after entering the mode was refused with `package-admission`, because PackageManager admits only from STABLE, about 12 s in. The readiness wait is the fix and was verified in the second attempt |

**Not verified (PENDING).**

- The dialog buttons and the result screen by eye and finger (the device runs
  used the development words INSTALL and SKIP).
- A re-attach during a run on the device (host-tested only).
- Flash behaviour: the number of package installs per mode entry that the
  LittleFS wear and write latency can take is unmeasured; the GUI was not
  observed during the writes.
- Windows, macOS and Android; the drive texts in the host's language.
- A package larger than the typical one: only a 1.9 KB package was imported.

### G3 finger results, 2026-11-04 (firmware 0.3.39)

Two runs with the user at the board, nothing sent by the development words.

| Case | Observation |
|---|---|
| Dialog and "Instaluj" | User-reported: the dialog showed and the package was installed. Status port: `IMPORTRESULT 0 file=MSCTEST.RPK result=installed name=MSC test version=1.0.0` |
| Dialog and "Pomiń" | User-reported: "Pominięto". Status port: `result=skipped name=MSC test two version=1.0.0`; normal mode returned (`storage=0`, `failures=0`, VALID) |

Not recorded: whether every Polish character and line fitted the circle on both
screens (asked, not answered). The test package was removed afterwards, so the
device holds hello, round, stopwatch, ui and watch.

**Still open for G3:** a re-attach during a run on the device, flash wear and
latency for repeated installs, Windows, macOS and Android.

## 11. Gate G4 results, 2026-11-04 (firmware 0.3.41)

**Implemented.** The BOOT power menu has a "Pamięć USB" item (six 200 x 38 px
targets every 41 px between y=56 and y=299, inside the round panel). It is a
worker command next to Restart: the worker unloads the home application as for
light sleep (3 s bound), evaluates the same admission rule as RS1 `STORAGE`,
stores the one-shot request and restarts as a planned restart. A loaded
application other than home refuses with a Polish message. RW1 `STORAGE`
(`tools/roundie_power.py storage`) starts the same worker path for tests. The
quick settings panel has no entry (decision D2 named the BOOT menu only).

**Fix found on the way.** After a screen-off timeout the panel stayed black
after wake, as it did after light sleep: user-reported "czarny ekran", and a
`SCREEN ON` that reported `active` did not bring the picture back. Turning the
screen on now re-initialises the ST77916 (reset, init table, DISPON) once no
transfer is in flight, and repaints, as the sleep wake already did. Cause not
identified; the earlier 2026-11-03 note that the screen-off path was verified
rested on counters, not on seeing the panel.

**Verified**

| Evidence | Result |
|---|---|
| Host | Full suite, 88 tests OK. The menu and worker glue are device-only code |
| Target build | 0.3.40 and 0.3.41, no new warnings |
| Device, home running (0.3.40, RW1 command) | Home unloaded, restart, USB storage enumerated 3.2 s after the command, status port `usb=ok`; `EXITNOW`: `storage=0`, `failures=0`, VALID, stage 4, home running again |
| Device, another application loaded | The command was accepted, no restart happened, the application kept running |
| Regression | Timed light sleep with the home application: 4,991 ms, `refusal none`, home relaunched. RP1 package list, launch and remove, and the earlier import runs, worked on 0.3.39-0.3.41 |
| Device, by finger (user-reported "Działa") | The screen went off on its own, a touch woke it (this was the black-screen case on 0.3.40, fine on 0.3.41), BOOT opened the menu, "Pamięć USB" entered the mode, "Zakończ" returned to the watch face |

**Not verified.** Whether all six menu targets are easy to hit near the top and
bottom edge was asked but not answered. A restart refusal with a real OTA
session is still unreachable in the mode. Windows, macOS and Android.

