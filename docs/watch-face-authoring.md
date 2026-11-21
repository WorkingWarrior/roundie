# Authoring a watch face

A face is an XML file compiled into an RWF1 package by
`tools/roundie_face.py`. The firmware interprets the compiled definition with
its declarative backend; no code from the package runs. Format and limits:
[watch-face-packages.md](watch-face-packages.md). Lifecycle:
[watch-face-abi.md](watch-face-abi.md).

## Quick start

```bash
python3 tools/roundie_face.py build faces/minimal_digital/face.xml --output minimal.rwf
```

```bash
python3 tools/roundie_face.py inspect minimal.rwf
```

On the desktop (no board needed):

```bash
SDL_AUDIO_DRIVER=dummy build-desktop/roundie-desktop --board 1.85b --install-face minimal.rwf
```

Long-press the face to open the gallery, choose with the arrows, **Ustaw**.
On a board with the development transport: `upload --install`, then `select`
(see the packages document).

## The face element

```xml
<face id="org.example.face.simple" name="Prosta" version="1.0.0" background="#000000">
  ...
</face>
```

- `id`: lower-case dotted, at least two parts, ≤ 47 bytes; it is also the
  file name on the watch. `name`: shown in the gallery, ≤ 31 bytes UTF-8.
- `version`: `major.minor.patch`. A reinstall must raise it (except to
  repair a corrupt copy).
- The canvas is the round 360 × 360 panel. Keep content inside the circle;
  text near the edge is cut off. Touch targets should be at least 48 px.
- No DOCTYPE or entities (refused).

## Elements

Positions are in panel pixels; children of a `rect` are relative to it.
Colors are `#rrggbb`. Every element takes `opacity` (0–255), `when` and,
for touch, `action` (see below).

| Element | Attributes |
|---|---|
| `label` | `x y w`, `align` left/center/right, `font` 14/20/28/48, `color`, `text` (template) |
| `rect` | `x y w h`, `radius` (px or `circle`), `color`, `border`, `border-color`; may contain other elements (depth ≤ 4) |
| `arc` | `x y size width`, `bind` battery/second/minute/hour, `color`, `track` |
| `image` | `resource`, `x y`, `color` (tints A8 images) |
| `hand` | `kind` hour/minute/second, `cx cy` (default 180), `length width tail`, `color` |
| `ticks` | `count`, `cx cy radius length`, `major` (length of every fifth), `color`, `major-color` |

`setting` declares a per-face option: `<setting key="ring" options="2"
default="1"/>` (≤ 4 settings, ≤ 8 options). `resource` declares an image:
`<resource name="icon" src="icon.pgm" preview="true"/>` — binary PGM (P5,
alpha mask, tinted) or PPM (P6, RGB565), up to 360 × 360, 48 KiB in all.

## Text templates

| Token | Meaning |
|---|---|
| `{HH}` `{H}` | hour 00–23 / 0–23 |
| `{hh}` `{h}` | hour 01–12 / 1–12 |
| `{MM}` `{SS}` | minutes, seconds |
| `{DD}` `{D}` `{MO}` `{YYYY}` | day, month number, year |
| `{MON}` `{MONTH}` | sty…gru / stycznia…grudnia |
| `{WD}` `{WEEKDAY}` | pon…nd / poniedziałek…niedziela |
| `{UTC}` `{OFF}` `{TZ}` | UTC hh:mm, offset +hh:mm, zone rule |
| `{BAT}` `{CHG}` | battery percent (`--` without a reading), "ładowanie" |

Without a trusted time every time token shows `--`; show a hint with
`when="time-invalid"`. `{{` is a literal brace; an unknown token or a stray
`}` is refused by the compiler and again by the watch.

## Conditions and actions

`when`: `always`, `time-valid`, `time-invalid`, `battery-valid`, `charging`,
`not-charging`, `setting:KEY=N`, `setting:KEY!=N`.

`action` on an element makes it a tap target: `cycle` with `setting="KEY"`
moves to the next option (persisted per face); `refresh` asks for a redraw.

## Update cost

The compiler derives the face's needs: a seconds token, a `second` hand or a
`second` arc subscribes to second updates; otherwise the face is redrawn on
minute boundaries only. Battery tokens and bindings subscribe to battery
changes. Use second updates only where they are visible: they cost a redraw
every second while the screen is on.

## Examples

`faces/` holds the three demo faces, each a complete reference:

- `classic_analog` — dial ticks, three hands, date window; tap the centre to
  hide the second hand (a setting); preview image.
- `minimal_digital` — large time, weekday and date; a seconds ring toggled by
  tapping the time.
- `information` — time, seconds, date, UTC time and offset, zone rule,
  battery arc, icon and percent, charging note.

## Testing a face

`tools/roundie_face.py build` checks the source. Then run the desktop with
`--install-face` and look at it with and without a set time (`rt1 SET` in a
script, see [virtual-hardware.md](virtual-hardware.md)), with the battery
present and absent, and while charging.
