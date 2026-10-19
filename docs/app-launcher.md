# Native application launcher — Milestone 3E

## Design decision, 2026-10-19

REP §§5–10, 12–13 and 23 remain authoritative. The existing GUI task owns
launcher objects and system navigation. A fixed mailbox copies an exact ID,
snapshot revision and request serial; it admits one selection at a time.
The existing system worker consumes it, calls PackageManager launch-by-ID
(including committed selection, digest and ELF validation), and observes
ELFManager. Only the existing executor calls native code. There is no new task,
public ABI, persistent registry or execution state machine.

Worker-owned discovery publishes a bounded metadata snapshot of at most eight
records. GUI copies contain no ELF or application pointers. Initial discovery
waits for normal/STABLE and OTA confirmation. Successful package scans advance
an internal cache revision; completed install/remove/update scans invalidate
the presentation without filesystem polling on every frame. Refresh failure
preserves the previous presentation and disables selection until controlled
retry. Discovery and validation never run under the launcher lock or on GUI.

Navigation is WAITING → REFRESHING → LIST → REQUESTED → LOADING → ACTIVE
→ EXITING → LIST. ERROR retains a usable retry affordance after safe cleanup.
Request serials, snapshot revisions and runtime generations have distinct
roles. Stale selection, duplicate admission and mismatched runtime generations
are refused. Runtime unload and input-buffer retirement, UI generation zero
and zero application resources are required before list admission reopens.
Failed teardown keeps Exit/diagnostics available and prevents another launch.

ABI 1 unloads automatically and enters the persistent system RESULT presentation.
The copied installed display name and actual application return value remain
until Back. Return value 42 is valid data, not a loader failure. Admission errors,
loader errors and execution/cleanup errors use the same system-owned page with
distinct failure text. Matching request, instance and advanced completion serial,
input-buffer retirement and zero resources gate successful completion; historical
global result data is insufficient. Registry publication cannot dismiss RESULT.
Back checks the request serial and submits no ELF work. ABI 2 uses a system status
screen; ABI 3 uses its existing UI bridge. The system chrome provides Exit
independently of app widgets. Trusted development USB launches are also
observed so that system navigation remains usable. No application autostarts.

The worker retains its 12 KiB stack. Snapshot and GUI backing arrays are static;
the lock protects only bounded copies and admission. No filesystem, LVGL,
runtime API, waiting or callback occurs while holding it. The earlier 3D
LittleFS panic remains evidence of a failure with an inferred, unproven stack
cause; target stack usage and physical high-water measurements are required.

Implementation, host/target and physical evidence are recorded separately
in the [milestone acceptance record](launcher-milestone-3e-acceptance.md).
Finger/visual acceptance requires a human
or physical observation; semantic injection and flush counters cannot supply it.

The [3E.1 correction and display measurements](launcher-3e1-performance.md)
preserve the original 3E observations, including the 517828 us queue delay.
