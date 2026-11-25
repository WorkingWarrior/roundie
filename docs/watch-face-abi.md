# Watch Face ABI v1

The contract between the Roundie watch face host and a face. Header:
[`include/rwf/rwf_face_v1.h`](../include/rwf/rwf_face_v1.h). Platform overview:
[watch-face-platform.md](watch-face-platform.md); package format:
[watch-face-packages.md](watch-face-packages.md).

## Scope and relation to REP

- **Decision (M10):** the face ABI is separate from the REP application ABI
  (`rep_app*.h`, ABI 1/2/3). REP is unchanged. A face is not an application:
  it has no entry point, no event loop of its own, no storage, audio or input
  services, and never runs in the Application Executor.
- **Decision:** there is one face lifecycle for every backend. The firmware
  ships one backend, *declarative*: the host interprets a validated
  definition from the package (`components/roundie_watchface/declarative.c`)
  and exposes it through the same `rwf_face_v1_t` table a native face would.
- **Decision / limitation:** the *native* backend (code in the package) is
  reserved (`ROUNDIE_FACE_BACKEND_NATIVE`, manifest backend 2) and **refused**
  in this release with `native-face-unsupported`. ABI v1 has no drawing API
  that keeps LVGL out of the public ABI, and a second ELF loader is excluded.
  The host keeps the admission rule for it (`roundie_face_host_admit_native`
  in `host.c`, host-tested): without an ARMED guard (FAILED or trial boot)
  only the fallback is allowed, and with it a valid native table is still
  refused with `RWF_ERR_UNSUPPORTED`. No package can reach this path.

## Versioning

`RWF_ABI_MAJOR` 1, `RWF_ABI_MINOR` 0. Every structure starts with `size`.
The host refuses a face whose `abi_major` differs, whose `abi_minor` is newer
than the host's, whose `size` is smaller than `rwf_face_v1_t`, or whose `requires` names a capability the host lacks
(`RWF_HOST_CAP_*`); a minor increase may only append fields and flags. The
store refuses a package with another major before it reaches the GUI
(`face-abi`).

## Lifecycle

All calls are made on the GUI task, never concurrently, never re-entrantly.

```
create ──► activate ──► update(ACTIVATED) ──► update(...)* ──► deactivate ──► destroy
               ▲                                                   │
               └──────────────── resume (park / Home left) ◄──────┘
```

| Call | Purpose | Failure handling |
|---|---|---|
| `create(instance, host)` | validate, allocate, bind resources; no drawing | negative result: the face is destroyed and refused; the previous face stays |
| `activate(instance, surface)` | build the visible face under `surface` | negative: the new face is destroyed, the previous face is activated again |
| `update(instance, event, data)` | apply one event with the current sample; bounded, no waiting | the first (`ACTIVATED`) update counts as activation; a later failure, or a failed resume, replaces the face with the system face |
| `deactivate(instance)` | remove everything `activate` built; cannot fail | — |
| `destroy(instance)` | release everything `create` allocated; cannot fail | — |

Host rules (verified by `tests/test_watchface.c`, host lifecycle with mock
faces, and `tests/test_watchface_gui.c` on LVGL):

- A switch first creates the new face; a create failure leaves the old face
  untouched. Then the old face is deactivated but kept created (never two
  active faces), and the new one is activated with its first
  `RWF_EVENT_ACTIVATED` update. If that fails, the new face is destroyed and
  the old one is activated again; if even that fails, the system face takes
  over. Only after success is the old face destroyed (`switches`,
  `failures`, `refused` counters).
- Park (light sleep, M8-B) and hide (Home left for the list or an
  application) deactivate the face but keep it created. No call reaches a
  parked face. Resume activates it again and delivers exactly one
  `RWF_EVENT_RESUMED` with a fresh sample from the RTC-backed clock; missed
  seconds and minutes are not replayed.
- Abandon (Safe Mode handoff, `roundie_watchface_gui_abandon`) tears the face
  down and the host stops.

## Events and needs

The host delivers one `rwf_event_v1_t` per `update`. It schedules only what
the face's `needs` ask for (`RWF_NEED_SECOND`, else minute boundaries;
`RWF_NEED_BATTERY`; `RWF_NEED_TAP`). Change detection is pure and host-tested
(`roundie_face_changes`): `DATE` on a local date change (midnight, a long
sleep, a DST step that crosses midnight), `ZONE` when `time_revision`
changes (sync, zone change, wall-clock jump), `BATTERY` when
`battery_revision` changes, `VISIBLE` when the panel turns on or off.

## Data sample

`rwf_data_v1_t` is one consistent sample taken by the host: local days and
milliseconds of day, UTC seconds, UTC offset (DST included), the POSIX TZ
rule, battery percent and charging. Without `RWF_DATA_TIME_VALID` the time
fields are unspecified and faces show placeholders ("--", "Brak czasu"); a
face never invents a time. Battery fields are meaningful only with
`RWF_DATA_BATTERY_VALID`.

## Host services

`rwf_host_v1_t` gives the face: `data` (copy of the latest sample),
`request_refresh` (one coalesced `REFRESH` update), `set_needs`, `resource`
(package resource by index, read-only, valid until `destroy`), and
`setting_get` / `setting_set` (per-face option indices; the store persists
them outside the GUI task). No storage, network, timer, task or LVGL object
is exposed.

## Ownership and isolation

The package bytes, settings and resources are owned by the host for the
face's lifetime. A declarative face owns only the LVGL objects it creates
under the surface; `deactivate` deletes them (the GUI test checks the LVGL
pool returns to the same level after 100 switches). There is no memory
isolation; the declarative backend runs only firmware code over validated
data, which is why it is the only backend admitted.
