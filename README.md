# Roundie

Native C firmware for the Waveshare ESP32-S3 Touch-LCD-1.85B.
Architecture baseline: [REP v0.4](docs/specification.md).

Use the pinned ESP-IDF v6.1 checkout:

```sh
cd /mnt/Dane/Development/roundie
. /home/solve/.espressif/v6.1/esp-idf/export.sh
# Existing manufacturer-layout development device:
idf.py -B build -DROUNDIE_PROFILE=legacy build
# Future REP layout, build only; not compatible with the installed layout:
idf.py -B build-rep -DROUNDIE_PROFILE=rep build
```

The default profile is `legacy`. Never use IDF's printed flash commands on
the currently installed device. The [guarded bring-up updater](docs/bringup.md)
requires separate hardware authorization and rejects REP artifacts.

- [Storage/build profiles and validation](docs/storage.md)
- [Flash migration proposal — not executed](docs/flash-migration.md)
- [Hardware audit](docs/hardware-audit.md)
- [Bring-up evidence and remaining Milestone 1 checks](docs/bringup.md)
- [Physically verified ROM recovery route](docs/rom-recovery.md)
