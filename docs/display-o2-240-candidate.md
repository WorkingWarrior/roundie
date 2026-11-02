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

**Assumption (unverified):** LVGL software rasterization is CPU-bound, so `-O2`
and 240 MHz shorten that interval. The gain, and whether it is worth the extra
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
- **Device tests:** none. The candidate was not flashed, so no timing, power,
  stability, or watchdog claim is made.

| Frozen build identity | Value |
|---|---|
| Image | 761,808 bytes, 79.2% slot headroom (2,908,208 B) |
| `roundie_rep.bin` SHA-256 | `40c6374590052a1d2102e9d6eaac43e51e2168e79ff872ae1d2b289b2cc20fb7` |
| `roundie_rep.elf` SHA-256 | `6412732c699768065c19dd1746a8bf681fa13ee6efc90796ee2eed681b891b24` |
| DIRAM | 168,934 B used, 172,826 B remaining |

Flashing needs separate explicit authorization. The established route is an
RO1 OTA into the inactive slot, preserving the confirmed 0.3.8 slot.
