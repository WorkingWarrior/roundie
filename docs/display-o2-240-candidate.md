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
