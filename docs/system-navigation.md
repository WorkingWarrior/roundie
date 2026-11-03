# System navigation — Milestone 3E

The launcher and navigation chrome are firmware resources, separate from the
generation-owned native UI root. Only the existing GUI task creates or touches
LVGL objects. The launcher owns a persistent screen with eight reusable rows,
name/version labels, a status label, a busy spinner and a Refresh control.
The existing diagnostic screen remains allocated; its sensor labels and normal
boot self-test continue unchanged. The launcher becomes the default system
screen after normal/STABLE and confirmed OTA readiness, without executing ELF.

## Foreground and Exit

The system owns a 120 × 44 Exit button on LVGL's top layer, x=120..239,
y=12..55. The top 60 pixels are the system navigation safe area. Existing
ABI 3 geometry stays 360 × 360 and its descriptors are unchanged; applications
should leave this top central region free of essential content. The installed
UI example's first label starts at y=64, so its counter, TAP ME and uptime
remain unobscured. This policy adds no ABI and provides no native memory
isolation. Ordinary application widget handles cannot address system chrome.
The BOOT power menu ([PowerManager](power-manager.md)) is a later full-screen
top-layer object, so it covers the Exit button and the application while open.

The control is visible for ABI 2/3, including suspension, and during loading
after generation admission. It becomes disabled `Closing...` while an accepted
Exit is pending. Exit copies the exact runtime generation into the system
mailbox; it never becomes an ELF button event. The worker submits the existing
UNLOAD lifecycle command, which closes ordinary event/UI admission, lets an
executing callback return, stops timers/subscriptions, invokes stop, waits for
GUI DESTROY acknowledgment, invokes destroy, retires handles, reclaims SDK
resources and unloads code. The executor then frees its owned input buffer.

The bridge may expose the disabled launcher while deleting the app screen;
selection reopens only after UNLOADED, input retirement and zero application
resources/UI generation. A teardown failure retains ownership and blocks new
execution. No task is forcibly deleted, no code is freed during a callback,
and an unreturning native callback remains subject to the existing watchdog.

ABI 1 uses the loading presentation, unloads, then shows a persistent system
result with the installed display name, actual return value and a central
160 × 52 Back control at x=100, y=268. Back dismisses the matching request;
it does not submit execution or lifecycle work. Loader failure is distinct
from an intentional nonzero return. The four additional LVGL objects belong
to the persistent launcher, not to the unloaded instance. ABI 2 uses the system running/status screen.
ABI 3 retains the existing touch/event bridge and view show/hide barriers.
Input devices reset and wait for release on navigation transitions, matching
the existing bridge policy. Rows carry copied ID plus snapshot revision; stale
rows, removed IDs, unhealthy records and double admission fail closed.

## Recovery and diagnostics

Launcher discovery is a worker operation after normal startup confirmation;
it is never a Safe Mode or boot-confirmation prerequisite. Safe Mode disables
launcher admission and hides chrome before the existing recovery root cleanup.
RO1/RE1/RP1 writes remain excluded while ELF/input ownership persists; RS1 keeps
its existing deferred teardown/restart policy. Refresh performs no mutation,
automatic repair, formatting or package selection change.

RP1 adds bounded `NAV`, `REFRESH`, `SELECT <revision> <id>`, `BACK <request>` and `EXIT <generation>`
operations under the existing development package configuration. SELECT is
explicitly INJECTED and traverses the same coordinator as the LVGL row callback.
EXIT injects system navigation, not application input. NAV exposes state,
snapshot/request/generation identities, input counters, results, timings,
persistent GUI resources/pool and worker stack margin. Existing UI STATUS
continues to report application resources, command depth/latency and LCD DMA.
These diagnostics are neither a general remote control nor finger evidence.

3E.1 appends RESULT=8 without renumbering old states. NAV retains its existing
`us` fields and adds `name`, `completion` (request, completed serial, loader
result, executed, success, verified-widget/foreground flag, apply us,
unload-completion-to-GUI us), and `paint` (state, request, generation,
publication-to-GUI-ready us, monotonic applied timestamp). These indicate LVGL
mutation, not visible pixels. `PERF [cursor]` and idle-only volatile `BATCH 4|12`
provide the bounded [comparison procedure](launcher-3e1-performance.md).
Final 3E.1 device tests verified persistent 42, request-checked Back, zero
resources, ABI 3 native buttons/timers/suspend/resume/Exit and retired-input
rejection, using INJECTED input. Actual finger/visual acceptance remains PENDING.

```sh
python tools/roundie_launcher.py --port "$PORT" status
python tools/roundie_launcher.py --port "$PORT" select REVISION org.roundie.ui
python tools/roundie_launcher.py --port "$PORT" exit GENERATION
```

Manual finger/visual acceptance: confirm both rows and versions are readable;
tap Hello and verify persistent 42, dismiss with Back; tap UI and its TAP ME button; double tap while
loading; tap the separate top Exit; confirm application disappearance and a
readable usable launcher. Record real TOUCH counters separately from injected
ones. No camera or actuator observation is implied by software test results.
