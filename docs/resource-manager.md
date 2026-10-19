# Milestone 3B resource ownership

**Milestone 3C extension:** [UI ownership](app-ui.md) tracks generation-owned
logical view/label/button slots, input registrations and copied GUI commands.
These counters join runtime resource diagnostics for ABI 3. UI teardown must
acknowledge LVGL root/descendant deletion before ownership retirement and ELF
release. Text backing storage belongs to the GUI and is cleared after deletion.
The [input bridge](input-bridge.md) uses the existing owned runtime event FIFO.

**Decision (REP §10):** extend the existing `roundie_elf` manager, rather than
creating an unused global service. It is the ResourceManager foundation for
resources exercised by one loaded application. Peripheral leasing, GUI handles,
packages and application multitasking remain outside this milestone.

Each instance has a monotonically increasing nonzero generation; exhaustion
refuses another instance rather than wrapping. The executor owns the instance
record, copied callback-reference record, upstream loader image/all executable
and data allocations, four SDK allocation slots, one implicit event subscription
and at most one timer registration. The locked mailbox owns copied queue entries.
The system executor separately owns the submitted ELF input until teardown;
its atomic reservation keeps `busy=1` until the actual buffer is freed.

SDK slots store pointer, size and generation. Both ABIs retain the bounded
allocator: at most **4 allocations / 4096 aggregate bytes**, zero-size returns
NULL, oversize/overflow-safe remaining-budget checks return NULL. Release
requires the current generation, exact live pointer and executor callback
context. Foreign pointers, foreign/stale generations and duplicate releases
fail. Forgotten allocations are reclaimed after destroy returns, including
partial-init and callback failure. Raw-pointer allocations cannot detect an
old pointer whose address has subsequently been reused within the same
generation; this is the preserved ABI 1 ownership contract, not memory isolation.

Timer handles are opaque nonzero uint32 tokens unique within a boot. Their
typed timer slot belongs to the current generation; invalid/stale/foreign and
double-cancel requests fail. New timer acquisition is refused at token
exhaustion, while stopping/unloading or after system policy denial. Cancel is
executor-only and removes queued events for that handle. No private timer,
task or manager pointer crosses the ABI.

Teardown closes admission, drops queued entries, detaches subscription/timer,
finishes any currently admitted callback, invokes stop if needed, invokes destroy
once, reclaims outstanding SDK allocations, clears every callback reference,
deinitializes the loader, retires the instance and then releases executor input.
There are no application workers requiring cancellation acknowledgement in 3B.
Future workers must join this ordering before freeing memory they can access.

`resources` is `[acquired, released, reclaimed, outstanding]`. It counts the
instance and callback records, initialized upstream image, SDK slots, subscription,
timer and queued events. The loader image represents its owned allocation group,
not one record for each upstream malloc. Queue entries retire on delivery or
cancellation. A timer left registered at stop and forgotten allocations are
counted as reclaimed; explicit SDK release and normal record retirement are
released. The separately owned input reservation is represented by `busy`;
it is not included in these manager-resource counts. Normal final idle status
requires both zero outstanding and `busy=0`.

Memory snapshots retain the 3A order: internal free/largest, PSRAM free,
code-allocation heap free/largest. Before/after snapshots occur while the
executor still owns the same candidate input, so they compare application
load/unload directly. Input is freed immediately afterward, before busy clears.
Persistent system executor task/queue/stack are not application leaks. Largest
block changes can indicate fragmentation even if byte counts recover.

**Verified host:** production logic with ASan/UBSan covers allocation count and
aggregate limits, zero/UINT32_MAX sizes, foreign owner/pointer, double release,
timer stale generation/token, cleanup after failed init/start/event, stop while
running/suspended and 12 lifecycle modes. Every completed teardown recovers
mock heap bytes and leaves zero resources/events/timers/subscriptions.
**Physical verified, 2026-10-19:** three complete native event-app generations
each ended with `resources=[12,11,1,0]`, no allocation/timer/subscription/event,
and busy zero. Each reclaimed one still-registered timer at stop and explicitly
released its 16-byte SDK state at destroy. Internal free bytes recovered from
246611 to 246631; PSRAM recovered from 8381628 to 8382300. Largest internal/code
blocks remained 172032/8257536 bytes. These identical before/after results
exclude the persistent executor and retain the same input in both snapshots.
The original ABI 1 binary also recovered its measured heap exactly.
Full timings, identities and evidence boundaries are in
[app-lifecycle.md](app-lifecycle.md#evidence). Forgotten-allocation and partial-init
reclamation are host-test evidence; no deliberately failing native ELF was run.
