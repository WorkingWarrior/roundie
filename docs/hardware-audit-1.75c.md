# Roundie hardware audit: ESP32-S3-Touch-AMOLED-1.75C

**Date:** 2026-11-15
**Scope:** source and schematic audit for the second board implementation
(`components/roundie_board/boards/waveshare_s3_touch_amoled_1_75c/`).
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
| Touch | CST9217 at I²C 0x5A, INT 11, RST 2 | S3 |
| I²C | One bus, SDA 15, SCL 14: touch, ES8311 (0x18), AXP2101 (0x34), QMI8658 IMU | S2, S3 |
| Audio | ES8311 codec, I²S MCLK 16, BCLK 9, WS 45, DOUT 8; amplifier enable GPIO46. ES7210 microphone ADC (DIN 10) not used | S1, S3 |
| Power | AXP2101 PMIC, MX1.25 battery connector. The PWR button goes to the AXP2101, not to a GPIO | S1, S2 |
| Buttons | BOOT on GPIO0 (ROM Download strap) | S1, S2 |

The pins in `board_pins.h` were compared with S3's `esp32_s3_touch_amoled_1_75c.h`
and match. GPIO0 stays an input; no PSRAM or Flash pin is used.

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

**Decision:** keep `CONFIG_ESPTOOLPY_FLASHSIZE_32MB` and
`ROUNDIE_BOARD_FLASH_BYTES` = 32 MiB. The pre-flash read-out (below) reports the
chip size, so the question is settled before the first write.

If the chip is 16 MB:
- A 32 MB image header would stop at boot ("detected size smaller than the
  header"). ROM Download still recovers the board.
- The fix is `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` and 16 MiB in
  `roundie_board_config.h`. The layouts fit 16 MiB already.

## AXP2101 (read only)

**Decision:** the firmware never writes to the PMIC. A wrong write can switch
off a rail that powers the SoC, Flash or panel. Power-off, PWR-button handling
and charger configuration stay out of scope.

**Hypothesis** (register map from XPowersLib; not in S1–S3 and not checked on
the board). `roundie_board_battery_read()` uses:

| Register | Meaning used |
|---|---|
| 0x00 bit 3 | Battery connected |
| 0x01 bits 6..5 | 01 charging, 10 discharging |
| 0x34..0x35 | Battery voltage in mV, 14 bits. Reads 0 while the battery ADC is disabled, which gives `voltage_valid = false` |

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
  - Mirrored on both axes, as in S3 (assumption).
  - A report without the 0xAB acknowledge byte counts as "no touch", not
    as a bus error, so it does not trigger the 5 s touch back-off
    (assumption).
- **Flash.**
  - `CONFIG_ESPTOOLPY_FLASHSIZE_32MB`, with IDF's default
    `CONFIG_BOOTLOADER_FLASH_32BIT_ADDR=y`.
  - Both partition layouts still end at 16 MiB.
  - The experimental 32-bit cache map is off.
  - The OTA self-test expects `esp_flash_get_size()` to report 32 MiB.
- **Unused hardware.** QMI8658, ES7210, the PWR button and TE are unused.

## Verified so far (host and build only)

- Target builds pass with 0 warnings: 1.75C legacy and REP; 1.85B legacy and
  REP.
- The 1.85B sdkconfig is identical to the previous build. Its loadable ELF
  sections are byte-for-byte the same size; only debug info differs.
- The offline REP validator accepts the 1.75C REP build (32MB chip, 16 MiB
  layout).
- The scaled flush path and its host harness:
  [display-rendering.md](display-rendering.md#scaled-panels).

## Pending device tests

**Before the first flash:**

- Explicit approval to flash.
- `esptool flash-id` must report a 32 MB chip (see "Flash size" above).
- A full read-out of the unit's own Flash, kept with its SHA-256.
  S5 is a reference image, not a backup of this unit.

| # | Area | Pass condition |
|---|---|---|
| D1 | Boot | 32 MB Flash and 8 MB PSRAM detected. The OTA self-test confirms (REP) |
| D2 | I²C scan | ACK at 0x5A, 0x34, 0x18. No ACK at 0x51 (no RTC) |
| D3 | Display | Full canvas shown, centred, no edge columns missing (gap 6). No tearing or stale strips. `transfer_errors` 0 |
| D4 | Scaling cost | `scale_us`, `chunks` and DMA busy time recorded in the PERF trace for a full frame and for a small area |
| D5 | Touch | Taps at the four edges of the safe area land where touched (confirms mirroring) |
| D6 | Brightness | 30/60/100 % steps visible. 0 % on screen-off. Brightness kept after the screen is restored |
| D7 | Screen off/on, light sleep | Panel restored with a full repaint. Wake latency measured. Touch and BOOT wake work |
| D8 | Battery | With a battery: present = 1, plausible voltage (3.0–4.3 V), charge direction follows USB plug/unplug. Without one: `BATTERY_ABSENT` |
| D9 | Audio | `tools/roundie_audio.py status`: `step ok`, `codec_ack` 1. Tone audible (person) |
| D10 | Wall clock | The tests in [rtc-time-sync.md](rtc-time-sync.md#boards-without-an-external-rtc) |
| D11 | ROM Download | Hold BOOT, reset: the ROM serial loader is reachable over USB |
