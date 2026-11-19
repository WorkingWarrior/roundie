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
  revision changes. The revision is the zone revision plus the wall-clock sync count,
  so a zone change or a sync is delivered within about a second.
- **Invalid wall time.** Nothing is sent while wall time is invalid (no
  trusted sync; see [boards without an external RTC](rtc-time-sync.md#boards-without-an-external-rtc)).
- **Scope.** Only an ABI 3 application started with `REP3_CAP_CLOCK` receives
  clock events. The subscription is cleared whenever any application starts,
  so an ABI 2 application never inherits it from the previous one (fixed in
  0.3.54; see [M8 validation](hardware-validation-m8.md#clock-subscription-leaked-into-abi-2-applications-fixed-in-0354)).
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
  - The dial is display only by default: not clickable, so touches reach widgets
    below, and injection is refused. Each view has at most one.
  - `REP_UI_OPTION_LONG_PRESS` (16, dial only), added 2026-11-03: the dial's
    340 × 340 box becomes clickable, and a long press (LVGL's
    `LONG_PRESSED`, 400 ms by default) sends `REP3_EVENT_BUTTON` with the dial's
    handle. It covers widgets created before the dial; later, non-clickable
    labels pass touches to it. RP1 `UI VALUE g <dial> 0` injects a long press.
  - `REP_UI_OPTION_TAP` (64, dial only) does the same for a tap (LVGL
    `CLICKED`). It cannot be combined with `LONG_PRESS`, because the event would
    not say which one happened.
  - `REP_UI_OPTION_SECOND_RING` (32, dial only, not with `NO_SECONDS`) draws
    no ticks or hands. Instead, a 6 px ring around the face fills from 12
    o'clock to the current second of the minute (`value % 60`), and `COLOR`
    sets the fill. A first version lit scale ticks via an LVGL scale section.
    On the device, every second change then redrew the whole face (187 ms per
    frame), and each 0.1 s text update cost 42 ms. With an `lv_arc` only the
    changed part is redrawn: 18 ms and 15 ms (0.3.28).
- **Styled labels.** A version-2 `LABEL` takes `REP_UI_OPTION_FONT_MEDIUM`
  (28 px) or `REP_UI_OPTION_FONT_LARGE` (48 px), plus `REP_UI_OPTION_HIDDEN`.
  The fonts are described below.
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

## Fonts with Polish letters, 2026-11-03

LVGL 9.5.0's built-in Montserrat fonts cover ASCII, `°`, `•` and the
FontAwesome symbols only, so `Ś` or `Ź` did not render. The user asked for a
Polish watch face and accepted Google Sans instead of Montserrat.

- **Source.** Google Sans Flex, the variable font released by Google Fonts under
  the SIL Open Font License 1.1, with no Reserved Font Name. The classic Google
  Sans is not openly licensed and is not used. The source TTF is pinned by
  SHA-256 in `tools/generate_fonts.py` (google/fonts commit `bd8f81dd`) and is not
  stored in Git. The license is in `components/roundie_fonts/OFL.txt`. "Google
  Sans" is a Google trademark, so the derived fonts are named `roundie_font_*`.
- **Generation.** `tools/generate_fonts.py` makes one static instance per size
  with fontTools: weight 500, optical size equal to the pixel size, width 100, no
  rounding or slant. lv_font_conv 1.5.2 then renders it at 4 bpp without
  compression, with LVGL's own options and FontAwesome symbol list, so
  `LV_SYMBOL_*` keep working. It is followed by the LVGL 9 post-processing that
  LVGL applies to its own fonts. The same converter reproduced the bitmaps,
  glyph table, cmaps and kerning of LVGL's built-in Montserrat 14.
- **Tabular digits (added 2026-11-03 for the stopwatch).** The generator gives
  0–9 the widest digit's advance, centres each digit in it and removes the
  digits' kerning classes, so a changing number keeps its width.
- **Coverage.** Each font has ASCII, `°`, `•`, all 18 Polish letters
  (`ĄĆĘŁŃÓŚŹŻąćęłńóśźż`) and the symbols. Other scripts are absent.
- **Sizes.** 14 px (line height 18) is LVGL's default font in every component.
  The top-level `CMakeLists.txt` sets `LV_FONT_DEFAULT` and
  `LV_FONT_CUSTOM_DECLARE` as global compile definitions, because LVGL's Kconfig
  cannot name a custom default. 20 px is for button labels. 28 px (line 34) and
  48 px (line 58) serve the styled labels. Montserrat 28 and 48 are no longer enabled. List rows stay 44 px,
  because their spacing is computed from the line height.
- **Cost.** The three fonts take 164,037 B of Flash (linker map, 0.3.25). Firmware
  0.3.25 is 17,376 B larger than 0.3.24, which had Montserrat 14, 28 and 48.

## The example: `examples/watch_elf`

- **Widgets:** a view, the dial, a 28 px date in Polish (`WT 3 LIS`) and a 48 px
  24-hour time. Four widgets in all, with no buttons on the face: the user found
  the **Styl** and Exit buttons out of place on a watch (2026-11-03). Exit is now
  the system's [edge swipe](system-navigation.md).
- **Language:** Polish weekday abbreviations (`ND PN WT ŚR CZW PT SOB`) and
  months (`STY LUT MAR KWI MAJ CZE LIP SIE WRZ PAŹ LIS GRU`), stored as UTF-8.
- **Updates:** a one-second timer moves the hands with one `VALUE` per second.
  The time and date text change only when the minute or the day changes.
- **No time yet:** before the first clock event, it shows `--:--` and
  `Ustaw czas`.
- **Themes:** a long press on the dial cycles terminal green, blue and amber
  through `COLOR` updates.
- **Size:** 4,356 B (1.2.0). It imports only `rep_abi_v3`, with the same relocation types
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
    - first event text `12:34` / `WT 3 LIS`, with the hands at 2096 s;
    - timer extrapolation;
    - a zone revision posted promptly;
    - coalescing to the newest sample across midnight;
    - the theme button;
    - clean unload.
  - **Fonts, 0.3.25:** the Polish letters, ASCII and `LV_SYMBOL_OK` exist in all
    three fonts, Cyrillic does not, and the default font is `roundie_font_14`.
    All 85 host tests passed.
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
- **User-reported, 0.3.24:** the face shows the correct local time and date and
  looks right. The Style button was not reported separately, and the number of
  checks was not recorded.
- **Device, 0.3.25 (Polish fonts), 2026-11-03.**
  - **Firmware:** 951,712 B, SHA-256 `2fb253ed…ff82d722`, no project warnings.
    The image contains `roundie_font_14/28/48` and no Montserrat font.
  - **Install:** OTA after leaving the watch. 0.3.25 runs from `ota_0`, VALID,
    self-test passed; 0.3.24 stays VALID in `ota_1`.
  - **Time zone:** the rule and a valid RTC (OS=0) survived the OTA restart.
  - **Watch:** `org.roundie.watch` 1.1.0 (SHA-256 `2d4430db…a76199f`) was
    installed while the old watch was closed. It launched with 5 widgets,
    18 updates applied in 4 s, 0 failures and 9 objects.
  - **User-reported, 0.3.25:** the Polish text and the new font look good on
    the panel. Individual letters were not checked one by one.
- **Device, 0.3.26 (edge swipe, long press), 2026-11-03.**
  - **Firmware:** 952,672 B, SHA-256 `44f9c4d6…8892934d`, no project warnings.
    OTA after RP1 EXIT; runs from `ota_1`, VALID, self-test passed; 0.3.25 stays
    VALID in `ota_0`.
  - **Watch:** 1.2.0 (SHA-256 `27ef974b…2c220dfd46`) launched with 4 widgets,
    7 objects and 0 failures.
  - **Long press, injected:** on the dial it was accepted and the theme updates
    applied with 0 failures. On the time label it was rejected.
  - **User-reported, 0.3.26:** a finger long press cycles the theme and the
    edge swipe closes the watch. The number of attempts was not recorded.
  - **Host:** a long press cycles the theme; a short tap does not; points over
    both labels reach the dial; a dial without the option and a label with it
    are refused. All 85 host tests passed.
- **Not tested:** RTC loss with the time zone kept, and a DST transition on the
  device.

Evidence is in `roundie-backups/watch-0.3.24-2026-11-03/`, outside Git.
