# Milestone 1: ESP-IDF v6.1 bring-up

**Board target:** User-identified Waveshare ESP32-S3 Touch-LCD-1.85B; the
printed PCB revision was not visible. The documented Rev1.1 pinout was the
working premise, and actual LCD/touch behavior is recorded below.

**Evidence scope:** Source integration, target compilation, and USB-only
device tests on 2026-10-17. Battery, cold-start, long-run, and manual recovery
gates remain open.

## Build

The selected ESP-IDF is tag `v6.1`, commit
`fff9895c82d744c7237be8847347bdd1b07c6643`. The local toolchain used
for this gate is `xtensa-esp-elf esp-15.2.0_20251204`. The IDF Python
environment is installed with `./install.sh esp32s3` in that exact IDF
checkout.
The top-level CMake file rejects any other IDF commit.

```sh
cd /mnt/Dane/Development/roundie
. /home/solve/.espressif/v6.1/esp-idf/export.sh
idf.py set-target esp32s3 build
```

`set-target` resets generated configuration and performs a clean target
build. It does not flash the board. `sdkconfig`, `build/`, and
`managed_components/` are generated; `sdkconfig.defaults` and
`dependencies.lock` are the tracked inputs.

**Do not use `idf.py flash` on the inspected device.** Its existing
partition table differs from this temporary single-app build table, and
the default flash command would rewrite that table and place the app at
`0x10000`. The board's read-back table has NVS at `0x9000`, OTA data at
`0xd000`, factory app at `0x20000`, another app at `0x6c0000`, and
assets at `0xac0000`. The authorized bring-up installation writes only
the bootloader at `0x0` and app at `0x20000`; it leaves the installed
partition table and data partitions in place. This is a device-specific
installation method, not a final REP partition design.

The component manager lock resolves:

| Component | Revision |
|---|---|
| ESP-IDF | 6.1.0 / commit above |
| `lvgl/lvgl` | 9.5.0 |
| `espressif/esp_lcd_st77916` | 2.0.2 |
| `espressif/esp_lcd_touch_cst816s` | 1.1.1 |
| transitive `espressif/esp_lcd_touch` | 1.2.1 |
| transitive `espressif/cmake_utilities` | 0.5.3 |

## Configuration and decisions

**Verified source fact:** The manufacturer's 1.85B BSP at commit
`139e6db584f3737fcfc6a958ee83b79fb69d317c` supplies two ST77916
command tables selected by register 0x04 and uses GPIO10/11 for the common
I²C bus. It also creates both new and legacy I²C controllers, as recorded in
[the hardware audit](hardware-audit.md). Only the two panel command tables
were copied into `lcd_init_tables.c`; the vendor BSP is not linked.

**Architectural decision (spec §§2.3, 3.2, 23):** One `i2c_master.h` bus
at 100 kHz owns CST816S, PCF85063, and BQ27220 access. The touch controller
uses Espressif's CST816S component and its panel IO on that same bus. The RTC
and gauge each get an explicit device handle when their probe responds, with
narrow read-only register access. The gauge reads voltage (0x08) and status (0x0A), as defined by
[TI's BQ27220 reference manual](https://www.ti.com/lit/ug/sluubd4a/sluubd4a.pdf).
The register address sent before each read does not alter gauge
configuration or calibration. No `i2c_bus` or legacy controller is present.

**Architectural decision for this gate (spec §§12.7, 25):** One 14,400-byte
internal DMA-capable LVGL strip buffer keeps the first asynchronous transfer
path easy to inspect. The SPI bus permits that strip plus command overhead;
the ESP-IDF completion ISR notifies the GUI task, and LVGL's flush-wait
callback blocks that task until completion before reusing the buffer. A
transfer timeout restarts the device rather than reusing a buffer still owned
by DMA. The spec's proposed two-buffer strategy and final buffering policy
remain open until board measurements.

**Device observation and remaining assumption:** The user identified the
unit as a 1.85B but could not inspect its PCB revision. Its LCD reported
`00 02 7f 7f`, one of the two documented IDs; unknown IDs fail startup and
no table is guessed. QSPI starts at 3 MHz,
well below the vendor's 80 MHz. The backlight PWM is active-high on GPIO5,
5 kHz, initially 60%. LCD TE is not used. The copied tables include the
vendor's controller commands, but no TE synchronization is configured.

`sdkconfig.defaults` selects ESP32-S3, 16 MB flash, Octal PSRAM at 80 MHz,
and an 8 KiB main-task stack for the small LVGL loop. Generated v6.1 config
uses DIO flash at 80 MHz, the default single-app partition table, and does
not enable PSRAM XIP. This temporary partition choice does not resolve the
later storage/OTA architecture in spec §18.

## Diagnostic image behavior

Boot logs identify the firmware version, build timestamp, ELF SHA-256 prefix, IDF
version, detected flash and PSRAM size, free and largest internal/PSRAM
blocks, three I²C probes, RTC and gauge read results, LCD ID/initialization,
and touch press/release. The 360 × 360 screen displays identity, memory,
device states, RTC/gauge values, touch feedback, a 30/60/100% backlight
button, color swatches, and an animated spinner. Tapping a swatch displays
a solid red, green, blue, or white field; tap the field to return. Touch
reads are triggered by the CST816S IRQ and continue while a press is active;
the controller can stop answering I²C when idle. A failed LCD enqueue stops
new pixel transfers; a completion timeout restarts the device before LVGL
can reuse the DMA buffer. Every five minutes the image logs free and largest
SRAM/PSRAM blocks and the LCD transfer-error count. The long-run test was
stopped before the first periodic sample, so no heap trend is established.

The firmware does not access NVS or a filesystem, erase flash, start USB
device mode, use Wi-Fi, or configure power/sleep features.

## Validation results

| Evidence tier | Result |
|---|---|
| Host/static | Both copied ST77916 tables matched the pinned vendor source byte-for-byte. Source scan found zero legacy I²C/I²S includes or `i2c_bus_create` uses in project C/H files. |
| ESP32-S3 target compilation | Clean `idf.py set-target esp32s3 build` under pinned v6.1 exited 0, with zero compiler warnings and zero errors. This proves target compilation only. |
| Physical device | User confirmed 1.85B, working Roundie page/controls after idle, correct red/green/blue/white fields, moving spinner, and coherent center plus four edge touch regions with release. ESP32-S3 revision 0.2 reported 16 MB flash and 8 MB embedded Octal PSRAM; panel ID selected variant 2; CST816S ID read 181. RTC ACKed/read once with voltage-low flag, then did not ACK on later USB-only resets; gauge did not ACK. PCB revision, battery behavior, and manual recovery remain unverified. |
| Long-running/fault injection | A short monitor run had an LVGL flush watchdog failure before the wait-callback fix. The later 65-second interrupt-driven touch run had 52 press/release pairs and zero touch I²C errors, LCD transfer errors, watchdog reports, or panics in its log. A planned 30-minute run was stopped at the user's request after about two minutes, before its first five-minute health sample. Endurance and fault injection remain pending. |

### Milestone 1 device checklist status

| Check | Status | Evidence and limit |
|---|---|---|
| Diagnostic LCD, orientation, color fields, spinner | **PASSED for this unit** | User observed the page, correct red/green/blue/white fields, and moving spinner; LCD ID `00 02 7f 7f` selected variant 2. |
| Touch center and four reachable edge regions, release, controls after idle | **PASSED** | User explicitly confirmed all five regions and release, and controls after at least five seconds idle. The 65-second log recorded 52 press/release pairs without touch-read errors. |
| Flash and PSRAM detection | **PASSED for detection** | Device reported 16,777,216 B Flash and 8,388,608 B PSRAM; startup heap values were logged. Executable PSRAM and memory stress are outside this gate. |
| Shared I²C devices | **OPEN** | Touch driver read ID 181. RTC response varied between USB-only starts. BQ27220 never ACKed with no battery attached. |
| Ten cold starts; RTC retention; battery behavior; manual ROM Download | **PENDING** | No battery was attached, no controlled cold-start series or physical BOOT procedure was performed, and the PCB revision could not be inspected. |
| 30-minute interaction and heap trend | **PENDING** | Five-minute telemetry is installed; the requested endurance run was stopped before its first sample. |

### Device installation and data preservation

The user explicitly identified the connected USB device as the 1.85B and
authorized backup and flashing. `esptool flash-id` reported an ESP32-S3
revision 0.2, 16 MB Flash, and 8 MB embedded PSRAM. Security information
reported Secure Boot and Flash Encryption disabled; no eFuse was changed.
The complete 16,777,216-byte pre-flash readback is
`/mnt/Dane/Development/roundie-backups/preflash-2026-10-17-pMVcHN/flash-16MiB.bin`,
SHA-256 `46d2d36244a9e303cc61a65fb29d9a71b9708d41d181e9e5b5befb9261039de7`.
The read log and flash logs are beside it. The first installation wrote
only offsets `0x0` (bootloader) and `0x20000` (factory app); later updates
wrote only the factory app. Each esptool write verified its image hash.

A full 16 MB post-flash readback after the interrupt-driven touch image
matched the pre-flash backup byte-for-byte in the partition table, NVS,
OTA data, PHY data, the gap before the factory app, factory tail, OTA app
slot, assets, and flash tail. Changed 4 KiB sectors were confined to
`0x00000–0x05fff` and `0x20000–0xaafff`, the bootloader and factory app
image ranges. Its SHA-256 was
`7e3b02df62c1b6ac66fa31fdfe64488ce02304967ca564e4bdaddd51a13ab9f9`.
The later five-minute-telemetry update wrote only the factory app at
`0x20000`, and esptool verified the written app hash. A full readback of that
final update was not compared with the original backup; the byte-for-byte
preservation statement above applies to the earlier readback.

The first flashed build reached the LCD but stalled inside LVGL's flush
wait and triggered the task watchdog. Adding `lv_display_set_flush_wait_cb`
fixed that measured failure. The next run showed repeated CST816S I²C
errors during idle polling; the controller's IRQ now starts reads, and
polling continues only while a press is active. The 65-second run of this
image logged no touch errors and the user confirmed controls work after idle.
All these tests used USB power without a battery.

### Final command record

On 2026-10-17, `idf.py set-target esp32s3 build` exited **0**; subsequent
incremental builds, including the final telemetry change, also exited **0**.
A separate fresh-directory build was started but stopped on the user's
instruction, so it is not counted as a completed final-source clean build.
The generated `sdkconfig` confirmed the 16 MB flash setting, Octal PSRAM
at 80 MHz, 8 KiB main-task stack, and PSRAM XIP disabled. Build
`compile_commands.json` retained `-Werror`; the build log contained
zero warnings. The component manager generated the versions above in
`dependencies.lock`. `python -m esptool image-info build/roundie.bin`
reported ESP32-S3, 16 MB flash, project `roundie`, version `0.1.0`,
and ESP-IDF `v6.1`. This inspects the built image, not the physical chip.

| Generated artifact | Size | SHA-256 |
|---|---:|---|
| `build/roundie.bin` | 569,568 bytes | `888691831e3eb4e8612ab7061b2a3da926f7186ea44b7880f131439751ac3585` |
| `build/bootloader/bootloader.bin` | 21,120 bytes | `c7a70b9af9d599095d524cac280838544d074bbd088f9a2e88d483aed95df26d` |
| `build/partition_table/partition-table.bin` | 3,072 bytes; generated, not flashed | `7f00b6c042a89b15b0cac534f82ed988caf29278ff5700b0c511eb1b5bb7c820` |

The host check compared both complete C table declarations with the
manufacturer's inspected commit and found exact matches. The legacy-driver
scan covered `main/` and `components/` C/H sources; it is a source check,
not a live I²C-bus measurement.

## Manual board checklist

Run these only on an identified 1.85B unit after flashing is separately
authorized. Preserve the existing image and any user data first. Record
revision/markings, supply and battery state, date, firmware hash, monitor
port, logs, and each pass/failure. Do not erase partitions or change eFuses.

1. Photograph both PCB sides. Compare the printed revision and LCD/TE R5
   population with Rev1.1. Stop pin validation if the revision differs and
   obtain its schematic.
2. Perform **10 cold starts** with USB power; repeat with battery where
   available. Check boot completion, no panics, and stable heap diagnostics.
   Record detected 16 MB flash and 8 MB Octal PSRAM, free and largest blocks.
3. Confirm the three 7-bit I²C probes at 0x15, 0x51, and 0x55, with no bus
   lockups. Record other expected device addresses separately with a safe
   scan if needed; do not configure the audio codecs in this milestone.
4. Check RTC time reads at boot, voltage-low flag, and behavior after power
   removal under USB-only, battery-only, and combined power. Time retention
   is a separate outcome from a successful read.
5. Check gauge voltage and status reads under USB-only and battery power.
   Compare voltage with an external meter when safe. Do not infer state of
   charge or calibration accuracy from an ACK or voltage value.
6. Inspect LCD orientation and touch coordinate mapping. Tap center and
   four reachable edge regions, then release each touch. The screen and log
   must show coherent press/release and no inverted or stuck coordinates.
7. Tap each color swatch and inspect the full red, green, blue, and white
   fields for channel order, missing rows, artifacts, and rotation. Return
   by tapping the field. Observe spinner motion and GUI response.
8. Run at least **30 minutes** of interaction and changes through the
   backlight button. Record
   LCD/I²C transfer errors, GUI stalls, heap trend, and any reboot. The gate
   requires zero logged transfer errors and stable visual output.
9. With the image running, exercise manual ROM Download: switch power off,
   hold BOOT (GPIO0), reapply USB power, release BOOT, and use a read-only
   chip-identity command such as the following, replacing `PORT` with the
   enumerated serial port:

   ```sh
   python -m esptool --chip esp32s3 --no-stub --before no-reset --after no-reset -p PORT chip-id
   ```

   Record enumeration and console/debug path. Repeat
   with battery if that use is intended. Do not write or erase flash.

## Known blockers and pending experiments

- **Device gate open:** the USB-only device established boot, LCD variant 2,
  touch, and memory detection, but did not establish battery-dependent
  behavior, RTC/gauge reliability, manual ROM recovery, ten cold starts, or
  the 30-minute visual/interaction criterion. Printed PCB revision and R5/TE
  population could not be inspected.
- **LCD identity:** the tested unit returned `00 02 7f 7f`. The other
  documented ID `00 7f 7f 7f` remains source-supported but untested here.
- **LCD timing/buffering:** measure 3 MHz flush latency, GUI response, DMA
  safety, and tearing during the 30-minute test; only then choose clock,
  strip height, and whether to move to two buffers. Check R5/TE electrically
  before considering synchronization.
- **Power/recovery:** test the BOOT/PWR procedure with USB and battery,
  including a confirmed independent debug path. Schematic inference is
  not a recovery result.
- **RTC/gauge:** RTC responded once with a voltage-low indication but did not
  ACK on several subsequent USB-only starts. The gauge did not ACK without a
  battery. Repeat under controlled power conditions and measure I²C levels
  if intermittent RTC behavior persists; do not infer a software-only cause.
