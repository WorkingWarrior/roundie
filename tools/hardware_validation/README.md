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

## Endurance (M8.1)

`endurance.py` extends this runner for long unattended runs. It reuses the
discovery, preflight, serial monitor, reset detection and restore of
`run_m8.py`, and needs firmware 0.3.55 or later for `RS1 HEALTH` (task count,
stack high-water marks, minimum heap, failed allocations). On older firmware
the baseline is BLOCKED and those checks are UNTESTED.

```bash
python3 tools/hardware_validation/endurance.py --level smoke --serial 28:84:85:87:98:7C --board 1.85b --out <dir>
python3 tools/hardware_validation/endurance.py --level stress --cycles 100 --ui-rounds 30 --serial … --out <dir>
```

| Level | What runs |
|---|---|
| `smoke` | baseline, 2 lifecycle cycles, 2 UI rounds, recovery, 3 min idle with RTC edges |
| `stress` | baseline, `--cycles` lifecycle cycles of six applications, `--ui-rounds` round UI rounds, recovery |
| `soak` | baseline, `--hours` idle on the home application with telemetry every `--interval` s, an RTC second edge every `--rtc-every` s and `--batch-cycles` lifecycle cycles every `--batch-every` s |

- Installed packages start through the launcher, which writes no Flash. The
  event example is staged once per run through RE1. The bad-package check
  writes one more file to `/storage/dev`.
- The RTC is only read (`RT1 DIAG`); drift comes from second-edge captures
  against the host `CLOCK_MONOTONIC`, `CLOCK_MONOTONIC_RAW`, `CLOCK_REALTIME`
  and the device's own monotonic clock.
- `--restart` adds one software restart to the recovery step.
- `--max-hours` and `--max-log-mb` stop the run at the next safe point.
- SIGTERM or Ctrl-C keeps a partial report.
- For runs that should outlive the terminal, start it as a user unit:

  ```bash
  systemd-run --user --unit=roundie-soak --collect python3 tools/hardware_validation/endurance.py --level soak …
  systemctl --user stop roundie-soak
  ```

The PASS/FAIL criteria are fixed in `endurance.CRITERIA` and copied into every
report. A criterion without data is UNTESTED, never PASS. Host tests:
`python3 -m unittest tests.test_endurance`.

## E0 baseline (`run_e0.py`)

Collects the E0 baseline of one board without touch or manual checks
(plan: `docs/baseline/e0-plan.md`, results: `docs/baseline/e0-<board>.md`).
It needs a development (`rep`) image with `RP1 FSBENCH` and `RP1 HOME`.

```bash
python3 tools/hardware_validation/run_e0.py --board 1.85b --serial <MAC> --light-sleep
```

- `--board` is required, because the board is not detected from the RTC.
- Scenarios in order: `home`, `stopwatch`, `after_close`, `full_refresh`, `littlefs` and `light_sleep`. Run a subset with `--only`.
- During the scenarios the screen is held on with `RW1 SCREEN ON` and a diagnostics-owned DISPLAY_AWAKE inhibitor. The runner releases the inhibitor before light sleep and at the end.
- `--ota <bin>` installs an image first and checks the rollback state. `--elf <elf>` compares the running image with a local build.
- `littlefs` writes `/storage/dev/e0-{64,512,1024}.bin`, reads each one `--fs-repeats` times with SHA-256, and deletes it. It needs 1 MiB + 128 KiB free.
- `light_sleep` runs only with `--light-sleep`. USB is expected to drop while the board sleeps. The USB return time is not the wake moment, so the wake time stays PENDING.
- Current measurements and manual checks are always PENDING.
- Exit codes: 0 PASS, 1 FAIL, 2 BLOCKED, 130 interrupted.

Host tests: `python3 -m unittest tests.test_run_e0`.
