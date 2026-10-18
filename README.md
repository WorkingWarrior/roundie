# Roundie

Native C firmware for the Waveshare ESP32-S3 Touch-LCD-1.85B.
Architecture baseline: [REP v0.4](docs/specification.md).

Use the pinned ESP-IDF v6.1 checkout:

```sh
cd /mnt/Dane/Development/roundie
. /home/solve/.espressif/v6.1/esp-idf/export.sh
# Installed REP development device:
idf.py -B build-rep -DROUNDIE_PROFILE=rep build
# Historical manufacturer-layout diagnostic image, build only:
idf.py -B build -DROUNDIE_PROFILE=legacy build
```

The connected board was migrated to REP v1 in Milestone 2B. The default build
profile remains `legacy`, so select `rep` explicitly. Raw IDF flash targets
are disabled; never run the printed esptool commands. For an authorized
app-only update use `python tools/flash_rep.py --port /dev/ttyACM0 --write`
after the REP build. Without `--write`, it performs read-only preflight.
See [deployment limits and recovery](docs/flash-migration.md#rep-development-updates).
The historical bring-up updater rejects the physical REP table.

- [Storage/build profiles and validation](docs/storage.md)
- [Physical Flash migration and provisioning](docs/flash-migration.md)
- [Hardware audit](docs/hardware-audit.md)
- [Bring-up evidence and remaining Milestone 1 checks](docs/bringup.md)
- [Physically verified ROM recovery route](docs/rom-recovery.md)
