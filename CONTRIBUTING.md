# Contributing to Roundie

Thank you for helping. Roundie runs on real hardware with Flash, a PMIC and a
recovery path that can be damaged. These rules keep changes reviewable and
keep the boards recoverable.

## Before you start

- Read the relevant part of [docs/specification.md](docs/specification.md) and
  [docs/roundie-core.md](docs/roundie-core.md) before you change architecture,
  the REP ABI, storage, boot or hardware behaviour.
- For a larger change, open an issue first and describe the approach.

## Pull requests

- **Small and focused.** One pull request does one thing. Put independent
  features in separate pull requests, even when you developed them together.
  For example, a microphone driver and an IMU driver are two pull requests.
- **Minimal scope.** No drive-by reformatting, renaming or refactoring of
  code you did not need to touch. Do not update dependencies unless the
  change needs it, and say why.
- **Commits.** Use [Conventional Commits](https://www.conventionalcommits.org/)
  with an English message (`fix(board): ...`, `docs: ...`).
- **CI must be green.** Every pull request runs the workflow in
  [docs/ci.md](docs/ci.md): the four firmware builds (with warnings treated as
  errors), the host tests, the desktop and the Virtual Hardware Lab.

## REP ABI compatibility

Applications are built against the public SDK headers and the versioned API
tables ([docs/app-abi.md](docs/app-abi.md)). Existing packages must keep
working.

- Do not change the layout, order or meaning of an existing table entry or
  public structure. Additions need a new ABI version and a stated reason.
- Keep LVGL, FreeRTOS, ESP-IDF and private libc types out of the public
  headers. Use fixed-width fields with explicit lengths and ownership.
- Do not export private firmware symbols to applications.

## Tests

Add or update tests for what you change:

| Change | Expected tests |
|---|---|
| Board support (`components/roundie_board`) | Host tests for decoders and start-up logic (see `board_logic.c` and `tests/test_board_1_75c.c`). If Core reads the behaviour, update the lab model (`platform/linux/vhw.c`). Build both profiles for every board |
| Runtime, loader, packages | Host or desktop tests in `tests/`, including failure and cleanup paths |
| Applications and UI | Desktop tests on both panel profiles (`--board 1.85b` and `--board 1.75c`) |
| Tools and CI | The matching test group (`python3 tools/ci/run_tests.py --list`) |

A new test file must belong to exactly one group; `run_tests.py
--check-coverage` fails otherwise.

## Host tests and hardware validation

Host tests, the desktop, the lab and CI do not prove board behaviour. In the
pull request description, keep these apart:

- **Automatic checks**: what ran, and the result.
- **Device tests**: board, unit count, firmware commit or version, what was
  measured or observed, and by whom. Logs help.
- **Not verified**: anything still PENDING.

You do not need both boards. Test the board you have and say which one. The
maintainers will mark the other board's checks as PENDING.

One unit passing is evidence for that unit, not validation of the board as a
whole.

## Power, OTA, flashing and recovery

Changes in these areas can brick a board or destroy user data. Call them out
in the pull request title or description.

- **PMIC, charger and power rails.** Write only the bits the change needs,
  read-modify-write. Never change a voltage or charger setting without a
  datasheet reference and a device test. Put start-up logic where it can be
  host-tested against a register mock.
- **OTA and rollback.** Keep application-confirmed rollback. Confirm an image
  only after the critical self-test passes.
- **Flashing and installers.** Raw IDF flash targets stay disabled. A new
  installation path must read the Flash out first, must not erase NVS or
  LittleFS by default, and needs review before anyone runs it.
- **Recovery.** Keep ROM Download (GPIO0/BOOT) usable. Safe Mode must start
  without LittleFS, applications, MTP or Wi-Fi.
- **Never** enable Secure Boot, Flash Encryption or hardware anti-rollback,
  and never burn eFuses, in a contribution.

## Adding a board

Follow [docs/board-support.md](docs/board-support.md#adding-a-board):

- a hardware audit with sources;
- a board directory behind `roundie_board.h`;
- builds for both profiles;
- a list of the device checks still pending.

A new board must not need changes to the launcher, OTA, the ELF loader or
applications. If it does, raise that as a design question first.

## What not to commit

- Flash dumps, factory images or backups of a device.
- Secrets, keys, tokens or certificates.
- Personal data, or user data read from a device.
- Build outputs, logs, generated `sdkconfig` files (only the `*.defaults`
  files are tracked), and editor or tool state.

## License

By contributing you agree that your contribution is licensed under the
[MIT License](LICENSE).
