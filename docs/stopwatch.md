# Stopwatch example

## Decisions, 2026-11-03

After the watch face, the user chose a stopwatch as the next application. It is
built only from ABI 3 and Round UI facilities. Version 1.1 added two dial
options and changed the system button look (see below).

- **Time source.** Elapsed time comes from `monotonic_ms`, not wall time, so an RTC sync or
  time-zone change does not move it.
  - It is kept as 32-bit milliseconds, which wrap after about 49 days.
  - All arithmetic is 32-bit after one 64-bit subtraction, so the ELF imports
    only `rep_abi_v3`.
- **Updates.**
  - A 100 ms timer runs only while the stopwatch runs.
  - Each tick updates the time text, and each new second updates the dial's
    ring. Stop cancels the timer.
- **Controls, chosen by the user after 1.0.**
  - A **tap anywhere on the face** starts and stops.
  - The lap button stays at the top, above the time, so the lap list below can
    grow without being covered.
- **Language.** Polish, like the watch face.

## Layout (1.1.0)

| Widget | Geometry | Behavior |
|---|---|---|
| Dial (`CLOCK_HANDS`, `SECOND_RING` and `TAP`) | System | Ring fills in orange to the current second. A tap starts or stops |
| Time (48 px label) | 40, 136, 280 × 60 | `MM:SS.d`, or `H:MM:SS.d` from one hour on. Amber while stopped with time on it |
| Laps (14 px label, gray) | 70, 204, 220 × 96 | Last five laps, newest first: `6.  00:12.4`. At zero: `Dotknij, aby uruchomić` |
| Lap button | 110, 72, 140 × 48 | **Okrążenie** while running, **Zeruj** (red) when stopped with time, disabled at zero |

- **Widgets and background.** Five widgets including the view, on a black
  background.
- **Laps.** A lap is the time since the previous lap.
- **Startup.** Startup queues 5 creations and 6 updates, within the 12-entry UI
  queue. An earlier draft with 13 failed to start in the host test.
- **History.** 1.0.0 had a Start/Stop edge button, the lap button on top and
  hour/minute/second hands. The user said the buttons were ugly.

## Verification

- **Host, 2026-11-03.** `stopwatch_runtime_tests` in `tests/test_app_round.c`
  runs the example's C callbacks with the production manager, bridge and LVGL
  9.5.0. It covers:
  - the initial state and the hint;
  - hit testing: points over the time and laps reach the dial, while the
    button keeps its own area;
  - a long press does nothing;
  - a tap starts the timer;
  - the ring value;
  - six laps with five shown;
  - a tap stops (amber time, `Zeruj`, timer cancelled, time held);
  - reset;
  - the hour format;
  - an injected dial tap;
  - clean unload.

  The dial tests also refuse `TAP` with `LONG_PRESS` and `SECOND_RING` with
  `NO_SECONDS`. All 85 host tests passed.
- **Target build.** 4,448 B. It imports only `rep_abi_v3`.
- **Device, 0.3.28** (`ota_1`, VALID, SHA-256 `4611fce5…c748a38`).
  - **Install:** `org.roundie.stopwatch` 1.1.0 (SHA-256 `a267eb8c…286a3ee6`), 5
    widgets, 7 objects, 0 failures.
  - **Injection:** a dial tap, a lap, then a tap to stop were accepted with 0
    rejected.
  - **Running:** each 0.1 s frame rendered 27,104 px in about 15 ms; second
    changes took 18 ms. The longest queue latency was 11.9 ms, and input
    latency 9.3 ms.
  - **Superseded measurement.** 0.3.27 used scale-section ticks and measured
    42 ms and 187 ms.
  - **Development package.** A temporary measurement package
    (`org.roundie.swexp`) was installed and removed.
- **Device, 1.0.0 on 0.3.26.** The user confirmed it worked.
- **User-reported, 1.1.0 on 0.3.28:** looks good, including the new button
  style. Individual controls and the number of attempts were not recorded.

Evidence is in `roundie-backups/stopwatch-1.0.0-2026-11-03/` and
`stopwatch-1.1.0-2026-11-03/`, outside Git.
