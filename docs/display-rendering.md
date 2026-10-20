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
or physical timing validation. Byte-aligned unaligned RGB565 starts are outside
the supported halfword-aligned LVGL buffer contract.

## Pending physical comparison

Clock candidates, measured timing attribution, buffer selection and final
acceptance are recorded after the bounded experiments. Camera/finger/visual
acceptance remains PENDING until directly observed.
