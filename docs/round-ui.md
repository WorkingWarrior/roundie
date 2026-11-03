# Stage 5 — Round UI widgets

## Architectural decision, 2026-11-03

REP §12 and §23 stage 5 require RoundList, RoundDialog, ArcSlider, EdgeButton
and shadow state behind the asynchronous UI API, with no LVGL access from
applications. These are an **extension of ABI 3**, not a new ABI:

- A new capability, `REP3_CAP_ROUND_UI` (32), is required in the descriptor.
  Firmware without it (0.3.21 and older) rejects such a descriptor before
  `init`, because the required capabilities are not a subset of its own.
- The import `rep_abi_v3`, the entry `rep_app_entry_v3`, metadata version 3 and
  the 52-byte API table are unchanged. Existing ABI 3 applications behave as
  before: version-1 requests keep their old validation.
- Round widgets use a version-2 widget request, `rep_ui_widget_v2_t` (148 B: the
  132-byte version-1 layout followed by `value`, `minimum`, `maximum`, `options`).
  It is passed to `ui_create` as a pointer to its leading `base` member.
- Shadow state is read through a version-2 `ui_result`, `rep_ui_result_v2_t`
  (48 B), with the widget handle as input.

A separate ABI 4 was rejected: it would duplicate the import, entry, metadata and
loader paths for an additive change that versioned structures and a capability
bit already express. Layouts are fixed by `_Static_assert`s in
`include/rep/rep_app_v3.h`, compiled for Xtensa by the example and the firmware.

## Widgets

| Type | Geometry | Request fields | Input |
|---|---|---|---|
| `LIST` (RoundList) | Application box. Every corner within radius 176 of the centre, at least 44 × 44 | text `"a\nb\nc"`, 1–16 non-empty items; `value` = selected index | `VALUE` with the selected index |
| `ARC_SLIDER` | System: 340 × 340 ring at (10, 10), 24 px wide, right edge from 320° to 40°, larger values upward | no text; `minimum < maximum`, both within ±1,000,000; `value` in range | `VALUE` with the slider value |
| `EDGE_BUTTON` | System: 160 × 56 at (100, 280), pill shaped | label text | `BUTTON`, as an ordinary button |
| `DIALOG` (RoundDialog) | System: opaque full-panel modal; message at y 90–200; 120 × 52 buttons at y 214 (primary right, secondary left, or one centred) | text `"message\nprimary[\nsecondary]"` | `VALUE` 1 (primary) or 2 (secondary) |

- **Geometry and the navigation area.** The system places the slider, edge button
  and dialog. Each view has at most one of each, since a second would overlap.
  None of them reaches into the top 60-pixel navigation area of the
  [Exit chrome](system-navigation.md), and all touch targets are at least 44 px.
- **List rows.** List rows are 44 px tall: font line height plus line spacing.
- **Slider hit area.** The slider uses LVGL advanced hit testing plus a 10 px
  click extension. It therefore owns only a 44 px band of its ring within its
  angles, not its 340 × 340 box. Without this, the box lay above the earlier
  list and took every touch on it. The user reported that on 0.3.22, and
  0.3.23 fixes it.
- **Initial visibility.** `options` bit `REP_UI_OPTION_HIDDEN` creates any round
  widget hidden. The example uses it for its dialog.
- **Updates.**
  - `REP_UI_VALUE` (4) sets a list index or a slider value. It is validated
    against the newest accepted item count or range.
  - `TEXT` replaces list items, which selects item 0. It also replaces dialog
    texts, but the number of fields must stay the same.
  - `TEXT` on a slider is refused.
  - `VISIBLE` and `ENABLED` apply to every round widget. Showing a dialog moves it
    above later widgets, and disabling a dialog disables its buttons.
- **Dialog choice.** A choice hides the dialog in the GUI task. The application
  shows it again with `VISIBLE 1`.

## Events

`REP3_EVENT_VALUE` (4) carries `value` = widget handle and `reserved` = the
committed value as int32 bits. It does not carry an origin; the bridge counts
physical and injected input separately.

- **Coalescing.** List and slider events describe state. An undelivered event of
  the same widget takes the newer value in place: same queue position, original
  post time. A drag therefore occupies one mailbox entry. Dialog choices are
  actions and are never coalesced.
- **Stale rejection.** At delivery, a list or slider event whose value no longer
  equals the committed value is dropped as stale and counted as rejected. This
  happens when a later input or an applied application request superseded it.
  A dialog choice is delivered while the dialog is enabled, even though it is
  already hidden.
- **Delivery conditions.** Hidden, disabled, unrealized, background and other
  instances' widgets produce no input and no delivery, as for buttons.

## Shadow state (REP §12.6)

| Question | Answer |
|---|---|
| Owner | The UI bridge, per logical widget, under its spinlock. Applications never read LVGL |
| `committed` | The value realized in GUI order: creation, an applied `VALUE` (or list `TEXT`, which commits 0), or user input |
| `requested` | The newest accepted application value. It equals `committed` when nothing is pending |
| `pending` | 1 while an accepted creation, `VALUE` or list `TEXT` is not yet applied |
| Getter | `ui_result` version 2 with `widget` set. It copies and never waits. Version 1 and `widget = 0` return only the counters |
| Conflict | Input during a pending request updates `committed` only. The request, applied later, wins |
| Errors | A failed GUI operation sets the existing sticky error and the runtime tears the application down. There is no partially valid shadow |

Shadow reads are available for lists, sliders and dialogs (last choice). For other
types they return −1.

## Diagnostics

RP1/RE1 `UI VALUE <generation> <widget> <[-]value>` stores one pending
injection. The GUI task validates it, sets the LVGL value as touch would, and
reports it as INJECTED through the same path. For a dialog it simulates a choice.
`tools/roundie_ui.py value` sends it. The status array gains `values` at its end,
and the tool accepts both the old and the new length.

## Costs

- **Command FIFO:** a copied command grows from 160 to 176 B. The 12-entry FIFO
  is therefore 2,112 B (the size is asserted).
- **Bridge slots:** each slot gains six 32-bit fields.
- **LVGL memory:** the round widgets of the host test hold 4,928 B of the LVGL
  pool in 11 objects. On the device, the example (7 widgets, 13 objects) holds
  3,432 B, with a peak of 3,500 B. Both are within the existing 12 KiB creation
  admission and 16 KiB ceiling.
- **List options:** `lv_roller_set_options` copies the items into the LVGL pool.

## Verification

- **Host, verified 2026-11-03.** `tests/test_app_round.c` runs under ASan/UBSan
  with the production bridge, renderer, manager and pinned LVGL 9.5.0, plus the
  example's C callbacks. The loader, IDF and time are mocked. It covers:
  - validation and round geometry;
  - the per-view limits;
  - shadow transitions, including input during a pending request;
  - stale-event rejection and coalescing;
  - dialog self-hide, with a dropped choice leaving it open;
  - injection, including out-of-range rejection without touching LVGL;
  - the disabled state;
  - the capability gate for version-2 requests and results;
  - cleanup.

  All 85 host tests passed.
- **Target build.** The example builds with GCC 15.2.0 to 4,440 B, imports only
  `rep_abi_v3` and has the same relocation types as the ABI 3 UI example.
  Firmware 0.3.22 (REP profile) is 789,120 B, SHA-256 `12b9c177…cbf1b3`, with no
  project warnings.
- **Device, 2026-11-03, under XIP (OTA authorized by the user):**
  - **Install:** 0.3.22 runs from `ota_1`, VALID, self-test passed; 0.3.21 stays
    VALID in `ota_0`. Package `org.roundie.round` 1.0.0 (4,664 B, SHA-256
    `cff1e2c7…77854`) installed and launched: 7 widgets applied, the 2 deliberate
    misfits refused, 0 failures.
  - **Injection:** one run of list index 2, slider 77, edge button, then dialog
    primary. All 4 inputs were accepted and delivered, with 5 resulting
    application updates applied. Slider 500 (out of range) was rejected. Maximum
    input latency 9.3 ms, GUI error 0.
  - **User, 0.3.22:** the list neither scrolled nor reacted to taps, because the
    slider's box captured them (above). The host test now routes points through
    `lv_indev_search_obj`. It failed on the old code and passes with the fix.
  - **0.3.23:** OTA after exiting the application (OTA is refused while an
    application holds the Flash exclusion). It runs from `ota_0`, VALID, with
    SHA-256 `ca41cb3b…f4eb0`; 0.3.22 stays VALID in `ota_1`. The application
    relaunched with 7 widgets and 0 failures, and a list injection was delivered.
  - **Pending, user:** finger drag on the arc and the list, the visual layout on the
    round panel, and dialog taps. Injection does not prove CST816S touch.

Evidence is in `roundie-backups/round-ui-0.3.22-2026-11-03/`, outside Git.
