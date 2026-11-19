# Unattended hardware validation (M8)

`run_m8.py` checks a Roundie board over USB without touch, button presses,
cable changes or a manual restart. It uses only the board's existing
protocols (RO1, RE1, RP1, RW1, RT1, RS1) and writes its report locally; it
sends nothing anywhere else. Results and their meaning are in
[docs/hardware-validation-m8.md](../../docs/hardware-validation-m8.md).

## What it needs

- A board running a REP firmware image (`ROUNDIE_PROFILE=rep`), connected by
  USB. It is found by its USB serial number under `/dev/serial/by-id`, never
  by `ttyACM` order.
- The example applications built for the ESP32-S3 in one directory
  (default `build-ci/m8-apps`): `hello`, `event`, `ui`, `round`, `watch` and
  `stopwatch`, each as `<name>.app.elf`. Build them as described in
  [docs/native-apps.md](../../docs/native-apps.md) and copy or link each
  `*.elf` to that name.
- Python 3 and `pyserial`, as for the other tools in `tools/`.

## Running

```bash
python3 tools/hardware_validation/run_m8.py --serial <MAC> --board 1.85b
```

- `--board 1.85b|1.75c` is checked against RTC presence (the 1.75C has no
  PCF85063).
- `--only` / `--skip` select steps.
- `--no-rtc-sync` leaves the board's time as it is. Otherwise the RTC step
  sets it from the host clock; the time zone is always restored.
- `--no-restart` skips the software restart.
- `--light-sleep` allows one timed light sleep (timer wake only). It is off
  by default, because a board whose timer wake is not verified could need a
  person to wake it.
- `--ota <roundie_rep.bin>` installs an image first through RO1. It runs only
  if the preflight passes and the other slot holds a VALID rollback image.
  The bootloader, partition table, NVS and LittleFS are never written.

The report goes to `build-ci/hardware-validation/<UTC>/` (or `--out`):
`report.json`, `report.md` and `serial.log`, rewritten after every step, so
an interrupted run keeps its partial results.

Exit codes: 0 PASS, 1 FAIL, 2 BLOCKED, 130 interrupted.

## Statuses

| Status | Meaning |
|---|---|
| PASS | Checked on the board through its protocols |
| FAIL | Checked on the board and wrong, or the board faulted |
| BLOCKED | Not run, because a precondition failed (Safe Mode, no rollback image, an earlier fault, a missing file) |
| PENDING | Needs a person or instrument: finger touch, what the panel shows, audio, BOOT, touch wake, current, battery |

UI input in `launcher` and `round_dialog` is **injected** through the UI
bridge, not finger touch. A PASS there says nothing about the touch panel.

## Safety

- The runner never erases Flash, never uses ROM Download, never writes the
  bootloader, partition table or eFuses, and never enters deep sleep.
- A serial panic, watchdog or unexpected reset fails its step and blocks the
  rest. The runner does not leave Safe Mode by itself; that is an operator
  decision (`tools/roundie_safe.py reboot`, see
  [docs/safe-mode.md](../../docs/safe-mode.md)).

## 1.75C

The same command with `--board 1.75c` and the 1.75C REP image. The board
reports no RTC, so the RTC step checks only the system clock against the host
(`roundie_time.require_valid`). Light sleep stays off unless `--light-sleep`
is given. This kit has not been run on a 1.75C yet.

## Host tests

```bash
python3 -m unittest tests.test_run_m8
```

The tests use a fake device and clock and are part of the CI `host` group.
