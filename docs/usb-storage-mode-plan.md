# USB storage mode — integration plan (proposal, not approved)

**Status: plan only.** No firmware, partition, ABI or protocol changes are made
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
