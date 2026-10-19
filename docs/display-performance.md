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

## Target build and genuine OTA

Implementation commit: `79e12ec`. Exact pinned IDF v6.1 REP build and offline
layout/image validation exited 0. The explicit build version was 0.3.7; an
earlier local build's cached 0.3.6 PROJECT_VER was caught before freezing or
deployment. No compiler warning was emitted; existing external IDF CMake
include-validation notices remain. Only the new LCD profile keys differ in
SDK configuration. CPU 160 MHz, `-Og`, 33 ms refresh, 10 ms loop, all stacks,
Flash/PSRAM/rollback/security and native loader settings stay unchanged.

| Frozen identity | Value |
|---|---|
| Version / image bytes | 0.3.7 / 729904 |
| App SHA-256 | `b833d55bad3b61ed74c6eee12eb2c64690e6b8af0dbc2adec4a3fcf844ea3a73` |
| Full ELF SHA-256 | `59980b0f29d4eda901aa14d3f9935e1a4e1582f2eb5cb3c2414d96f2de180bb5` |
| Preserved A 0.3.6 ELF | `4cdce8594162012c2038e76e5d5b89554a49ba4f1306e4b3f649be3ca12f5802` |
| B application sector envelope | `0x390000..0x443000`, half-open |
| OTA metadata envelope | `0xd000..0xf000` |

One genuine RO1 upload to inactive **B** took 74.873 host seconds through
activation. Captured states: B READY, B NEW, boot PENDING_VERIFY, critical
self-test ESP_OK, B VALID, BootGuard STABLE. The post-boot SPI2 register sample
is **requested=20000000, actual=20000000 Hz, source=80000000, pre=1, n=4,
h=2, direct=0**. ST77916 variant 2 and CST816S initialized successfully.
Independent USB diagnostics remained usable. A remains the original 0.3.6
VALID/compatible fallback; it was never selected or overwritten in this goal.
Its frozen image remains available in the previous 3E.1 evidence directory.
No second clock candidate or OTA was needed. No bootloader/table, NVS/LittleFS,
package, eFuse/security or RTC write, format, raw flashing, power cut, recovery
rewrite or endurance test occurred. Normal boot/OTA metadata updates are not
application-slot or storage erasure.

## Comparable on-device measurements

Two cold/warm Hello and two UI cycles at batch twelve on **each** firmware;
three injected button events per UI instance, ordinary timer updates, cooperative
Exit, and a two-second settled launcher idle window. The candidate's second UI
also suspended/resumed; its Exit timing is therefore not compared as an ordinary
Exit. No physical finger, camera or panel-pixel observation was available.
Samples below are microseconds unless stated; pairs are individual samples,
not a percentile, upper bound, FPS guarantee or proof of tear-free scanout.

| Metric / boundary | A 0.3.6, nominal 3 MHz | B 0.3.7, 20 MHz |
|---|---:|---:|
| Requested / actual configured clock | 3 / 2.962963 MHz (source-derived) | 20 / 20 MHz (register-verified) |
| Full UI frame pixels / strips | 129600 / 18 | 129600 / 18 |
| Full UI LVGL handler | 247307 / 247832 | **104876 / 99781** |
| Full-frame-row DMA-duration sum (*) | 181088 / 180693 | 37943 / **138506 (*)** |
| Full-frame-row GUI wait sum | 163829 / 164442 | **24220 / 25884** |
| Full-frame-row submission including swap | 15356 / 14979 | 14229 / 13525 |
| Render START→READY span | 236865 / 236864 | 93296 / 89834 |
| Handler minus notification wait | 83478 / 83390 | **80656 / 73897** |
| GUI pump in full-frame iteration | 26448 / 27169 | 25445 / 31467 |
| UI selection→worker-observed RUNNING/SHOW | 289413 / 289438 | **209321 / 159276** |
| ACTIVE publication→GUI object-ready | 242468 / 242455 | 101170 / 102229 |
| Instance initial GUI enqueue→dequeue maximum | 152080 / 151612 | **72161 / 22386** |
| Button label refresh, 8208 pixels / 2 strips, six samples | 16332–17954 | **7863–8812** |
| Hello selection→worker-observed clean unload | 139129 / 139248 | 139266 / 139478 |
| Hello unload→result object-ready | 118268 / 117540 | **39395 / 38472** |
| Result-only refresh, 36384 pixels / 7 strips | 82407 / 83627 | **45676 / 45686** |
| First Back refresh, 76840 pixels / 12 strips (**) | 181322 / 181370 | **98605 / 96335** |
| Settled launcher idle flush delta | 0 | 0 |
| Observed transfer errors / completion timeouts | 0 / 0 | 0 / 0 |

Equivalent full-frame handlers improved **2.36× / 2.48×**, reductions of
**57.6% / 59.7%**. Waiting fell **85.2% / 84.3%**. Label handlers approximately
halved while retaining partial invalidation; this does not mean label LVGL
mutation itself became faster. Individual callback→visible-update latency is
not exported and remains unmeasured. Adding the adjacent selection→RUNNING
and ACTIVE-publication→object-ready intervals gives approximately 532/532 ms
before versus 310/262 ms after; this is a composed software readiness boundary,
with the worker's sampling/publication gap, not exact selection→visible pixels.
The below-100 ms handler target was reached in one of two full-frame samples;
the other was 104.9 ms. Do not claim it is uniformly achieved.

(*) DMA accounting is attributed when GUI **consumes** completion, not when
the strip was submitted. A row can consume the preceding row's last strip and
leave its own final strip for the next row. The candidate's warm loading row
recorded handler 148973, submission sum 103109, and a **99768 us single-submit
maximum**. Its last completion is consumed in the next full-frame row, whose
138506 sum therefore includes that preceding submission's delay. It is not
138506 us of pixel-only transfer for that full frame. This retained outlier is
not omitted, subtracted to manufacture a passing figure, or called a DMA error.
Its precise origin inside/surrounding panel submission is **PENDING**; older
3E.1 evidence already retained comparable ~98 ms submit/DMA outliers. The
existing trace does not separate swap, individual panel commands, driver wait
and preemption. No additional clock escalation or speculative CPU/scheduling
change was made. Exact per-frame wire duration and a separately isolated pure
LVGL rasterization time remain unmeasured.

(**) Back is a multi-refresh operation. At 3 MHz the subsequent 53292-pixel
refresh took 118399/119895 us. At 20 MHz, the trace snapshot may precede that
later refresh; the first cycle's 53292-pixel tail appears in the next captured
window at 70779 us. Do not compare a first-refresh number with the baseline's
two-refresh total. GUI-ready and DMA-idle snapshots do not promise no later
dirty region. Both Back operations returned the persistent launcher without
re-executing Hello. Exact touch→last-visible-pixel time remains PENDING.

## Remaining bottleneck and buffer decision

After the bus change, **74–81 ms of full-frame handler work occurs outside
the measured completion wait**, versus about 24–26 ms waiting. This demonstrates
that the bus wait is no longer dominant. Remaining work includes LVGL layout/
rasterization, draw dispatch, swapping, polling commands, timer processing and
preemption; this trace does not uniquely assign all of it to LVGL rasterization.
Loading/spinner redraw and native startup still affect ordinary navigation.

Retain the single 20-line internal buffer. The same 18 strips now transfer with
much less waiting; another buffer would cost 14400 internal bytes and require
a new ownership/overlap validation gate. Even ideal overlap could only hide the
remaining transfer portion, not the measured non-wait work or native loading
outlier. A 40 MHz payload could theoretically save another 12.96 ms, but would
not address the dominant remaining stage and is not worth another OTA/electrical
experiment here. No double buffer, larger strip, PSRAM DMA, refresh schedule,
task priority/affinity, CPU-frequency or compiler optimization change was made.

## Memory, resources, recovery and limits

Target static memory is unchanged: `.dram0.data` **16751**, `.dram0.bss`
**96256**, `.iram0.text` **73587** bytes. Flash code grows 188 bytes and rodata
48 bytes; final image has 80.1% slot headroom. The size report's DIRAM aggregate
is 171238 bytes, with 170522 remaining; that is linker-region accounting,
not an independent live free-heap measurement. There is still one 14400-byte
DMA buffer and no added persistent trace/GUI allocation.

Live idle internal free is **218439 bytes** on both baseline and candidate.
Every Hello/UI runtime restores its before/after heap samples exactly: internal
free/largest **218419/139264** bytes; PSRAM free **8382780** for Hello or
**8380764** for UI. Runtime snapshots still include executor-owned input, whose
separate retirement is proven by busy=0; these are not direct idle PSRAM samples.
Final launcher/chrome retains **37 objects**, LVGL pool free **42880 bytes**
(fresh baseline 42888); native UI peaks **1792/1812 bytes**. Pool snapshots can
include pending system presentation, so they are not exact per-app allocations.
Both UI instances balance their resources; final UI acquired/released 18/18,
generation/outstanding/pending zero. Runtime final counters `[28,27,1,0]` include
one reclaimed resource and **zero outstanding**. No accumulating instance heap
or object growth was observed in this bounded sample.

Observed minimum remaining stack: **4832 worker / 2744 GUI bytes**, versus this
goal's fresh baseline **5040/2744** (historical 3E.1 GUI minimum was 2776).
Stacks remain 12288 worker, 8192 GUI, 12288 executor. Target compiler frames:
start_lcd 320, app_main 960, worker 1680, flush 48, ISR 48, wait 32 bytes.
These are individual frames, not worst-case call-chain proofs.

Final independent read-only state: **B 0.3.7 VALID/STABLE**, A 0.3.6
VALID/compatible, OTA IDLE, rollback=1, self-test checked/passed=1,
safe/failures/latch/trial/fault/test-build=0. USB remained functional throughout.
No native app autostarted. Two Hello applications really executed Xtensa and
returned **42**; the system result remained present for the bounded hold, and
Back did not execute again. Two installed ABI 3 UI instances executed native
code, delivered three button events each, advanced timers, exited via system
navigation and fully unloaded; second-cycle suspend/resume succeeded. Wrong
Back, duplicate launch and retired input/Exit were rejected. ABI 2 is host
regression evidence, not an installed physical ABI 2 package test.

Settled final counters: **659 submissions / 659 ISR completions, zero errors**,
no idle flush, launcher foreground, no ELF/input ownership or application
resources. Captured boot/acceptance traffic contains no panic, watchdog,
brownout, completion-timeout, overlapping-flush or enqueue-failure marker.
Boot-inclusive maxima remain handler 157474, wake lateness 311885,
flush-entry→ISR 101104, submit 99768 and wait 3860 us. Early evicted samples
cannot be attributed to a particular operation. No counters were reset to
remove an outlier. Safe Mode/BootGuard policy is covered by host regression and
fresh RS1 status; no new fault/recovery/power campaign was run.

Installed packages and full-package digests are unchanged:

- `org.roundie.hello` 1.1.0, ABI 1, **Roundie Hello Update**:
  `9a36506f21136344e557235aaa8caa532a52551dd9eb28936ffa3a8a6d054cd1`.
- `org.roundie.ui` 1.0.0, ABI 3, **Roundie UI**:
  `76ab4c5be328397900de02784f0d5f3f8660a56ba82e974bf5b4c8e17d5bfcd7`.

RTC remains valid/OS=0 without SET. This is not a host-UTC accuracy, drift or
continuous-discipline claim. NVS and LittleFS remain healthy/mounted.

## Human acceptance — PENDING

No camera or physical observer was available. Faster handler/transfer counters
do not prove correct pixels, reliable physical touch, absence of progressive
painting, tearing, panel scanout timing, electrical margin or long-run stability.
The user should perform these actions sequentially, without power cycling:

1. Tap **Roundie Hello Update** and compare the transition with 0.3.6; verify
   **42** persists.
2. Tap **Back** and compare launcher repainting.
3. Launch **Roundie UI**, check the label/button with a real finger, then use
   system **Exit** and compare the return.

**Does progressive screen painting remain noticeable in those transitions?**
Record the user's answer separately. Until supplied, visual correctness,
perceived responsiveness and physical touch acceptance stay **PENDING**.
Exact wire/scanout timing, isolated CPU substage attribution, the retained
loading-submit outlier, worst-case performance and endurance also remain open.

## Final answers

- **Why 3 MHz?** Conservative bring-up kept the vendor's slow ID-read phase
  for pixel writes; it was not a demonstrated panel write limit.
- **Supported hardware clock?** Published controller write timing permits at
  most 62.5 MHz under its stated conditions; vendor requests 80 MHz, which is
  excluded here. This unit's bounded software/device evidence supports 20 MHz;
  visual and electrical margin are still unverified.
- **Selected clock and reason?** 20 MHz writes, 3 MHz ID read: substantial gain
  with margin, unchanged buffers and no speculative next-stage optimization.
- **Full-screen improvement?** 247.3–247.8 → 99.8–104.9 ms, 2.36–2.48× faster.
- **Ordinary labels?** Same 8208-pixel dirty region, 16.3–18.0 → 7.9–8.8 ms.
- **Dominant stage?** Non-wait handler work now dominates; pure LVGL CPU cost
  has not been separately isolated.
- **DMA/synchronization failures?** None observed in bounded acceptance;
  timeout/reuse/ISR invariants also pass host tests.
- **Visual repainting verified?** PENDING human observation; no claim of
  artifact elimination or tear-free output.
- **Remaining work?** Human gate and the measurement limits above. No 3F work.
- **Delivery?** Implementation/tests/source investigation in `79e12ec`; final
  measurements and cross-links in the subsequent documentation commit. Nothing
  pushed; final repository status is checked after that commit.

Evidence is outside Git in
`/mnt/Dane/Development/roundie-backups/display-3e2-2026-10-19/`.
No generated binary, raw traffic, backup or private device identifier is committed.
