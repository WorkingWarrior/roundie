# Milestone 3E.3 acceptance — 2026-10-20

Exact board: Waveshare ESP32-S3-Touch-LCD-1.85B. Scope: QSPI throughput and
LVGL partial buffering. No 3F/MTP, ABI, package, partition, task-priority,
CPU-frequency or global optimization change. Measurement boundaries and
buffer decision are in [display rendering](display-rendering.md).

## Evidence archive and preflight

Raw logs, frozen images, manifests and host scripts are outside Git at
`/mnt/Dane/Development/roundie-backups/display-3e3-2026-10-20/`.
They include `preflight-passed.json`, each `candidate-*.json`, each
`candidate-*/` artifact directory, `*-ota-{ready,new,restart}.json`, first-boot
logs, and the per-operation `baseline`, `clock30`, `clock40`, `optimized` records.
No private device identifier or firmware binary is committed.

**Verified device facts:** read-only preflight checked USB/ROM identity,
ESP32-S3, 16 MiB Flash, 8 MiB PSRAM, exact REP table and installed rollback
bootloader bytes, both VALID compatible firmware identities, healthy NVS and
LittleFS, valid RTC, OTA IDLE and zero loaded application resources. Original
recovery backup/hash and ROM instructions are available. Read scope was
65,536 bytes plus two 288-byte headers: **66,112 bytes**, once. No full-Flash
dump or raw write occurred. Expected initial state was confirmed live:
B/`ota_1` 0.3.7 VALID/STABLE, A/`ota_0` 0.3.6 VALID.

**User-reported visual evidence:** 20 MHz substantially improved perceived
responsiveness; slightly progressive painting remains visible. This is actual
human evidence for 3E.2, not a pending overall responsiveness assessment.
Final candidate pixel correctness, tearing, scanout and finger testing require
separate human observation.

## Frozen candidates and OTA

| Candidate | Firmware | Request / actual | Draw buffers | Source |
|---|---|---|---|---|
| A | 0.3.8-a | 30 / 26.666666 MHz | 1 × 20 lines | `911b43c` |
| B | 0.3.8-b | 40 / 40 MHz | 1 × 20 lines | `911b43c` |
| C | 0.3.8 | 30 / 26.666666 MHz | 2 × 20 lines | `1cd62c0` |

All use pinned IDF `fff9895c82d744c7237be8847347bdd1b07c6643`, LVGL 9.5.0,
ST77916 2.0.2, loader 1.3.5, CPU 160 MHz, global `-Og`, RGB565 software swap,
33 ms refresh, 10 ms GUI delay and queue depth two. ID reads remain slow;
each candidate reads `00 02 7f 7f` before replacing/deleting the slow IO and
initializing variant 2. Hardware register measurements are configuration
evidence, not electrical waveform measurements.

The 80 MHz source divider is pre=1,n=3,h=1,direct=0 for request 30 MHz;
pre=1,n=2,h=1,direct=0 for 40 MHz. The integer Hz report truncates 80/3 MHz.
Nominal high/low are 12.5/25 ns and 12.5/12.5 ns respectively. One-cycle
write CS setup/hold is 37.5 ns and 25 ns. Controller timing justification and
unmeasured electrical limits are recorded in the rendering document and
[ST77916 source record](display-performance.md#source-facts-and-clock-decision).

Exactly three genuine RO1 candidate uploads were performed; each targeted A,
with B's application image preserved. Before each subsequent upload, the
existing RS1 workflow returns to and independently confirms original B.
An initial SELECT outside Safe Mode was rejected with ESP_ERR_INVALID_STATE
(259), before any boot selection or image write. That tooling error is retained
in `return-a.log`. The corrected workflow is REQUEST → Safe Mode → CLEAR →
SELECT exact B ELF → REBOOT. It uses existing BootGuard metadata persistence,
does not erase NVS, and independently verifies B VALID/STABLE before A reuse.
Selecting the existing image can retrigger IDF's NEW/PENDING_VERIFY confirmation;
it does not rewrite B's application bytes.

## Comparable clock-only measurements

Each configuration runs two Hello and two native-UI cycles, with three injected
button events per UI cycle and timer progression. These are short device tests,
not endurance tests or real finger presses. The full-frame comparison is
specifically native UI activation: exactly 129,600 pixels and 18 strips.
Do not pool it with Exit, loading screens or Back as an equivalent workload.

| Configuration | Full native-UI handler, n=2 | Counter update, n=6 | Hello result, n=2 | Back, n=2 |
|---|---:|---:|---:|---:|
| Refreshed 20 MHz baseline | 99.781–99.788 ms | 8.053–8.833 ms | 45.661–45.674 ms | 96.343–96.447 ms |
| 30 MHz request / 26.667 actual | 96.335–96.383 ms | 8.629–8.804 ms | 45.230–45.401 ms | 91.385–92.211 ms |
| 40 MHz actual | 89.338–89.373 ms | 8.066–8.824 ms | 44.983–45.287 ms | 87.567–87.584 ms |

Counter update is 8,208 pixels; Hello result is 36,384 pixels; Back is 76,840
pixels. Earlier loading frames and tails are retained separately. Clock-only
full-frame improvement is approximately 3.4% at 26.667 MHz and 10.5% at 40 MHz
relative to this refreshed baseline. The historical 3E.2 range remains
99.8–104.9 ms and is not silently replaced by the new small sample.

## Final candidate

**Selected and retained:** 0.3.8, 30 MHz requested / **26.667 MHz actual**,
**two 20-line internal DMA buffers**. No RGB565 algorithm or optimizer change.
The third candidate is the final artifact; there was no fourth deployment.

| Configuration | Actual QSPI | LVGL buffers | Full native-UI frame | Counter partial | DMA errors | Draw SRAM |
|---|---:|---|---:|---:|---:|---:|
| Previous 3E.2 | 20 MHz | 1 × 20 lines | 99.8–104.9 ms, historical | 7.9–8.8 ms, historical | 0 observed | 14,400 B |
| Clock candidate A | 26.667 MHz | 1 × 20 lines | 96.335–96.383 ms | 8.629–8.804 ms | 0 observed | 14,400 B |
| Clock candidate B | 40 MHz | 1 × 20 lines | 89.338–89.373 ms | 8.066–8.824 ms | 0 observed | 14,400 B |
| Buffer candidate C | 26.667 MHz | 2 × 20 lines | 78.397–81.814 ms | 8.197–8.831 ms | 0 observed | 28,800 B |
| Final, same C artifact | 26.667 MHz | 2 × 20 lines | 78.397–81.814 ms | 8.197–8.831 ms | 0 observed | 28,800 B |

New full-frame entries have **n=2**; new counter entries **n=6**, with exact
workload/pixel sizes described above. This is not a latency distribution or
a worst-case guarantee. No P95/P99 claim is made. All omitted physical
configurations, including 1×40, 2×40 and 40 MHz with two buffers, are **NOT TESTED**.

Relative to the same-clock single-buffer A, double buffering improves these
full-frame samples by approximately **15–19%**. Relative to the refreshed
20 MHz baseline, the reduction is **18–21%**. It also beats the tested
40 MHz single-buffer candidate while retaining more controller timing margin.
The preferred <70 ms target and <50 ms stretch target were **not reached**.
Remaining rendering-path cost is explicitly documented rather than hidden.

Final Hello result redraw: **43.485–43.535 ms**, n=2. Back redraw:
**82.482–82.512 ms**, n=2. Native foreground readiness was
**169.226–169.254 ms** (system readiness, not human-visible completion).
The refreshed baseline was 159.259–159.268 ms, candidate A
179.281–199.320 ms and B 179.277–179.282 ms: faster rendering did **not**
demonstrate faster first foreground readiness in this small sample. Loading,
worker validation, GUI phase and timer alignment still matter. Native UI
queue latency maxima at initial readiness were 30.605/30.622 ms on C, versus
24.502/22.359 ms on the refreshed baseline; no touch-latency improvement is
claimed from these queue measurements. Real finger latency remains PENDING.

Ordinary final UI Exit cycle 0 rendered one full launcher frame in **99.539 ms**;
cycle 1 additionally exercised suspend/resume before Exit and retained two
129,600-pixel records (75.481/71.744 ms) and a 75,376-pixel tail (73.972 ms).
Those extra transitions are retained, not pooled with the ordinary Exit sample
or presented as redundant ordinary navigation frames. Loading/spinner and
native startup frames are included in the raw records; no pixels or essential
loading feedback were suppressed. Launcher objects remain persistent, hidden
roots are not independently refreshed, and ordinary idle produces no redraw.
No confirmed redundant normal transition justified an additional renderer edit.

## Timing attribution and outliers

| Native-UI full-frame stage | A: 26.667 MHz, one buffer | B: 40 MHz, one buffer | Final: 26.667 MHz, two buffers |
|---|---:|---:|---:|
| Handler wall time | 96.335–96.383 ms | 89.338–89.373 ms | 78.397–81.814 ms |
| GUI completion wait, summed | 16.883–21.057 ms | 10.856–12.992 ms | 0.483–0.486 ms |
| Handler minus measured wait | 75.278–79.500 ms | 76.346–78.517 ms | 77.914–81.328 ms |
| Layout/dirty-area setup wall time | 8.815–8.942 ms | 9.616–10.005 ms | 7.718–8.795 ms |
| RGB565 swap interval | 4.278–6.183 ms | 4.358–5.024 ms | 4.316–4.327 ms |
| Panel driver submission interval | 7.389–8.177 ms | 7.421–8.152 ms | 9.287–10.175 ms |
| Combined submission interval | 11.667–14.360 ms | 12.445–12.510 ms | 13.603–14.502 ms |
| Render event wall interval | 85.936–86.280 ms | 78.027–78.130 ms | 69.438–70.413 ms |
| Render minus wait/submission, approximate residual | 53.556–54.693 ms | 52.525–54.829 ms | 54.453–56.324 ms |

These are wall intervals and overlapping aggregates, not independently measured
on-core CPU execution. The final render residual includes draw dispatch,
object traversal, rasterization and preemption. Pure rasterization has **not**
been uniquely isolated. It is therefore incorrect to call all 78–81 ms
non-wait time rasterization. Layout now has its own bound; swapping costs
roughly **4.3 ms / 5–6%** of ordinary full-frame handler time. The existing
LVGL word-width/unrolled swap is retained: there is no evidence that replacing
it warrants complexity. Host colors, odd/even counts, halfword alignment and
partial final regions pass byte-for-byte tests.

Final legacy `dma_us` is **33.074–33.982 ms** summed across consumed completions;
it includes submission/software effects and a carried previous-iteration
completion. Pure pixel wire minimum at 26.667 MHz is **19.440 ms**, not this
aggregate. Each full-frame trace has `carry_done=1`: its final submitted ID
is one greater than the last consumed ID. The next refresh consumes that tail.
Low wait plus the real-LVGL ownership test and final physical measurements
support useful render/DMA overlap; `dma_us` and handler time must not be added.

The historical **99.768 ms individual submission** remains in the 3E.2 record.
It was reproduced in the refreshed 20 MHz baseline as **99.931 ms** in the
boot-inclusive maximum. Clock B retained **89.348 ms** in its boot-inclusive
submission maximum, although its retained workload traces had much smaller
individual submissions. Early ring records were evicted; the instrumentation
does not retrospectively recover their cause. No counter reset removed them.

New retained A/C traces show roughly 100 ms extra delay in **layout/setup**
(A up to 107.635 ms; final Hello-loading setup 106.274/106.651 ms), and another
A delay inside the rendering interval with ordinary submission times. This
rules out equating the entire recurring delay with pixel wire time or RGB565
swapping. Source review found one SPI2 write owner, one outstanding color
transfer, one color chunk per strip, unchanged correct ISR yield, and pinned
driver polling/transaction recycling using unbounded IDF waits under the GUI
watchdog. Worker preemption and polling/recycling delays remain plausible;
their causal contribution is **unverified** without scheduler/driver-stage
instrumentation. The original outlier is **investigated but not fully explained**.

Final boot-inclusive observed maxima are handler **146.642 ms**, pump
**56.282 ms**, wake lateness **304.885 ms**, submission **2.730 ms**,
flush-entry→ISR **3.809 ms**, and individual GUI wait **47 µs**. These maxima
include startup/loading, are not equivalent workloads, and are not proven
worst-case bounds. The final configuration still has substantial loading and
CPU-side delay even though ordinary transfer waits are largely hidden.

## RAM, stacks and correctness gates

Runtime before/after native UI heap samples restore exactly in both final
cycles: internal free/largest **201,455 / 126,976 B**, PSRAM free/largest
**8,380,764 / 8,380,764 B**. These snapshots still include retained executor
input until its separate retirement; they are not direct idle PSRAM readings.
Final independent RS1 idle internal free is **201,475 B**. Same-clock single
buffer A had **216,307 / 139,264 B** internal free/largest after unload; observed
whole-image free-heap cost is **14,852 B**, while the new draw allocation is
exactly **14,400 B**. The remaining difference was not isolated into allocator
and link-layout contributions. Versus refreshed 20 MHz firmware the total
observed decrease is **16,964 B**, including **2,112 B** already present in the
instrumented single-buffer firmware; the ring alone grows by 2,048 B.

Final static `.bss` is **98,368 B**, `.data` **16,751 B**, DIRAM aggregate
**173,350 B**, linker-region remainder **168,410 B**. This is separate from
heap telemetry and the dynamically allocated 28,800 draw bytes. Compiler
frames: app_main **1056**, worker **1680**, LCD startup **320**, flush **48**,
ISR **48**, wait **32 B**. Measured remaining stack minima are **2632 GUI /
4912 worker B**. These are bounded observations and individual compiler frames,
not worst-case call-chain or endurance proofs. LVGL pool free at final unload
is **42,896 B**, native UI allocation peak **1788 B**; all 18 acquired UI
resources were released and zero objects remain attributed to the retired app.

Host validation: **82 tests PASS** under the pinned environment. Applicable
production C and real-LVGL tests use ASan/UBSan; they cover divider/profile
acceptance/rejection, GPIO/protocol setup, slow read and IO cleanup failures,
byte order, odd/even partial pixels, allocation failures, transfer ownership,
completion ordering/cross-refresh reuse, timeout restart, launcher completion
42, ABI v3, cancellation/resources, Safe Mode and OTA regression. The divider
test compiles the actual pinned S3 calculation. Mocks do not establish physical
DMA, panel pixel correctness or electrical timing.

Target validation: candidates A/B/C build with pinned IDF v6.1; final REP
layout and ESP32-S3 image checksum/hash validate. Image **730,576 B** fits both
OTA slots; no global warnings, assertions, comparisons or protections were
disabled. Raw evidence: `tests-final.log`, `build-c.log`, `layout-c.log`,
`image-c.log`, `size-c.log`, frozen C config and artifact manifest.

Device validation: all three candidates captured A READY → NEW → boot
PENDING_VERIFY → core self-test ESP_OK → VALID → BootGuard STABLE. B was
independently returned to and checked before each A rewrite. Original B's ELF
identity is unchanged. Upload/activation host times were 75.603, 75.664 and
75.633 s for A/B/C. Final independent check records **673 completed flushes,
673 ISR completions, zero errors**, and zero additional flushes during a
two-second idle observation. These counters are boot-inclusive, not the count
of just the controlled benchmark frames. No observed DMA timeout, ownership
violation, deadlock, panic, watchdog, brownout or USB loss occurred. A rejected
out-of-mode SELECT was the retained tooling failure described above.

Each final Hello really executed native Xtensa code and returned **42**; its
copied result remained through the hold and Back did not execute it again.
Each ABI v3 UI handled three injected buttons, advanced its timer and returned
through cooperative Exit; the second cycle also passed suspend/resume.
Retired input and concurrent launch were rejected. Final ELF state is UNLOADED,
busy=0, timers/subscriptions/control pending=0, runtime resources `[28,27,1,0]`
(last field zero outstanding), UI generation/pending/outstanding=0. Every
candidate's first post-boot snapshot had instance=executed=0: no app autostart.

## Exact final identity and recovery state

- Running A/`ota_0`: **0.3.8 VALID/STABLE**, boot selection A, OTA **IDLE**,
  rollback enabled, core self-test checked/passed, Safe Mode off, failures/latch/
  trial/fault/test-build zero. Launcher is foreground; packages are installed
  and not running; PackageManager is IDLE.
- Final image SHA-256:
  `dafdb8ec50a887d3623b6b253a3464160f50dfbaa15207011a3ac90a3d9a48a8`.
- Final ELF identity:
  `23b9ba7c13083f5bdb21177d6c90722468b458efab55b79d61af749d885c321c`.
- Preserved B/`ota_1`: **0.3.7 VALID/compatible**, ELF
  `59980b0f29d4eda901aa14d3f9935e1a4e1582f2eb5cb3c2414d96f2de180bb5`.
- A experimental image digests: 0.3.8-a
  `96f4e4ecb91f83652a7f5ccf2fa083309f266a1bb4958660e27c1d99647de034`,
  0.3.8-b `f46d22e81a73d2d8b1a7c2919143bdd05159098f05e87ffc7c0dea8ee30063f8`.
- Packages unchanged: Hello `org.roundie.hello` **1.1.0**, package digest
  `9a36506f21136344e557235aaa8caa532a52551dd9eb28936ffa3a8a6d054cd1`;
  UI `org.roundie.ui` **1.0.0**, package digest
  `76ab4c5be328397900de02784f0d5f3f8660a56ba82e974bf5b4c8e17d5bfcd7`.
- NVS healthy, LittleFS mounted/capacity valid, RTC valid/OS=0, recovery USB
  and existing Safe Mode selection exercised. No RTC SET, bootloader/table
  write, NVS erase, filesystem format, package change, eFuse/security change,
  raw Flash update, power-cut test or endurance campaign occurred.

## Explicit conclusions and human gate

- **30 MHz request:** actual **26.667 MHz**, confirmed from pinned algorithm
  and hardware divider. **40 MHz:** completed bounded initialization, OTA and
  application tests without observed transfer errors; long-run electrical
  safety and correct physical pixels are not established by those tests.
- **Faster SPI benefit:** 3.4% at 26.667 and 10.5% at 40 MHz for the controlled
  single-buffer full frames. Final buffering adds 15–19% versus same-clock A.
- **Remaining non-wait cost:** about **78–81 ms** on final full frames. Only
  roughly 54–56 ms is a residual rendering-path wall interval; pure LVGL
  rasterization dominance is **not proven**. Layout, driver setup, byte swap
  and scheduling have distinct costs or documented measurement gaps.
- **Double buffering:** useful overlap measured; retained at an added 14,400 B
  draw allocation. **Larger strips:** correctness host-tested, physical speed
  **NOT TESTED**, not retained. **RGB565:** approximately 4.3 ms, existing
  implementation retained. **~100 ms outlier:** investigated, still unresolved;
  no claim it disappeared from the system's loading/scheduling behavior.
- **Best tested tradeoff:** 26.667 MHz and 2×20 lines. 40 MHz with two buffers
  was **NOT TESTED**; near-zero waits at 26.667 leave little measured transfer
  wait to remove and do not justify a fourth OTA or less timing margin here.
- **Progressive painting:** may remain noticeable. A 78–82 ms software full
  frame is still long enough to expose strips, and panel scanout/tearing are
  not measured. The user's 20 MHz improvement observation remains verified
  as user-reported; the final candidate's perceived improvement is **PENDING**.

Final human procedure, without power cycling (all final observations PENDING):

1. Tap **Roundie Hello Update**.
2. Check the persistent **42** and observe progressive result painting.
3. Press **Back** and observe launcher repainting.
4. Launch **Roundie UI**.
5. Press the counter with a real finger and observe text responsiveness.
6. Use system **Exit** and observe the return to the launcher.

Record detailed pixel correctness, visible strip painting, tearing and real
finger behavior separately from injected controls and software timestamps.
If a subsequent direct observation finds incorrect pixels or unreliable
operation, use the guarded recovery path to the preserved 0.3.7 image. No new
hardware experiment or 3F work is part of this acceptance record.

## Delivery

Implementation/test commits: `911b43c` (30 MHz profile and bounded stage
instrumentation, divider/pixel/ownership tests) and `1cd62c0` (double-buffer
allocation, failure tests and selected default). Frozen final C was built
from `1cd62c0`; subsequent changes are acceptance documentation only. This
report and cross-references form the final focused documentation commit.
No historical commit was amended and no remote push was performed.
