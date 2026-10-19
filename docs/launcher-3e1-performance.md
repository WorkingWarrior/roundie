# Launcher maintenance 3E.1 — 2026-10-19

Scope: persistent one-shot completion and measured GUI responsiveness. No 3F,
package/ABI format change, new task, bus-clock change or storage migration.
Physical finger/visual acceptance remains PENDING until supplied by a human.

## Verified source and implementation

At 3E commit `a3a27d5`, clean ABI 1 teardown immediately reopened LIST.
Hello executed, returned 42 and unloaded in approximately 0.14 s; no persistent
completion widget existed. This explains the reported apparently inactive row.

RESULT extends existing navigation. Selection copies the installed display
name/ID and request serial. The worker records the admitted runtime generation
and pre-submission completion serial. Success requires the matching instance,
advanced completion evidence, actual execution, zero loader/runtime error,
UNLOADED, retired input and zero runtime/UI resources. A valid nonzero ABI 1
return remains data. Failure after execution retains that value and reports
failure separately. Admission failure has no application result. Fully copied
system strings/values survive ELF unload. No callback or ELF pointer reaches
the result renderer. Back is request-checked and submits no execution.

The existing launcher is retained, with four additional system LVGL objects
(37 total including chrome). It is not reconstructed on ordinary return.
Registry changes cannot replace a pending result. Safe Mode closes admission
and hides chrome before existing cleanup; boot confirmation is independent.

## Display pipeline: pinned source facts

- IDF v6.1 `fff9895c82d744c7237be8847347bdd1b07c6643`, LVGL 9.5.0,
  ST77916 2.0.2, ELF Loader 1.3.5; default CPU 160 MHz, debug `-Og`.
- GUI is `app_main`, priority 1, core 0, 8192-byte stack. It pumps UI/navigation,
  calls `lv_timer_handler`, feeds its own watchdog, and delays one 10 ms tick.
  The system worker and executor retain their existing tasks/ownership.
- Tick callback is monotonic `esp_timer_get_time()/1000`. Refresh and pointer
  timers use the pinned 33 ms default; handler work can exceed that period.
- One 14400-byte internal DMA-capable draw buffer, RGB565, 360 × 20 partial
  strips. No PSRAM DMA buffer or new cache synchronization requirement.
- SPI2 QSPI remains 3 MHz, mode 0, four data lines, queue depth 2, 32 command
  bits / 8 parameter bits. The exact 1.85B wiring and panel tables are retained.
  Pixel-only full-screen transfer has an ideal lower bound of 172.8 ms at the
  requested rate; software, command overhead and divider effects add cost.
- GUI byte-swaps RGB565 before `esp_lcd_panel_draw_bitmap`. The pinned panel
  IO sets QIO for color, and signals its callback only for the final color chunk.
  ISR timestamps completion and notifies GUI. The GUI wait callback consumes
  that notification before LVGL clears `flushing`. No early flush-ready signal
  was introduced; timeout still restarts rather than reusing an owned buffer.
- A screen switch invalidates the display; small property changes retain LVGL
  dirty-area behavior. The existing loading spinner can invalidate during
  loading. Native view construction is offscreen until lifecycle SHOW.
- Package scan/hash/launch remains worker-only, outside GUI locks. The GUI
  performs no package enumeration. Launcher/chrome and native roots stay distinct.

## Instrument boundaries and bounded comparison

Existing native UI latency is **enqueue to dequeue/start of apply**, despite
older prose calling it enqueue-to-apply. Its definition/counter is unchanged.
Create/update maxima cover the LVGL mutation function. NAV's historical maximum
is publication-to-start-of-navigation-apply. New `paint` covers publication
through completed object mutation; result completion uses a monotonic timestamp
published at executable unload, before executor input-buffer retirement.
Duration reporting for a delayed result saturates rather than wrapping uint32.

`tools/roundie_display.py` reads RP1 PERF. The fixed 32-entry ring records only
active iterations, uses 64-bit monotonic timestamps/totals, refuses sequence
wrap, allocates no trace memory dynamically and logs nothing in hot paths.
Oldest/latest expose ring eviction. Counters never reset when changing batch.
Trace fields are named by the host tool: loop timestamp, navigation sampled at
publication, request/generation, batch, pre/post pump depth and applied delta,
pump/handler/wake durations, flush/wait-completion counts, pixels, submit/DMA/wait
durations, render span and invalidation calls/areas/full-screen requests.

Render START→READY includes synchronous rendering, SPI submission and inter-strip
DMA waits; it excludes the final outstanding DMA tail. It is not pure CPU render
cost. Submit includes byte swap and panel calls; DMA is flush entry→ISR, not wire
time alone. `done` means GUI-consumed completion; the separate cumulative
`dma_completions` counts actual interrupts, published once per GUI iteration.
The historical UI `flushes` counter remains GUI-consumed completions. Invalidation
area totals include overlapping/repeated requests; flushed pixel totals describe
actual strip submissions. Neither metric proves pixel visibility or panel scanout.
Wake delay is lateness beyond the requested 10 ms loop delay, not touch latency.
Physical touch acquisition and pixel visibility remain uninstrumented gaps.

The comparison changes only a bounded GUI batch, four versus the fixed mailbox
capacity of twelve. Six initial native widgets formerly needed two pump passes
with possible whole-screen render/transfer between them. Twelve still bounds
work, preserves FIFO and lifecycle priority, and drops no accepted update.
Hardware benefit is to be measured on the frozen candidate; no faster-bus claim
is implied. Volatile BATCH changes are refused while an ELF/input is owned.

```sh
python tools/roundie_display.py --port "$PORT" status
python tools/roundie_display.py --port "$PORT" batch 4
# Bounded launch/return samples, then idle:
python tools/roundie_display.py --port "$PORT" batch 12
python tools/roundie_display.py --port "$PORT" trace 0
```

## Original hardware baseline

On running B 0.3.5, two Hello and two UI cycles used INJECTED selection and
buttons. Two seconds of idle launcher produced zero new flushes. Hello request
to clean LIST was 139431/139490 us. UI request to RUNNING/SHOW admission was
309251/389265 us, queue depth six, initial enqueue-to-dequeue maxima
177737/257737 us. The first button update, requested during initial redraw,
raised these to 299120/299077 us; second host-observed button completion was
49.58/50.04 ms including serial sampling. These are individual maxima/samples,
not percentiles. The historical 3E maximum remains **517828 us**.

## Host/target gates before deployment

77 Python regression tests pass, including production C coordinator, bridge,
runtime and actual pinned LVGL under ASan/UBSan with mocked IDF/loader.
This does not execute Xtensa on host. Coverage includes 42, persistent result,
matching request/instance/completion, stale/double admission, actual Back callback
without execution, loader failure, post-execution failure, ABI 2/3 lifecycle,
teardown, Safe Mode, missing/stale registry entries and full FIFO ordering.
Production performance diagnostics are tested for ring eviction, invalid framing,
idle-only tuning and timestamps beyond uint32 range. Target REP build, layout,
identity and static stack gates are recorded with the frozen candidate.

Device deployment, corrected baseline/comparison, measured stack/memory margins
and final OTA state are PENDING below until actual execution.

## First instrumented candidate and measured scheduling defect

Candidate `a68d0bc`, 729664 bytes, SHA-256
`f5fbe49a4ae77def5333162326422f801927786d603e04a13f2b15d720ca440a`
(ELF `efa41c0f72402e90d4b3e317b083a64790dd746febdeb3ca0098ba36b202022c`)
passed the genuine A OTA, NEW→PENDING_VERIFY→VALID/STABLE. B 0.3.5 remained
the unchanged compatible image. Four Hello and four UI launches then compared
two cold/warm cycles at batch four with two at batch twelve on this same image.
Every selection/button/Back/Exit was INJECTED. Installed name was exactly
**Roundie Hello Update**, not an assumed name from the example source.

| Boundary, microseconds | Batch 4, two samples | Batch 12, two samples |
|---|---|---|
| Hello request→worker-observed clean unload | 139157 / 139317 | 139085 / 139258 |
| Result object mutation | 1607 / 1592 | 1599 / 1599 |
| Executable unload→result GUI ready | 177616 / 176850 | 176867 / 176867 |
| Back publication→launcher GUI ready | 4219 / 4219 | 4186 / 4217 |
| UI request→observed RUNNING/SHOW | 419411 / 439382 | 349410 / 349409 |
| Instance enqueue→dequeue observed maximum | 286600 / 306784 | 211459 / 211451 |
| UI Exit→clean worker observation | 49453 / 39426 | 39427 / 319411 |
| List publication→GUI ready after UI Exit | 333591 / 4391 | 4317 / 333588 |

The final slow Exit followed an explicitly tested suspend/resume; preserve that
state context rather than combining it with ordinary Exit samples. Batch twelve
eliminated an intermediate four/five-strip spinner redraw between the initial
four and remaining two commands. UI readiness improved by 70–90 ms in these
samples; queue maximum by 75–95 ms. This does not make full redraw fast.
Both two-second steady idle windows produced zero flushes. The first attempted
idle window included asynchronous registry publication after INFO scans; that
assertion and traffic are retained, no application had launched, and sampling
continued after presentation settled without a reset or counter reset.

The UI full-screen traces contain 18 flushes / 129600 pixels and handlers of
345021–355863 us. Batch-twelve examples had flush-entry→ISR sums of
178370/178371 us but GUI notification wait sums of 279476/279450 us.
Source inspection of pinned `esp_lcd_panel_io_spi.c:lcd_spi_post_trans_color_cb`
shows it calls `on_color_trans_done` **without using the bool return**. The
application previously returned `higher_priority_woken` expecting the driver
to request a switch. Thus a notified GUI can wait for the next 100 Hz tick on
each strip; the trace contains roughly 100 ms of avoidable post-DMA scheduling.
This is a demonstrated source defect with a hardware-supported causal inference;
the exact isolated historical 517828 us sample had no stage trace and cannot
be uniquely reconstructed. Current traces place large delays in queued work
behind serialized GUI render/transfer, not in the 1–2 ms label mutation itself.

A focused correction explicitly requests `portYIELD_FROM_ISR` after notifying
GUI. Xtensa defers the task switch to ISR exit; existing notification wait and
LVGL flushing ownership remain unchanged. The SPI clock, buffer placement,
refresh period, priorities and managed IDF sources stay unchanged. A production
callback C test checks completion timestamp publication, notification, and
conditional switch request under ASan/UBSan. This measured defect justifies a
second bounded candidate/OTA within 3E.1: use existing Safe Mode selection to
boot preserved B, confirm B normally, then genuine OTA to inactive A again.
No active image, B application bytes, bootloader or partition table are written.
The first frozen artifact and all measurements remain preserved. Final-candidate
hardware effect is PENDING until the comparison below.
