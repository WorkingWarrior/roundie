# USB mass storage (MSC) as the file transfer path — probe results, 2026-11-04

**Status: feasibility probe only; nothing is integrated.** The user asked
whether the board could appear as an ordinary USB drive with folders instead of
MTP ([MTP analysis](usb-mtp.md), [USB architecture gate](usb-architecture.md)).
This page records the comparison and the first device results. It changes no
shipped firmware, partition, ABI or protocol.

## Comparison (analysis, not measurement)

| | MTP | MSC with a RAM or SD volume |
|---|---|---|
| Backend | Custom restricted backend needed: the released one is FATFS-specific and bypasses PackageManager ([usb-mtp.md](usb-mtp.md)) | Released esp_tinyusb 3.0.0 supports SPI flash, SD/MMC and a generic block device |
| Access while the watch runs | Yes, in principle | No: one owner at a time. The firmware needs a "USB storage" mode and an unloaded application |
| Host support | Needs MTP stack; Linux behaviour unmeasured | Any host with a mass-storage driver |
| Admission | Per object, in our code | None at USB level; files are admitted by PackageManager after the host lets go |
| Exposure of system data | Virtual folders only | Only the volume we create. An internal-flash FAT partition would need repartitioning and is not proposed |
| Persistence | LittleFS | RAM volume is lost at reset; an SD card persists |

## Verified source (esp_tinyusb 3.0.0 `tinyusb_msc.h`, `tinyusb_msc.c`)

- Storage media: SPI flash with wear levelling, SD/MMC, or an
  `esp_blockdev` (needs IDF ≥ 6.0.4; the project pins 6.1).
- The volume has a mount point, `USB` or `APP`; never both. With automatic
  mounting on, a host eject (START STOP UNIT with load/eject) moves it to the
  application, and a TinyUSB mount event moves it to the host.
- FAT only. SCSI `SYNCHRONIZE CACHE` is not handled (see below).

## Device results, firmware 0.3.34 plus the probe (one run each)

The drive was **not mounted by an operating system**: the running Linux kernel
(7.3.0-4) had lost its modules to an update, so `usb_storage` could not load.
A userspace Bulk-Only client (`examples/usb_msc_ramdisk_probe/tools/msc_host.py`)
stood in for the host. It does what a file manager does at block level and
cannot show cache or ordering behaviour.

| Case | Observation |
|---|---|
| Enumeration | Composite `303a:4003`: CDC plus interface class 08, subclass 06, protocol 50 |
| Initialisation | All four init calls `ESP_OK`; folders and two files created on a 4 MB volume while owned by the application |
| Volume layout | MBR with one FAT12 partition at sector 63, 8129 sectors; mtools lists `Inbox`, `Applications` (long name), `README.TXT`, `Applications/LIST.TXT` |
| Read | 4096 KiB in 4.1 s, about 992 KiB/s |
| Write | `TEST.RPK` (120 KiB) added with mtools; 242 changed sectors written in 0.12 s; read-back of the first 200 matched |
| Eject | START STOP UNIT status 0; afterwards "medium not present"; the log shows owner `APP` and `inbox=TEST.RPK:122880`, equal to the file size |
| Re-attach | After the cable was pulled for about 10 s and replugged: `attached=2`, `suspended=1`, owner `USB` again. Contents were not read back |
| `SYNCHRONIZE CACHE` | Returns an error status. Linux tolerates this; other hosts are unmeasured |
| PSRAM | 4 MB volume leaves 4,188,860 B of the 8 MB free |

## Real operating system, same day (Linux 7.3.1, one run)

After the computer restarted into a kernel with `usb_storage`, the probe
(rebuilt to keep the last Inbox listing taken while the application owned the
volume) was tested through the desktop's normal storage stack (udisks).

| Case | Observation |
|---|---|
| Recognition | `sdc`, 4 MB, one vfat partition; mounted by udisks without any option |
| Copy | Two files (100 KiB and 64 KiB) copied into `Inbox` with `cp`; `sync` took 0.3 s; SHA-256 read back from the mount matched the originals |
| Persistence across a cable pull | A file written in an earlier session was still there, with the same SHA-256, after the cable was removed and replugged |
| Eject (udisks D-Bus `Drive.Eject`) | The media size became 0 B, the USB device stayed on the bus with its CDC port; the log showed owner `APP` and `inbox=REAL_OS.RPK:102400,SECOND.RPK:65536`, equal to the file sizes |
| "Safely remove" (`udisksctl power-off`) | The whole USB device left the bus, CDC included; the log kept no APP-owned window to read, hence the rebuilt probe. One `detached` event was counted in a window that also held a cable pull, so it is not attributed |
| Stray `eject` / `sg_start` | Not permitted for this user (no SG_IO access); no inference |

**Still not measured:** Windows and macOS; an unclean unplug while the host
holds dirty data (the cache); the cost of 4 MB of PSRAM against the GUI and
XIP budget; integration with PackageManager, Safe Mode, sleep and home
parking.

## What this does not settle

- Windows and macOS, and an unclean unplug while the host holds dirty data
  (Linux write caching with a clean eject is covered below).
- Time to hand the volume back after a cable pull without eject: no detach
  event arrives ([cable-removal result](usb-architecture.md)), so the design
  needs a policy, for example taking it back after a suspend timeout.
- A smaller RAM volume, since 4 MB takes half of PSRAM; and an SD card.
- Integration: PackageManager import of a dropped `.rpk`, the "USB storage"
  screen and its interaction with Safe Mode, light sleep and home parking.

## Recommendation (judgement)

MSC with a RAM volume is the cheaper first path for installing packages: it
needs no custom USB backend and keeps PackageManager as the only admission
point. MTP stays the answer only if live access while the application runs is
wanted. This is not an architectural decision; none has been taken.
