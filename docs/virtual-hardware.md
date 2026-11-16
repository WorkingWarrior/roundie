# Virtual hardware lab (M6)

Roundie Desktop (`docs/roundie-core.md`) runs the real Roundie Core, REP
runtime, launcher and LVGL against an emulated board. The lab adds emulated
peripherals that can be changed while the system runs, from a control panel
or from a script, so that hardware scenarios can be tested without an ESP32.

The peripherals are emulated at the level of the functional API Core uses
(`roundie_board_*`, the clock's PCF85063 and ESP RTC model, the audio
backend, the package manager's file system calls), not at the level of
registers or electrical signals. Nothing here is a claim about how the real
boards behave unless it says so; the battery and power numbers are
assumptions chosen to make scenarios easy to set up.

## Structure

| Part | File | Role |
|---|---|---|
| Model | `platform/linux/vhw.c`, `vhw.h` | Battery, charger, supply, PWR latch, pending resets, peripheral faults. Deterministic integer arithmetic; one mutex; no SDL. Host-tested on its own (`tests/test_vhw.py`, ASan/UBSan and TSan). |
| Commands | `platform/linux/lab.c`, `lab.h` | Text commands that change the model, the clock or the keys. The script and the panel both send lines here. |
| Storage faults | `platform/linux/vhw_storage.c` | `-Wl,--wrap` of the POSIX calls the package manager makes; see Storage. |
| Control panel | `platform/linux/panel.c`, `panel.h` | A view of the model in its own frame and window. A click only produces a command line. |
| Integration | `platform/linux/desktop.c`, `board.c`, `audio.c`, `app_runtime.c`, `clock.c` | Applies the model's actions (power off, power on, reset) and reads the faults where Core calls the board. |

The model does not know about the panel and the panel does not change the
model directly: every panel control is a command line that the GUI thread
applies through `lab_command()`, exactly as a script line. So every operation
of the panel is also available to automated tests.

## Running

```
roundie-desktop --lab-panel [--board 1.85b|1.75c] [--native-apps] [--virtual-time] [--script FILE]
```

`--lab-panel` opens a second window, "Roundie virtual hardware", shown through
the same SDL3 window and Vulkan presentation code as the emulated panel
(`vk_present.c`), drawn with SDL's software renderer and its built-in 8 × 8
debug font. No GUI framework is added. The emulated panel's window, logical
resolution, frame and GUI are unchanged; closing the lab window leaves the
board running. Without `--lab-panel` (and always with `--headless`) the panel
still exists as a frame for `panel-click` and `panel-screenshot`.

The panel shows the board's power state, the cell (state of charge, charge,
current, open-circuit voltage), what the gauge reports, the RTC and USB, and
rows of controls; the active choice of each fault is highlighted.

## Script commands

All of them are also accepted while the board is off or its bring-up stopped,
together with `wait`, `release`, `screenshot` and `quit`.

| Command | Effect |
|---|---|
| `hw` | One status line: `hw powered= off= usb= battery= soc= charge_uah= ocv= current= sample= mv= gauge= rtc_backup= touch= display= audio= storage= limit= cycles= sleeping=` |
| `usb 0\|1` | USB supply. Connecting it switches an off board on. |
| `battery soc N` | State of charge, 0–100 %. |
| `battery present 0\|1` | Cell in or out. Without USB, removing it cuts power. |
| `battery capacity MAH` | Full charge, 1–60000 mAh (keeps the state of charge). |
| `battery load AWAKE [SLEEP]` | Discharge current in mA, awake and in light sleep. |
| `battery charge MA` | Charging current with USB. |
| `gauge-fault none\|timeout\|absent` | The fuel gauge (1.85B BQ27220) or PMIC (1.75C AXP2101) times out or does not answer. |
| `power-cut` / `power-restore` | Disconnect or reconnect all supplies. |
| `key boot\|pwr [MS]` | Press a key for MS (default 100); the script continues after the release. |
| `reboot sw\|panic\|wdt\|brownout\|pin` | Reset the board with that reason. |
| `rtc-fault none\|stop\|bus\|absent` | PCF85063 oscillator stopped, bus timeouts, chip missing. |
| `rtc-backup 0\|1` | Whether the PCF85063 keeps time through a power loss. |
| `display-fault none\|restore\|init` | Re-initialisation after screen off fails; or the start-up initialisation fails. |
| `touch-fault none\|timeout\|noise\|absent` | Touch reads time out, return spurious points, or the controller is missing. |
| `audio-fault none\|unavailable` | The output device fails. |
| `audio-underrun` | Drop what the stream ring holds, once. |
| `storage-fault none\|absent\|io [CALL]` | No storage medium, or I/O errors: of every wrapped call, or with CALL (`open`, `read`, `write`, `fsync`, `mkdir`, `rmdir`, `unlink`, `rename`, `stat`, `opendir`) of that one only; `hw` shows `storage=io:CALL`. |
| `storage-limit KB` | Capacity limit for packages and application data; 0 removes it. |
| `panel-click X Y` | The panel control at frame pixel (X, Y): prints `panel-click COMMAND` and applies it; fails off a control. |
| `panel-screenshot FILE.png` | The panel's frame (360 × 392). |

The existing `reset KIND` command (M3) still restarts only the time
subsystem inside the running process; `reboot` restarts the whole process.

## Models and what they cover

### Battery, charger and PMIC

- Charge is kept in µAh with a sub-µAh residual in mA·µs, integrated on every
  poll from the current; the same inputs give the same numbers.
- Defaults (assumptions): 500 mAh, 80 %, 120 mA awake, 10 mA in light sleep,
  250 mA charging current.
- Open-circuit voltage: a generic Li-ion curve, linear between 0 % 3300 mV,
  10 % 3600 mV, 50 % 3750 mV, 90 % 4100 mV and 100 % 4200 mV; 150 mΩ internal
  resistance gives the voltage under load or charge. Not fitted to either
  board's cell (assumption).
- With USB the cell charges at the charging current until full and the load is
  supplied by USB; without USB it discharges at the load current. At 0 % the
  board loses power (`off=depleted`).
- What Core reads (`roundie_board_battery_read`):
  - 1.85B, BQ27220 profile: voltage, state of charge, current, temperature
    25.0 °C, flags 0x0008 (present) plus 0x0001 while discharging, operation
    status 0x0020 (initialised); state VALID. The firmware's sensor worker
    logs these every 5 s, as on the board.
  - 1.75C, AXP2101 profile: voltage only; state UNCALIBRATED.
  - Gauge timeout: COMM_ERROR with ESP_ERR_TIMEOUT; absent: NOT_FOUND; cell
    out: BATTERY_ABSENT.
- Light sleep from the power menu switches the model to the sleep current.

### Supply, PWR and power loss

- The board runs while a supply is connected: USB, or the cell through the
  PWR latch. Holding PWR for 2 s on battery opens the latch (`off=latch`);
  holding it for 1 s while off closes it. With USB the latch cannot switch
  the board off. These hold times and the latch itself are assumptions; spec
  §25 keeps a programmable PWR button and software power-off open.
- `power-cut` disconnects everything (`off=cut`); `power-restore` reconnects and
  closes the latch.
- Power loss, depletion and resets end the desktop process and start it again
  (`execv` of `/proc/self/exe` with `--resume STATE`): Core, LVGL, the
  launcher, applications and the audio device start from scratch, as after a
  reset of the board. The state file (`lab-state` in the data directory,
  deleted on read) carries the model, the emulated clocks and the script
  position; `--install` and older `--resume` arguments are dropped. While the
  board is off the window stays dark and only the lab acts.
- The state file is written as `lab-state.new` and renamed, and it ends with
  `lab.end 1`. Only the process started with `--resume` reads it, and it
  refuses a file without that last line before applying any of it. A start
  without `--resume` deletes a `lab-state` an earlier process left (one that
  ended between the rename and the restart) and any partial `lab-state.new`,
  and logs it, so a scenario never inherits the machine of an earlier one.
  Each test also runs in its own data directory.
- On power-on the clocks get a power-on reset; without `rtc-backup` the
  PCF85063 also loses its time and sets its oscillator-stop flag.
- Not modelled: brown-out thresholds, voltage-dependent shutdown before 0 %,
  charging termination current, temperature, charger faults, the AXP2101's
  own power-key handling, deep sleep (spec: not a power-off).

### Resets

`reboot` and the power menu's Restart end the process with the reset reason
the clock model and the firmware's time code expect (`start after sw reset`).
The power menu's Restart was refused on the desktop until M6; it is now a
planned software reset. USB storage mode and Safe Mode remain refused with
"Niedostępne na komputerze". The application marker (`app-loaded`, N7) is kept
only across panic and watchdog resets, so a planned reset does not count as an
application fault.

### RTC

The M3 clock model (`platform/linux/clock.c`): `rtc-fault absent` makes the
PCF85063 answer NOT_FOUND to reads and writes (the chip keeps counting; it is
only disconnected). On the 1.75C, which has no PCF85063, the RTC faults have no
effect on time.

### Display

- `display-fault restore`: the re-initialisation after screen off returns
  ESP_ERR_TIMEOUT (logged by the firmware's power code) and the panel's
  memory is lost (filled with magenta). The firmware's full repaint after the
  wake covers it, so the screen comes back the same; a panel that stays dead
  after a failed re-initialisation is not modelled.
- `display-fault init` takes effect at the next start (reboot, power cycle):
  `Bring-up stopped: ESP_ERR_TIMEOUT` as `main/main.c` logs it, a dark
  window, only the lab active. The firmware's headless recovery after a failed
  bring-up is not emulated.
- Brightness and the backlight follow the firmware's power code and the quick
  settings; the window shows the backlight level.

### Touch

- `timeout` and `absent`: reads fail; the desktop follows the firmware's
  policy of one log line and a retry 5 s after the failure, so touch returns at
  most 5 s after the fault clears.
- `noise`: a fixed xorshift sequence of pressed and released points, some off
  the panel, as a corrupted report might give; the same on every run.
- Gestures are emulated through the touch commands (`down`, `move`, `up`,
  `swipe`); the controller's own gesture registers are not used by Core and
  are not modelled.

### Audio

- `unavailable` at start-up: the speaker initialisation fails (`no audio
  output`, as for a codec that does not answer). While playing: the next write
  fails, the stream is released and the error counted, as a failed I2S write
  does on the board; applications get the same stream status as there.
- `audio-underrun` empties the ring before the next playback decision, so the
  shared playback state machine (`components/roundie_audio/stream.c`) sees an
  underrun and plays silence until the application refills it.
- The SDL device consumes samples in real time (see Virtual time).

### What is deterministic

Under `--virtual-time` every lab command, the model's state and what Core
reads from it (`hw`, gauge samples, faults, resets, power loss), application
callbacks, the launcher and the script take effect at the same virtual
instants on every run, and the tests compare whole outputs of two runs. The
exception is the pace of SDL audio: the device reads the stream ring at the
host's real rate, so the ring level and the playback phase of an open stream
(playing, or feeding silence after an underrun) vary between runs. The stream's
existence, its owner, plays, errors, underruns, pauses and the volume do not.
The audio test masks only `ring` and `phase` in the comparison and checks that
an open stream is always playing or feeding silence, never waiting or paused.

### Storage

- `storage-limit KB`: the package manager's capacity seam
  (`roundie_packages_capacity`) reports the limit minus the bytes of the files
  under `storage/`, never more than the host's free space. The firmware
  checks capacity the same way before an installation and before writing an
  application's data, so these refuse with `data-storage-reserve` and similar
  status. The limit does not make individual `write` calls fail.
- `storage-fault absent`: `roundie_packages_allowed()` is false and there is no
  capacity, as `main/app_runtime.c` does without a mounted storage partition:
  launches, scans, installations and data writes are refused
  (ESP_ERR_INVALID_STATE).
- `storage-fault io`: `open`, `read`, `write`, `fsync`, `mkdir`, `rmdir`,
  `unlink`, `rename`, `stat` and `opendir` on paths under `storage/`, or on
  descriptors opened there, fail with EIO; `storage-fault io CALL` fails only
  that call, so that a later step of an operation can fail after the earlier
  ones succeeded. The paths are the package manager's own
  (`components/roundie_packages/manager.c`, the code the firmware runs on
  LittleFS): a data write that fails in `open`, `write`, `fsync` or `rename`
  reports `data-open` or `data-commit`, removes its `data.tmp` and leaves the
  application's previous data as it was (`test_lab_storage_call_faults_keep_data`). The wrappers (`vhw_storage.c`) are
  linked into the whole executable with `-Wl,--wrap`; calls on other paths pass
  through unchanged, and the wrappers use only atomics so that `write` stays
  async-signal-safe for the fault handler. Calls made inside the C library or
  through `fstat`/`lseek`/`readdir`/`close` are not failed. LittleFS-specific
  failures (wear, power loss during a write, corruption) are not modelled.

## Virtual time: what still runs in real time

`--virtual-time` (N10, `docs/native-apps.md`) was checked for real-time
dependencies in M6:

| Path | Clock | Decision |
|---|---|---|
| A callback that never returns | virtual time stopped while the executor was busy, so the stall report and the script never came | **Moved (M6).** The same callback still running after 1 s of real time counts as hung and virtual time moves on without it: the stall report comes at 5000 ms and later script lines run, identically on every run (`test_hung_application_in_virtual_time`). A callback that legitimately runs longer than 1 s of real time would see time pass during it, as on the board. |
| Model, lab commands, key holds, touch retry, sensor sampling, battery integration | virtual | Already on the controlled clock. |
| Audio thread poll (5 ms) and SDL device consumption | real | **Kept.** The device takes samples at the host's rate; the ring level and where silence falls depend on it. Moving it needs a second virtual-time follower and an emulated sink in place of SDL; not done. Stream state, plays, errors and the lab's injected underruns are deterministic; the ring level and the playback phase (prefill or playing after an underrun) are not, and the tests ignore `ring` and `phase`. |
| Executor's 10 ms receive timeout | real | **Kept.** A poll interval that never changes what an application observes; waiting on virtual time here would deadlock while the clock stands. |
| GUI loop pacing (`SDL_WaitEventTimeout`, `nanosleep`), panel refresh (100 ms) | real | **Kept.** Presentation only. |
| `lab.real_ns` across a restart | real | Only outside virtual time, so that the emulated clocks run on while the process restarts; under virtual time the gap is zero. |
| `load_us` and similar measurements | real | **Kept.** Host CPU measurements. |

Outside `--virtual-time` nothing changes: time is the host's monotonic clock.

## Tests

`tests/test_vhw.py` (model alone, ASan/UBSan and TSan with a second reading
thread): discharge and charge, depletion, sleep current, PWR latch, power
cut, samples of both chips and faults, the storage call selection,
persistence.

`tests/test_desktop.py`, each scenario run twice in virtual time with the
whole stdout compared:

| Test | Scenario |
|---|---|
| `test_lab_battery_discharge_and_charge` | 100 mAh at 600 mA runs flat; off; USB on; 4167 µAh in 60 s; unplugged |
| `test_lab_screen_off_and_wake` | screen off and BOOT wake; failed re-initialisation; repaint |
| `test_lab_rtc_failure_and_restore` | PCF85063 absent and back; power cut with and without backup |
| `test_lab_unresponsive_peripherals` | touch timeout and retry; gauge timeout and recovery |
| `test_lab_storage_faults` | capacity limit, I/O error, missing medium, recovery |
| `test_lab_storage_call_faults_keep_data` | open, write, fsync or rename failing while Watch saves: reported, unloaded, previous data kept, no `data.tmp`; saved once the fault clears |
| `test_lab_state_is_not_inherited` | a left-over and a partial state file are discarded at a fresh start; `--resume` refuses an incomplete one (one run, not compared) |
| `test_lab_audio_faults_and_close_during_playback` | underrun, device failure mid-tune, close, next instance |
| `test_lab_restart_light_sleep_and_power_loss` | menu restart, light sleep current, PWR latch off and on, USB, watchdog reset |
| `test_hung_application_in_virtual_time` | stall reported at 5000 ms of virtual time |
| `test_lab_panel_controls` | panel clicks as commands, panel screenshot |
| `test_lab_panel_window_in_gamescope` | the panel window in a headless compositor, a mouse click applies a command |

## Limits

- The lab emulates what Core uses. Peripherals Core does not use yet (IMU,
  microphone, SD card, Wi-Fi) are not emulated.
- Timing of buses, interrupts and DMA is not emulated; a fault takes effect at
  the next call.
- Native applications remain trusted, unisolated code in the desktop process
  (`docs/native-apps.md`); the lab changes nothing there.
- No result here replaces the board tests of spec §23: battery behaviour,
  wake sources, power-off and recovery need the device.

## Verification (M6)

| Check | Result |
|---|---|
| Four firmware builds (1.85B and 1.75C, rep and legacy) | exit 0, no warnings; no firmware, SDK header or ABI file changed (later M6 changes touch only `platform/linux`, tests and documentation) |
| Host suite with the IDF environment, final run | 145 tests: 144 pass, 0 fail, 1 skipped (HiDPI KWin test: `fake-input.xml` not installed) |
| Earlier runs | `phase` of an open stream differed between two virtual-time runs (real-time SDL pace): masked in the comparison and checked against the states it may take. `test_sound_plays_through_sdl_audio` (real time, M5a) measured 752.5 Hz once under the load of the full suite: counting crossings over 0.2 s also counted a few ms of underrun silence (a synthetic 784 Hz tone with an 8 ms gap gives exactly 752.5 Hz); the old measurement then passed six runs alone; the test now measures the period between crossings without the gap and passed three runs alone and the full suite (the cause on the failed run is a hypothesis) |
| Desktop suite (ASan/UBSan desktop build) | all lab scenarios above, each run twice in virtual time with identical stdout (`test_lab_state_is_not_inherited` once) |
| ThreadSanitizer: the model with a concurrent reader (`tests/test_vhw.py`) | no report |
| Lab panel window in a headless gamescope (Vulkan swapchain 360 × 392) | a mouse click applies `usb 1` |
| Board behaviour behind the models (battery curve, PWR latch, wake, power loss) | PENDING: needs the device; the models are assumptions |
