# Display rendering — Milestone 3E.3

Scope: bounded 30/40 MHz clock and partial-buffer experiments on the 1.85B.
The [3E.2 record](display-performance.md) remains historical evidence.

## Evidence and experiment decisions, 2026-10-20

**User-reported visual evidence:** 20 MHz is substantially faster than the
earlier 3 MHz firmware; progressive repainting remains slightly visible.
Detailed pixel correctness, tearing and electrical margins remain unverified.

**Verified source:** pinned ESP-IDF v6.1
`fff9895c82d744c7237be8847347bdd1b07c6643`, LVGL 9.5.0, ST77916 2.0.2.
The S3 divider algorithm yields 80 MHz / 3 for a 30 MHz request. Its default
duty setting produces one source cycle high and two low (12.5/25 ns).
40 MHz gives 12.5/12.5 ns, a 25 ns write cycle and one-cycle CS setup/hold
of 25 ns. These nominal values meet the controller table's 7 ns high/low,
16 ns cycle and 15 ns CS setup/hold minima. Source review and registers do
not measure electrical edges, data margins or CS-high time on the board.
Reads remain at 3 MHz. No live bus reconfiguration is introduced.

**Decision:** compare clock-only candidates first, using one 20-line buffer,
queue depth two, CPU 160 MHz and global `-Og`. Preserve known-good 0.3.7 in B;
each candidate uses genuine RO1 OTA into inactive A after a fresh slot guard.
Further buffering is justified only by measured transfer/CPU overlap and RAM.

## Measurement boundaries

RP1 format 2 retains every original counter and appends swap, driver, layout,
completion-origin and transfer-identity fields. The host tool accepts historical
format 1. Counters are bounded by the existing 32-record ring; there is no
per-strip USB logging. Each successful submission has one monotonically
increasing identity; only one transfer is outstanding, so its completion cannot
belong to a later strip. `carry_done` marks completion consumed in a later GUI
iteration. Original `dma_us` still includes swap and driver submission, and
must not be called wire time or assigned blindly to the consuming frame.

`swap_us` covers the existing LVGL RGB565 swap plus timestamp/bookkeeping;
`driver_us` covers `esp_lcd_panel_draw_bitmap`, including polling commands,
driver preparation and possible scheduling. `submit_us` is their combined
wall interval. `layout_us` spans refresh start to render start, including
layout, dirty-area joining and refresh setup. `render_us` still includes
inter-strip waits and submissions. Subtracting those yields a residual
rendering-path wall interval, **not pure rasterization or on-core CPU time**.
The current firmware has no FreeRTOS runtime-stat facility enabled; scheduler
preemption and individual raster primitives are not independently isolated.
Final DMA tail may be consumed in the next handler. Overlapping metrics are
reported separately, never summed as disjoint stages.

Pinned LVGL's two-buffer PARTIAL path renders the next strip, waits for the
previous DMA, submits the new strip and switches buffers. The custom wait
callback therefore permits useful overlap. It keeps one outstanding transfer;
two DMA descriptors or a larger SPI queue are unnecessary. A real-LVGL host
test checks buffer hashes through delayed mock completions, full-frame and
odd-width partial pixels, final completion across refreshes, and RGB565 colors
at four-byte and two-byte aligned starts. This is software evidence, not DMA
or physical timing validation. Odd-byte RGB565 starts are outside this
firmware's input domain: internally allocated buffers begin on word boundaries.

## Third candidate decision

Two clock-only native-UI full frames measured 96.335–96.383 ms at 26.667 MHz
and 89.338–89.373 ms at 40 MHz, versus 99.781–99.788 ms in the refreshed
20 MHz baseline. Neither clock removed the main rendering-path wall cost.
At 40 MHz, 10.856–12.992 ms remained in GUI completion waits.

**Decision:** spend the third and final candidate deployment on two 20-line
buffers at **26.667 MHz**. Compare it directly with the first candidate;
this changes buffering independently of clock. With overlap, the rendering
path can hide the slower transfer, making this a better conservative-clock
hypothesis than adding more frequency. The added internal DMA allocation is
14,400 bytes; the first candidate retained 216,307 bytes internal free and
139,264 bytes as its largest internal block after native unload. No queue,
CPU, global optimizer or RGB565 algorithm change is needed.

Single 40-line buffering is pixel/ownership tested with real LVGL on the host,
but physical performance is NOT TESTED: halving submissions can save only
part of roughly 7–8 ms driver time and cannot overlap DMA with rendering.
Dual 40-line buffering and a full framebuffer are NOT TESTED; doubling memory
again has no measured justification in this bounded three-deployment matrix.

## Final physical comparison

The third candidate passed the bounded device tests and is retained as 0.3.8:
26.667 MHz, two internal 20-line buffers. Full native-UI frames improved to
78.397–81.814 ms with 0.483–0.486 ms summed completion waits. Pure rendering
and scheduling are still not independently isolated; the <70 ms goal was not
reached. The [acceptance report](display-3e3-acceptance.md) contains the complete
matrix, stage attribution, retained outliers, RAM costs, exact OTA identities
and final A VALID/STABLE / original B 0.3.7 VALID state. Camera/finger/visual
acceptance remains PENDING until directly observed.

## Milestone 3E.4 — clock-only dual-buffer comparison

The separate [3E.4 experiment](display-3e4-acceptance.md) tested the missing
40 MHz / dual 20-line configuration against a fresh five-cycle 0.3.8 baseline.
No buffer, swap, queue, scheduling, renderer or trace implementation changed.
All full native-UI samples retained 129,600 pixels / 18 strips. Median handler
time was **82.282 → 82.105 ms**, a **0.215%** change; measured completion waits
were **0.486 → 0.493 ms**. Faster transfers reduced the legacy aggregate DMA
interval, but did not demonstrate a consistent full-frame/navigation benefit.
The residual rendering-path wall interval remained about 56 ms. Repeated
approximately 106 ms loading layout/setup outliers remain unresolved.

Source/real-LVGL sanitizer tests and settled physical completion counters
support retained buffer ownership, including completion carried across refresh
iterations. Electrical data integrity and physical pixels remain separately
limited evidence. The user reported a small visible improvement on the live
candidate; it does not establish an electrical margin or a repeatable timing
gain. **26.667 MHz with two 20-line buffers remains the selected default.**
0.3.9 is a retained experimental image, not the recommended release.
