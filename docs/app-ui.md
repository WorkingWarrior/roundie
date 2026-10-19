# Milestone 3C — system-owned application UI

**3E.1 maintenance:** the GUI pump drains at most the fixed 12-command mailbox
before rendering (previously four). Lifecycle priority, FIFO ordering, admission
and teardown barriers remain unchanged; no accepted update is coalesced away.
Development-only idle `BATCH 4|12` permits the same-image hardware comparison.
The existing `latency_max_us` remains enqueue-to-dequeue/start-of-apply, excluding
LVGL mutation and LCD transfer. Create/update maxima measure the render function;
see the [pipeline and instrument boundaries](launcher-3e1-performance.md).

Milestone 3E adds a separate [system launcher](app-launcher.md) and
[Exit chrome](system-navigation.md), owned by the same GUI task. Neither is
an app widget or ResourceManager resource. The bridge's captured system screen
is normally the launcher; its deletion barrier remains unchanged. The top
central 60-pixel navigation safe area avoids the existing example's content;
ABI 3 geometry, descriptors and commands remain unchanged.

## Architecture decision (2026-10-19)

REP §§5–10 and 12 already require asynchronous UI commands, generation-owned
handles and a GUI owner. A small retained command protocol is the minimal
implementation of that contract. `roundie_app_ui` extends the existing GUI
loop and ResourceManager foundation in `roundie_elf`; it adds no task, display
driver, LVGL instance, launcher, registry or package service.

ABI 3 is explicit: import `rep_abi_v3`, entry `rep_app_entry_v3`, metadata
version 3, 52-byte Xtensa API table and 40-byte descriptor with required
capabilities. Existing ABI 1/2 headers, imports, entry contracts and layouts
remain intact. START admits ABI 2 or 3; RUN remains ABI 1. Unsupported metadata,
imports, callbacks or descriptor capabilities are rejected. Descriptor checks
precede init; trusted native entry itself is still necessary to obtain the
descriptor. This API is compatibility control, not native memory isolation.

## Public operations and bounds

`include/rep/rep_app_v3.h` is standalone C11/C23. `ui_create` accepts a view,
label or button description and returns a logical handle, or zero on refusal.
`ui_update` changes text, visibility or enabled state; zero means accepted,
minus one means refused. `ui_destroy` closes the single view irreversibly for
this generation; it is asynchronous and rejects duplicates. Individual widget
deletion/reparenting and a second view are intentionally unsupported. Root
visibility is owned by lifecycle. Init/start/running callbacks may create or
update; suspend/resume/stop/destroy callbacks may log/release/query but cannot
submit ordinary UI work. Resume restores retained widgets after its callback.

| Limit | Value |
|---|---|
| Live application views | 1 |
| Logical widgets, including root | 8 |
| Text storage per request/widget | 96 bytes; at most 95 UTF-8 bytes + NUL |
| Command FIFO | 12 fixed copied entries; reject newest, no coalescing |
| Processing per GUI iteration | at most 4 commands or one lifecycle transition |
| Widget/update public payload | 132 / 116 bytes |
| Geometry | positive dimensions, entire box within 360 × 360 |
| Button minimum | 44 × 44 pixels |
| Retained LVGL allocation admission | 16 KiB incremental ceiling; create refused above 12 KiB or below 8 KiB pool reserve |
| Absolute LVGL memory envelope | existing shared 64 KiB built-in pool |
| Fixed UI component writable symbols | 3118 bytes plus linker alignment; budget below 4 KiB |
| GUI lifecycle acknowledgment deadline | 1 second per attempt, outside native callbacks |

Fixed command/handle/GUI-text storage does not grow with updates. The small
number of LVGL objects shares the existing pool; a button owns one additional
label. The 16 KiB diagnostic is the immediate allocator delta during GUI
operations, not an isolated allocator or a bound on renderer caches. A creation
crossing it fails and triggers cleanup. The shared 64 KiB pool is the absolute
bound including system objects, rendering and transient construction. Dynamic
fonts/assets, animation and app-selected themes are unavailable. Built-in font
coverage can be smaller than the accepted well-formed UTF-8 repertoire.
The 3118-byte static-symbol total was read from the built Xtensa component
archive (1920-byte command FIFO, 96-byte logical slots, 896-byte GUI records,
and fixed counters/control/lock state). The backing envelope is the existing
shared 64 KiB pool plus less than 4 KiB of new static UI state; native ELF/SDK
allocations and the existing event FIFO retain their separate runtime budgets.

## Copies, identity and completion

The executor validates owner, callback context, current ABI/state, alignment
and complete pointer ranges using loaded ELF data or SDK allocations. Borrowed
executor-stack buffers are not accepted. It copies the entire request, validates
version/size, bounds, parent, UTF-8 and explicit terminating NUL, then copies it
into the mailbox. Output queries additionally require writable loaded data or
SDK memory. Overflow checks subtract range offsets; no unchecked pointer-end
addition is used. No application pointer reaches GUI storage.

Nonzero handles are boot-unique monotonic tokens; exhaustion refuses creation.
Every slot also has generation, type and realized state. Only the exact current
root may parent a label/button. A handle means queue acceptance, not creation,
LVGL application, DMA completion or visible pixels. `ui_result` copies cumulative
submitted/applied/failed/pending counters, sticky error and view identity without
waiting. GUI failure stops admission and the executor tears down the instance;
there is no partially operational screen. Diagnostics retain the failure.

The GUI backend stores strings in its own stable 96-byte arrays and deliberately
uses `lv_label_set_text_static`: LVGL retains SYSTEM storage until deletion.
Deletion precedes clearing/reusing arrays. This follows the pinned LVGL 9.5.0
source semantics; ordinary `lv_label_set_text` would allocate/copy instead.
See the [LVGL label ownership reference](https://lvgl.io/docs/open/9.3/details/widgets/label);
the installed 9.5.0 `src/widgets/label/lv_label.c` is the version-specific source
checked for this implementation.

## Lifecycle and shared-object review

Init/start build an offscreen root. Outside callbacks, the executor requests
SHOW and waits for all accepted creation commands to apply before exposing it.
Suspend closes event/UI admission, cancels queued events, invokes suspend, then
requests HIDE. Resume invokes resume, then SHOW and ordinary event admission.
The system screen remains allocated and resumes after hide/destroy. No display
reinitialization occurs.

Stop closes admission, clears events/timers/subscription and finishes stop.
DESTROY uses a reserved control mailbox independent of FIFO capacity. The GUI
retires pending commands, deletes the root and its descendants, restores the
system screen and acknowledges completion. Only then is STOPPED published.
Unload invokes destroy once, retires UI ownership, reclaims SDK resources and
unloads ELF. UI requests cannot resurrect a retired root.

If acknowledgment times out, STOPPING and loaded Flash exclusion persist, the
error is reported, and the executor retries cooperative teardown. It does not
free code/resources or report successful unload. A non-returning GUI/native
callback remains subject to the existing watchdog/BootGuard recovery path.

| Shared state | Writers/readers | Synchronization/lifetime |
|---|---|---|
| Logical slots, FIFO, counters, control serials | executor API, GUI pump, system diagnostics | bridge spinlock; fixed system storage; no LVGL/native calls under it |
| LVGL objects and stable text | GUI only | existing GUI task; delete before clearing backing text |
| Native callbacks and SDK allocations | executor only | existing manager ownership and callback admission |
| Native event FIFO/control | GUI input, USB worker, executor | existing manager spinlock; copied payload; callbacks outside lock |
| Submitted ELF input/Flash exclusion | executor/system worker | existing atomic reservation; released only after UI and ELF teardown |

No lock is held across a callback, LVGL operation, wait, USB request or the
other manager's lock. GUI input releases the UI lock before posting to the
runtime. The executor removes an event under its lock, then validates the UI
handle outside it. The GUI never waits for an application callback. Lifecycle
waits occur only on the executor outside native callbacks; ordinary submission
never waits. Safe Mode waits for UI retirement before replacing the system root.
It boots without creating app objects or starting the executor. Existing RO1,
RE1 and deferred RS1 Flash policy remains in force even while suspended/stopped.

Resources count accepted widgets, button input registrations, copied commands,
and existing runtime event entries. Pending creation counts ownership too;
destroy reclaims it regardless of app cleanup. GUI deletion retires every UI
slot before the loaded-instance reservation is released.

## Validation and device evidence

Host tests compile the production bridge, backend, manager and example against
the real pinned LVGL 9.5.0 under ASan/UBSan. IDF, loader, addresses and time are
mocked; these tests execute host C callbacks, not Xtensa instructions. They cover
validation, copying, UTF-8, FIFO ordering/saturation, ownership, stale handles,
input admission, lifecycle/recovery and delayed teardown retaining loaded code.
The original relocation/alignment and ABI 1/2 regression suite remains required.

**Physical verified:** firmware 0.3.3 passed genuine inactive-B OTA and three
complete native UI lifecycle generations. Each rendered seven LVGL objects,
delivered three timers and four INJECTED button callbacks to Xtensa, applied
counter updates and removed all application objects/resources. Global SRAM and
PSRAM recovered exactly. The measured shared LVGL pool varied by 20 bytes;
this is reported separately rather than claiming exact pool recovery.
Maximum observed enqueue-to-apply latency was 310338 us, including screen
transition workload; this is not a low-latency guarantee. Physical finger touch
and human visual acceptance remain **PENDING**. Exact artifacts, timings,
limitations and final slot state are in the
[3C acceptance record](ui-milestone-3c-acceptance.md).
