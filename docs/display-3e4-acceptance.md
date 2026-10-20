# Milestone 3E.4 acceptance — 2026-10-20

**Decision:** retain 0.3.8, **26.667 MHz / two 20-line buffers** as the default.
40 MHz with the same buffers worked in bounded physical testing, but the
native-UI full-frame median improved only **0.215%**, within sample variation.
The user reports a modest visible improvement on 0.3.9; this is recorded
separately below and does not establish a reproducible timing advantage.
No 3F work or further performance experiment is included.

## Evidence scope and initial state

Board: Waveshare ESP32-S3-Touch-LCD-1.85B, ESP32-S3, 16 MiB Flash / 8 MiB
PSRAM, ST77916 variant 2 and CST816S. Exact initial physical state was
**A/ota_0 0.3.8 VALID/STABLE selected; B/ota_1 0.3.7 VALID/compatible**.
The worktree was clean before editing. Prior [3E.3 evidence](display-3e3-acceptance.md)
is preserved as historical evidence, including its then-untested dual/40 MHz entry.

Raw traffic, per-operation JSON, frozen BIN/ELF/config, manifests, scripts and
build/test logs are outside Git:
`/mnt/Dane/Development/roundie-backups/display-3e4-2026-10-20/`.
Important records: `preflight-passed.json`, `original-0.3.7-archive.json`,
`baseline-{hello,ui}-[0-4].json`, `clock40-{hello,ui}-[0-4].json`,
`{baseline,clock40}-final.json`, `candidate.json`, `config-diff.json`,
`candidate-ota-{ready,new,restart,timing}.json`, `candidate-confirmed.json`,
`comparison.txt`, `returned-a.json`, `final-readonly-state.json`.
No raw logs, artifacts or private device identifiers enter Git.

**Verified read-only preflight:** physical USB/ROM identity, Flash size, boot
PSRAM detection, exact REP partition table, installed rollback bootloader bytes,
two VALID compatible images and identities, A selected/running, BootGuard
normal/STABLE, OTA IDLE, no loaded ELF/resources, healthy NVS/LittleFS, valid
RTC/OS=0 and unchanged package identities. The one bounded ROM read was
65,536 bytes plus two 288-byte headers (**66,112 B**), followed by a normal
return to A. No full-Flash read was repeated. The original 16 MiB recovery
backup hash and ROM procedure were checked. Original B 0.3.7's external frozen
artifact was parsed and its ELF identity matched before B was overwritten.

Installed packages throughout:

- `org.roundie.hello` 1.1.0, digest
  `9a36506f21136344e557235aaa8caa532a52551dd9eb28936ffa3a8a6d054cd1`.
- `org.roundie.ui` 1.0.0, digest
  `76ab4c5be328397900de02784f0d5f3f8660a56ba82e974bf5b4c8e17d5bfcd7`.

## Configuration, timing and ownership

**Verified source/build:** ESP-IDF v6.1 commit
`fff9895c82d744c7237be8847347bdd1b07c6643`, LVGL 9.5.0, ST77916 2.0.2,
ELF Loader 1.3.5. Candidate source `7330235`; test extension `7d17539`.
The only functional source/configuration change was the LCD write-clock
selection; the candidate also carries version 0.3.9. Frozen configuration
differs from final 3E.3 only in the two clock-choice booleans and write Hz.

Unchanged: CPU 160 MHz, global `-Og`, 33 ms LVGL refresh, 10 ms GUI delay,
batch 12, SPI2 queue depth two, PARTIAL RGB565, **2 × 360 × 20 × 2 = 28,800 B**
draw storage, internal DMA-capable SRAM, geometry/orientation, GPIO assignment,
panel initialization/detection, swap algorithm, ISR/wait path and tracing.
No additional source correction was needed for 40 MHz.

Pinned `esp_driver_spi`, S3 `spi_ll_master_cal_clock`, SPI panel IO, ST77916
driver and production board startup were inspected. Default SPI source is APB;
device register telemetry after panel initialization independently confirmed:

| Firmware | Source | pre | n | high cycles | direct | Actual write clock |
|---|---:|---:|---:|---:|---:|---:|
| 0.3.8 | 80 MHz | 1 | 3 | 1 | 0 | 26,666,666 Hz, integer truncation |
| 0.3.9 | 80 MHz | 1 | 2 | 1 | 0 | **40,000,000 Hz** |

The [ST77916 preliminary controller PDF](https://dl.espressif.com/AE/esp-iot-solution/ST77916_SPEC_V1.0.pdf),
page 39, §7.4.5/Table 5, was checked from the archived PDF. Write minima are
16 ns cycle, 7 ns high/low and 15 ns CS setup/hold under its specified rails,
25 °C and loading/edge conditions. 40 MHz gives nominal **25 ns cycle,
12.5/12.5 ns high/low**, and the unchanged one-cycle CS setup/hold gives
**25/25 ns**. These meet the stated nominal minima. CS-high gap, electrical
edges, data setup/hold, actual rails/loading and temperature margin were not
measured with instruments. This is not complete board electrical acceptance.

ID read remains a **3 MHz request** (pinned calculation: 80 MHz/27,
2,962,962 Hz integer report). Only the original slow IO reads opcode 0x0b;
after synchronous ID `00 02 7f 7f`, that IO is deleted before creating the
write IO. Detection/init commands and four QSPI data lanes are unchanged;
there is no fast read, runtime retuning or new command protocol.

**Ownership evidence:** both buffers still request
`MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`. Pinned LVGL renders into the alternate
buffer while DMA owns the previous buffer, waits before the next submission,
then switches buffers. The custom wait consumes the ISR notification; LVGL
clears its flushing state on return. Success does not call `flush_ready`
prematurely. The sole outstanding transfer has a monotonically increasing
identity; the ISR records completion and requests the existing conditional
yield. A pending second submission restarts; a two-second missing completion
restarts instead of permitting buffer reuse. No assertion/check was disabled.

Real-LVGL host tests check held-buffer hashes, byte-exact RGB565 including
halfword alignment, odd-width partial regions and final completion across
refresh iterations. Production callback tests cover notification consumption,
enqueue failure, timeout and ISR yield. On each measured full native frame,
`carry_done=1`, last submitted ID = last consumed ID + 1: final DMA carries
into the next refresh. Settled counters reconcile exactly in both runs.
Together these support correct ownership in this bounded test; physical bus
data and all possible race schedules are not proven by telemetry.

## Host and target validation

**Host:** all **82 tests PASS**, including production C/real LVGL with
ASan/UBSan where applicable. Existing profile acceptance/rejection, slow-read
IO creation/deletion/error paths, DMA allocation failures, ownership/order,
cross-refresh reuse, timeout, RGB565/partial output, GUI queue, Hello result 42,
ABI v3 lifecycle/ResourceManager, Safe Mode and OTA regressions remain covered.
The divider test now also checks the actual pinned 3 MHz calculation and all
40 MHz clock fields/nominal timing under sanitizers. This is software evidence.

**Target:** pinned REP build PASS; unchanged REP table, RPK1 and ABI layouts,
ESP32-S3 image checksum/validation hash PASS. Candidate image **730,576 B**,
slot headroom **2,939,440 B**. DIRAM **173,350 B**, linker remainder
**168,410 B**, `.bss` **98,368 B**, `.data` **16,751 B**, matching 3E.3.
Draw buffers are dynamic and separate from these link-time figures. GUI/worker
stack configuration and code paths are unchanged; measured margins follow below.
No warning, guard or comparison was suppressed.

The first local build inherited cached `PROJECT_VER=0.3.8`; image inspection
caught this before freeze/OTA. It was rebuilt with explicit `-DPROJECT_VER=0.3.9`.
The incorrect-version artifact was never uploaded. Successful final commands:

```sh
. /home/solve/.espressif/v6.1/esp-idf/export.sh
python -m unittest discover -s tests -v
idf.py -B build-rep -DROUNDIE_PROFILE=rep -DPROJECT_VER=0.3.9 build
python tools/validate_storage_layout.py --build build-rep
python -m esptool image-info build-rep/roundie_rep.bin
idf.py -B build-rep size
```

Frozen 0.3.9 identity:

- BIN SHA-256 `0639b5d9dee35289a78f047996b0f68162befb3953707a54f2288925e2a81eb5`.
- ELF SHA-256 `64f714841eccff8e3d5b0fbcfdf33b7a24cf09875354c4a897bdcdd71f8ed68a`.

## Genuine OTA

Exactly **one** genuine RO1 upload targeted inactive **B/ota_1 at 0x390000**.
It replaced 0.3.7; 0.3.7 is now external recovery evidence, not an installed
fallback. A 0.3.8 was never rewritten. Recorded transitions:
**B READY → NEW → boot PENDING_VERIFY → core self-test ESP_OK → VALID →
BootGuard normal/STABLE**, then OTA IDLE. Upload/activation host time:
**74.903 s**. Independent post-boot status confirmed B selected/running 0.3.9,
40 MHz actual, two 20-line buffers, A VALID/compatible, healthy NVS/LittleFS,
RTC valid/OS=0 and working USB diagnostics. No app autostart occurred.

No bootloader/table write, format, NVS erase, package change, RTC SET, eFuse,
security, raw Flash update, power interruption or endurance test occurred.

## Comparable physical measurements

Each firmware ran **five Hello transitions, five Back transitions, five native
UI activation/Exit cycles and fifteen counter updates**, plus timer progression
and a settled two-second idle observation. Every automated input was
**INJECTED**. The same script and definitions were used; counters were never
reset to discard outliers. Native UI uses the same seven realized objects,
129,600 pixels, 18 strips and the same installed package on every activation.

All times below are **ms**. Main table: median **[minimum–maximum]**. Full-frame
metrics have n=5; partial counter n=15. Hello's ordinary 36,384-pixel redraw
has n=4 baseline and n=5 candidate; the additional baseline first redraw is
reported separately. No P95/P99 or worst-case guarantee follows from these samples.

| Metric | 26.667 MHz / dual | 40 MHz / dual |
|---|---:|---:|
| Actual QSPI clock | 26.667 MHz | 40 MHz |
| Draw buffers | 2 × 20 lines | 2 × 20 lines |
| Full-frame handler | **82.282 [81.825–83.501]** | **82.105 [80.505–83.147]** |
| Full-frame render span | 70.437 [67.331–74.475] | 70.070 [68.625–70.624] |
| DMA completion wait, sum | 0.486 [0.474–0.496] | 0.493 [0.462–0.512] |
| DMA submission, sum | 13.918 [13.562–14.256] | 13.546 [12.435–15.941] |
| Panel driver submission, sum | 9.052 [7.375–9.892] | 8.118 [7.358–10.754] |
| Layout/dirty-area setup | 8.819 [7.543–11.123] | 9.164 [8.818–11.057] |
| RGB565 swap | 5.204 [4.288–6.218] | 5.187 [4.317–6.188] |
| Render residual | 56.364 [52.590–60.083] | 56.104 [52.172–56.578] |
| Legacy DMA interval, sum | 32.668 [31.798–32.923] | 26.671 [25.190–27.242] |
| Hello result redraw, 36,384 pixels | 43.540 [43.443–44.539] | 43.507 [43.494–44.493] |
| Back redraw, 76,840 pixels | 82.447 [81.411–82.501] | 82.449 [82.408–82.451] |
| Partial counter, 8,208 pixels | 8.226 [8.087–8.751] | 8.718 [8.110–8.809] |
| Native UI readiness | 169.259 [169.253–199.303] | 169.256 [169.255–179.256] |
| Initial UI queue delay maximum, per instance | 31.022 [30.731–60.650] | 31.073 [30.696–40.812] |
| Settled idle additional flushes | 0 | 0 |
| Observed DMA errors | 0 | 0 |
| Internal post-workload idle free heap, B | 201,475 | 201,475 |
| GUI stack remaining minimum, B | 2632 | 2648 |
| Worker stack remaining minimum, B | 4912 | 4992 |

`handler`, `render`, `layout`, `submit` and `wait` retain RP1 format 2 boundaries.
Render includes submissions/inter-strip waits and excludes final DMA tail;
residual = render − submit − wait, a wall interval including scheduling,
not pure on-core rasterization. Legacy DMA includes software and a carried
completion, not just wire time. Queue delay is enqueue→start-of-apply maximum
for the initial widget batch of that instance; the first command alone is
not separately timestamped. Readiness is system foreground readiness,
not last visible pixel. All these metrics can overlap and are not additive.

Individual full-frame samples, ordered cycles 0–4, ms:

| Firmware/cycle | Handler | Render | Wait | Submit | Layout | Swap | Driver | Residual |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.3.8 / 0 | 83.501 | 74.475 | 0.474 | 13.918 | 7.543 | 6.218 | 7.700 | 60.083 |
| 0.3.8 / 1 | 82.306 | 67.373 | 0.496 | 14.189 | 11.103 | 4.297 | 9.892 | 52.688 |
| 0.3.8 / 2 | 81.912 | 70.493 | 0.486 | 13.562 | 8.819 | 6.187 | 7.375 | 56.445 |
| 0.3.8 / 3 | 82.282 | 67.331 | 0.485 | 14.256 | 11.123 | 5.204 | 9.052 | 52.590 |
| 0.3.8 / 4 | 81.825 | 70.437 | 0.490 | 13.583 | 8.819 | 4.288 | 9.295 | 56.364 |
| 0.3.9 / 0 | 80.505 | 70.070 | 0.480 | 13.486 | 9.164 | 4.324 | 9.162 | 56.104 |
| 0.3.9 / 1 | 82.105 | 70.624 | 0.496 | 13.550 | 8.823 | 6.187 | 7.363 | 56.578 |
| 0.3.9 / 2 | 82.594 | 68.625 | 0.512 | 15.941 | 11.057 | 5.187 | 10.754 | 52.172 |
| 0.3.9 / 3 | 82.050 | 70.595 | 0.493 | 13.546 | 8.818 | 6.188 | 7.358 | 56.556 |
| 0.3.9 / 4 | 83.147 | 68.753 | 0.462 | 12.435 | 10.414 | 4.317 | 8.118 | 55.856 |

Remaining individual samples, ms, in execution order:

| Metric | 0.3.8 | 0.3.9 |
|---|---|---|
| Hello result | **45.792 (43,040 pixels)**; 43.561; 43.518; 44.539; 43.443 | 43.503; 43.494; 44.435; 43.507; 44.493 |
| Back | 81.411; 82.501; 82.431; 82.481; 82.447 | 82.451; 82.408; 82.449; 82.421; 82.451 |
| Counter cycle 0 | 8.087; 8.205; 8.727 | 8.726; 8.796; 8.729 |
| Counter cycle 1 | 8.191; 8.138; 8.751 | 8.226; 8.638; 8.781 |
| Counter cycle 2 | 8.226; 8.746; 8.741 | 8.242; 8.110; 8.718 |
| Counter cycle 3 | 8.198; 8.125; 8.731 | 8.218; 8.721; 8.667 |
| Counter cycle 4 | 8.179; 8.653; 8.743 | 8.723; 8.809; 8.688 |
| Readiness | 199.303; 169.253; 169.259; 169.260; 169.258 | 169.258; 169.255; 169.256; 169.255; 179.256 |
| Initial queue maximum | 60.650; 31.043; 30.769; 31.022; 30.731 | 31.073; 30.722; 31.133; 30.696; 40.812 |
| Legacy DMA sum | 31.798; 32.668; 32.869; 31.834; 32.923 | 26.496; 26.682; 27.242; 26.671; 25.190 |
| Full launcher on ordinary UI Exit | 99.582; 99.513; 99.547; 99.561; 100.691 | 99.557; 99.528; 102.270; 99.550; 99.521 |

The first baseline result merged another dirty area; it is retained rather
than compared as a 36,384-pixel frame or silently discarded. Ordinary Hello
median reduction is about **0.075%** with the stated unequal n. Back median
is **0.002% slower**, counter median **5.98% slower**, readiness changes by
only **3 µs**. Small samples and timer/GUI phase variation do not establish
causal regressions or a startup improvement. Ordinary Exit also has a preceding
6,656-pixel strip update in both runs; it is not pooled with native activation.

Full handler improvement is **0.177 ms / 0.215%**, with overlapping ranges
and per-cycle changes of −3.588%, −0.244%, +0.833%, −0.282%, +1.616% in
candidate duration relative to baseline (negative means faster). Render median
improves **0.521%**, submission median **2.673%**; completion wait remains
roughly half a millisecond. Legacy DMA median drops **18.36%**, consistent
with shorter transfers, without a comparable handler benefit. Source/trace
evidence supports transfer/render overlap; it does not uniquely isolate
polling command time, driver recycling, preemption or completion cadence.
No additional scheduling or rendering optimization was attempted.

## Outliers, memory and correctness

Approximately 100 ms extra layout/setup remains present. Baseline retained
sequence 286: layout **105.991 ms**, handler **143.200 ms**, 68,720 pixels.
Hello-loading sequences 298/316/334/352: layout
**106.668/106.629/106.630/106.522 ms**. Candidate Hello-loading sequences
281/297/315/333/351: layout
**106.242/106.684/106.646/106.487/106.571 ms**, handlers
**145.890/146.350/146.188/146.468/146.242 ms**. Their largest individual
driver submissions are about 0.5–2.2 ms, not 100 ms. These retained stage
boundaries locate the current outlier interval without identifying its cause.

Boot-inclusive baseline/candidate maxima respectively: handler
**146.503/149.461 ms**, pump **56.421/56.310 ms**, wake lateness
**288.763/306.105 ms**, individual submission **2.554/2.565 ms**, DMA interval
**3.633/3.279 ms**, individual wait **0.055/0.846 ms**. Counters include startup;
earliest trace records were evicted before benchmark capture. The historical
approximately 100 ms individual submission did not recur in these boot-inclusive
maxima; its underlying cause is **not solved**. No counters/outliers were reset.

Every UI cycle in both runs restored the exact runtime before/after heap tuple:
internal free/largest **201,455/126,976 B**, PSRAM free/largest
**8,380,764/8,380,764 B**, executable mapping free **8,257,536 B**.
These snapshots include retained executor input until its separate retirement;
idle internal free was **201,475 B** on each. No observed heap growth, added
SRAM allocation or stack-budget change follows from the clock. Remaining stack
minima are **2632/2648 GUI B** and **4912/4992 worker B** for baseline/candidate;
the small high-water difference is a bounded observation, not added capacity
or a worst-case proof.

Each Hello executed actual Xtensa code, returned **42**, completely unloaded,
and retained its copied result across a hold; Back did not re-execute it.
Each ABI v3 UI delivered three INJECTED button callbacks and timer progression,
then cooperatively exited with zero app resources/objects/pending commands.
Stale input and concurrent launch were rejected. Both runs ended UNLOADED,
busy/timers/subscriptions=0, no active UI generation, clean foreground launcher.
Settled display counts: baseline **927 consumed/927 ISR**, candidate
**916 consumed/916 ISR**, each **0 errors**. Different counts reflect retained
loading/dirty-region differences, not equivalent-work frame counts. There were
no observed completion timeouts, overlapping submissions, unexpected resets,
watchdogs, panics, DMA corruption symptoms or USB losses during automated tests.
No claim of physical pixel correctness is inferred from those counters.

## Human observation and final selection

The user was offered the live 0.3.9 procedure: Hello Update → verify 42 and
observe repaint → Back/launcher → UI → counter with a real finger → Exit.
Their direct response was: **“Szczerze przyznam, że jest lepiej, ale nie jest
to ogromny breakthrough.”** Record **USER-REPORTED VERIFIED** for a modest
perceived improvement on 0.3.9. This does not separately confirm every procedure
step, pixel accuracy, tearing absence or finger reliability; those detailed
acceptance items remain **PENDING**. The earlier 20 MHz improvement with slight
progressive repainting remains separate historical user evidence.

**Engineering decision:** the modest visual report is acknowledged, but the
controlled full-screen gain is 0.215%, ranges overlap and ordinary Back/partial
updates/readiness show no consistent quantitative gain. This does not justify
reducing nominal clock timing margin. Restore the supported source default and
version to 0.3.8/26.667 MHz, retaining the tested optional 40 MHz profile and
stronger divider coverage. Retain frozen 0.3.9 and logs outside Git. No further
OTA, artifact substitution, experiment or rebuild is needed for selection.

**Verified final selection:** existing RS1 workflow REQUEST → Safe Mode →
CLEAR → SELECT exact A ELF identity → REBOOT, then independent normal-boot
confirmation. This changes guarded boot metadata, not A's application bytes.
A again passed PENDING_VERIFY/core self-test/VALID and BootGuard STABLE.
No second OTA was performed.

Exact independently verified final state:

- **A/ota_0: 0.3.8 VALID/STABLE, selected and running**, 26,666,666 Hz actual,
  two internal DMA-capable 20-line buffers; ELF
  `23b9ba7c13083f5bdb21177d6c90722468b458efab55b79d61af749d885c321c`.
  Preserved external original BIN SHA-256
  `dafdb8ec50a887d3623b6b253a3464160f50dfbaa15207011a3ac90a3d9a48a8`.
- **B/ota_1: 0.3.9 VALID/compatible, unselected experimental image**; ELF
  `64f714841eccff8e3d5b0fbcfdf33b7a24cf09875354c4a897bdcdd71f8ed68a`.
  Its bounded correctness acceptance is recorded, without long-term electrical
  certification or a recommendation to use it as the default.
- OTA **IDLE**, rollback enabled, critical self-test checked/passed; Safe Mode
  off, BootGuard stage 4/STABLE, failures/latch/trial/fault/test-build=0.
- NVS ESP_OK, LittleFS mounted/capacity valid (9,240,576 B total,
  57,344 B used), RTC valid/OS=0, package identities/digests unchanged, USB
  diagnostics operational. No app has run since the final return: ELF
  UNLOADED, busy=0, resource tuple `[0,0,0,0]`, no timer/subscription/UI owner.
- Fresh-return idle internal free **213,967 B**, GUI remaining stack minimum
  **2648 B**. This is a **different workload phase** from the post-application
  benchmark's 201,475 B; it is not a clock-related memory gain. Final display
  **388 consumed/388 ISR completions, zero errors**, zero additional flushes
  in the separate two-second idle check. Counters are from the new boot,
  without any diagnostic reset to hide benchmark history in the saved logs.

Implementation/test history: `7d17539` (divider/read/timing tests), `7330235`
(clock-only 0.3.9 candidate), `be7c562` (restore 0.3.8/default 26.667 MHz after
comparison). This report and cross-links form a separate documentation commit.
No previous commit was amended, no generated artifact was committed, and
no remote push occurred. Frozen candidate identities were rechecked after
selection; no further build or deployment was needed.
The local build directory was reconfigured back to PROJECT_VER=0.3.8 and
the 30 MHz request to avoid retaining the experimental CMake cache override.
It was not relinked; its existing BIN/ELF are still the experimental build.
The frozen external candidate directory is the authoritative 0.3.9 artifact.

Long-term electrical stability, measured signal integrity/CS-high timing,
temperature/load margin, scanout, detailed pixel/tearing/finger acceptance and
endurance remain separate unverified questions. No 3F work was started.
