# Milestone 1: ESP-IDF v6.1 bring-up

**Board target:** User-identified Waveshare ESP32-S3 Touch-LCD-1.85B; the
printed PCB revision was not visible. The documented Rev1.1 pinout was the
working premise, and actual LCD/touch behavior is recorded below.

**Evidence scope:** Source integration, target compilation, USB-only device
tests on 2026-10-17, and two short USB-plus-battery reboot logs on
2026-10-18. Battery-only operation, cold-start, long-run, and manual recovery
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

**Do not use `idf.py flash` or `idf.py app-flash` on the inspected device.** Its existing
partition table differs from this temporary single-app build table, and
the default flash command would rewrite that table; both generated commands
place the app at `0x10000`. The board's read-back table has NVS at `0x9000`, OTA data at
`0xd000`, factory app at `0x20000`, another app at `0x6c0000`, and
assets at `0xac0000`. The earlier authorized bring-up installation wrote
the bootloader at `0x0` and app at `0x20000`; it left the installed
partition table and data partitions in place. This is a device-specific
installation method, not a final REP partition design.

### Guarded deployment for this diagnostic image

**Architectural decision for Milestone 1:** Keep the generated single-app
table solely for target compilation. For the already inspected 1.85B, update
only its existing factory slot at `0x20000`; do not change the installed
partition table, bootloader, OTA metadata, NVS, or other data. This is not
the REP production layout: spec §§15, 18, 23, and 25 require a later,
separately reviewed two-slot OTA layout without a factory application.

**Verified source/backup facts:** The original 16 MiB backup named below has
SHA-256 `46d2d36244a9e303cc61a65fb29d9a71b9708d41d181e9e5b5befb9261039de7`.
Its installed table has factory offset `0x20000`, size `0x692000`, one OTA
slot at `0x6c0000`, and assets at `0xac0000`; its OTA metadata is erased.
The pinned IDF v6.1 bootloader's `bootloader_utility.c` defaults to the
factory image when both OTA records are empty and a factory slot exists.
The current generated `build/flasher_args.json` instead writes a single-app
table at `0x8000` and an app at `0x10000`. The earlier device installation
already installed a v6.1 bootloader. Its 21,120-byte image in the verified
post-flash readback has SHA-256
`c7a70b9af9d599095d524cac280838544d074bbd088f9a2e88d483aed95df26d`.
The current clean build differs in five header bytes and the final image
digest; its code region matches. The guarded updater checks that known
installed image and code compatibility on the live device before any write.

After a successful `idf.py set-target esp32s3 build`, with the v6.1
environment exported, use this read-only preflight on the specific inspected
unit and its preserved backup:

```sh
python tools/flash_bringup.py --port /dev/ttyACM0
```

Under a separately authorized device-flash operation, the same command with
`--write` repeats preflight and writes only `build/roundie.bin` to the
installed factory slot:

```sh
python tools/flash_bringup.py --port /dev/ttyACM0 --write
```

**Fail-closed checks:** The script verifies the original backup's size and
hash, reads only the live partition-table sector, two OTA-metadata sectors,
and the bootloader image range, then requires an exact table match, erased
OTA metadata, the known installed bootloader and matching code region, the unchanged diagnostic
flash manifest, and an app whose sector-rounded size fits the factory slot.
It does not perform a full-Flash readback. A failed check stops deployment;
do not bypass it by using IDF's printed flash command. A different table,
active OTA selection, or unrecognized bootloader requires a reviewed deployment proposal.
No repartitioning or image relocation is part of this milestone. A passing
preflight is a layout check, not proof of boot or recovery behavior.

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

**Architectural decision for P1 supervision:** ESP-IDF initializes the task
watchdog at boot with a 10-second timeout and panic/reboot policy. The GUI
task subscribes after setup and feeds from its own loop only after
`lv_timer_handler()` and health logging return. A panel-I/O call that blocks
inside `esp_lcd_panel_draw_bitmap()` therefore cannot be masked by another
task feeding the watchdog. The existing two-second completion wait restarts
on timeout before LVGL can reuse the DMA buffer. On watchdog panic, the
whole device reboots; no task attempts to free or reuse an in-flight buffer.
Ten seconds gives substantial margin over one 14,400-byte transfer at the
configured 3 MHz QSPI clock, but the actual worst-case GUI path and recovery
on the physical board remain pending measurements.

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

### P1 remediation validation, 2026-10-17

| Evidence tier | Result and limit |
|---|---|
| Host/source | Inspected the checked touch callback registration and failure cleanup, bus-error return path, GUI-only watchdog feed, and generated flashing offsets. No legacy I²C/I²S include or `i2c_bus_create` use was added to project sources. Offline preflight accepted the recorded post-flash layout and rejected a changed table, non-erased OTA metadata, and an unrecognized bootloader. These checks do not simulate an actual board failure. |
| ESP32-S3 target compilation | `idf.py set-target esp32s3 build` exited 0 after regenerating config; generated `sdkconfig` has `CONFIG_ESP_TASK_WDT_PANIC=y` and a 10-second timeout. An incremental `idf.py build` after the final touch cleanup change also exited 0. Final `build/roundie.bin` is 569,968 bytes, SHA-256 `0c34a13a8ab047493e8138262b99f43b97be61e6303da00366620832a1cd7ceb`. IDF emitted Kconfig/CMake notices during the clean build; no project compiler error was reported. |
| Physical device | **Not tested for these changes.** No preflight was run against the live unit and no firmware was flashed. Bus-creation/IRQ failure behavior, worst-case LCD timing, watchdog reset, boot, and recovery remain unverified. |
| Long-running/fault injection | Not run for this remediation. The original pending Milestone 1 checks remain pending. |

### Final autonomous validation, 2026-10-17

The user first requested repository-only validation, then explicitly
authorized application-only deployment and an unattended USB smoke test.
The guarded deployment procedure below was used after that authorization.
No manual board or touch interaction, full-Flash readback, or endurance test
was performed.

| Evidence tier | Result and limit |
|---|---|
| Source/host — I²C | `start_i2c()` returns the bus-creation error, records it for the three dependent devices, and `roundie_board_start()` releases partial resources before returning that error. Optional probe failures remain per-device status after a successful bus start. Project sources use `driver/i2c_master.h`; a scan found no legacy I²C/I²S include or legacy controller call in `main/` or `components/`. Forced bus failure was not run on hardware. |
| Source/host — CST816S | The constructor receives a null callback; Roundie then checks `esp_lcd_touch_register_interrupt_callback()` and deletes the touch and IO handles if it fails. `roundie_board_stop()` deletes the touch handle before its IO and I²C bus. Touch on this new image, ISR failure, and repeated teardown remain untested on hardware. |
| Source/host — GUI watchdog | The GUI/main task registers with Task WDT after setup and feeds only after `lv_timer_handler()` returns. Generated config enables panic and a 10-second timeout. The two-second LCD completion wait and restart-before-buffer-reuse path remain in place. LCD timing, false resets, and actual watchdog recovery on this image are pending device tests; no artificial hang was introduced. |
| Source/host — safe deployment | The original 16 MiB backup still hashes to `46d2d36244a9e303cc61a65fb29d9a71b9708d41d181e9e5b5befb9261039de7`. Its parsed table has NVS `0x9000`, OTA metadata `0xd000`, factory app `0x20000` size `0x692000`, OTA app `0x6c0000`, and assets `0xac0000`. Offline `preflight()` accepted the saved post-flash table/bootloader and current build, then rejected a changed table, active OTA metadata, unknown installed bootloader, incompatible rebuilt bootloader code, unsupported flash manifest, invalid app header, oversized app, and changed backup. Mocked `main()` made three bounded reads and no write by default; mocked `--write` issued one factory-app write at `0x20000`. Ordinary `idf.py flash` and `idf.py app-flash` remain prohibited for this installed layout. |
| ESP32-S3 target build | One clean `. /home/solve/.espressif/v6.1/esp-idf/export.sh` then `idf.py set-target esp32s3 build` exited **0** on IDF commit `fff9895c82d744c7237be8847347bdd1b07c6643`. `image-info` reports project `roundie`, version `0.1.0`, IDF `v6.1`, ESP32-S3, and 16 MB Flash. The app is **569,968 bytes**, SHA-256 `c8e141a5a9fc478f42572f6a50b5cadf253c77726a39bea9ad528b5c022387f5`, unchanged in size from the prior P1 build. The bootloader is 21,120 bytes, SHA-256 `04fb63e6d82c1adc7a153ee8168bb9de9f4ea90a565631474c46d3962d4213ec`; its code region passed the updater's compatibility check against the installed bootloader. The log has **zero compiler warnings and zero errors**; it has five ESP-IDF `esp_wifi`/`wpa_supplicant` component-validation CMake warnings and IDF Kconfig notices. `dependencies.lock` still pins the listed component revisions. This is target compilation, not device validation. |
| Physical device — identity and deployment | The connected USB-Serial/JTAG device was ESP32-S3 revision v0.2, MAC `28:84:85:87:98:7c`, with detected 16 MB Flash and 8 MB embedded PSRAM. A live `python tools/flash_bringup.py --port /dev/ttyACM0` preflight exited **0** after bounded reads of the table, OTA metadata, and bootloader; it matched the inspected layout, erased OTA metadata, and known compatible bootloader. The separately authorized `--write` repeated preflight, then esptool erased only `0x20000–0xabfff`, wrote 569,968 app bytes at `0x20000`, and reported **“Hash of data verified”** before resetting. No command wrote the bootloader, partition table, NVS, OTA metadata, OTA app, or assets; their post-write bytes were not independently read back. The preserved full-Flash backup was not modified. |
| Physical device — unattended serial smoke | A single approximately five-minute `idf.py monitor` capture after deployment recorded one boot of `roundie` 0.1.0 / IDF v6.1, 16,777,216 B Flash, 8,388,608 B PSRAM, CST816S ID 181 and driver `ESP_OK`, LCD ID `00 02 7f 7f`, and ST77916 variant 2 initialization. At 301,604 ms, the GUI loop logged internal SRAM free/largest **284,575/204,800 B**, PSRAM free/largest **8,384,620/8,257,536 B**, and **0 LCD errors**. The capture contained no LCD transfer error, task-watchdog report, panic, or reboot. Its startup probe reported RTC and gauge `ESP_ERR_NOT_FOUND` under USB power; the early touch probe also NACKed before the driver reset and successful ID read. One telemetry sample and serial output cannot establish visual fields, spinner motion, touch/release, worst-case LCD timing, or a long-term heap trend. |

**Pending on the remediated image:** human inspection of LCD color fields,
spinner motion and GUI response, plus center/four-edge touch and release.
Manual ROM Download with read-only chip identification is pending with USB
power and, if available, battery power. RTC validity, battery-only behavior,
gauge calibration, ten controlled cold starts, PCB revision inspection, and the
separately authorized 30-minute visual/interaction criterion also remain
open. These checks require later physical access; Milestone 1 is not closed,
and this record does not authorize starting Milestone 2.

### Battery-connected RTC and gauge check, 2026-10-18

**Power condition:** The user reported a battery connected while USB stayed
connected. The USB-Serial/JTAG device remained `/dev/ttyACM0`. Two short
`idf.py -p /dev/ttyACM0 monitor` captures each performed a normal software
reset and recorded one boot of the same `roundie` 0.1.0 image (ELF SHA-256
prefix `afdcf3ed2`). No firmware rebuild, flash write, I²C configuration
write, storage operation, or endurance test was performed.

| Check | Earlier USB-only observation | USB plus battery, two fresh boots |
|---|---|---|
| PCF85063ATL RTC at `0x51` | ACKed once with the flagged time, then NACKed on several starts; the latest 2026-10-17 boot logged `ESP_ERR_NOT_FOUND`. | Probe and seven-byte time read returned `ESP_OK` twice. The firmware logged `01:52:10` then `01:53:40`, both with `voltage_low=1`. Communication and an advancing displayed value are verified; wall-clock validity and retention are not. |
| BQ27220 at `0x55` | Did not ACK without a battery; no voltage/status value was available. | Probe and read-word commands returned `ESP_OK` twice. Both boots reported `3970 mV` and `BatteryStatus() = 0x5028`. This is a device-reported voltage, not an independently metered or calibrated value. |
| Shared-bus context | The early CST816S probe may NACK while the controller is still in reset. | The early `0x15` probe again NACKed, then the touch driver read ID 181 and initialized with `ESP_OK` on both boots. No bus error, panic, or watchdog report appeared in either short log. No touch interaction was performed. |

The firmware field `voltage_low` reads bit 7 of the RTC seconds register.
For the schematic's PCF85063ATL, [NXP's PCF85063A datasheet](https://www.nxp.com/docs/en/data-sheet/PCF85063A.pdf)
defines that bit as **OS (oscillator stop)**: when set, clock integrity is
not guaranteed. The observed time must not be used as a valid date/time;
the flag was neither cleared nor was the clock set during this test.

Per [TI's BQ27220 reference manual](https://www.ti.com/lit/ug/sluubd4a/sluubd4a.pdf),
`Voltage()` at `0x08` reports measured millivolts. `0x5028` in
`BatteryStatus()` at `0x0A` has `OCVCOMP`, `SLEEP`, `OCVGD`, and `BATTPRES`
set. `BATTPRES` corroborates the reported battery attachment, and the
voltage is plausible, but these bits do not prove fuel-gauge configuration,
capacity calibration, or state-of-charge accuracy. The present firmware
does **not** read `StateOfCharge()` at `0x2C` or `OperationStatus()` at
`0x3A`; neither a percentage nor the gauge's `INITCOMP` state was captured.
Battery-only operation, external voltage comparison, RTC retention/setting,
and gauge configuration/SoC validation remain pending.

### Milestone 1 device checklist status

| Check | Status | Evidence and limit |
|---|---|---|
| Diagnostic LCD, orientation, color fields, spinner | **PASSED for this unit** | User observed the page, correct red/green/blue/white fields, and moving spinner; LCD ID `00 02 7f 7f` selected variant 2. |
| Touch center and four reachable edge regions, release, controls after idle | **PASSED** | User explicitly confirmed all five regions and release, and controls after at least five seconds idle. The 65-second log recorded 52 press/release pairs without touch-read errors. |
| Flash and PSRAM detection | **PASSED for detection** | Device reported 16,777,216 B Flash and 8,388,608 B PSRAM; startup heap values were logged. Executable PSRAM and memory stress are outside this gate. |
| Shared I²C devices | **PARTIAL** | Touch driver read ID 181. RTC and BQ27220 both ACKed and returned data on two USB-plus-battery reboots. RTC OS flag remains set, gauge SoC/calibration is unverified, and battery-only behavior was not tested. |
| Ten cold starts; RTC retention; battery-only operation; manual ROM Download | **PENDING** | Two software resets with USB and battery are not cold starts or retention tests. No battery-only run or physical BOOT procedure was performed, and the PCB revision could not be inspected. |
| 30-minute interaction and heap trend | **PENDING** | A separate five-minute unattended run logged one health sample and zero LCD errors; the 30-minute visual/interaction run and a heap trend remain untested. |

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

- **Device gate open:** USB-only testing established boot, LCD variant 2,
  touch, and memory detection. Two USB-plus-battery reboots established RTC
  and gauge I²C communication, but not valid RTC time, fuel-gauge accuracy,
  battery-only operation, manual ROM recovery, ten cold starts, or the
  30-minute visual/interaction criterion. Printed PCB revision and R5/TE
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
- **RTC/gauge:** The USB-only RTC intermittently NACKed and the gauge never
  ACKed without a battery. With USB and battery, both ACKed and returned data
  on two resets, but the RTC oscillator-stop flag remained set and gauge SoC
  was not read or calibrated. Test battery-only operation and RTC retention;
  compare voltage with an external meter when available. If intermittent
  RTC behavior persists under controlled power, measure I²C levels rather
  than assuming a software-only cause.
