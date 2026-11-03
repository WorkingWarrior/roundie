# Stopwatch example

## Decisions, 2026-11-03

After the watch face, the user chose a stopwatch as the next application. It is
a second SDK example built only from existing ABI 3 and Round UI facilities, so
there is no ABI or firmware change.

- **Time source.** Elapsed time is `monotonic_ms`, not wall time, so an RTC sync
  or a time-zone change does not move it. It is kept as 32-bit milliseconds, so it
  wraps after about 49 days. All arithmetic is 32-bit after one 64-bit
  subtraction, so the ELF imports only `rep_abi_v3`.
- **Updates.** A 100 ms timer runs only while the stopwatch runs. Each tick
  updates the time text. The dial gets one `VALUE` per new second. Stop cancels
  the timer, so a stopped stopwatch sends nothing.
- **Language.** Polish, like the watch face.

## Layout

| Widget | Geometry | Behavior |
|---|---|---|
| Dial (`CLOCK_HANDS`) | System | Chronograph: the hands show elapsed seconds, minutes and hours |
| Time (48 px label) | 40, 150, 280 × 60 | `MM:SS.d`, or `H:MM:SS.d` from one hour on |
| Laps (14 px label) | 70, 214, 220 × 58 | The last three laps, newest first: `3.  00:12.4` |
| Top button | 110, 76, 140 × 48 | **Okrążenie** while running, **Zeruj** when stopped with time, disabled at zero |
| Edge button | System | **Start** (green) / **Stop** (red) |

Six widgets including the view. Resume uses **Start**. A lap is the time since the
previous lap. The lap count keeps growing, and only the last three are shown.

## Verification

- **Host, 2026-11-03.** `stopwatch_runtime_tests` in `tests/test_app_round.c`
  runs the example's C callbacks with the production manager, bridge and LVGL
  9.5.0. It covers:
  - the initial state;
  - start with the timer and colors;
  - time advancing;
  - two laps in order;
  - stop cancelling the timer and holding the time;
  - reset;
  - the hour format and the dial value past one hour;
  - clean unload.

  All 85 host tests passed.
- **Target build.** 4,540 B with GCC 15.2.0. It imports only `rep_abi_v3`
  (one `JMP_SLOT`; otherwise `RELATIVE` and `RTLD` relocations).
- **Device, 0.3.26.** Package `org.roundie.stopwatch` 1.0.0 (SHA-256
  `d6402807…fe18af0f`) installed and launched: 6 widgets, 11 objects, 0
  failures.
  - Injected start, lap and stop were all accepted.
  - While running, updates were applied at about 11 per second. The longest
    single update was 745 µs, and the longest queue latency 11.3 ms.
  - After stop the applied count held.
  - The deepest queue was 10 of 12, during creation.
- **User-reported, 2026-11-03:** the stopwatch works with a finger and looks
  right. Individual controls and the number of attempts were not recorded.

Evidence is in `roundie-backups/stopwatch-1.0.0-2026-11-03/`, outside Git.
