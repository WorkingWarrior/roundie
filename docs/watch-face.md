# Watch face: local time, clock hands, fonts and colors

## Decisions, 2026-11-03

The user chose a watch face as the next SDK example, ahead of the stopwatch. It
replaces the calculator named in REP §23 stage 6. That required time of day for
applications, a time zone, larger text, colors and an analog dial. All of these
extend ABI 3 in the same way as the [Round UI extension](round-ui.md): new
capability bits and versioned requests, with the 52-byte API table unchanged.

### Time zone

- **Rule.** The system clock stays UTC. A POSIX TZ rule, for example
  `CET-1CEST,M3.5.0,M10.5.0/3`, provides the local offset and daylight saving
  time.
- **Storage.** The rule is stored in NVS namespace `clock`, key `tz`. Without a
  stored rule, the default is `UTC0`.
- **Locking.** newlib's TZ state is global. `setenv`/`tzset` and `localtime_r`
  are called only under one mutex in `main/local_time.c`, and nothing else in
  the firmware uses local time.
- **Command.** RT1 `TZ [rule]` sets the rule, persists it, applies it, and
  replies `!RT1 <id> <result> zone=<active rule>`.
  - The rule is checked for length (1–47 bytes) and characters.
  - Its meaning is newlib's interpretation.
  - An NVS failure leaves the previous rule active.
- **Tool.** `tools/roundie_time.py --port P tz` sends the rule from the TZif v2+
  footer of the host's `/etc/localtime`. `--rule` sends a given rule, and
  `--query` only reports the active one.

### Clock events

The `REP3_CAP_CLOCK` capability (64) gives applications time as an event, never
as a call. `REP3_EVENT_CLOCK` (5) carries:

| Field | Meaning |
|---|---|
| `value` | Local days since 1970-01-01 |
| `reserved` | Local milliseconds since midnight (below 86,401,000) |
| `posted_us` | Monotonic microseconds at which the two held |

- **Sampling.** The executor samples `roundie_local_time_now` at most once per
  second, outside native callbacks.
- **When it posts.** It posts after start, at each local minute, and when the
  revision changes. The revision is the zone revision plus the RTC sync count,
  so a zone change or a sync is delivered within about a second.
- **Invalid wall time.** Nothing is sent while wall time is invalid (no
  successful RTC sync this boot).
- **Coalescing.** A queued clock event is replaced in place by a newer sample,
  including its time.
- **Extrapolation.** Applications extrapolate from `posted_us` of later events
  (for example their timer) or from `monotonic_ms`.
- **Date helper.** `rep_clock_civil()` in the header converts days to a date and
  weekday with 32-bit integer arithmetic. It needs no libgcc helper on Xtensa.

### UI additions (require `REP3_CAP_ROUND_UI`)

- **Clock hands.** `REP_UI_CLOCK_HANDS` (8) uses system geometry: a 340 px dial
  at (10, 10), the same as the ArcSlider. It has 60 ticks, with every fifth one
  major, and hour, minute and red second hands.
  - `REP_UI_OPTION_NO_SECONDS` removes the second hand.
  - `value` is the seconds of 12 hours (0–43,199), set by `REP_UI_VALUE`, and has
    shadow state.
  - The dial is display only: not clickable, so touches reach widgets below, and
    injection is refused. Each view has at most one.
- **Styled labels.** A version-2 `LABEL` takes `REP_UI_OPTION_FONT_MEDIUM`
  (Montserrat 28) or `REP_UI_OPTION_FONT_LARGE` (Montserrat 48), plus
  `REP_UI_OPTION_HIDDEN`. Both fonts are enabled in the REP profile.
- **Colors.** `REP_UI_COLOR` (5) takes a value of `0xRRGGBB` or below:

  | Widget | What the color sets |
  |---|---|
  | View | Background (the only update a view accepts) |
  | Label | Text |
  | Button, edge button | Background |
  | List | Selected row |
  | Slider | Indicator and knob |
  | Dialog | Buttons |
  | Clock hands | Major ticks, hour and minute hands |

- **Capability gate.** `VALUE` and `COLOR` updates are refused for applications
  without `REP3_CAP_ROUND_UI`.

## The example: `examples/watch_elf`

- **Widgets:** a view, the dial, a 28 px date (`TUE 3 NOV`), a 48 px 24-hour time
  and a **Style** edge button. Five widgets in all.
- **Updates:** a one-second timer moves the hands with one `VALUE` per second.
  The time and date text change only when the minute or the day changes.
- **No time yet:** before the first clock event, it shows `--:--` and
  `Sync time`.
- **Themes:** **Style** cycles terminal green, blue and amber through `COLOR`
  updates.
- **Size:** 4,588 B. It imports only `rep_abi_v3`, with the same relocation types
  as the other examples.

## Verification

- **Host, verified 2026-11-03.** All tests run under ASan/UBSan with pinned
  LVGL 9.5.0. The loader, IDF and time are mocked. All 85 host tests passed.
  - **RT1:** `TZ` parsing and rule bounds, and `days_from_civil` for known dates.
  - **Widgets:** dial, styled label and color validation; the minute-needle
    geometry at 15 and 45 minutes; shadow state; dial pass-through; colors on
    LVGL objects.
  - **Runtime with the watch example's C callbacks:**
    - no event while the source is invalid;
    - sampling at most once per second;
    - first event text `12:34` / `TUE 3 NOV`, with the hands at 2096 s;
    - timer extrapolation;
    - a zone revision posted promptly;
    - coalescing to the newest sample across midnight;
    - the theme button;
    - clean unload.
- **Target build.** 0.3.24 (REP profile) is 934,336 B, SHA-256 `919dce1b…3d7d48`.
  The two fonts add about 145 KB. There are no project warnings.
- **Device, 2026-11-03.**
  - **Install:** OTA after leaving the launcher list. 0.3.24 runs from `ota_1`,
    VALID, self-test passed; 0.3.23 stays VALID in `ota_0`.
  - **Time:** the RTC reported OS=1 (invalid) before sync. `roundie_time.py sync`
    succeeded (−1.16 s against the host), then `tz` set
    `CET-1CEST,M3.5.0,M10.5.0/3`, confirmed by `--query`.
  - **Watch:** package `org.roundie.watch` 1.0.0 (SHA-256 `5afa1bf9…d64dc6`) was
    installed and launched with 5 widgets, 0 failures, 2,448 B in LVGL and 9
    objects. Over 10 s, 10 more updates were applied, with 0 failures, 0 pending
    and a maximum single update of 842 µs.
- **Pending, user:** that the displayed local time and date are correct, how the
  face looks, and the Style button by touch.
- **Not tested:** RTC loss with the time zone kept across a restart, and a DST
  transition on the device.

Evidence is in `roundie-backups/watch-0.3.24-2026-11-03/`, outside Git.
