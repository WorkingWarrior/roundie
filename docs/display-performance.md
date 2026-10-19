# Display throughput — Milestone 3E.2

Scope: exact Waveshare ESP32-S3-Touch-LCD-1.85B, ST77916, RGB565 360 × 360.
Historical 3E.1 timings remain in [the original record](launcher-3e1-performance.md).
No 3F, ABI, partition, CPU, optimization or task-scheduling change.

## Source facts and clock decision

Reviewed 2026-10-19:

- [1.85B documentation](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85B):
  SPI2 routes through the GPIO matrix: CLK40, D0=46, D1=45, D2=42, D3=41,
  CS21; reset3 and backlight5. These are the B model's existing pins.
- [Vendor BSP, commit 139e6db584f3737fcfc6a958ee83b79fb69d317c](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B/blob/139e6db584f3737fcfc6a958ee83b79fb69d317c/Examples/ESP-IDF-V5.5.3/02_lvgl_demo/components/waveshare__esp32_s3_touch_lcd_1_85B/esp32_s3_touch_lcd_1_85B.c):
  lines 823–865 start at **3 MHz**, synchronously read ID using opcode 0x0b,
  then create another IO at **80 MHz** before panel initialization. The example
  does not delete the old IO there. It proves a vendor setting, not electrical
  stability or compliance. Roundie's initial bring-up commit `08a6a82` retained
  the 3 MHz setting for all transfers and omitted this second phase. No source
  evidence establishes an intentional 3 MHz pixel-write restriction.
- [ST77916 component 2.0.2](https://components.espressif.com/components/espressif/esp_lcd_st77916/versions/2.0.2/readme)
  links [the controller PDF](https://dl.espressif.com/AE/esp-iot-solution/ST77916_SPEC_V1.0.pdf).
  Despite its filename, its footer says Preliminary 0.1. Page 39, §7.4.5/Table 5:
  write cycle ≥16 ns (reciprocal ceiling **62.5 MHz**), high/low ≥7 ns,
  data setup/hold ≥7 ns, write CS setup/hold ≥15 ns. Read cycle ≥150 ns
  (≤6.67 MHz), high/low ≥60 ns. The table specifies VDDI 1.65–3.3 V,
  VDD 2.65–3.3 V, 25 °C; transition times ≤15 ns and loading constraints apply.
  This is a controller timing envelope, not a guaranteed board maximum.
  Actual rails, loading, edges, temperature margin and signal integrity have
  not been measured on this unit. No slower initialization-write limit was
  found; reads have their own lower limit.
- Pinned IDF v6.1 `fff9895c82d744c7237be8847347bdd1b07c6643`:
  `esp_lcd/spi/esp_lcd_panel_io_spi.c` uses one clock for command, parameter
  and color transactions. Color alone uses QIO; command/parameters remain
  single-lane. There is no public live clock setter or panel-IO SPI-device
  getter. `esp_driver_spi/src/gpspi/spi_master.c` chooses S3's APB source,
  default duty 128/256, and the pinned `esp_hal_gpspi/esp32s3/include/hal/spi_ll.h`
  selects the nearest integer divider. GPIO-matrix receive timing cannot be
  treated as a high-speed write limit; ID reads remain at the proven slow rate.
  See also [IDF SPI documentation](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/spi_master.html).

**Architectural decision (§§12.7, 25):** First evaluate **20 MHz writes**,
preserving 3 MHz ID reads. 10 MHz is a conservative alternative; 40 MHz fits
the published write timing and is reserved for a further justified experiment.
80 MHz's 12.5 ns cycle violates the published 16 ns minimum and is excluded,
even though the vendor example requests it. 60/62.5 MHz are not admitted:
unnecessary margin reduction and no bounded validation case. No frequency is
advertised as electrically guaranteed. Profiles are compile-time only; no NVS
setting, runtime retuning or autocalibration.

At an 80 MHz APB, pinned divider calculations give:

| Request | Expected pre × n | Actual configured frequency | Pixel-only full-frame minimum |
|---|---:|---:|---:|
| 3 MHz | 1 × 27 | 2.962963 MHz | 174.960 ms |
| 10 MHz | 1 × 8 | 10 MHz | 51.840 ms |
| 20 MHz | 1 × 4 | 20 MHz | 25.920 ms |
| 40 MHz | 1 × 2 | 40 MHz | 12.960 ms |

Nominal 3 MHz would give 172.800 ms. Payload is 129600 pixels / 259200 bytes,
four bits per pixel clock; commands, strip setup, rasterization, scheduling and
panel scanout add cost. The historical ~247 ms handler is consistent with
this slow bus. Baseline clock rounding is source-derived; the new firmware
logs the actual programmed SPI2 divider after completed initialization. Neither
is an oscilloscope measurement of the physical clock waveform.

## Implementation and ownership

`roundie_board` creates the original slow IO, completes and validates the
synchronous ID read, deletes that IO, clears its owned pointer and creates one
write IO. At that point no panel, GUI, callbacks, buffers or queued DMA exist.
Failure preserves existing startup cleanup/headless USB recovery. There is no
live SPI reconfiguration, and no leaked slow SPI device. Initialization tables,
lane order, orientation, geometry, command encoding and byte order stay intact.
The write IO requests one cycle of CS setup/hold (50 ns at 20 MHz, 25 ns at
40 MHz). Software transaction separation provides the CS-high interval; edge
and inter-transaction electrical timings remain unmeasured.

One **14400-byte internal DMA buffer**, 20 lines, remains. SPI maximum transfer
is still 14464 bytes and queue depth two. Each full strip fits one color chunk.
Pinned panel IO enables the callback only on its final chunk; ISR timestamp,
notification and explicit conditional ISR yield remain unchanged. LVGL 9.5.0's
custom flush-wait callback consumes completion before its internal flushing flag
can clear and before buffer reuse. No successful flush calls flush-ready early.
A two-second timeout still restarts rather than releasing a possibly owned
buffer; rejected submission retains existing display-fault handling. RGB565
swapping remains inside the measured submission interval.

The existing RP1 PERF tool and its bounded 32-record ring are reused unchanged.
`dma_us` is flush-entry→ISR, including swap/submission overhead, and `wait_us`
is GUI time blocked awaiting completion. `render_us` includes rendering and
inter-strip waits but excludes the final DMA tail. These overlap: do not add
them or call them pure rasterization/wire time. `handler_us - wait_us` is only
non-wait handler work, including layout, rasterization, byte swap and SPI setup.
Object-ready/presentation timestamps do not establish human-visible completion.

## Validation status before deployment

Host: 80 Python regression tests passed, including production C under
ASan/UBSan and actual pinned LVGL with mocked IDF/loader. Added tests cover
all four admitted clock profiles, rejection of unsupported clocks, exact pins/
protocol/queue/size, synchronous slow-read/deletion/write-IO ordering, both
supported panel IDs, unknown-ID rejection and API failure paths. Production
flush tests cover partial coordinates/pixels, ownership until completion,
timeout and overlapping-submission restart, enqueue failure, and existing ISR
timestamp/notification/conditional yield. These are software evidence only.
Existing package, ABI v1/v2/v3, resource, launcher, Safe Mode and OTA regressions
remain covered. Target build/layout/image checks are separate from device tests.

Read-only live preflight passed: expected device, 16 MiB Flash / 8 MiB PSRAM,
exact installed rollback bootloader and REP table, A 0.3.6 VALID/STABLE, B 0.3.5
VALID/compatible, OTA IDLE, no ELF, healthy NVS/LittleFS, original backup and ROM
procedure available, installed package identities unchanged. One 66112-byte
bounded read (64 KiB prefix + two headers), no full-Flash read and no raw write.
Two Hello and two UI baseline cycles completed with injected controls, result
42, partial updates and zero final application resources. Idle had zero flushes.

Candidate OTA, divider, before/after timings, final memory/stack/resource and
human visual status are recorded below after bounded acceptance.

Evidence is outside Git in
`/mnt/Dane/Development/roundie-backups/display-3e2-2026-10-19/`.
No generated binary, raw traffic, backup or private device identifier is committed.
