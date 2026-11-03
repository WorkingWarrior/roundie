# App storage (rep_storage_v1)

Status: implemented in firmware 0.3.29 and host-tested. Device check is
PENDING (see the end of this document).

## Decision

Spec §7.3 says that in the MVP, an app restores its persistent state
explicitly through a Storage API. §9.1 proposes per-domain tables. The first
such table is `rep_storage_v1` (`include/rep/rep_storage_v1.h`).

- **One opaque blob per installed package**, at most `REP_STORAGE_CAPACITY`
  (512) bytes. The app defines its own format and should start it with a
  version byte. There are no keys, files or paths. This covers settings and
  small app state, such as the watch theme or stopwatch laps.
- **A second import, not a larger table.** The 52-byte `rep_api_v3_t` stays
  unchanged, because apps check its `struct_size` exactly. An ABI 3 ELF may
  import `rep_storage_v1` beside `rep_abi_v3`; the validator allows it only for
  ABI 3. Firmware older than 0.3.29 rejects the ELF with `import:rep_storage_v1`
  before loading, so it fails closed. No capability bit is needed.
- **Storage is optional at run time.** `rep_storage_v1(REP_STORAGE_V1)`
  returns NULL when the launch has no storage. That is a development (RE1)
  upload, which has no package ID. Apps then run with their defaults.
- **Calls touch RAM only.** `read` and `write` copy to and from a system
  buffer. They check owner and instance, and accept only app data sections or
  SDK allocations, not stacks. They can be called from any callback, including
  stop and destroy, so an app can save on exit.

## Flash writes

The policy that excludes Flash writes while an ELF is loaded
(`docs/app-lifecycle.md`, "Flash, OTA and Safe Mode") still holds:

1. Launch. On the system worker, `roundie_packages_execute` reads
   `/storage/packages/<id>/data` into the blob, calls
   `roundie_elf_storage_set`, then submits the ELF.
2. Run. The executor reads and writes the RAM blob within callbacks.
3. Unload. Unload withdraws the blob from the instance; a written blob is
   kept.
4. Save. The worker poll or the next launch calls `roundie_elf_storage_take`
   once `roundie_elf_busy()` is false, after input retirement. It then writes
   the file. The write needs the app runtime to be allowed (no OTA, no Safe
   Mode) and idle, with no upload or package transaction.

While a written blob is unsaved, `storage_set` refuses, so no package launch
is admitted. There is no Flash write and no extra Flash mapping while native
code runs.

### File format and integrity

The `data` file is `"RPD1"`, the little-endian length, the SHA-256 of the
data, then the data. It is replaced atomically: written to `data.tmp`,
`fsync`, then `rename`. A missing file is an empty blob.

A file with a bad header, length or digest is reported and logged; the app
starts with an empty blob. The file is **not** deleted or repaired (no
automatic erase); the app's next write replaces it.

The file is kept across package updates and removed with the package
(`roundie_packages_remove`).

### Limitation

Changes since launch are lost if power fails, the board resets, or the
firmware crashes while the app runs. They are written only after unload. An
app that needs earlier durability would need a separate, measured design,
because writes while native code runs are excluded and unmeasured.

## Use

`examples/watch_elf`: two bytes `{1, theme}`. The watch reads them in `init`
and writes them on each long-press theme change.

## Evidence

- **Host-verified:**
  - `tests/test_packages.c` group 23: replace, interrupted replace (rename
    fault leaves the old file), corruption kept in place, admission while
    busy or not installed, removal with the package.
  - `tests/test_app_round.c` (watch): stored theme applied at start, write on
    long press, take after unload only, set refused while a blob is unsaved,
    argument checks.
  - `tests/test_elf.c`: import allowlist and resolver.
- **Not host-tested:** the glue in `main/app_runtime.c`
  (`persist_storage`, the load in `roundie_packages_execute`). It is compiled
  out in the host harnesses.
- **Device check (PENDING until reported):**
  1. Change the watch theme.
  2. Exit with the edge swipe.
  3. Relaunch: the theme is kept.
  4. Reboot and relaunch: the theme is still kept.
