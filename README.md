# Roundie

Native C firmware for the Waveshare ESP32-S3 Touch-LCD-1.85B.
Architecture baseline: [REP v0.4](docs/specification.md).

Use the pinned ESP-IDF v6.1 checkout:

```sh
cd /mnt/Dane/Development/roundie
. /home/solve/.espressif/v6.1/esp-idf/export.sh
# Installed REP development device:
idf.py -B build-rep -DROUNDIE_PROFILE=rep -DPROJECT_VER=0.3.1 build
# Historical manufacturer-layout diagnostic image, build only:
idf.py -B build -DROUNDIE_PROFILE=legacy build
```

**Current bounded 3A checkpoint:** B 0.3.1 VALID/running and normal/STABLE;
A 0.2.5 VALID/fallback. A standalone native ELF passed real execution from
PSRAM, ABI 1 services, repeated cleanup, negative candidates and Safe Mode
refusal. See [ELF evidence and limitations](docs/elf-loader.md).

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
- [Native ELF proof, workflow and evidence](docs/elf-loader.md)
- [REP C application ABI 1](docs/app-abi.md)
- [Local USB OTA, confirmation, rollback and pending physical gates](docs/ota.md)
- [Physical Flash migration and provisioning](docs/flash-migration.md)
- [Hardware audit](docs/hardware-audit.md)
- [Bring-up evidence and remaining Milestone 1 checks](docs/bringup.md)
- [Physically verified ROM recovery route](docs/rom-recovery.md)
