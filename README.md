# Roundie

Native C firmware for the Waveshare ESP32-S3 Touch-LCD-1.85B.
Architecture baseline: [REP v0.4](docs/specification.md).

Use the pinned ESP-IDF v6.1 checkout:

```sh
cd /mnt/Dane/Development/roundie
. /home/solve/.espressif/v6.1/esp-idf/export.sh
# Installed REP development device:
idf.py -B build-rep -DROUNDIE_PROFILE=rep -DPROJECT_VER=0.3.24 build
# Historical manufacturer-layout diagnostic image, build only:
idf.py -B build -DROUNDIE_PROFILE=legacy build
```

**Current checkpoint, 2026-11-03:** normal 0.3.24 runs from `ota_1`, VALID
under XIP from PSRAM; normal 0.3.23 is the VALID fallback in `ota_0`.
REP stages 0–4 are done. Stage 5 has the launcher and the
[Round UI widgets](docs/round-ui.md) (RoundList, ArcSlider, EdgeButton,
RoundDialog, shadow state), verified on the device by injection and user touch. Stage 6's example is a
[watch face](docs/watch-face.md) with local time, a time zone and clock hands
instead of the calculator, plus a [stopwatch](docs/stopwatch.md). Stage 6 has packages without an SDK calculator example. Stage 7 USB
MTP is NO-GO; Roundie Manager over Web Serial is used instead. Stage 8 has screen
off, the BOOT power menu and light sleep, verified on the device. Its current
measurements and deep sleep remain open. See [PowerManager](docs/power-manager.md).

The connected board was migrated to REP v1 in Milestone 2B. The default build
profile remains `legacy`, so select `rep` explicitly. The approved Milestone
2C.1 A-to-B OTA and separately approved automatic rollback passed. The earlier
authorized restoration on 2026-10-19 left both slots with normal 0.2.1 and the
armed test disabled; later milestone results are recorded separately above.
Raw IDF flash targets
are disabled; never run the printed esptool commands. The first OTA-capable
firmware needs a separately authorized app-only bootstrap with `flash_rep.py`
and the exact installed 2B bootloader identity. Later updates use
`tools/roundie_ota.py`. Choose the actual stable serial alias explicitly.
Without `--write`, the bootstrap updater performs read-only preflight.
See [deployment limits and recovery](docs/flash-migration.md#rep-development-updates).
The historical bring-up updater rejects the physical REP table.

- [Storage/build profiles and validation](docs/storage.md)
- [Roundie Manager: local Web Serial application management](tools/roundie-manager/README.md)
- [Manager software acceptance and pending physical checks](docs/roundie-manager-milestone-3f-w-acceptance.md)
- [Native ELF proof, workflow and evidence](docs/elf-loader.md)
- [REP C application ABI 1](docs/app-abi.md)
- [Local USB OTA, confirmation, rollback and pending physical gates](docs/ota.md)
- [Physical Flash migration and provisioning](docs/flash-migration.md)
- [Hardware audit](docs/hardware-audit.md)
- [Bring-up evidence and remaining Milestone 1 checks](docs/bringup.md)
- [Physically verified ROM recovery route](docs/rom-recovery.md)
- [BootGuard, native fault and watchdog tests](docs/boot-guard.md)
- [Safe Mode and RS1 recovery commands](docs/safe-mode.md)
- [Application lifecycle and repeated-cycle tests](docs/app-lifecycle.md)
- [Launcher and system navigation](docs/system-navigation.md)
- [Packages](docs/package-manager.md)
- [App storage (rep_storage_v1)](docs/app-storage.md)
- [PowerManager: screen off, power menu, light sleep](docs/power-manager.md)
- [Round UI widgets and shadow state](docs/round-ui.md)
- [Watch face: local time, time zone, clock hands](docs/watch-face.md)
- [Stopwatch example](docs/stopwatch.md)
