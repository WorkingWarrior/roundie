# Milestone 3B: event-driven native application lifecycle

Milestone 3D adds [explicit launch by installed ID](app-registry.md). Packages
remain persistent after stop/unload; runtime input is separately allocated and
owned by the existing executor generation. ABI 1 uses RUN, ABI 2/3 use START.
No boot autostart is added. Loaded states, including SUSPENDED/STOPPED, continue
to exclude every package write until completed unload and input retirement.

**Milestone 3C extension:** [Application UI](app-ui.md) adds a GUI creation/show
barrier after start, hide/show on suspend/resume, and acknowledged GUI deletion
before STOPPED/unload. Callback signatures retain ABI 2 value layouts with
explicit ABI 3 context negotiation. A stalled GUI keeps code and Flash exclusion
alive; it never reports successful unload. Historical 3B evidence below is
unchanged; new device evidence belongs to app-ui.md.

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

**Host acceptance, 2026-10-19:** 68 unittest cases passed, including the original
112-case OTA boot matrix, actual RO1 controller exclusion, ASan/UBSan manager
tests, and C11/C23 Xtensa layouts for both public headers. After final review,
a focused BootGuard/RS1 test also passed for teardown finishing during REQUEST:
restart cannot overtake deferred NVS persistence. Its production correction was
rebuilt and verified before freezing. No warnings were disabled.

**Target verified:** clean REP build with pinned ESP-IDF v6.1 commit
`fff9895c82d744c7237be8847347bdd1b07c6643`, plus the final focused recovery
rebuild, passed. Production partition/layout validation passed with unchanged
`rep-v1`, 16 MiB, two 3.5 MiB slots and 2,968,816 bytes of image headroom.
Loader 1.3.5, memory protection and rollback remain enabled; broad exports,
XIP, security changes and both fault-injection configurations remain disabled.

| Frozen artifact | Bytes | SHA-256 |
|---|---:|---|
| Firmware 0.3.2 | 701200 | `38819c10d1d218bd00f8477fcb7fe627449c82a2099e4b6c44094c33949f8649` |
| Firmware embedded ELF identity | — | `32e888585278c7590ffc47f50a9bdf0d153de531df5244ddf9ec74441174dbde` |
| Independent event ELF, ABI 2 | 2116 | `8aaee0a856ad590a34ca5931404c8c62cd7b6fc499135b9a1bd397f9cbc9aa6b` |
| Original 3A hello ELF, ABI 1 | 1692 | `5edbfbf49c72890dd80f33263d2e52ffeeba9bfb0c5ef577047428a7360f0c88` |

The event ELF has 472 code bytes, a 191-byte worst-case data allocation,
21 relocations and exactly one import. The original hello binary was copied
unchanged from the preserved 3A acceptance archive, rather than substituting a
new build as compatibility evidence.

**Physical OTA verified:** current read-only preflight checked unique Espressif
USB identity `303a:1001`, serial/MAC `28:84:85:87:98:7C`, ESP32-S3, 16 MiB Flash,
security state, exact installed bootloader/table, CRC-valid A/B metadata and
the expected A 0.2.5 / B 0.3.1 image headers. Reads were bounded to a 64 KiB
prefix and two 288-byte headers. The original 16 MiB factory backup and tested
ROM Download recovery instructions were present; no whole-Flash dump was made.
After return to normal/STABLE B, the immediately repeated runtime checks passed.

Genuine RO1 streamed the frozen image into inactive **A**, while **B 0.3.1**
remained running/VALID. END verified the exact SHA-256 and embedded identity;
ACTIVATE reported A NEW; acknowledged RESTART produced A PENDING_VERIFY at
1158 ms and VALID at 3298 ms after the real core self-test. Host upload through
activation/restart acknowledgement took **72.462 s**. BootGuard became STABLE.
No B application bytes, bootloader, partition table, security configuration or
Flash partition layout were written. Existing NVS/user storage were preserved;
only the development staging files and normal BootGuard persistence were used.

**Physical runtime verified:** one load per generation, with executable entry
`0x420a1a40` in the PSRAM instruction alias, write allocation `0x3c0a18a8` and
data `0x3c0a1a84`. Three complete generations each delivered **5 timer callbacks
and 1 explicit user callback with value 42**, across separate system dispatches.
The same relocated image remained loaded between them. Each completed
suspend stayed quiet for 600 ms (more than two timer periods), rejected a new
event and resumed later timer delivery. One cycle stopped directly from
SUSPENDED. Invalid resume, stale-generation event, event after stop and
idempotent stop/unload were checked. No event callback followed unload.

| Measured on-device metric, microseconds | Cycle 1 | Cycle 2 | Cycle 3 |
|---|---:|---:|---:|
| Load/relocation | 9225 | 9182 | 9273 |
| Init | 4220 | 4198 | 4126 |
| Maximum callback/entry duration | 4200 | 4182 | 4328 |
| Suspend (last in cycle) | 1202 | 1162 | 1183 |
| Resume | 1143 | 1227 | 1147 |
| Stop | 1246 | 1291 | 1145 |
| Destroy/unload | 1452 | 1385 | 1409 |
| Maximum enqueue-to-event-callback latency | 729 | 934 | 737 |

The nine sampled completed timer/user callback durations were **841–1048 us**.
The overall callback maximum includes init and lifecycle work, not just events.
These are bounded observations with logging enabled, not worst-case guarantees.
Enqueue latency excludes timer deadline/poll jitter and host USB transport.
Host START completion took 58.450–59.569 ms; whole acceptance cycles, including
intentional suspend waits and polling, took 2.129–2.151 s. These host timings
must not be described as on-device callback latency.

All three cycles had identical heap snapshots (bytes):

| Snapshot | Internal free | Largest internal | PSRAM free | Code-heap largest |
|---|---:|---:|---:|---:|
| Before load | 246631 | 172032 | 8382300 | 8257536 |
| After relocation | 246631 | 172032 | 8381628 | 8257536 |
| Running with 16-byte SDK state | 246611 | 172032 | 8381628 | 8257536 |
| After destroy/unload | 246631 | 172032 | 8382300 | 8257536 |

Every cycle ended with `resources=[12,11,1,0]`: 12 acquired, 11 normally
released, one timer reclaimed at stop and zero outstanding. SDK state was
explicitly released; 20 measured internal heap bytes and 672 PSRAM bytes
recovered exactly. Events, subscription, timer, allocation and busy counts
were zero. Dropped/coalesced were **0/0**; three rejected event probes per cycle
were counted. Overflow/coalescing behavior is covered by host tests, not a
physical overload experiment. The persistent executor is excluded from the
per-instance heap comparison, as is the same retained input before/after.

**Physical compatibility and exclusion verified:** the unchanged ABI 1 hello
executed with ten relocations, six REP calls and result 42. Its SRAM/PSRAM also
recovered exactly. In a separate bounded instance, real RO1 BEGIN and RE1 BEGIN
were rejected with ESP_ERR_INVALID_STATE in RUNNING, SUSPENDED and STOPPED;
OTA stayed IDLE and no development transfer session opened. This checks the
guards before mutation, not simultaneous Flash/native execution.

**Safe Mode and final service acceptance:** see the final state record below.
Raw device logs, frozen artifacts and JSON measurements are intentionally
outside Git in `/mnt/Dane/Development/roundie-backups/runtime-3b-2026-10-19/`.
Production implementation and tests: commit `8a000ce`.
Historical 3A evidence remains in [elf-loader.md](elf-loader.md).

## Final Safe Mode and device state — 2026-10-19

An additional bounded native generation received two real timer events before
`RS1 REQUEST`. Its reply reported reason=request and `restart=0`, acknowledging
the deferred path rather than restarting inside the command. The subsequent
boot retained the request latch and entered Safe Mode, without a LittleFS
mount or application executor initialization. START, RUN and EVENT were refused;
resources, queue and busy were zero. The exact stop/destroy log lines between
the REQUEST acknowledgement and reopening USB were not captured: physical
evidence establishes successful deferred recovery, while callback/NVS ordering
is established by the production-source host tests. A callback interrupted
mid-instruction was not deliberately exercised on hardware.

An explicit normal trial then returned the same A image to normal/STABLE.
Final independent RO1/RS1/RT1/RE1 queries and startup logs established:

| Final item | Observed result |
|---|---|
| Running and selected firmware | A / ota_0, 0.3.2, VALID; exact candidate embedded ELF |
| Eligible fallback | B / ota_1, 0.3.1, VALID and compatible; rollback=1 |
| Fallback embedded ELF | `64e17722a3d8665399ca049eb1277e423e6bd6c3775b3057c008b0f6510a4298` |
| BootGuard | normal/STABLE, latch=0, failures=0, trial=0, persistence=1 |
| Fault injection | test_build=0, fault_remaining=0 |
| OTA | IDLE, checked=1, passed=1, last_error=0 |
| Runtime | UNLOADED, busy=0, allocations/resources/events/timers/subscriptions=0 |
| Normal startup | NVS initialization ESP_OK; no erase fallback |
| LittleFS | mounted, valid capacity; total 9240576 / used 20480 bytes |
| RTC | valid, OS=0, error=0; no clock-setting operation performed |
| USB | actual RO1, RS1, RT1 and RE1 replies received |
| Fault logs | no unexpected panic, task watchdog trigger or brownout in captured acceptance logs |

The post-recovery runtime has generation zero because it is a fresh boot with
no automatic application execution. The measured three-cycle cleanup remains
recorded separately in `app-results.json`, not inferred from that reset.
The staging file remains a development artifact; no installed-app registry or
automatic launch was introduced. ROM recovery was preserved and checked
against its existing physical procedure, not physically repeated unnecessarily.

## Repeatable bounded acceptance and remaining limits

The following performs at most three cycles and the original ABI 1 execution;
it checks the selected firmware/fallback identities before staging. It does
not perform OTA, recovery restart, malformed native execution or endurance work.

```sh
python tools/roundie_runtime_acceptance.py --port "$PORT" \
  --event examples/event_elf/build/event.app.elf \
  --legacy /mnt/Dane/Development/roundie-backups/elf-3a-2026-10-19/fix-0.3.1/hello.app.elf \
  --expected-elf 32e888585278c7590ffc47f50a9bdf0d153de531df5244ddf9ec74441174dbde \
  --fallback-elf 64e17722a3d8665399ca049eb1277e423e6bd6c3775b3057c008b0f6510a4298 \
  --cycles 3
```

No deliberate invalid-memory, infinite-loop or watchdog-triggering native
application was executed. Fault-injection lifecycle failures, forgotten SDK
allocations, queue saturation/coalescing and Safe Mode during an active callback
are host-test results, not physical failure-injection evidence. Native code is
trusted and unisolated. A non-returning callback still requires system recovery.

**PENDING / outside 3B:** GUI responsiveness and display timing, concurrent
Flash/cache/PSRAM execution, physical overload/latency worst cases, long endurance,
power interruption and battery-dependent recovery. RTC clock discipline and
battery calibration remain separate earlier limitations. No Milestone 3C,
package installation, network, MTP, peripheral leasing or GUI SDK work was begun.
