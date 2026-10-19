# Milestone 3B: event-driven native application lifecycle

## Contract and ownership

**Architectural decision (2026-10-19, REP §§5–10, 23):** preserve ABI 1 and
introduce a separate ABI 2, rather than extending its fixed 12-byte descriptor.
`RUN <sha256>` remains a one-shot ABI 1 operation. `START <sha256>` loads,
initializes and starts ABI 2 asynchronously. The independent event application
uses `rep_app_entry_v2` and imports only `rep_abi_v2`. Metadata, entry, import,
table and descriptor must all agree on the version. This is a development
contract, not a production SDK compatibility freeze.

The existing `roundie_elf` manager owns the single instance and its copied
callback references. Its one persistent `rep_executor` task runs entry and all
callbacks. USB, GUI, timer interrupts and other workers never call application
code. No application tasks or workers are created. Static application state
and SDK allocations persist between callbacks. No application starts at boot.

The dedicated task keeps its existing priority 1, 12288-byte stack and watchdog
policy. It waits at most 10 ms for system work; this is a scheduling granularity,
not a latency guarantee. Task watchdog subscription covers native execution
and teardown, and is removed while waiting. Every callback and entry is timed;
exceeding the existing 100 ms soft budget sets a sticky diagnostic and warning.
A timeout does not mean instructions stopped. Non-returning native code is
left to the configured watchdog/BootGuard; no task deletion or forced unload.

## States and failure handling

Numeric states retain ABI 1 diagnostics 0–5:

| State | Meaning |
|---|---|
| 0 UNLOADED | No callback references, loader or owned SDK resources |
| 1 LOADING | Input reserved; preflight, allocation and relocation |
| 2 READY | Relocated; entry/descriptor and initialization |
| 3 RUNNING | Ordinary event dispatch permitted |
| 4 STOPPING | Admission closed; timer and subscription detached; stop callback |
| 5 UNLOADING | Destroy callback, SDK reclamation, loader release |
| 6 INITIALIZED | Successful init, before start |
| 7 STARTING | Start callback |
| 8 SUSPENDING | Suspend callback |
| 9 SUSPENDED | No ordinary event dispatch or admission |
| 10 RESUMING | Resume callback |
| 11 STOPPED | Stop completed, ELF remains loaded until UNLOAD |

All seven ABI 2 callbacks are mandatory and validated inside relocated code
before any init call. Entry returns only a descriptor; callbacks receive a
borrowed 16-byte context identifying the current generation. Return 0 means
success; nonzero means failure. Init runs once. Destroy runs once even after
init failure, so it must tolerate partial initialization. Stop also runs after
an attempted but failed start; it does not run when init failed. Invalid
descriptors receive no cleanup callbacks because their addresses are untrusted.
The manager always reclaims remaining SDK allocations after permitted cleanup.

Suspend/resume failures tear down instead of publishing a successful stable
transition. Event/start/init/stop failures also lead to teardown. Destroy failure
is recorded and remaining resources are reclaimed after it returns. The first
nonzero application result is retained. Stop is idempotent in STOPPED/UNLOADED;
unload is idempotent in UNLOADED for the same generation. Suspend twice and
resume without suspension are rejected. STOP from RUNNING or SUSPENDED cancels
ordinary work and invokes stop once. UNLOAD also performs any required stop,
then destroy and loader cleanup. No event/start callback follows stop; destroy
is the sole permitted subsequent application callback.

## Events and timer

The FIFO holds **8 copied 24-byte events**. Public fields are type, exact
structure size, one unsigned 32-bit value, reserved zero and 64-bit monotonic
microsecond enqueue timestamp. There are no external payload pointers.
An admitted FIFO belongs entirely to the current generation: producer admission
checks it under the mailbox lock; teardown and the next prepare erase its contents.

Only RUNNING admits events. A full FIFO rejects the newest user event with
`ESP_ERR_NO_MEM` and increments dropped. At most one timer event may be queued;
another is coalesced and counted. Admitted user/timer events retain FIFO order.
Timer overruns do not generate a catch-up storm: the next deadline starts from
the current poll. Tick events carry the timer handle; user events carry the
injected value. Cancellation removes pending events for that timer.

The executor itself is the system-owned periodic source. Applications register
one timer through the ABI, with period **50–10000 ms**. There is no ELF callback
in an `esp_timer` or ISR. Suspend discards queued events, pauses the timer and
rejects new ordinary work; resume starts a fresh full timer period. Stop cancels
the timer and the implicit development/timer subscription before cleanup.

One reserved control mailbox is independent of the ordinary FIFO. A lifecycle
request closes admission immediately, even when ordinary work is queued. Only
one control may be pending. Completion publishes a serial and result; a second
request before completion is rejected. Safe Mode shutdown has priority. Callback
admission is linearized at dequeue under the same lock; an already admitted
callback may return before shutdown. Executor-only guards prohibit recursion.

## Flash, OTA and Safe Mode

**Decision:** every loaded state, including SUSPENDED and STOPPED, excludes OTA
and development staging writes. Unload, rather than suspend, is necessary.
The sole system worker serializes RE1 staging, RO1 mutations and RS1 policy.
The input reservation remains busy until the executor frees its retained input,
preventing another submission from overtaking completed teardown. OTA requests
cannot race new starts through a separate caller in the current integration.

PENDING_VERIFY, failed self-test, non-STABLE BootGuard, active OTA and Safe Mode
all deny START/RUN. A policy denial closes admission and requests teardown on
the executor. A currently executing callback is allowed to return; cleanup
release/log calls remain usable, but new allocations/timers are denied.

`RS1 REQUEST` sets the in-memory recovery latch and denies application admission
immediately. While an ELF or development transfer is busy, NVS latch persistence
and restart are deferred. The executor stops/destroys/unloads; only after the
input owner retires does the system worker commit NVS and restart. A failed
commit refuses that deferred restart. CLEAR, SELECT, FAULT and restart are
also refused while development ownership is active. Periodic BootGuard NVS
sync is skipped during it. Startup NVS initialization occurs before any ELF.
No automatic formatting or NVS erase was added.

Safe Mode still starts without the executor, LittleFS or application resources.
The known production RO1/RE1/RS1 writers are covered by this policy. This is not
a global Flash scheduler or proof for arbitrary future writers. Concurrent
Flash programming/native PSRAM execution, GUI responsiveness and cache stalls
remain **PENDING**; this milestone deliberately excludes their overlap.

## Development protocol and independent build

All mutations are strictly framed `!RE1 <8 lowercase hex request ID> ...`.
The existing fixed staging file `/storage/dev/hello.elf` is reused, with atomic
temporary-file replacement and exact SHA-256; it holds either example. There
is no application registry or arbitrary filesystem access.

```text
STATUS
RESOURCES
RUN <64 lowercase hex SHA256>       # ABI 1 only, one-shot
START <64 lowercase hex SHA256>     # ABI 2 only, persistent instance
EVENT <decimal generation> <decimal uint32 value>
SUSPEND <decimal generation>
RESUME <decimal generation>
STOP <decimal generation>
UNLOAD <decimal generation>
```

BEGIN/DATA/END/ABORT retain the 3A contract. Replies are bounded to 2048 bytes.
START/controls acknowledge acceptance; observe state and `control` serials for
completion. EVENT acknowledges enqueue, not callback completion. STATUS exposes
queue depth, delivered timer/user events, last user value, dropped/coalesced/
rejected counters, callback count/max duration, init/suspend/resume/stop/unload
durations, memory snapshots and resource accounting. `control` is
`[submitted, completed, result]`. Host tools wait boundedly without retrying
mutations or forcibly unloading after a lost acknowledgement.

```sh
. /home/solve/.espressif/v6.1/esp-idf/export.sh
cmake -S examples/event_elf -B examples/event_elf/build
cmake --build examples/event_elf/build
python tools/roundie_elf.py inspect examples/event_elf/build/event.app.elf
python tools/roundie_elf.py --port "$PORT" upload examples/event_elf/build/event.app.elf
python tools/roundie_elf.py --port "$PORT" start --sha256 <exact SHA256>
python tools/roundie_elf.py --port "$PORT" event --instance <generation> --value 42
python tools/roundie_elf.py --port "$PORT" suspend --instance <generation>
python tools/roundie_elf.py --port "$PORT" resume --instance <generation>
python tools/roundie_elf.py --port "$PORT" stop --instance <generation>
python tools/roundie_elf.py --port "$PORT" unload --instance <generation>
```

## Evidence

**Host verified:** actual manager/validator/dispatcher compiled with ASan/UBSan,
mock IDF/loader and independently compiled target fixtures. Tests exercise
lifecycle failures, FIFO/overflow/coalescing, suspend, stale generations/handles,
callback ownership/recursion, Safe Mode during a callback, repeated teardown,
partial-init and forgotten allocation reclamation. They execute host mock
callbacks, not native Xtensa instructions. The original rounded-text overlap
regression and forbidden helper/TLS probes remain.

**Target and physical acceptance:** PENDING final frozen build, genuine RO1 OTA,
native repeated event callbacks, measured memory recovery and final slot/service
checks. Historical 3A evidence remains in [elf-loader.md](elf-loader.md).
