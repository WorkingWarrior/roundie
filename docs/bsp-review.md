# BSP and hardware initialization review

**Date:** 2026-10-17  
**Scope:** source review of the current Milestone 1 ESP-IDF v6.1 image. No
firmware, configuration, dependency, or device state was changed for this
review. Prior build and device results are recorded in [bringup.md](bringup.md);
this review did not repeat them.

## Executive summary

The small board component is a reasonable **diagnostic bring-up baseline**:
it creates one new-driver I²C bus, uses the 1.85B QSPI pin map and explicit
panel ID selection, owns its device handles, keeps LVGL in the GUI task, and
uses an internal DMA-capable strip buffer with completion notification. The
source review found no confirmed P0 issue, duplicate I²C controller, legacy
I²C wrapper, or normal-path handle leak.

Four P1 items need resolution before treating this as a reliable base for
later milestones: a bus-start error can be returned as board-start success;
the pinned touch component discards an IRQ-registration error; the GUI task
has no watchdog for blocking panel-I/O calls before the two-second DMA wait;
and the generated partition layout differs from the installed device. The
last item is already guarded by the warning in `bringup.md`, but remains a
deployment hazard. The unresolved physical RTC/gauge, cold-start, endurance,
and manual recovery gates remain separate from this source review.

## Review scope and evidence

Inspected `main/main.c`, `components/roundie_board/board.c` and its public
header, both copied ST77916 tables, root/component CMake and manifests,
`dependencies.lock`, `sdkconfig.defaults`, the generated `sdkconfig`, the
existing compile database and link map, and only the relevant paths in pinned
Espressif LCD/touch and LVGL components. Architecture was checked against
[specification.md](specification.md) §§2–3, 11–12, 22–23, 25 and the
[hardware audit](hardware-audit.md). Generated files were read as evidence of
the current build, not treated as tracked configuration.

**Evidence tiers:** Source and generated-build inspection support the findings
below. The earlier clean target build and incremental final-image build are
documented in `bringup.md`. Earlier USB-only device tests support the panel,
color, spinner, and touch observations there. This review ran no new build,
hardware test, Flash operation, endurance test, or fault injection.

## Confirmed findings, ordered by severity

No P0 defect is confirmed by the inspected source or the available device
evidence. Conditional failure paths below are source-proven; their triggering
conditions were not observed on the tested device.

| Priority | Location | Source evidence and consequence | Minimal correction |
|---|---|---|---|
| **P1** | [`board.c:332`](../components/roundie_board/board.c#L332), [`roundie_board.h:27`](../components/roundie_board/include/roundie_board.h#L27) | `start_i2c()` returns a bus-creation error, but `roundie_board_start()` only skips touch initialization, then starts the LCD and can return `ESP_OK`. The public contract says a failed bus start returns an error. A caller therefore cannot rely on the return value to mean core board initialization succeeded; the failure is visible only in status/logs. | Propagate and clean up a bus-creation failure, or explicitly define and return a partial-diagnostic result. Keep optional device NACKs distinct from failure to create the bus. |
| **P1** | [`board.c:166`](../components/roundie_board/board.c#L166), [`board.c:410`](../components/roundie_board/board.c#L410), pinned `managed_components/espressif__esp_lcd_touch_cst816s/esp_lcd_touch_cst816s.c:67–70` | The pinned CST816S constructor calls `esp_lcd_touch_register_interrupt_callback()` without checking its `esp_err_t`. It can finish with `ESP_OK` after ISR registration fails. Roundie's touch read then waits for `touch_irq_pending`, which cannot be set, so touch remains unavailable while status says OK. | Create the touch handle without an initial callback, register the IRQ once through the checked public API, and fail/clean up on registration error; alternatively fix and pin the upstream constructor. |
| **P1** | [`main.c:60`](../main/main.c#L60), [`main.c:92`](../main/main.c#L92), [`main.c:375`](../main/main.c#L375), `sdkconfig:2079–2089`, pinned IDF `components/esp_lcd/spi/esp_lcd_panel_io_spi.c:370,405` | The two-second timeout begins only after `esp_lcd_panel_draw_bitmap()` returns. IDF's SPI panel IO uses `portMAX_DELAY` while obtaining/queueing transactions. The GUI/main task is not enrolled in the task watchdog; generated config watches the idle tasks. A stalled panel-I/O call before the flush-wait callback can therefore leave the GUI blocked without the LCD timeout or health telemetry running. No such stall was observed in the corrected short device run. | Bound or independently supervise the GUI task's panel-I/O path; register a GUI-task watchdog with a defined recovery policy before relying on it for unattended operation. Preserve the DMA-buffer ownership rule on timeout. |
| **P1** | [`sdkconfig.defaults`](../sdkconfig.defaults), generated `sdkconfig:960–974`, [`bringup.md:29`](bringup.md#L29) | The build still generates the default single-app partition table and an app at `0x10000`; the inspected device has an existing table and factory app at `0x20000` plus OTA/data partitions. A normal `idf.py flash` would write the wrong layout and can make installed data or the previous app inaccessible. The documented device-specific install avoids this, so this is a known deployment hazard rather than an observed bad flash. | Before standard flashing is permitted, make the tracked partition/flash configuration match a reviewed layout and add an install guard or command that preserves existing data. Do not silently replace the current table. |
| **P2** | [`main.c:112`](../main/main.c#L112), [`main.c:124`](../main/main.c#L124), [`board.c:410`](../components/roundie_board/board.c#L410) | Any non-`NOT_FOUND` touch read error suppresses GUI touch reads for five seconds. IRQs collapse into one `touch_irq_pending` flag, so taps during the cooldown are not delivered promptly and may be stale by the next read. This is a conditional recovery weakness, not a failure observed in the 65-second corrected run. | Separate log rate limiting from touch retry policy; permit a fresh IRQ to trigger a bounded read promptly after an error. |
| **P2** | [`main.c:229`](../main/main.c#L229), [`main.c:378`](../main/main.c#L378), generated `sdkconfig:3931–3957`, pinned LVGL `src/widgets/label/lv_label.c:162–168` | The useful startup/five-minute telemetry reports ESP-IDF internal/PSRAM heap, but LVGL uses a separate fixed 64 KiB built-in pool. Touch label updates allocate within that pool; its free space and fragmentation are not represented by the existing heap numbers. A later GUI pressure problem could be missed even if reported SRAM/PSRAM remains stable. | Keep existing telemetry and add LVGL pool statistics when memory pressure or longer tests make that distinction relevant. |
| **P2** | [`board.c:93`](../components/roundie_board/board.c#L93), [`board.c:111`](../components/roundie_board/board.c#L111), [`board.c:120`](../components/roundie_board/board.c#L120), [`board.c:135`](../components/roundie_board/board.c#L135) | If device registration fails after a successful probe, `status->rtc` or `status->gauge` records that error, while `rtc_read`/`gauge_read` remains `ESP_ERR_NOT_FOUND`. The later “read” log then describes the device as absent rather than showing why no read happened. No such registration failure was observed. | Log probe, registration, and register-read outcomes separately, or copy a registration failure into the corresponding read-status field. |

## Potential problems requiring verification

- **RTC and gauge availability — pending experiment.** USB-only logs showed
  one RTC read with a voltage-low flag and later NACKs; the gauge did not ACK
  without a battery. The code performs one boot-time probe and does not
  re-enumerate an absent device. That behavior is visible in `board.c:81–137`,
  but its physical cause is unknown. Under controlled USB/battery conditions,
  record voltage, bus levels, ACKs, and RTC retention before assigning blame
  to firmware or changing probe timing.
- **Touch teardown — conditional risk.** The pinned CST816S deletion path
  resets GPIO4, calls `gpio_isr_handler_remove()` without checking its result,
  then frees the touch object
  (`managed_components/espressif__esp_lcd_touch_cst816s/esp_lcd_touch_cst816s.c:155–170`).
  Roundie stops the touch object before its I²C IO and bus (`board.c:370–385`),
  which is the right ownership order. Repeated create/delete and forced ISR
  registration/removal failures would be needed to establish whether the
  external component can leave a callable handler after deletion; this has
  not been shown on the current one-time bring-up path.
- **Timing and memory margins — pending experiment.** The 3 MHz QSPI clock,
  one 14,400-byte strip, 8 KiB main stack, and initial 10 ms LCD reset pulses
  are bounded and have worked on the tested unit. The controller's own reset
  and manufacturer command-table delays are hardware initialization, not
  automatically technical debt. Measure flush time, stack high-water mark,
  prolonged heap trend, and failed-transfer recovery before optimizing them.

## Dependency and configuration assessment

- The tracked manifests pin IDF `6.1.0`, LVGL `9.5.0`, ST77916 `2.0.2`, and
  CST816S `1.1.1`; `dependencies.lock` fixes the transitive LCD-touch
  `1.2.1` and `cmake_utilities` `0.5.3` revisions. ST77916 actually depends
  on `cmake_utilities`, and CST816S actually depends on `esp_lcd_touch`.
  Their indirect status is not evidence that they are unused.
- `main` uses its declared board/LVGL/timer/LCD, app-format, Flash, PSRAM,
  and heap APIs; `roundie_board` uses LCD/touch and the declared I²C, SPI,
  LEDC, GPIO, and FreeRTOS APIs. No Waveshare BSP, `i2c_bus`, audio,
  SD-card, USB, or network component was copied into this project. No
  project-level warning suppression or legacy I²C/I²S include was found.
  The compile database retains `-Werror` and `-std=gnu23` for the three
  project C files. The one `read_register()` helper only shares the bounded
  RTC/gauge transaction; no extra bus framework or unused project wrapper
  was found. The project sources contain no TODO/FIXME or dead feature stub.
- LVGL's component build compiles optional C++ translation units, but the
  inspected map did not show those optional LVGL C++ objects linked into the
  image. The map does show `libstdc++` members pulled at least in part by
  IDF's default `nvs_flash` C++ objects; it does not establish an extra
  project-specific C++ dependency. Do not remove an IDF component based only
  on `main.c` usage.
- `sdkconfig.defaults` is small and explains the board memory settings and
  8 KiB main stack. Generated `sdkconfig` selects 16 MB Flash, 80 MHz Octal
  PSRAM, an internal 64 KiB LVGL pool, and no PSRAM XIP. The generated
  single-app partition choice is the P1 deployment mismatch above. Neither
  the config nor the code assumes executable PSRAM.

## Memory and resource ownership

| Resource | Owner and lifetime | Assessment |
|---|---|---|
| I²C bus, RTC/gauge handles, touch IO/driver | `roundie_board_t`; acquired in `start_i2c()`/`start_touch()`, released in reverse order by `roundie_board_stop()` | One bus, no duplicate initialization. An absent device is represented in status. Bus-creation propagation and touch IRQ-registration checking need the P1 fixes. |
| SPI bus, LCD IO/panel, reset/backlight GPIO | `roundie_board_t`; initialized once, panel then IO then bus released on stop | No duplicate LCD IO handle. ID read failure and unsupported ID return before selecting a table; caller invokes cleanup. Backlight API checks LEDC errors. No normal-path leak found. |
| LVGL display/objects and 14,400-byte draw buffer | `app_main()` GUI task; buffer is allocated with both `MALLOC_CAP_INTERNAL` and `MALLOC_CAP_DMA` and remains live for the process | `lv_timer_handler()`, touch/UI callbacks, and object mutations remain on the GUI task. LCD ISR only notifies the task. Wait callback gates reuse until DMA completion. The persistent buffer and LVGL pool are intentional; final buffering is still open under spec §25. |

The RGB565 byte swap is in place; the project does not copy each strip into
another staging buffer. The 14,400-byte allocation is a multiple of four
and requests IDF DMA-capable internal memory. No direct DMA-capability or
alignment defect is visible in the current allocation path. The tiny RTC
and gauge register buffers are stack-local (seven and two bytes). No
application code is placed in PSRAM or assumes ordinary PSRAM is executable.

## Recommended minimal fixes and baseline decision

1. Reconcile the board-start return contract with I²C bus creation failure.
2. Make touch IRQ registration an explicitly checked initialization step.
3. Add a GUI-task supervision policy that covers blocking panel I/O before
   the existing flush-wait timeout.
4. Resolve the partition-layout mismatch before enabling ordinary flash or
   deploy commands for this board.
5. Keep current SRAM/PSRAM and LCD-error telemetry. Address the P2 error
   reporting and LVGL-pool visibility when the corresponding failure or
   memory-pressure tests are scheduled.

**Recommendation:** Keep this image as the bounded Milestone 1 diagnostic
baseline. It should not yet be treated as a fully validated hardware gate or
as the deployment foundation for later storage/runtime milestones. Resolve
the P1 source/deployment issues and the still-open device criteria before
making that stronger claim. This report proposes fixes only; it implements
none.

## P1 remediation after this review

The findings above are retained as the source-review record. The following
changes were made afterward, within Milestone 1; the original review's
no-change/no-new-test statement applies only to that earlier review.

| Original P1 finding | Remediation status | Evidence and limit |
|---|---|---|
| I²C bus-creation error hidden by board success | **Fixed in source.** `roundie_board_start()` now returns the actual bus error and calls `roundie_board_stop()` before LCD initialization. Optional touch/RTC/gauge failures still remain in status, and the failed bus logs its cause. | Source inspection and ESP32-S3 target build. Forced bus failure and board cleanup have not been exercised on hardware. |
| CST816S IRQ registration unchecked in the pinned constructor | **Fixed at the Roundie call site.** The constructor receives a null callback, so it cannot take its unchecked path. Roundie then calls `esp_lcd_touch_register_interrupt_callback()` once, checks the result, logs failure, and deletes the partially started touch and IO handles. Managed component sources were not changed. | Inspected pinned CST816S 1.1.1 and touch 1.2.1 implementations; source inspection and target build. Forced ISR-registration failure and repeated teardown remain untested on hardware. |
| GUI can block before the DMA completion timeout | **Fixed in source/configuration.** The GUI/main task subscribes to the ESP-IDF task watchdog after setup and feeds it only after each LVGL iteration. Tracked defaults select a 10-second timeout with panic/reboot. The two-second completion timeout still restarts before buffer reuse. | Generated configuration and target build confirm the policy. Physical stall injection, worst-case LCD timing, and watchdog-triggered recovery remain untested. |
| Generated table mismatches installed table | **Guarded for diagnostic deployment; layout mismatch remains.** `tools/flash_bringup.py` uses the inspected factory offset `0x20000` only after checking the original backup, live table, erased OTA metadata, the known installed bootloader and compatible code, build manifest, and app size. Its default mode reads and checks; `--write` is explicit and writes only the factory app. Ordinary `idf.py flash` and `idf.py app-flash` remain unsafe. The production REP A/B table is still an open §25 decision for Milestone 2. | Original backup and generated table inspected; offline preflight accepted the recorded layout and rejected changed-table, non-erased OTA, and unrecognized-bootloader cases. No new device read, Flash write, partition migration, boot test, or recovery test was performed. |

**Remaining gate:** The P1 source changes are compiled, and the diagnostic
deployment path fails closed under the inspected layout assumptions. The
Milestone 1 physical checks listed in `bringup.md` remain open. A build or
offline preflight does not establish device behavior.

Final validation on 2026-10-17 completed one clean ESP-IDF v6.1 build and
extended the offline deployment checks with changed backup, incompatible
bootloader code, invalid/oversized app, and mocked read-only/write command
paths. After explicit authorization, the connected 1.85B passed live
deployment preflight; the application-only write at `0x20000` passed
esptool's image-hash verification. A five-minute serial smoke test showed
normal boot and GUI-loop health telemetry with zero reported LCD errors,
watchdog resets, or panics. See the final autonomous validation table in
`bringup.md` for exact commands, evidence, and image hash. Bus-creation and
IRQ-registration failure paths remain source/build verified only; visual
LCD/touch behavior on the new image and manual ROM Download recovery remain
**pending**, as do the other device gates.
