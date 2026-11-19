# Roundie

Roundie is a software platform for small round-display ESP32-S3 devices. It is
written in C for ESP-IDF v6.1. One shared **Roundie Core** (launcher, power
policy, wall clock, package manager and application runtime) runs on the
devices and, unchanged, in **Roundie Desktop** on Linux. Applications are
native **REP** packages: C programs built against a versioned SDK, loaded as
ELF objects and driven by events.

Architecture baseline: [REP specification v0.4](docs/specification.md).
Current state of the Core and its milestones: [docs/roundie-core.md](docs/roundie-core.md).

## Supported boards

| Board | Panel | Validation status |
|---|---|---|
| Waveshare ESP32-S3-Touch-LCD-1.85B | 360 × 360 ST77916 LCD, CST816S touch, BQ27220 gauge, PCF85063 RTC, ES8311 | Main development board. Boot, OTA with rollback, ROM Download recovery, Safe Mode, storage, applications, audio, screen off and light sleep were tested on the device. Some person-at-the-board checks remain PENDING ([board support](docs/board-support.md#device-results-2026-11-15-firmware-0350-and-0351)) |
| Waveshare ESP32-S3-Touch-AMOLED-1.75C | 466 × 466 CO5300 AMOLED (360 × 360 canvas, upscaled), CST9217 touch, AXP2101 PMIC, ES8311, no RTC | Tested on **one** unit: boot, display, touch, brightness, screen off/on, light-sleep wake, battery readings and audio. Wall clock, wake latency and endurance are PENDING, and there is **no installer** for a first install yet ([1.75C audit](docs/hardware-audit-1.75c.md#pending-device-tests)) |

Both boards build from the same sources. Everything board-specific sits behind
`components/roundie_board` ([board support and adding a board](docs/board-support.md)).
Both boards carry an ES7210 microphone ADC and a QMI8658 IMU; Roundie does not
support either yet.

## What is in the repository

- **Roundie Core**: the launcher and round UI widgets, quick settings, the
  power manager (screen off, power menu, light sleep), wall clock and RTC
  policy, package manager and per-application storage. Platform ports
  (`platform/linux`, the ESP-IDF components) supply display, input, clock,
  storage, audio and the ELF loader ([roundie-core.md](docs/roundie-core.md)).
- **REP applications.** An application is an `.rpk` package with a native ELF
  object, entered through a versioned API table ([ABI](docs/app-abi.md),
  [package format](docs/package-format.md)). One application runs at a time on
  an Application Executor. Applications are trusted native code: the symbol
  allowlist controls the API, not memory access. On the ESP32-S3 they are
  Xtensa ELF objects (`esp32s3.rep1`, [ELF loader](docs/elf-loader.md)). On
  Linux the same sources build as x86-64 objects (`linux-x64.rep1`,
  [native apps](docs/native-apps.md)). Examples are in `examples/`.
- **Roundie Desktop** (Linux): the real Core in an SDL3 window with Vulkan
  presentation and LVGL, a panel profile for each board, the mouse as the
  touch panel, scripted headless runs, virtual time, and REP x86-64
  applications with SDL3 audio.
- **Virtual Hardware Lab**: deterministic models of the battery, charger,
  PMIC/gauge, supply, PWR latch, resets and RTC. It injects faults into touch,
  display, audio, gauge and storage calls. A control panel and script
  commands drive it ([virtual-hardware.md](docs/virtual-hardware.md)). The
  models are assumptions, not measurements of the boards.
- **OTA and recovery**: the standard ESP-IDF bootloader, two OTA slots,
  application-confirmed rollback after a self-test, Safe Mode in the firmware,
  and a manual ROM Download path ([OTA](docs/ota.md),
  [Safe Mode](docs/safe-mode.md), [ROM recovery](docs/rom-recovery.md)).
- **Tools**: the OTA and installation scripts, the package builder and
  validators, device diagnostics over USB serial (`tools/`), and Roundie
  Manager, a local Web Serial application manager
  ([tools/roundie-manager](tools/roundie-manager/README.md)).

## Quick start

### Firmware (build only)

You need ESP-IDF v6.1. CI uses the `espressif/idf:v6.1` image pinned in
[docs/ci.md](docs/ci.md). Select the `rep` profile explicitly; the default
`legacy` profile is a historical diagnostic layout.

```bash
. "$IDF_PATH/export.sh"
```

```bash
tools/ci/build_firmware.sh waveshare_s3_touch_lcd_1_85b rep
```

```bash
tools/ci/build_firmware.sh waveshare_s3_touch_amoled_1_75c rep
```

The script runs `idf.py build` and `idf.py size` in `build-ci/`. It fails on
any compiler warning. A plain `idf.py` build of the 1.85B, as the host tests
expect it:

```bash
idf.py -B build-rep -DROUNDIE_PROFILE=rep -DPROJECT_VER=<version> build
```

### Roundie Desktop

You need Meson, Ninja, SDL3, Vulkan and Mbed TLS 4. LVGL comes from
`managed_components`, so run any firmware build (or `idf.py reconfigure`) first.

```bash
meson setup build-desktop -Ddesktop=enabled
```

```bash
ninja -C build-desktop
```

```bash
build-desktop/roundie-desktop --board 1.85b
```

Use `--board 1.75c` for the 466 × 466 profile, `--native-apps` for REP x86-64
packages, `--virtual-time` for scripted runs and `--lab-panel` for the
Virtual Hardware Lab window. The mouse is the touch panel, `B` is BOOT, `P` is
PWR and `Esc` quits. All options and script commands:
[docs/roundie-core.md](docs/roundie-core.md#running-the-desktop).

### Tests

The host tests live in `tests/` and run in named groups:

```bash
python3 tools/ci/run_tests.py --list
```

```bash
python3 tools/ci/run_tests.py host
```

For the same isolated environment that CI uses (Podman or Docker):

```bash
podman build -f tools/ci/Containerfile -t roundie-ci tools/ci
```

```bash
tools/ci/container.sh python3 tools/ci/run_tests.py --strict lab
```

[docs/ci.md](docs/ci.md) lists the groups, their prerequisites and how to
reproduce each job.

## Installing on a device

**Read [flash-migration.md](docs/flash-migration.md#rep-development-updates)
before you write anything to a board.** The project's rules:

- Raw IDF flash targets (`idf.py flash` and the others) are disabled on
  purpose. Do not run the esptool commands that `idf.py` prints: they bypass
  the partition and profile checks.
- A board gets its first REP layout once, by a reviewed, board-specific
  procedure. Read the board's Flash out first.
- Later updates go over USB with `tools/roundie_ota.py`. The new image must
  pass its self-test before it is confirmed; otherwise the bootloader rolls
  back ([OTA](docs/ota.md)).
- The 1.75C has no supported installer yet: `tools/flash_rep.py` and
  `tools/flash_bringup.py` refuse it
  ([installation gap](docs/hardware-audit-1.75c.md#flash-size-conflicting-sources)).
- Keep ROM Download (GPIO0/BOOT) usable. Do not change eFuses, Secure Boot,
  Flash Encryption or anti-rollback settings.

## Continuous integration

GitHub Actions ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) runs
on every pull request and on pushes to `main`. It covers:

- the four firmware builds (two boards, `legacy` and `rep`);
- the host tests and validators;
- Roundie Manager;
- the desktop build;
- REP x86-64 applications and the desktop runtime under ASan/UBSan;
- the Virtual Hardware Lab;
- graphics tests on gamescope and KWin with Mesa lavapipe;
- a ThreadSanitizer run.

CI runs no hardware. A green run is not hardware validation
([docs/ci.md](docs/ci.md#hardware-checks-outside-ci)).

## Documentation

- [Roundie Core: architecture, milestones M1–M7, desktop](docs/roundie-core.md)
- [Project history: REP stages on the 1.85B](docs/project-history.md)
- [Board support and adding a board](docs/board-support.md)
- [Hardware audit, 1.85B](docs/hardware-audit.md) and [1.75C](docs/hardware-audit-1.75c.md)
- [Native ELF proof, workflow and evidence](docs/elf-loader.md)
- [REP C application ABI](docs/app-abi.md), [packages](docs/package-manager.md), [app storage](docs/app-storage.md)
- [REP applications on Linux x86-64](docs/native-apps.md)
- [Virtual Hardware Lab](docs/virtual-hardware.md)
- [Storage and build profiles](docs/storage.md)
- [OTA, confirmation and rollback](docs/ota.md), [Flash migration and provisioning](docs/flash-migration.md)
- [ROM recovery](docs/rom-recovery.md), [Safe Mode](docs/safe-mode.md), [BootGuard](docs/boot-guard.md)
- [Application lifecycle](docs/app-lifecycle.md), [launcher and navigation](docs/system-navigation.md)
- [Round UI widgets](docs/round-ui.md), [quick settings](docs/quick-settings.md), [watch face](docs/watch-face.md), [stopwatch](docs/stopwatch.md)
- [Audio and rep_audio_v1](docs/audio.md), [PowerManager](docs/power-manager.md)
- [USB storage mode](docs/usb-storage-mode-plan.md), [Roundie Manager](tools/roundie-manager/README.md)
- [Continuous integration](docs/ci.md)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Small, focused pull requests are
welcome. Hardware results should say what was measured, on which board, and
with which firmware.

The AMOLED 1.75C panel-restore, AXP2101 and battery changes and their device
results were contributed by [darksv](https://github.com/darksv) ([pull request #1](https://github.com/WorkingWarrior/roundie/pull/1)).

## License

Roundie is licensed under the [MIT License](LICENSE). Third-party components
and fonts keep their own licenses and copyright notices.
