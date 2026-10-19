# Milestone 3C — touch/input bridge

The existing CST816S acquisition and LVGL pointer input remain the sole input
path. Orientation and coordinate mapping are unchanged. LVGL hit testing selects
the system-owned application button. Its CLICKED callback passes only the
generation, logical widget handle, TOUCH origin and on-device timestamp to the
bridge. The bridge validates realized button, foreground, visible/enabled state
and admission, releases its lock, then copies an event into the existing runtime
FIFO. It never calls an ELF callback. Only `rep_executor` invokes `on_event`.

ABI 3 reuses the fixed 24-byte event value layout: type `REP3_EVENT_BUTTON`,
value = widget handle, reserved = `REP3_ORIGIN_TOUCH` or `REP3_ORIGIN_INJECTED`;
the 16-byte context identifies the generation. UI events are never coalesced.
The existing eight-entry FIFO rejects newest on overflow, tracks drops, and
keeps lifecycle control serviceable. The executor checks widget admission again
before dispatch. Controls cancel already queued events; no suspended touch is
replayed. View switches reset input state and wait for release to prevent an
old press turning into a resumed click. No GUI lock waits for FIFO consumption.

`!RE1 <id> UI STATUS` returns a bounded array decoded by `tools/roundie_ui.py`.
It exposes generation/view/button, widgets, command progress/errors, resource
counts, physical/injected/accepted/dropped/rejected/delivered input counts,
microsecond queue/apply timings, LVGL allocator delta and existing LCD completed
DMA/error counters. Text hashes are diagnostic FNV-1a values of applied text,
not proof of visible pixels. Enqueue-to-apply and event-to-callback times exclude
the earlier sensor scan and later physical LCD response. Maxima are observed
samples, not worst-case or percentile guarantees.

`UI PRESS <generation> <widget>` is available only with the existing development
ELF transport (`CONFIG_ROUNDIE_DEV_ELF`). It requires the exact active, realized,
enabled visible button and normal runtime policy, then uses the same bounded
event sink/FIFO as TOUCH, with INJECTED origin. It cannot control arbitrary UI
objects, create views, send raw coordinates or call ELF directly. A successful
injection proves the semantic path only. Software-sent LVGL events on a host
also do not prove CST816S, finger interaction or actual display appearance.

## Manual acceptance (PENDING until actually performed)

1. Start the exact `examples/ui_elf` binary after upload with `roundie_elf.py`.
2. Confirm a readable centered title `Roundie ELF`, description, counter, button
   and uptime. Check edges on the real round screen.
3. Record UI STATUS, tap TAP ME three times with a finger, and confirm the visible
   counter increments once each time. Record TOUCH/delivered increments and
   real ELF callback logs. Do not use UI PRESS for this check.
4. SUSPEND the exact generation; taps must produce no application callbacks.
   RESUME, release, then tap again and verify exactly one increment.
5. STOP then UNLOAD; confirm the original system screen returns, zero widgets,
   zero pending commands/resources, and no later callback for that generation.

Build and lifecycle details are in [app-ui.md](app-ui.md). The exact authorized
device record will distinguish software-injected events, LVGL application,
LCD DMA completion and pending finger/visual checks.
