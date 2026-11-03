# Roundie hardware audit — ESP32-S3 Touch-LCD-1.85B

**Date:** 2026-10-17  
**Scope:** Milestone 0 source and schematic audit; feasibility of Milestone 1 on ESP-IDF v6.1.  
**Evidence level:** Desktop inspection only. No physical board was accessible to this audit; no firmware was built or flashed, and no electrical or USB measurements were made.

## Result and evidence rules

The **1.85B** has a published Rev1.1 schematic and board-specific ESP-IDF v5.5.3 examples. Its documented hardware can support a focused v6.1 bring-up, but the bundled BSP cannot be adopted unchanged: it instantiates new and legacy I²C drivers on the same port, and its BQ27220 dependency uses the legacy bus. LCD initialization also needs review before relying on it. A successful source review does **not** close the physical-revision, power, recovery, display, or stability checks in specification §23.

Here, **documented fact** means directly shown by a linked Waveshare/Espressif document or source at the stated revision; **inference** identifies a consequence of those facts; **decision** cites the REP specification; **pending experiment** gives a device check and pass condition. Physical board behavior is unverified throughout.

## Exact board and source baseline

| Item | Documented fact | Limit |
|---|---|---|
| Identity | Waveshare product **ESP32-S3-Touch-LCD-1.85B**, SKUs 34554 and 34556 (EN); ESP32-S3R8, 512 KB internal SRAM, 8 MB stacked PSRAM, 16 MB external flash, 360 × 360 LCD. [W1] | These are manufacturer specifications, not measurements of a particular unit. |
| Hardware drawing | Waveshare links **ESP32-S3-Touch-LCD-1.85B Rev1.1.pdf**. The PDF inspected here has SHA-256 `d4333dccd12f4c6a6f28351ee329eae08a4251cc61f083a37ef5ee59eff29b6d`. [W2] | The revision printed on the user's physical PCB, BOM population, and any later hardware revision remain unknown. Photograph both PCB sides and compare before using this as the physical authority. |
| Example snapshot | Manufacturer repository `waveshareteam/ESP32-S3-Touch-LCD-1.85B`, commit `139e6db584f3737fcfc6a958ee83b79fb69d317c` (2026-05-19); examples are under `Examples/ESP-IDF-V5.5.3/`. [W3] | Neither the directory name nor `idf: >=5.3` proves a v6.1 build. No `idf.py` was available in this workspace for a target compile. |
| Model separation | The Waveshare documentation lists `1.85` and `1.85B` separately; this audit uses only 1.85B materials. [W1] | Generic `ESP32_Display_Panel` board lists include the **non-B 1.85**, so its board preset is not a 1.85B pin source. [E8] |

### GPIO and peripheral map

The following is the **documented Rev1.1 / 1.85B map**, cross-checked against Waveshare's quick reference and BSP pin definitions. GPIOs not listed here are not declared free. [W1][W2][W4]

| Device / signal | GPIO / bus | Notes |
|---|---|---|
| BOOT | GPIO0 | Active low, pulled high; ROM boot strapping pin. Preserve for recovery. |
| CST816S touch | SDA 11, SCL 10; reset 1, interrupt 4 | Shared I²C; documented address `0x15` (7-bit). |
| QMI8658 IMU | SDA 11, SCL 10; schematic interrupt GPIO8 via 0 Ω link R51 | Shared I²C; documented address `0x6B`. GPIO8 interrupt route is schematic evidence only; link population and signaling remain unverified. |
| PCF85063A/ATL RTC | SDA 11, SCL 10; interrupt 6 | Shared I²C; documented address `0x51`. Schematic part marking is `PCF85063ATL`. |
| BQ27220 fuel gauge | SDA 11, SCL 10 | Shared I²C; documented address `0x55`. Gauge presence does not prove calibrated state of charge. |
| ES8311 output codec | SDA 11, SCL 10; MCLK 2, BCLK 48, LRCK 38, data out 47, data in 39, PA 9 | The quick table says I²C `0x30`, while the schematic labels `0x18`; likely 8-bit/7-bit notation, **not settled** without checking the codec configuration and scan. |
| ES7210 microphone ADC | SDA 11, SCL 10; MCLK 2, BCLK 48, LRCK 38, data in 39 | Quick table calls `0x80` a 7-bit address, which is impossible (`0x80 > 0x7f`). Treat it as an address-notation error; resolve from strap pins, driver and scan before configuration. |
| ST77916 LCD QSPI | CS 21, clock 40, data0 46, data1 45, data2 42, data3 41, reset 3, backlight PWM 5 | 360 × 360; check LCD variant and transfer rate on device. GPIO45/46 also have strapping implications per ESP32-S3 GPIO documentation. [E1] |
| LCD TE | **Schematic: GPIO18 through 0 Ω link R5** | Important discrepancy: the Waveshare GPIO table omits TE, but Rev1.1 drawing labels `LCD_TE(GPIO18)` at the LCD connector and IO map. Its actual link population and signal are pending inspection and scope measurement. Do not enable TE synchronization by default. |
| microSD | CLK 15, CMD 14, D0 16, D1 17, D2 12, D3 13 | SDMMC 4-bit in Waveshare documentation; outside Milestone 1 minimum. |
| USB Type-C | D− 19, D+ 20 | Native ESP32-S3 USB Full-Speed; normal USB use can affect USB-Serial-JTAG debug availability. [E4] |
| UART0 header | TX 43, RX 44 | Potential independent debug path; electrical access/adapter not tested. |
| Flash / stacked Octal PSRAM | Flash uses SPI0/1 pins; GPIO33–37 reserved by S3R8 Octal PSRAM | Do not repurpose memory pins. [E1] |

The shared I²C bus has six onboard clients (touch, IMU, RTC, gauge, output codec, microphone ADC), plus the expansion connector. **Decision:** one managed new-driver master bus with explicit devices (`driver/i2c_master.h`), as required by specification §§2.3 and 3.2. Address conflicts and pull-up/rise time must be checked on the actual unit. [E2]

## PWR, BOOT, and recovery

**Documented facts:** Waveshare's technical page says PWR has no custom function, describes about 2 seconds to switch power on/off and about 1 second to turn on, and identifies BOOT as GPIO0. Rev1.1 places PWR in a separate power on/off latch/switch circuit (`ECJ23001-4FCBD6`, `PWR_EN`), with no PWR signal in the MCU GPIO allocation; BOOT is a separate switch pulling GPIO0 low. The drawing shows an RC RESET/CHIP_PU circuit but no RESET pushbutton. [W1][W2] Waveshare's GPIO table instructs holding BOOT during power-on to enter download mode. Espressif documents low GPIO0 during reset/power-up as the download-mode mechanism. [E5]

**Inference and limit:** Firmware cannot treat PWR as a normal input or promise software power-off on the documented circuit. The Rev1.1 wiring supports the specification's manual recovery candidate: remove power with PWR, hold BOOT, reapply power or USB, then release BOOT. It does **not** prove that this sequence succeeds with USB already attached, with a battery, or after TinyUSB has claimed the internal PHY. Do not present deep sleep as hardware power-off. No eFuse change is justified. [W2][E4]

**Pending experiment:** On a revision-matched unit, record PWR response with USB only, battery only, and both; check BOOT level at startup; enter ROM Download manually in each powered state and read chip identity/enumeration without writing flash. Repeat after a reversible USB-OTG test in a later milestone. Pass only when a written, repeatable procedure works without relying on running application firmware. Do not erase or flash during this audit.

## Waveshare ESP-IDF example and BSP inventory

The manufacturer lists eight v5.5.3 projects: comprehensive demo, LVGL demo, MP3 player, speech wake word, Brookesia, QMI8658 test, PCF85063 test, and BQ27220 test. [W5] The examples vendor a board component `waveshare__esp32_s3_touch_lcd_1_85B`, `bsp_extra`, and (in most examples) QMI8658/PCF85063 components; the lockfile for `02_lvgl_demo` records IDF 5.5.3, LVGL 9.5.0, `esp_lcd_st77916` 2.0.2, `esp_lcd_touch_cst816s` 1.1.1, `esp_lvgl_adapter` 0.3.2, `esp_codec_dev` 1.5.6, `i2c_bus` 1.4.2, and `bq27220` 0.1.1. [W3][W6] This is a versioned source inventory, **not** a compatibility certification.

| Part | Observed implementation and consequence |
|---|---|
| Board BSP | Pin definitions, `bsp_i2c_init`, ST77916 panel and CST816S touch setup, LEDC backlight, SDMMC/FATFS and SPIFFS helpers, I²S/codec setup, LVGL adapter, QMI8658 helper. `idf_component.yml` says `idf >=5.3` but contains stale CAM board metadata; verify every dependency rather than trusting the manifest description. [W4] |
| I²C / sensors | QMI8658 and PCF85063 code use new `i2c_master_bus_handle_t`. The BQ27220 demo obtains an `i2c_bus_handle_t` from the BSP, and `bq27220` 0.1.1 depends on `i2c_bus` 1.4.x. [W4][W7] |
| LCD / LVGL | BSP uses GP-SPI QSPI with `SPI_DMA_CH_AUTO`, selects one of two ST77916 command tables from a panel ID read, then uses `esp_lvgl_adapter` and two PSRAM draw buffers of height 50. [W4] These are source settings, not measured bandwidth or a final buffering choice. |
| Audio | BSP already uses `driver/i2s_std.h`, I²S channel handles, `esp_codec_dev`, ES8311 and ES7210; full-duplex timing, codec I²C addresses, and 6.1 API compatibility remain untested. [W4] |
| Other demo features | SDMMC/FATFS, MP3, ESP-SR, Brookesia, audio player and SPIFFS are demonstration dependencies. They are not needed to prove the Milestone 1 gate. [W5] |

## ESP-IDF v6.1 assessment

| Area | Source-backed assessment | Milestone 1 action / gate |
|---|---|---|
| I²C master | **Confirmed blocker in bundled source:** `bsp_i2c_init()` calls both `i2c_new_master_bus()` and legacy `i2c_bus_create()` for I²C port 0. The new and legacy drivers cannot coexist, and the legacy driver is EOL in v6.0. `bsp_i2c_deinit()` deletes only the new bus. BQ27220 0.1.1 is tied to the legacy wrapper. [W4][W7][E2] | Adapt one bus owner to new master API; use a compatible gauge driver or narrow adapter after inspecting available components. Register each device once, propagate failures, and verify cleanup. Do not make the old wrapper a second controller. |
| LCD/QSPI/DMA | `esp_lcd_st77916` 2.0.2 documents a QSPI path and IDF v6.0 LCD migration lists changed types/API. In Waveshare BSP, panel IO is created twice without deleting the first handle; failed ID read leaves `register_data` uninitialized before variant selection; `max_transfer_sz` is zero and the passed display configuration is unused; several LCD calls do not check errors. These are concrete source risks, not a proven device failure. [W4][E3][E6] | Keep Waveshare's **1.85B** panel command tables, correct handle/error lifecycle, set transfer size for the chosen strip, use DMA-capable/aligned buffers as required by target APIs, and measure at an initially conservative clock before 80 MHz. |
| LCD TE / GUI buffering | Rev1.1 labels GPIO18 via R5, but public pin table omits it. BSP's optional TE branch references `BSP_LCD_TE` with no definition in its inspected board source and is disabled by default. The ordinary LVGL path uses PSRAM buffers. [W1][W2][W4] | Keep TE mode off for the first bring-up. Verify R5 population and TE waveform separately; only then decide synchronization and final LVGL buffer strategy. Log internal/PSRAM heap and largest free block. |
| Touch | CST816S I²C driver and board reset/INT mapping exist; IDF v6.0 LCD migration removes the legacy I²C IO layer, leaving new master API. [W1][W4][E3] | Bind CST816S panel IO to the single new I²C bus; prove touches at center and near round-screen edges, release and reset behavior, and no coordinate inversion. |
| PSRAM / Flash | Board specification says 8 MB stacked PSRAM and 16 MB flash; example requests Octal 80 MHz PSRAM and `CONFIG_SPIRAM_XIP_FROM_PSRAM`. IDF v6.1 documents that ordinary external RAM and executable memory are different capabilities and flash operations can affect cache/external RAM. [W1][W6][E1][E7] | Confirm detected size/mode from boot log and heap capability diagnostics. Verify `sdkconfig.defaults` symbols against the selected v6.1 tag. Do not infer dynamic ELF execution from `malloc()` or select XIP policy at this gate. |
| RTC / gauge | PCF85063 example is new-driver based; BQ27220 demo and pinned component are legacy-bus based. Waveshare RTC test notes battery-dependent behavior. [W5][W7] | Read RTC time and a non-destructive gauge status with USB and, if available, battery. Treat valid clock retention, gauge accuracy/configuration and interrupt wake as separate tests. No gauge data-memory writes in bring-up. |
| Audio | BSP uses modern I²S standard-mode API, but IDF 6.0 removed legacy `driver/i2s.h`; codec library and CMake dependencies still need 6.1 compile checks. [W4][E3] | Inventory and compile audio dependencies as an optional compatibility check; leave playback/recording out of the minimum Milestone 1 runtime. A later audio gate must test both codecs, shared clock, microphones and PA. |
| USB device | S3 native USB uses GPIO19/20. USB-OTG and USB-Serial-JTAG share the internal PHY, so application TinyUSB can change normal console/reset behavior. [W1][E4] | Preserve a manual ROM Download route and an independent UART debug option. USB MTP/composite is not part of Milestone 1. |
| IDF/C/component changes | IDF v6.0 removes legacy I²S and other APIs and requires explicit driver-component dependencies; LCD GPIO fields changed to `gpio_num_t`. Default C is GNU C23 and libc is Picolibc. [E3][E9] | Pin a specific v6.1 commit after integration, re-resolve the component lockfile, check Kconfig and public headers, and build without globally disabling `-Werror`. Diagnose each failing dependency locally. |

### Later-stage components: availability, not validation

- **Espressif ELF Loader:** registry release **1.3.5** lists ESP32-S3 and says it can execute ELF in PSRAM. [E10] That establishes availability only. Compatibility with the chosen v6.1 toolchain/libc, Xtensa relocations, TLS, symbol allowlist, allocation policy, repeated unload, and flash concurrency is **unverified**. Keep specification §23's separate ELF proof-of-concept gate before runtime/ABI work. Do not enable shared-library loading merely because the loader offers it.
- **TinyUSB MTP:** `esp_tinyusb` **2.3.0** changelog states it added MTP over already-mounted VFS/FATFS paths; **2.4.0** is a released component targeting esp32s3, and source Kconfig has `TINYUSB_MTP_ENABLED`. [E11][E12] This establishes that MTP is available in a released line, correcting any assumption that it exists only on `master`. It does **not** establish 2.4.0 + IDF 6.1 build/runtime, LittleFS semantics, safe virtual-folder policy, host interoperability, or recovery. Assess those at Milestone 7. MSC raw partition sharing is not the target architecture (spec §16).

## Known blockers and integration risks

1. **New/legacy I²C conflict:** fix before any combined sensor test; the current `bsp_i2c_init()` is not a valid one-bus design. [W4][E2]
2. **Gauge dependency:** `bq27220` 0.1.1 requires legacy `i2c_bus`; a newer release advertises 6.x test-app changes, but its bus model must be checked, not assumed migrated. [W7][E13]
3. **LCD variant and handle lifecycle:** panel ID fallback, duplicate IO allocation, DMA sizing, 80 MHz signal margin and partial error cleanup need v6.1 compile and device checks. [W4]
4. **TE evidence conflict:** schematic GPIO18/R5 versus omission from published pinout; electrical function and revision match unverified. [W1][W2]
5. **Address notation:** ES8311 `0x30`/`0x18` and ES7210 invalid “7-bit `0x80`” can misconfigure a shared bus; use a safe scanner and verify driver address conventions. [W1][W2]
6. **Power/recovery:** PWR behavior, battery/USB backfeed, RTC backup, BOOT timing and ROM enumeration require the actual board. [W1][W2]
7. **Dependency drift:** `^` and broad ranges in the vendor manifest are not pinning; lockfile is IDF 5.5.3. Resolve, record and test exact versions under v6.1. [W6]

## Outstanding physical experiments

| Question | Method without destructive operations | Pass / observation to record |
|---|---|---|
| Board revision and parts | Photograph PCB labels and IC markings; compare Rev1.1 connector/link positions. | Identify revision and any assembly differences, especially LCD TE R5 and reset links. If different, obtain matching schematic before firmware pin use. |
| PWR and recovery | Test documented button timing with USB only, battery only, both; hold BOOT at power-on, observe USB ROM enumeration and read chip info; do not write flash. | Repeatable manual ROM entry on each supported power state, or document a specific limitation and recovery path. |
| Flash/PSRAM | Read-only chip/flash identification, then a Milestone 1 boot with memory diagnostics. | Report detected 16 MB flash and 8 MB Octal PSRAM or investigate a mismatch; log minimum/largest internal and external free blocks. |
| I²C devices | Single new-master scan at conservative bus speed; read non-destructive identity/status registers. | Expected devices ACK with no bus lockups; record actual 7-bit codec addresses and any battery dependence. |
| LCD and touch | Display solid fields/grid; measure SPI clock and optional TE at GPIO18; touch center/edges and reset cycle. | Stable 360 × 360 output, correct color/rotation and touch coordinates, no failed transfers; TE accepted only if electrically observed. |
| RTC and gauge | Read RTC time before/after power states; read voltage/status with and without battery. | Separate powered functionality from time retention and gauge calibration; log RTC_INT levels. |
| Wake candidates | Later power milestone: measure GPIO0/4/6 levels, light/deep sleep wake, current and reset strapping. | No wake-source claim until repeated device tests pass. **2026-11-03:** GPIO4 (touch INT) and GPIO0 light-sleep wake user-verified ([PowerManager](power-manager.md)); GPIO6, deep sleep, current and strapping at deep-sleep wake remain open. |

## Recommended minimum Milestone 1 — IDF v6.1 bring-up

**Decision basis:** specification §§2–3 and §23. Keep this as a small C firmware exercise; do not add ELF, storage/OTA, packages, MTP, audio player, or a new framework.

1. Select and record one exact ESP-IDF v6.1 tag/commit and the component versions; target `esp32s3`, 16 MB flash and Octal PSRAM. Keep the first boot/recovery path simple. Resolve build warnings rather than suppressing them globally.
2. Adapt the board's essential pin/config code and make **one** new I²C master. Bring up CST816S, PCF85063 and non-destructive BQ27220 read access on that bus. If the published gauge component still requires legacy I²C, use a narrowly scoped compatible adapter or postpone only that device behind an explicit gate failure; never initialize both drivers.
3. Bring up ST77916 QSPI with one owned panel IO, checked register-read fallback, bounded DMA transfers, backlight control and a minimal LVGL 9 screen. Start without TE and at a conservative SPI clock. Use the manufacturer's 1.85B initialization tables; establish which variant responds on the actual panel.
4. Print boot/build identity, flash/PSRAM detection, internal/external heap totals and largest blocks, I²C probe/read outcomes, LCD transfer errors, and touch events. Retain BOOT/ROM Download access and use UART0 when USB debug is unavailable.
5. Record physical revision and repeat the bring-up on USB and battery where available. Document schematic differences and measured limits before expanding the scope.

### Explicit Milestone 1 acceptance criteria

**Target compilation:** exact pinned IDF v6.1 and component revisions are recorded; a clean `idf.py build` for ESP32-S3 succeeds with default project warning policy and no global `-Werror` disablement; no legacy `driver/i2c.h`, legacy `driver/i2s.h`, or unowned second I²C controller is linked into the bring-up path. A build alone is not hardware validation.

**Device tests:** on an identified 1.85B revision, ten cold starts complete without boot or heap errors; memory logs show the expected flash/PSRAM configuration; RTC and gauge non-destructive reads succeed under their documented power preconditions; touch center and four reachable edge regions report coherent press/release; an LVGL screen and solid-color pattern remain correct during at least 30 minutes of interaction/backlight changes with zero logged LCD/I²C transfer errors. Record actual conditions, timestamps, logs, and any failures. If battery testing is unavailable, state that limitation and leave its criterion open.

**Recovery:** after the bring-up image is running, manually enter ROM Download and read chip identity over the board's USB connection without erasing or writing flash; repeat with battery attached if that use is intended. Keep a confirmed method for console/debug access. Any failed recovery condition blocks the gate until a tested procedure is documented.

**Excluded from this gate:** TE synchronization, deep-sleep wake, audio, SD card, TinyUSB MTP, OTA, dynamic ELF execution, long-running Flash/PSRAM concurrency, and fuel-gauge calibration. Their feasibility remains a later measured decision, not a consequence of this audit.

## Sources

**Waveshare, exact 1.85B**

- [W1] [Waveshare 1.85B product documentation, specifications and GPIO tables](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85B), accessed 2026-10-17.
- [W2] [Waveshare 1.85B Rev1.1 schematic PDF](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B/blob/139e6db584f3737fcfc6a958ee83b79fb69d317c/hardware/ESP32-S3-Touch-LCD-1.85B%20Rev1.1.pdf); linked from [Waveshare Resources](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85B/Resources-And-Documents).
- [W3] [Waveshare 1.85B source repository at inspected commit](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B/tree/139e6db584f3737fcfc6a958ee83b79fb69d317c/Examples/ESP-IDF-V5.5.3).
- [W4] [1.85B board BSP C source](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B/blob/139e6db584f3737fcfc6a958ee83b79fb69d317c/Examples/ESP-IDF-V5.5.3/02_lvgl_demo/components/waveshare__esp32_s3_touch_lcd_1_85B/esp32_s3_touch_lcd_1_85B.c), [board header](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B/blob/139e6db584f3737fcfc6a958ee83b79fb69d317c/Examples/ESP-IDF-V5.5.3/02_lvgl_demo/components/waveshare__esp32_s3_touch_lcd_1_85B/include/bsp/esp32_s3_touch_lcd_1_85B.h), [manifest](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B/blob/139e6db584f3737fcfc6a958ee83b79fb69d317c/Examples/ESP-IDF-V5.5.3/02_lvgl_demo/components/waveshare__esp32_s3_touch_lcd_1_85B/idf_component.yml), and [Kconfig](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B/blob/139e6db584f3737fcfc6a958ee83b79fb69d317c/Examples/ESP-IDF-V5.5.3/02_lvgl_demo/components/waveshare__esp32_s3_touch_lcd_1_85B/Kconfig).
- [W5] [Waveshare 1.85B ESP-IDF example guide](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85B/ESP-IDF).
- [W6] [Waveshare LVGL demo dependency lockfile](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B/blob/139e6db584f3737fcfc6a958ee83b79fb69d317c/Examples/ESP-IDF-V5.5.3/02_lvgl_demo/dependencies.lock) and [sdkconfig.defaults](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B/blob/139e6db584f3737fcfc6a958ee83b79fb69d317c/Examples/ESP-IDF-V5.5.3/02_lvgl_demo/sdkconfig.defaults).
- [W7] [Waveshare BQ27220 demo source](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B/blob/139e6db584f3737fcfc6a958ee83b79fb69d317c/Examples/ESP-IDF-V5.5.3/08_bq27220_test/main/main.c) and [Espressif `bq27220` 0.1.1 usage](https://components.espressif.com/components/espressif/bq27220/versions/0.1.1/readme).

**Espressif primary references**

- [E1] [ESP32-S3 GPIO and PSRAM restrictions](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/gpio.html).
- [E2] [ESP-IDF I²C driver documentation](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/i2c.html).
- [E3] [ESP-IDF 6.0 peripheral migration guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/migration-guides/release-6.x/6.0/peripherals.html).
- [E4] [ESP32-S3 USB hardware/PHY and DFU guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/dfu.html).
- [E5] [ESP32-S3 serial/download connection guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/establish-serial-connection.html).
- [E6] [`esp_lcd_st77916` 2.0.2 QSPI component documentation](https://components.espressif.com/components/espressif/esp_lcd_st77916/versions/2.0.2/readme).
- [E7] [ESP-IDF 6.1 external RAM guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/external-ram.html) and [heap capability guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/system/mem_alloc.html).
- [E8] [`ESP32_Display_Panel` supported-board list](https://components.espressif.com/components/espressif/esp32_display_panel/versions/1.0.2/readme).
- [E9] [ESP-IDF 6.1 C support](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/c.html) and [system migration/Picolibc](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/migration-guides/release-6.x/6.0/system.html).
- [E10] [`espressif/elf_loader` 1.3.5 registry page](https://components.espressif.com/components/espressif/elf_loader/versions/1.3.5/readme).
- [E11] [`esp_tinyusb` 2.4.0 registry page](https://components.espressif.com/components/espressif/esp_tinyusb/versions/2.4.0/readme) and [2.3.0/2.4.0 changelog](https://components.espressif.com/components/espressif/esp_tinyusb/versions/2.4.0/changelog).
- [E12] [`esp_tinyusb` MTP Kconfig in Espressif source](https://github.com/espressif/esp-usb/blob/master/device/esp_tinyusb/Kconfig); inspect the selected release tag again before integration.
- [E13] [`bq27220` 0.1.2 changelog](https://components.espressif.com/components/espressif/bq27220/versions/0.1.2/changelog); reports 6.x test-app compatibility, not a confirmed new-bus port.
