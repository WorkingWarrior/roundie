# Watch face platform (M10)

Home is a watch face. This document covers the architecture, the
integration with the launcher, power and recovery, budgets and testing.
Contract: [watch-face-abi.md](watch-face-abi.md). Packages, store and the RF1
transport: [watch-face-packages.md](watch-face-packages.md). Writing faces:
[watch-face-authoring.md](watch-face-authoring.md).

## Decisions

- **Home is WatchFaceHost** (`components/roundie_watchface`), not an
  application. The launcher's new state `ROUNDIE_NAV_HOME` shows the face;
  boot and the end of an application return to it. The application list is
  one swipe up from Home's bottom strip; a swipe down from the top strip opens
  Quick Settings; the left-edge swipe on the list returns Home.
- **There is always a face.** The built-in *system face* (`fallback.c`,
  firmware code, no package) is shown when no face is preferred, while a
  face loads, after any face failure, and during a trial boot or Safe Mode
  handoff.
- **One lifecycle, one backend.** Declarative faces are interpreted by
  firmware code; native faces are reserved and refused (ABI document). No
  second ELF loader and no application system for faces exist.
- **The same code on Linux.** The desktop build compiles the same component
  (`platform/linux`), with the same store and the same GUI half; there is no
  separate Linux face system.
- `org.roundie.watch` stays an ordinary REP application on the list; it is no
  longer started at boot (see the migration note in
  [watch-face.md](watch-face.md)).

## Architecture

```
 system worker (app_runtime.c)          GUI task (launcher render.c)
 ┌──────────────────────────┐  mailbox  ┌───────────────────────────────┐
 │ store.c                   │ ───────► │ gui.c: face screen, gallery    │
 │  discovery, install, RF1  │  offer   │ host.c: lifecycle, switch      │
 │  load + full validation   │ ◄─────── │ declarative.c / fallback.c     │
 │  preference (tmp+rename)  │  report  │ text.c: templates              │
 └──────────────────────────┘          └───────────────────────────────┘
```

- **Store (worker side).** `roundie_watchface_poll` runs on the system worker
  (`main/app_runtime.c`, and `face_diagnostics.c` for RF1). It owns every
  file under `/storage/faces`. Every activation reads the whole package into
  a PSRAM buffer (the package store's allocator) and validates it completely
  before the buffer is offered to the GUI. The GUI never touches storage.
- **Mailbox.** One offer slot (face buffer, purpose, serial) worker → GUI and
  one report (success or the refusal reason) GUI → worker. The preference is
  written only after the GUI reports a successful activation. A stale offer
  (older serial) is dropped.
- **GUI side.** `roundie_watchface_gui_pump(home)` runs once per launcher
  render pass on the GUI task (`render.c`). It creates, activates, updates,
  hides and resumes faces through `host.c`, and draws the gallery. A face
  buffer is freed only when neither the host, the gallery (the face to
  restore) nor the pending slot refers to it.

## Home integration

| From | Gesture / event | To |
|---|---|---|
| boot (not a trial) | — | HOME |
| HOME | swipe up from the bottom strip | LIST |
| HOME | swipe down from the top strip | Quick Settings over Home |
| HOME | long press on the face | gallery (Home stays) |
| LIST / ERROR | left-edge swipe | HOME |
| application ends normally | — | HOME (via LIST on the next poll) |
| ABI 1 RESULT | back | LIST |

While the launcher is not in HOME the face is *hidden*: deactivated but kept
created, so returning is a resume, not a reload. `RP1 SELECT` from HOME
starts an application as from the list (development transport).

A trial boot (`boot_status.trial`, BootGuard) calls
`roundie_launcher_set_home_face(false)`: the launcher keeps the list as its
start screen, the store runs `fallback_only` (no package faces) and the
system face is all there is. The desktop does the same after a session that
ended with an application loaded.

## Power (M8-B)

- Light sleep: `power_gui.c` parks the face (`roundie_watchface_gui_park`)
  before the GUI parks; no face code runs while parked. After the display
  restore, `roundie_watchface_gui_resume` activates it again and delivers one
  `RESUMED` update with a fresh sample from the RTC-backed clock. Missed
  seconds are not replayed.
- Screen off: `roundie_watchface_gui_visible(false)` stops the update
  schedule; faces receive `VISIBLE` on change.
- Deep sleep is not used.

## Failure handling

- A face that fails `create`, `activate` or its first update is refused; the
  previous face is activated again, or the system face is shown.
- A later update failure or a failed resume replaces the face with the
  system face. The store keeps the preference; `failed` and `error` name the
  face and reason; the counters record it.
- A preferred face that no longer validates at boot is not shown (system
  face, `load_failures`); reinstalling the same version repairs it.
- Safe Mode handoff abandons the face (`roundie_watchface_gui_abandon`): the
  host stops and nothing is drawn after the root is cleaned.

## Budgets

Limits (enforced by the compiler and again on the watch): package ≤ 64 KiB,
definition ≤ 4096 B, ≤ 32 elements, depth ≤ 4, resources ≤ 48 KiB, ≤ 8
resources, 1024 B of strings.

Measured on the host (`tests/test_watchface_gui.c`, LVGL 360 × 360, x86-64;
**not device figures**):

| Face | LVGL objects | face bytes |
|---|---|---|
| system face | (built in) | — |
| `classic_analog` | 12 | 4632 |
| `minimal_digital` | 9 | 454 |
| `information` | 13 | 2906 |

100 alternating switches return the LVGL pool to within 256 B of the
baseline (measured: 13376 → 13344 B used) and leave the store's buffer count
unchanged.

**PENDING (device 1.85B):** activation and update time, PSRAM buffer peak and
LVGL pool use on the board; method: RF1 `STATUS` (`us`, `objects`, `pool`,
`bytes`) after install, select and 100 switches; acceptance: no failures,
pool free back within 256 B after the switches, update max below one frame
period (33 ms).

## Testing

| Level | Test | Covers |
|---|---|---|
| host | `tests/test_watchface.c` (`test_watchface`) | format parser, text templates, change detection, host lifecycle with mock faces, native admission, store install/repair/remove/preference, power loss during preference write |
| host | `tests/test_watchface.py` | compiler reproducibility and refusals, RF1 reply parser |
| host GUI | `tests/test_watchface_gui.c` (`test_app_ui`) | three demo faces on LVGL, gallery, park/resume, corrupt package refusal and repair, 100 switches memory |
| host | `tests/test_launcher.c` | HOME state, list gestures, return after an application |
| desktop / VHL | `test_desktop.py: test_watch_faces_on_home` | install ≠ activation, select, 100 switches, list and back, light sleep and wake, persistence across restart, corrupt package → system face |
| desktop | updated navigation tests | every flow starts at HOME |
| device | RF1 via `tools/roundie_face.py` | see the M10 device report |

VHL and desktop results are emulator evidence, not hardware evidence.

### On the virtual hardware

The desktop (`roundie-desktop`, [virtual-hardware.md](virtual-hardware.md))
adds:

- `--install-face FILE.rwf` (repeatable): installs through
  `roundie_watchface_install_file` at start, before Home is shown;
- script commands `face-select ID`, `face-remove ID`, `face-status` (one
  `face active=… preferred=… failed=… error="…" count=… fallback_only=…
  home=… parked=… …` line) and `wait-face ID TIMEOUT_MS`;
- `wait-nav HOME` / `expect-nav HOME` for the new launcher state.
