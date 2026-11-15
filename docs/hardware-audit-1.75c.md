# Roundie hardware audit: ESP32-S3-Touch-AMOLED-1.75C

**Date:** 2026-11-15
**Scope:** source and schematic audit for the second board implementation
(`components/roundie_board/boards/waveshare_s3_touch_amoled_1_75c/`).
**Status:** hardware-ready, hardware-unverified.
**Evidence level:** desktop inspection and target builds only. No 1.75C was
connected and nothing was flashed. Every physical behaviour below is PENDING.

Terms as in [hardware-audit.md](hardware-audit.md): **documented fact** is shown
by a source listed at the end, at the stated revision; **hypothesis** is an
unverified working premise; **pending** names a device check and its pass
condition.

## Sources

| Id | Source | Revision / hash |
|---|---|---|
| S1 | Waveshare wiki page "ESP32-S3-Touch-AMOLED-1.75C" (Onboard Resources, Features) | Fetched 2026-11-15 |
| S2 | `ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf` from `waveshareteam/ESP32-S3-Touch-AMOLED-1.75C` | Repo commit `4bfdc80e` (2026-10-20); PDF SHA-256 `7bcc0a3a9dc02893741e1e555b203a17055b5cc9cce3b5c548e160e21683900d` |
| S3 | Waveshare BSP `waveshare/esp32_s3_touch_amoled_1_75c` 3.0.0 (Apache-2.0) | Upstream commit `010c5fb3`; archive SHA-256 `23542dfada54a5f7345026d4f09adbd07f517627f980ac4d6d1f3f75767e655e` |
| S4 | `espressif/esp_lcd_co5300` 2.2.0, `waveshare/esp_lcd_touch_cst9217` 2.0.0 | Pinned in `components/roundie_board/idf_component.yml` |
| S5 | Waveshare factory image `ESP32-S3-Touch-AMOLED-1.75C_FactoryOnly_20260114.bin` (33,488,896 bytes) | SHA-256 `46e65fb708f325f6830d6f14ecbba3d357dbdab6a939e13fdf2206e147c063d2`; same repo as S2 |

## Board facts

| Item | Documented fact | Source |
|---|---|---|
| SoC and memory | ESP32-S3R8 (8 MB stacked PSRAM). Flash: 32 MB per S1 and S5, but S2 names a 16 MB part (see below) | S1, S2, S5 |
| Display | 1.75" 466 × 466 AMOLED, CO5300 on QSPI: SCLK 38, D0–D3 4–7, CS 12, RST 1. TE on 13 is not used. Column gap 6. No backlight; brightness is DCS 0x51 | S3, S4 |
| Touch | CST9217 at I²C 0x5A, INT 11, RST 2. No pull-up on TP_INT on the main board (J3 pin 30 straight to GPIO11) | S2, S3 |
| I²C | One bus, SDA 15, SCL 14, with external 2.2 kΩ pull-ups (R23, R24): touch, ES8311 (0x18), AXP2101 (0x34), QMI8658 IMU (0x6B) | S2, S3 |
| Audio | ES8311 codec, I²S MCLK 16, BCLK 9, WS 45, DOUT 8; amplifier enable GPIO46. ES7210 microphone ADC (DIN 10) not used | S1, S3 |
| Power | AXP2101 PMIC, MX1.25 battery connector. The PWR button (Key2) pulls the AXP2101's PWRON low and also drives `SYS_OUT` on GPIO3 through a BSS138 inverter (T1, 10 kΩ pull-up R9), so GPIO3 should read high while the button is held | S1, S2 |
| Buttons | BOOT on GPIO0 (ROM Download strap) | S1, S2 |

The pins in `board_pins.h` were compared with S3's `esp32_s3_touch_amoled_1_75c.h`
and match. GPIO0 stays an input; no PSRAM or Flash pin is used.

S2's pin table puts `QMI_INT1` in the GPIO38 row (the LCD's QSPI clock), but
the net label beside the IMU ties `QMI_INT1` to GPIO21. The table row is
probably shifted. The firmware never configures the IMU, so its interrupt
outputs stay at their reset state. Nothing may enable QMI8658 INT1 until the
wiring is checked on the board.

GPIO3 (`SYS_OUT`, the PWR button) is unused. Reading it is a possible later
feature and needs device test D12 first. GPIO3 is a strapping pin only for the
JTAG source eFuse option, which Roundie does not set.

## No external RTC (PCF85063)

**Conclusion:** the 1.75C has no external RTC chip. `ROUNDIE_BOARD_CAP_RTC` is
clear and the wall clock uses the RTC-timer path
([rtc-time-sync.md](rtc-time-sync.md#boards-without-an-external-rtc)).

Evidence:

- S1's Onboard Resources list names ESP32-S3R8, Flash, QMI8658, ES7210,
  ES8311, AXP2101, the buttons and connectors. It lists no RTC.
- S2 (page 1 is the circuit; pages 2 and 3 are the PCB top and bottom
  silkscreen) has exactly seven ICs, and none is an RTC:
  - U1 ESP32-S3R8, U2 XM25QH128DHIQT Flash, U3 QMI8658 IMU;
  - U6 ES8311 codec, U7 NS4150B amplifier, U8 ES7210 microphone ADC;
  - UP1 AXP2101 PMIC.

  The PCB silkscreen shows the same designators and no other IC footprint.
  The "RTC" strings in S2 are the AXP2101's own backup outputs (`RTCLDO`
  → test point `VCC-RTC`, `VBACKUP` → `VBAT2`) and the ESP32-S3's
  `VDD3P3_RTC` supply pin.
- S3 has no RTC driver, address or dependency.
- **No 32 kHz crystal.** In S2, U1 pins 22 and 23 (XTAL_32K_P/N) go to the
  nets GPIO15 and GPIO16. These carry I²C SDA and I²S MCLK (S3). The only crystal
  is X1, 40 MHz, on XTAL_P/N. The RTC slow clock is the internal RC (`CONFIG_RTC_CLK_SRC_INT_RC`).

One contrary hint: a comment in S3's public header says the I²C bus is shared
by "touch, audio, IMU, RTC, and power-management devices". Nothing else in S3
matches it, so it is probably copied from another Waveshare board.
**Pending:** an I²C scan on the device must show no ACK at 0x51.

## Flash size: conflicting sources

| Source | Says |
|---|---|
| S1 (wiki) | 32 MB NOR Flash |
| S2 (schematic) | U2 = XM25QH128DHIQT, 128 Mbit = 16 MB |
| S5 (factory image) | Image header size code 5 = 32 MB. The partition table runs to 0x1FF0000 (`storage` ends there), which only fits a 32 MB chip |

Firmware that needs 32 MB is shipped for this board, so a 32 MB chip on
production units is the likely case. The schematic's part number is probably
out of date. This is not verified on our unit.

**Decision (revised 2026-11-15):** build the image with
`CONFIG_ESPTOOLPY_FLASHSIZE_16MB` and `ROUNDIE_BOARD_FLASH_BYTES` = 16 MiB on
either chip. The first decision, a 32 MB header, was withdrawn before any
flash.

Why. ESP-IDF v6.1 (`components/spi_flash/esp_flash_spi_init.c`,
`esp_flash_init_default_chip`) compares the detected chip with the header:
- A chip **smaller** than the header fails the Flash probe
  (`ESP_ERR_FLASH_SIZE_NOT_MATCH`), and the application does not start.
- A chip **larger** than the header only logs a warning. The header size is
  then used, and `esp_flash_get_size()` returns it.
- A header above 16 MB also needs the 32-bit address feature check and
  `CONFIG_BOOTLOADER_FLASH_32BIT_ADDR`.

Both partition layouts end at 16 MiB, so a 32 MiB header gained nothing. A 16
MiB header boots on both possible chips. On a 32 MB chip the OTA self-test's
size check still passes, because IDF reports the header size. The upper 16 MiB
stays unused and unreachable.

The pre-flash `flash-id` read-out is still required, to record the actual chip.

**Installation gap.** No installer supports this board yet:
- `tools/flash_rep.py` updates an existing REP layout. It refuses a chip whose
  ID does not say 16 MiB, and a bootloader or partition table other than
  Roundie's.
- `tools/flash_bringup.py` is tied to the 1.85B's backup.

Both refuse; neither writes. A first install on the 1.75C needs a reviewed
installer with a full read-out first. That is part of the first flash, not
of this port.

## AXP2101 (read only)

**Decision:** the firmware never writes to the PMIC. A wrong write can switch
off a rail that powers the SoC, Flash or panel. Power-off, PWR-button handling
and charger configuration stay out of scope.

**Hypothesis** (register map from XPowersLib, `src/XPowersAXP2101.hpp` and
`src/REG/AXP2101Constants.h` at commit `17132a58`; not in S1–S3 and not
checked on the board). `roundie_board_battery_read()` uses:

| Register | Meaning used |
|---|---|
| 0x00 bit 3 | Battery connected |
| 0x01 bits 6..5 | 01 charging, 10 discharging |
| 0x34..0x35 | Battery voltage in mV, 13 bits (XPowersLib `getBattVoltage()`: `readRegisterH5L8`, `(h & 0x1F) << 8 \| l`). Reads 0 while the battery ADC is disabled, which gives `voltage_valid = false` |

The state is at best `ROUNDIE_SENSOR_UNCALIBRATED`. There is no state of
charge, current or temperature, and the PMIC's fuel gauge is not used.

## Implementation decisions and assumptions

- **Display.**
  - Init sequence from S3, except that brightness starts at 0, so the panel
    is never shown at full brightness before the firmware sets it. The
    board stores the last brightness and applies it again after every
    re-initialization.
  - 40 MHz pixel clock as in S3 (assumption, not measured).
  - The 360 × 360 canvas is upscaled per strip
    ([display-rendering.md](display-rendering.md#scaled-panels)). Draws are at
    most 466 × 24 pixels, with two queued.
- **Panel restore.** Same policy as the 1.85B: reset, initialize, repaint
  (`*repaint = true`). It is safe whether or not the CO5300 keeps its memory
  after DISPOFF or light sleep, but S3's 600 ms delays make it take about
  1.2 s. Shortening it needs a device test.
- **Recovery start.** It starts no I²C, so the panel relies on the AXP2101
  power-on defaults (assumption).
- **Touch.**
  - Mirrored on both axes, with `x_max`/`y_max` 466 as in S3 (assumption).
    The driver mirrors as `466 - x`, so a raw 0 arrives as 466. The board
    clamps it to 465, and so does the canvas mapping.
  - The CST9217 driver returns `ESP_ERR_INVALID_RESPONSE` for a report without
    the 0xAB acknowledge byte, and keeps its previous data. S3 reads through
    the LVGL adapter, which then reports the old point again, so a held touch
    stays held.
  - Roundie does the same, but bounded: up to 6 consecutive unacknowledged
    reads (about 200 ms at LVGL's 33 ms read period) keep the last point;
    then the touch is released. An unacknowledged read never starts a touch
    and never triggers the 5 s back-off.
  - Earlier this port released the touch at once, which could make a held
    finger chatter. The limit is an assumption (`board_logic.h`).
  - INT gets the internal pull-up, because the main board has none and the
    controller's output type is unknown.
- **Flash.**
  - `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` on either chip, the same as the 1.85B.
    There is no 32-bit addressing in the bootloader or the application.
  - Both partition layouts end at 16 MiB.
  - The OTA self-test expects `esp_flash_get_size()` to report 16 MiB.
- **Unused hardware.** QMI8658, ES7210, the PWR button (also on GPIO3) and TE
  are unused.

## Verified so far (host and build only)

- Target builds pass with 0 warnings: 1.75C legacy and REP; 1.85B legacy and
  REP.
- The 1.85B sdkconfig and partition table are identical to the build before
  this port. Its loadable ELF sections are the same size; only debug info
  differs. Its bootloader differs only in the compile time and checksum.
- The 1.75C image and bootloader headers say 16 MB at 80 MHz, without 32-bit
  addressing. The offline REP validator accepts both boards' REP builds.
- Host tests: the touch filter and the AXP2101 decoding (`board_logic.c`),
  the canvas touch mapping, the scaled flush path, and resets and power loss
  without an RTC. A short mutation check broke each new rule in turn, and the
  tests failed every time except once. That one change (counting
  unacknowledged reads while idle) has no visible effect.
- The scaled flush path and its host harness:
  [display-rendering.md](display-rendering.md#scaled-panels).

## Pending device tests

**Before the first flash:**

- Explicit approval to flash.
- `esptool flash-id`: record the chip size. 16 MB and 32 MB are both
  acceptable (see "Flash size" above).
- An installer for this board (see "Installation gap").
- A full read-out of the unit's own Flash, kept with its SHA-256.
  S5 is a reference image, not a backup of this unit.

| # | Area | Pass condition |
|---|---|---|
| D1 | Boot | 8 MB PSRAM detected. With a 32 MB chip, the "larger than the header" warning; `esp_flash_get_size()` 16 MiB. The OTA self-test confirms (REP) |
| D2 | I²C scan | ACK at 0x5A, 0x34, 0x18. No ACK at 0x51 (no RTC) |
| D3 | Display | Full canvas shown, centred, no edge columns missing (gap 6). No tearing or stale strips. `transfer_errors` 0 |
| D4 | Scaling cost | `scale_us`, `chunks` and DMA busy time recorded in the PERF trace for a full frame and for a small area |
| D5 | Touch | Taps at the four edges of the safe area land where touched (confirms mirroring). A 3 s press and a slow drag log one press and one release. Count the unacknowledged reads during a hold (temporary log) to check the limit of 6 |
| D6 | Brightness | 30/60/100 % steps visible. 0 % on screen-off. Brightness kept after the screen is restored |
| D7 | Screen off/on, light sleep | Panel restored with a full repaint. Wake latency measured. Touch and BOOT wake work |
| D8 | Battery | With a battery: present = 1, plausible voltage (3.0–4.3 V), charge direction follows USB plug/unplug. Without one: `BATTERY_ABSENT` |
| D9 | Audio | `tools/roundie_audio.py status`: `step ok`, `codec_ack` 1. Tone audible (person) |
| D10 | Wall clock | The tests in [rtc-time-sync.md](rtc-time-sync.md#boards-without-an-external-rtc) |
| D11 | ROM Download | Hold BOOT, reset: the ROM serial loader is reachable over USB |
| D12 | Schematic details | GPIO3 high only while PWR is held. GPIO11 idles high with the pull-up. The QMI8658 answers at 0x6B, and GPIO38 shows no activity from it |
