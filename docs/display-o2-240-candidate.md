# Display CPU-cost candidate — `-O2` and 240 MHz (build only)

Scope: Waveshare ESP32-S3-Touch-LCD-1.85B. Follows the
[3E.4 comparison](display-3e4-acceptance.md), where a full native-UI frame
(129,600 pixels, 18 strips) took about 82 ms (about 12 FPS) and measured
completion waits were about 0.5 ms.

## Evidence and hypothesis

**Verified fact (source: 3E.4 record, `sdkconfig.rep`):** transfer waits are no
longer dominant; about 56 ms of the full-frame handler is a residual
rendering-path wall interval. The firmware builds at `-Og`
(`COMPILER_OPTIMIZATION_DEBUG`) with the CPU at 160 MHz, one software draw unit
and no draw assembly (`LV_USE_DRAW_SW_ASM=0`).

**Assumption (unverified, see "Why 60 ms remains"):** LVGL software rasterization
is CPU-bound, so `-O2` and 240 MHz shorten that interval. The gain, and whether it is worth the extra
current draw, is a **pending experiment**.

**Pending experiment:** OTA the candidate to the inactive slot and repeat the
3E.4 workload (five cycles, native-UI full frame, Hello result, Back, counter
update). Acceptance criterion: median full-frame handler below the 82.105 ms
0.3.8 median with no new DMA errors, watchdog events, or reduced stack margins.
Cache size (16 KB instruction cache), second-core drawing and the RGB565 swap
are not part of this candidate.

## Candidate

Only two configuration values differ from `sdkconfig.rep`:

| Setting | 0.3.8 | Candidate 0.3.10 |
|---|---|---|
| `COMPILER_OPTIMIZATION` | DEBUG (`-Og`) | PERF (`-O2`) |
| `ESP_DEFAULT_CPU_FREQ_MHZ` | 160 | 240 |

The candidate configuration and build live outside Git in
`/mnt/Dane/Development/roundie-backups/perf-o2-240-2026-11-02/`. The default
`sdkconfig.rep` and the production profile are unchanged; this is not yet an
architectural decision to change the project-wide optimization or CPU frequency.

## Source change required by `-O2`

`-O2` made GCC 15.2 report `-Wformat-truncation` in `path_for()`
(`components/roundie_packages/manager.c`), because a directory entry name may be
255 bytes. The function already rejected IDs of 64 bytes or more and checked the
`snprintf` result, so behavior was correct. The format now bounds the ID to
`ROUNDIE_PACKAGE_ID - 1` characters, which does not change accepted paths.
`-Werror` was not relaxed.

## Verification, by category

- **Host tests:** 82 tests pass with the IDF environment active
  (`python -m unittest discover -s tests`). Without `IDF_PATH`, two tests fail
  at import/environment setup, identically on the unmodified tree.
- **Target compilation:** pinned IDF v6.1 REP build exits 0 with no warnings,
  explicit `-DPROJECT_VER=0.3.10`. `validate_storage_layout.py` passes against
  the build. `image-info` reports a valid ESP32-S3 image.
- **Device tests:** see "Device results" below; short software-driven runs only.

| Frozen build identity | Value |
|---|---|
| Image | 761,808 bytes, 79.2% slot headroom (2,908,208 B) |
| `roundie_rep.bin` SHA-256 | `40c6374590052a1d2102e9d6eaac43e51e2168e79ff872ae1d2b289b2cc20fb7` |
| `roundie_rep.elf` SHA-256 | `6412732c699768065c19dd1746a8bf681fa13ee6efc90796ee2eed681b891b24` |
| DIRAM | 168,934 B used, 172,826 B remaining |

## Device results (2026-11-02)

**Deployment (authorized for this goal):** one genuine RO1 upload (761,808 B,
digest matches the frozen `roundie_rep.bin`) and ACTIVATE to inactive `ota_1`.
The running 0.3.8 in `ota_0` was not written and stays VALID as fallback. After
restart: running `ota_1`, version 0.3.10, OTA state VALID (self-test confirmed
rollback), Safe Mode off, 0 failures, stage 4, RO1 `verify` matches the image.
The previous `ota_1` content (experimental 0.3.9) was overwritten; its frozen
image remains in the 3E.4 backup directory. No bootloader, table, NVS, LittleFS,
package, eFuse or full-Flash operation occurred.

**Workload:** the 3E.4 scenario, five Hello and five native-UI cycles, injected
input (no finger), three counter events per UI. Raw records are outside Git in
the candidate directory (`o2-*.json`, `measure_o2.py`). Medians of n=5 (full
frame, Hello, Back) and n=15 (counter); 0.3.8 values are from the 3E.4 record.

| Metric | 0.3.8 | 0.3.10 | Change |
|---|---:|---:|---:|
| Full native-UI frame handler (129,600 px, 18 strips) | 82.282 ms | **60.493 ms** | −26.5% |
| Full-frame range | 81.825–83.501 | 59.162–60.560 | |
| Render (includes waits) | 70.437 | 54.183 | |
| Layout | 8.819 | 5.226 | |
| RGB565 swap | 5.204 | 2.772 | |
| Driver submit | 9.052 | 7.087 | |
| Completion wait | 0.486 | 0.720 | |
| Hello result redraw | 43.561 | 29.489 | |
| Back redraw | 82.447 | 59.822 | |
| Counter partial (8,208 px) | 8.226 | 5.113 | |

Full-frame handler 60.5 ms corresponds to about 16.5 full redraws per second,
versus about 12 before. This is a handler duration, not a measured display
frame rate or human-visible latency. The earlier <70 ms goal is met on these
samples; the <50 ms stretch goal is not. Residual time is still CPU-side.

**Observed:** display errors 0, flushes equal DMA completions, all Hello/UI
cycles returned to a clean launcher with zero application resources. Internal
free memory sampled by the runtime was 205,803 B before and after every
instance (no growth); the 3E.4 and RS1 samples were taken with different probes
and a 213,967 B RS1 figure on 0.3.8, so a code-size cost of roughly 8 KB
internal RAM is **not established** here and should be checked with RS1 on both
images.

**Not measured (PENDING):** current draw and temperature at 240 MHz; stack
high-water marks and watchdog margin under load (no endurance run); visual
smoothness, tearing and finger latency (needs human observation); behavior of
the 0.3.10 image after repeated OTA or Safe Mode/rollback transitions.

## Why 60 ms remains, and whether 33 ms is reachable

Analysis of the 0.3.10 traces above and of source; no new firmware was deployed.

**Verified fact (traces, `o2-*.json`, median per workload):** the residual
rendering-path interval follows strip count, not pixels.

| Pixels | Strips | Residual ms | ms per strip | µs per pixel |
|---:|---:|---:|---:|---:|
| 840 | 1 | 3.10 | 3.10 | 3.69 |
| 6,016 | 1 | 2.16 | 2.16 | 0.36 |
| 8,208 | 2 | 3.62 | 1.81 | 0.44 |
| 36,384 | 7 | 20.63 | 2.95 | 0.57 |
| 76,840 | 12 | 48.70 | 4.06 | 0.63 |
| 129,600 (native UI) | 18 | 43.6 | 2.42 | 0.34 |

An 840-pixel update costs as much per strip as a full frame. The RGB565 swap
touches all 129,600 pixels in 2.8 ms, which bounds plain pixel work on internal
RAM at this order. The native UI is a flat background, four labels and one
rounded button (`examples/ui_elf`), so arcs, shadows and blending do not explain it.
This replaces the rasterization assumption above: most of the 43.6 ms is fixed
work repeated per strip, about 2–3 ms each, or 500,000+ cycles at 240 MHz.

**Verified fact (source):** LVGL redraws the object tree for each PARTIAL strip,
and LVGL runs from Flash through a 16 KB instruction cache with 32-byte lines.
Flash is DIO at 80 MHz. `FREERTOS_HZ=100`, so the GUI loop's 10 ms delay is one
tick; `LV_DEF_REFR_PERIOD=33` caps refresh at about 30 Hz. `main.c` sleeps after
every handler, so a 60 ms frame gives about 14 full frames per second sustained,
not the 16.5 implied by handler time alone. Sustained cadence was never measured:
every number here comes from single frames triggered by navigation.

**Assumption (unverified):** instruction-cache misses dominate the per-strip
cost. A 32-byte DIO refill is roughly 150 flash clocks (about 1.9 µs), so 2.4 ms
equals about 1,300 misses, or 40 KB of code, which exceeds the cache. Clock and
`-O2` together cut the residual only 22%, less than the 33% from clock alone,
which suggests a wall-time component that does not scale with CPU frequency.
No cache-miss counter has been read.

**Limits for 33.3 ms:** wire time is 19.4 ms at 26.667 MHz and 13.0 ms at 40 MHz.
With two buffers it overlaps rendering, so it is not a hard limit. CPU-side work
must fall from about 60 ms to below about 30 ms. The 3E.3 statement that 40-line
buffering saves only part of the 7–8 ms driver time predates this per-strip
analysis and ignores the residual.

**Pending experiments, one variable each, by RO1 OTA to the inactive slot:**

| # | Change | Cost | Question |
|---|---|---|---|
| 1 | Two 60-line buffers (6 strips), raising the 14,464-byte SPI limit | +57.6 KB internal DMA RAM | Does residual fall with strip count? If it is fully per-strip, the frame reaches about 25–33 ms |
| 2 | 32 KB instruction cache | Less internal RAM | Is it cache pressure? |
| 3 | LVGL hot paths in IRAM, or XIP from PSRAM (§25 item 8) | IRAM or PSRAM; XIP needs ELF-loader revalidation | Is it Flash fetch latency? |
| 4 | Loop waits on LVGL's next timer; lower `LV_DEF_REFR_PERIOD` | Measured GUI/idle share | Does cadence reach handler time? |

Acceptance for any 30 FPS claim: a sustained workload of consecutive full-frame
invalidations, reporting measured frame period over at least 300 frames, plus
DMA errors, watchdog and stack margins. Single-frame handler time is not enough.
Without LCD_TE synchronization (§25 item 18) tearing stays unaddressed: wire time
exceeds a 16.7 ms scan at 26.667 MHz, though that scan rate is itself unverified.

## Experiment 1 result — two 60-line buffers (2026-11-02)

**Candidate:** 0.3.11, source `d6a4f1f`, the 0.3.10 configuration plus
`CONFIG_ROUNDIE_LCD_STRIP_LINES=60`. The repository default stays at 20 lines.
Configuration, build and raw records are outside Git in
`/mnt/Dane/Development/roundie-backups/perf-strip60-2026-11-02/`.
App SHA-256 `c88ed2b372587bf9fdc5f9987fd4e309cacb799e6fcfb1f60a22765e50be540a`,
ELF SHA-256 `70edec092b2f7f9a8cd7abd0534c1c05fffcbf7afa015923fe061f4873a6344b`.

**Verification:** 82 host tests pass; pinned REP build exits 0 with no warnings;
layout validation passes. One RO1 upload and ACTIVATE to `ota_0`; after restart
0.3.11 is VALID, self-test passed, Safe Mode off, stage 4, RO1 `verify` matches.
0.3.10 stays VALID in `ota_1`. 0.3.8 was overwritten; its image remains frozen
in `display-3e3-2026-10-20/candidate-c/` (ELF identity checked first).

**Workload:** identical five-cycle injected scenario. Medians, n=5:

| Native-UI full frame, ms | 0.3.10 (18 strips) | 0.3.11 (6 strips) |
|---|---:|---:|
| Handler | 60.493 | **32.937** |
| Handler range | 59.162–60.560 | 32.835–34.584 |
| Residual rendering path | 43.614 | 23.219 |
| Layout | 5.226 | 2.996 |
| Submit (swap + driver) | 9.857 | 4.564 |
| Completion wait | 0.720 | 1.202 |

Other handler medians: counter update 5.113 → 3.657 ms (now one strip), Back
59.822 → 38.313 ms, launcher full frames 71.420 → 40.982 ms. Hello result pixels
changed to 43,040 and 7 strips with a handler of 32.602 ms, so it is not directly
comparable. The 68,720-pixel loading frame improved only from 101.436 to
93.662 ms; its residual stays above 80 ms, so it is a different, unresolved cost.

**Interpretation:** the per-strip hypothesis holds. Two points fit about 1.7 ms
per strip plus 13 ms of fixed work, which is a two-point estimate, not a model
check. The instruction-cache explanation stays unverified.

**Observed:** display errors 0, flushes equal DMA completions, every cycle
returned to a clean launcher with zero application resources.

**Memory cost:** runtime internal free falls from 205,803 to 145,211 B and the
largest internal block from 131,072 to 69,632 B. PSRAM is unchanged. This is more
than half of the previous largest block, and no budget for it has been agreed.

**Not 30 FPS yet:** 32.9 ms is single-frame handler time, excluding the 10 ms
GUI delay, the final DMA tail and the 33 ms refresh timer. Source suggests about
23 full frames per second sustained (about 43 ms period). Experiment 4 and the
300-frame acceptance workload are still PENDING, as are power, stack margin,
visual tearing and finger latency. 0.3.11 is an experimental image; adopting 60
lines as the default needs a recorded decision.

## Sustained frame rate — 0.3.12 (2026-11-02)

**Candidate:** 0.3.12, source `4aa8dd7`, the 0.3.11 configuration plus
`CONFIG_FREERTOS_HZ=1000` and `CONFIG_LV_DEF_REFR_PERIOD=16`. Source changes: the
GUI delay follows LVGL's next timer, capped at 10 ms and never under one tick, and
RP1 `BENCH <frames>` (30–1000) invalidates the active screen in the GUI task.
Repository defaults stay 20 lines, 100 Hz and 33 ms, where the delay remains one
tick. App SHA-256 `c4654a04ea53c43b92f9b73e0583f5b1bbbd5f4f686c14110a3aa1e9f87162c0`.
Records: `/mnt/Dane/Development/roundie-backups/perf-paced-2026-11-02/`.

**Verification:** one RO1 upload and ACTIVATE to `ota_1`; 0.3.12 is VALID, Safe
Mode off, stage 4. 0.3.11 stays VALID in `ota_0`; 0.3.10 was overwritten and its
build remains in the candidate directory. `BENCH 10` and `BENCH 1001` are rejected
on the device.

**Measured, 300 consecutive full frames (129,600 px, 6 strips):**

| Scene | Mean FPS | Mean period | Min–max period | Mean handler |
|---|---:|---:|---:|---:|
| Native UI (`org.roundie.ui` active) | **31.5** | 31.7 ms | 30.0–35.8 ms | 30.5 ms |
| Launcher | **26.1** | 38.3 ms | 37.5–43.4 ms | 37.2 ms |

Period spans GUI iteration ends, so it measures frame production and not panel
scanout. Both runs: errors 0, 1,806 flushes. After the five-cycle regression,
flushes equal DMA completions (4,781), errors 0, every cycle returned to a clean
launcher, and GUI stack minimum is 2,424 B (0.3.11: 2,472 B).

**Five-cycle comparison with 0.3.11:** first native-UI frame handler 32.9 → 37.0 ms
(slower; cause not established), foreground readiness about 149 → 130 ms, runtime
internal free 145,211 → 144,731 B.

**Assessment:** 30 FPS holds on average for the native UI scene, not as a
per-frame guarantee, since its longest period is 35.8 ms. The launcher stays below
30 FPS. The full-frame benchmark is a synthetic worst case; real animations with
transforms, or the loading spinner (about 94 ms a frame on 0.3.11), can cost more.

**PENDING:** idle CPU and current draw at a 1 kHz tick, 240 MHz and a 16 ms refresh
period; visual tearing (no LCD_TE sync) and finger latency; runs longer than 300
frames. 0.3.12 is experimental. Adopting any of these settings needs a recorded
decision, including the internal memory budget.

## Cheap variant and cache/Flash experiments (2026-11-02)

All three use `-O2`, 160 MHz, two 20-line buffers, 100 Hz tick and 33 ms refresh,
with the same five-cycle workload and 300-frame `BENCH` as 0.3.12. Records are in
`/mnt/Dane/Development/roundie-backups/perf-{o2-160,icache32,iram}-2026-11-02/`.
Every image passed RO1 verify as VALID, with Safe Mode off, stage 4, display errors
0, flushes equal to DMA completions, and clean launcher return in each cycle.

| Variant | Full UI frame, median | Residual | BENCH UI handler | BENCH UI / launcher FPS | Runtime internal free / largest |
|---|---:|---:|---:|---:|---:|
| 0.3.13: `-O2` only | 70.0 ms | 48.8 ms | 59.1 ms | 15.0 / 12.5 | 205,323 / 131,072 B |
| 0.3.14: + 32 KB I-cache | 47.8 ms | 28.6 ms | 36.7 ms | 24.8 / 16.7 | 188,939 / 114,688 B |
| 0.3.15: + LVGL core in IRAM | 32.3 ms | 13.8 ms | 27.2 ms | 25.0 / 25.0 | 111,867 / 38,912 B |

With a 100 Hz tick and 33 ms refresh, BENCH periods settle on 40 ms, so 25.0 FPS
is that configuration's ceiling. Handler time is the better comparison there.

**Findings.** `-O2` alone took the 0.3.8 full frame from 82.3 to 70.0 ms. Moving
from 160 to 240 MHz then cut residual only 11% (48.8 → 43.6 ms), against the 33%
a CPU-bound path would show. Growing the instruction cache cut it 41%, and
moving the core out of Flash cut it a further 52%. This confirms by experiment that
instruction fetch, not CPU speed or pixel count, was the main cost. Miss counters
were still not read. 0.3.15's IRAM share is broad, not profiled: 31 objects,
about 94 KB of DIRAM by the size report and 77 KB of runtime free memory.

**Not deployed: XIP from PSRAM.** A build (`perf-xip-2026-11-02/`) exits 0 but
`validate_storage_layout.py` rejects it with `config mismatch:
SPIRAM_XIP_FROM_PSRAM`. The guard reflects the documented REP decision and has a
test, so it was neither bypassed nor changed. §25 item 8 stays open.

**PENDING:** power at each variant; ELF behavior under low internal memory on
0.3.15; a trimmed IRAM list based on profiling; visual tearing and finger latency.

## XIP from PSRAM (2026-11-02, user-authorized experiment)

The user explicitly authorized deploying XIP despite the REP layout guard. The
guard was not changed: `validate_storage_layout.py` still rejects both images
with `config mismatch: SPIRAM_XIP_FROM_PSRAM`, and §25 item 8 stays open. A copy
of the 0.3.16 build with only that flag cleared passed all other layout checks.
Only applications were uploaded; the installed bootloader was not touched, and
the bootloader these builds generate was not compared with it.

Base: `-O2`, 160 MHz, two 20-line buffers, 100 Hz tick, 33 ms refresh, HEAD
`ab727bd` plus `SPIRAM_XIP_FROM_PSRAM`, `SPIRAM_FETCH_INSTRUCTIONS` and
`SPIRAM_RODATA`. Records: `perf-{xip,xip16}-2026-11-02/`.

| Variant | Full UI frame, median | Residual | BENCH UI handler | BENCH UI / launcher FPS | Internal free / largest | PSRAM free |
|---|---:|---:|---:|---:|---:|---:|
| 0.3.13: no XIP, 16 KB I-cache | 70.0 ms | 48.8 ms | 59.1 ms | 15.0 / 12.5 | 205,323 / 131,072 B | 8,380,764 B |
| 0.3.14: no XIP, 32 KB I-cache | 47.8 ms | 28.6 ms | 36.7 ms | 24.8 / 16.7 | 188,939 / 114,688 B | 8,380,764 B |
| 0.3.16: XIP, 32 KB I-cache | 34.2 ms | 16.9 ms | 29.0 ms | 25.0 / 25.0 | 187,795 / 114,688 B | 7,670,984 B |
| 0.3.17: XIP, 16 KB I-cache | **35.7 ms** | 19.9 ms | 32.1 ms | 25.0 / 20.0 | **204,179 / 131,072 B** | 7,670,976 B |

Both XIP images passed RO1 verify as VALID with self-test passed, Safe Mode off,
stage 4, display errors 0, flushes equal to DMA completions, and five Hello/UI
cycles with clean return. FPS is capped at 25.0 here by the tick and refresh
settings, as above.

**Findings.** XIP cuts Flash fetch cost nearly as far as 0.3.15's IRAM placement
(32.3 ms), without its internal memory cost. 0.3.17 costs about 1.1 KB of internal
free memory and about 710 KB of PSRAM against 0.3.13, and halves full-frame time.
With XIP, the 32 KB cache adds only 1.5 ms, so it is not worth 16 KB of RAM.

**ELF:** native Hello and UI packages loaded, ran and unloaded under XIP in every
cycle, on the existing `ELF_LOADER_CACHE_OFFSET` path. This is short-run evidence.

**Flash writes:** `BENCH 1000` ran on the launcher on 0.3.16 during the RO1 upload
of 0.3.17 (77.7 s total, first ~51 s covered): mean period 51.4 ms, maximum
68.7 ms, errors 0, against 40.0 ms mean and 42.3 ms maximum idle. There is no
non-XIP comparison from the same run.

**PENDING:** XIP boot time; Safe Mode, rollback and recovery paths under XIP; package
installation with GUI load; power; long-duration operation; ELF stress with large
applications. Adopting XIP needs a recorded decision and an updated guard and test.
