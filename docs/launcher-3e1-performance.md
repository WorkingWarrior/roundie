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

The following sections record actual deployment, comparison, stack/memory
measurements and the final device state separately from the host gates.

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
hardware effect is recorded in the comparison below.

## Final candidate: verified device effect

Final source commit `de61b73`; pinned REP build and **78 host tests passed**,
including the production ISR callback under ASan/UBSan. The final frozen image
is **729680 bytes**, SHA-256
`3395c0c58d4bbb7bd8328d8e74c332484c5de60f425c85b919fd4281d1a3e46c`,
ELF `4cdce8594162012c2038e76e5d5b89554a49ba4f1306e4b3f649be3ca12f5802`,
version 0.3.6. Its SDK configuration and partition table are byte-identical to
the first candidate. Only the explicit ISR scheduling request changes firmware
behavior between these two candidates.

Read-only preflight proved ESP32-S3/expected USB identity, 16 MiB Flash / 8 MiB
PSRAM, exact installed rollback bootloader and REP table, B 0.3.5 VALID/STABLE,
A 0.3.4 VALID/compatible, OTA IDLE, no ELF, healthy NVS/LittleFS, unchanged
package digests and available recovery backup/instructions. One bounded ROM
read comprised 64 KiB plus two 288-byte headers (**66112 bytes**). There was no
full-Flash read. The measured defect then justified the second candidate;
preflight evidence was reused with fresh live identity/health checks, without
another ROM read. Existing RS1 REQUEST/CLEAR/SELECT/REBOOT booted preserved B;
selection marked B NEW, and B confirmed normally through PENDING_VERIFY to
VALID/STABLE. This exercised Safe Mode from an active launcher without apps.
It did not rewrite B's application bytes. The final genuine RO1 upload wrote
only inactive A, took **75.564 host seconds** through activation, and recorded
A NEW→PENDING_VERIFY→VALID/STABLE. Both frozen candidates/logs remain available.
No partition/bootloader write, format, NVS erase, eFuse/security change, clock
adjustment, malformed native execution, power interruption or endurance test
was performed.

Two cold/warm Hello and two cold/warm UI cycles ran on the final image, all
INJECTED. Hello used the actual installed package, executed Xtensa code,
returned 42, fully unloaded and showed verified system title/value/Back widgets
in the foreground. The result survived an explicit 650 ms hold; source has no
dismissal timer and the host test also advances ten seconds. Wrong Back and
double launch were refused; correct Back left the same completed generation,
so dismissal did not execute again. UI realized seven system-owned LVGL objects,
delivered three button events per instance, advanced ordinary timer updates,
and exited cooperatively. The second UI cycle also suspended/resumed before
Exit. Retired input and Exit identities were refused after teardown. No native
ABI 2 package was installed for this milestone; ABI 2 regression is host evidence.

All values below are **on-device microseconds** unless stated. Each slash pair
is two samples (first cold native instance / second warm); it is not P95/P99.
The first candidate's launcher had already been built before the batch-4/12
comparison; the final candidate's initial construction took 51765 us and was
not repeated on normal returns.

| Operation / exact boundary | Previous observed | Corrected batch-4 baseline | Final batch-12 + ISR yield |
|---|---|---|---|
| Initial launcher LVGL construction, excludes transfer | 46676 in 3E | 50851 | 51765 (four additional result objects) |
| GUI command enqueue→dequeue maximum | Historical 517828 | 286600 / 306784 | **151684 / 151749** |
| Hello request→worker-observed clean unload | 139431 / 139490 in new 0.3.5 baseline | 139157 / 139317 | 139213 / 139262 |
| Result object mutation | Not previously measured; no result page | 1607 / 1592 | 1608 / 1599 |
| Executable unload→result LVGL ready | Not previously measured | 177616 / 176850 | **118484 / 117504** |
| Back publication→launcher LVGL ready | Not previously measured | 4219 / 4219 | 4316 / 4318 |
| UI request→worker-observed RUNNING/SHOW | Historical 499262 / 649253; new 0.3.5 baseline 309251 / 389265 | 419411 / 439382 | **289394 / 289436** |
| UI initial full-frame handler | Not previously measured | 353492 / 345021 | **241186 / 247834** |
| Label LVGL mutation maximum within instance | Historical 1433; new baseline 1385 / 1372 | 1554 / 1524 | 1524 / 1532 |
| Small label refresh handler (six applied button-update samples) | Not previously measured | Approximately 17429–22176 | 17456–17967; 2 strips / 8208 pixels |
| UI Exit→worker-observed clean unload | Historical 39387 / 319300 (second suspended) | 49453 / 39426 | 49482 / 209502 (second after resume) |
| UI Exit list-publication→LVGL ready | Not previously measured | 333591 / 4391 | 224568 / 233419 |

Readiness improved by **31.0% / 34.1%**, and instance queue maxima by
**47.1% / 50.5%**, relative to the corrected batch-four samples. Isolating the
ISR correction at batch twelve, UI readiness fell from 349410/349409 to
289394/289436 us; full-frame handlers from 355853/355863 to 241186/247834 us
(approximately **30–32%**). Full-frame wait sums fell from 279476/279450 to
162329/164533 us while flush-entry→ISR sums remained 179087/180736 us.
This confirms a scheduling benefit without a faster bus or early completion.
Two-second final steady idle produced **zero flushes**. The original 518 ms
sample remains valid historical evidence; final sampled maxima are not a new
upper-bound guarantee, nor proof that such an exceptional delay is impossible.

Result drawing required seven strips / 36384 pixels and an 82279/82396 us
handler after loading. Back drawing remained two partial refreshes: 12 strips /
76840 pixels at 181319/181318 us, then eight / 53292 at 118431/118368 us. These
separate handlers do not equal physical button-to-visible latency. UI initial
frames required 18 strips / 129600 pixels. Ordinary timer-label refreshes used
one strip / 6016 pixels and approximately 4.4 ms handler work. Some updates were
applied in one iteration and rendered in the next refresh; an applied command
is not dropped merely because that iteration has zero flushes. Per-command
steady update queue latency is not separately exported; the instance maximum
above includes initial construction, and host polling is not substituted for
that missing boundary. Small refreshes are much cheaper than screen transitions.

## Remaining bottlenecks and evidence limits

**Verified limitation:** at unchanged 3 MHz QSPI and one serialized internal
strip buffer, full render/transfer still occupies the single GUI task for
approximately 0.24–0.25 s. Initial command submission can wait behind the loading
screen's approximately 234 ms refresh. Exit/suspend/resume can encounter a
previous refresh, system-root redraw and subsequent list redraw; final ordinary
Exit had a 224568 us publication→GUI wait, while the post-resume clean-unload
sample took 209502 us. Therefore the broad goal of avoiding every material
transition delay is **PENDING**, not an instantaneous-navigation claim.
Persistent launcher reuse is verified; unrelated system resources are not
recreated. This maintenance stops after two measured improvements rather than
changing bus timing or introducing another renderer.

In 32 retained nonzero active-iteration traces from the final bounded cycles,
the maximum requested-loop-wake lateness was 762 us, handler 247834 us and pump
27154 us. The **boot-inclusive, never-reset** counters also retain larger
outliers: handler 293388, pump 56437, wake 304803, flush-entry→ISR 98164,
submission 90139 and wait 93258 us. Their early startup/confirmation trace
records were evicted before sampling, so exact attribution remains PENDING;
they are not erased or advertised as ordinary small-widget timing. Final
snapshot had 633 submissions and 633 actual completion interrupts, 2619629
flushed pixels and zero DMA errors. The captured boot/acceptance traffic had
no panic, watchdog, brownout, completion timeout or enqueue-failure marker.
These counters/logs are bounded observations, not fault/endurance guarantees.

Human LCD pixels, tearing, panel scanout and finger acquisition/response remain
**PENDING**. No camera/robot observation was available. Widget existence/text,
foreground state and completed DMA are software/device evidence only. No 60 FPS
or visually verified responsiveness claim is made. The initial boot launcher
construction is timed, but its exact first-presentation flush count/time was
not isolated before the early ring entries were evicted; that boundary remains
PENDING rather than being inferred from the subsequent UI full-frame samples.

## Memory, stack and final state

The worker retains 12288 bytes, GUI 8192 bytes, executor 12288 bytes. Final
observed minimum remaining stacks were **5040 worker / 2776 GUI bytes**.
Target `-fstack-usage` recorded worker 1680, package command 1456, navigation
command 448, launcher poll 752, GUI diagnostics command 304, GUI record 224,
app_main 960, LCD ISR callback 48 and display-wait callback 32 bytes. These are
individual compiler frames, not a complete worst-case call-chain proof. Worker
stack was not increased. The historical 3D LittleFS panic's precise cause stays
unresolved; these measurements do not retroactively prove a stack cause.

Each final Hello/UI instance restored its loader-before/after heap snapshots
exactly: internal free/largest **218419 / 139264 bytes**; PSRAM free was
**8382780** for Hello or **8380764** for UI. These runtime snapshots still include
the executor-owned input buffer, whose separate retirement is proven by busy=0;
they are not a later direct idle-heap sample. Native UI LVGL peak was 1828/1816
bytes. System launcher/chrome remained **37 objects**, final LVGL pool free
**42880 bytes**, with no monotonic instance heap growth in this bounded sample.
Generation four completed with resource counters **[28,27,1,0]**, UI generation/
outstanding/pending zero, no ELF owned, launcher foreground and batch twelve.

Final device: **A 0.3.6 VALID/STABLE**, exact final ELF above; **B 0.3.5 VALID**,
ELF `62e7cd192e75192da2a879d0f369438c0f7fcd0ef68b447b55f29469cbdac390`,
OTA IDLE, rollback enabled, healthy mounted LittleFS/NVS. Package identities
and full-package digests remain unchanged:

- `org.roundie.hello` 1.1.0 / ABI 1, **Roundie Hello Update**,
  `9a36506f21136344e557235aaa8caa532a52551dd9eb28936ffa3a8a6d054cd1`.
- `org.roundie.ui` 1.0.0 / ABI 3, **Roundie UI**,
  `76ab4c5be328397900de02784f0d5f3f8660a56ba82e974bf5b4c8e17d5bfcd7`.

RTC remained readable/valid with OS=0, unchanged by SET; final RTC/system
samples differed by one second. This does not establish host clock accuracy or
continuous discipline. No extra recovery or power experiment was added.

## Evidence and manual acceptance

Raw traffic, immutable candidate images, hashes, layout/image validation,
compiler stack records and exact bounded scripts remain outside Git:
`/mnt/Dane/Development/roundie-backups/launcher-3e1-2026-10-19/`, final candidate
and acceptance in `yield-corrected/`. Key records: `candidate.json`,
`preflight-passed.json`, `ota-new.json`, `a-first-boot.log`, `a-confirmed.json`,
`optimized-hello-0/1.json`, `optimized-ui-0/1.json`, `idle.json`,
`final-device-state.json`, plus the preserved first-candidate batch comparison.
The initial idle-sampling interruption is retained. No binary, raw device log,
private USB identity or backup is committed. Nothing was pushed.

Human manual gate (all six steps PENDING):

1. Tap **Roundie Hello Update** in the launcher.
2. Verify **Application finished / 42** remains visible until dismissed.
3. Press **Back** and verify usable launcher without another launch.
4. Launch **Roundie UI**.
5. Check visible drawing/transition responsiveness and TAP ME with a real finger.
6. Exit through system navigation and verify the launcher.

The automated maintenance work stops at 3E.1. No 3F work or long campaign follows.
