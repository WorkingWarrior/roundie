# Watch face packages (RWF1) and the face store

Format, validation, installation, activation, persistence and the RF1
development transport. ABI: [watch-face-abi.md](watch-face-abi.md). Authoring:
[watch-face-authoring.md](watch-face-authoring.md).

## Decisions (M10)

- A face package (`.rwf`, magic `RWF1`) is separate from REP application
  packages (`RPK1`, [package-format.md](package-format.md)) and lives in its
  own directory, `/storage/faces` on LittleFS. The Flash layout is unchanged.
- **Install and activation are separate.** Installing validates and stores a
  face; it never changes what is shown. Activation (`select`, the gallery) is
  a separate request, and the preference is written only after the GUI
  reports that the face activated.
- **Unverified data is never run.** Discovery reads only the fixed prefix to
  list faces; every activation reads the whole file again and checks the
  envelope, SHA-256 digest, ABI, backend, resources and the full definition
  before the GUI gets the buffer. The GUI never reads storage.
- The face store follows the package store's admission
  (`roundie_packages_allowed`: BootGuard STABLE, OTA image valid, storage
  mounted). Until then, and for the whole of a trial boot or Safe Mode, only
  the built-in system face is shown.
- No signatures in this release: like RPK1, faces are trusted development
  content. The digest protects against corruption, not against an attacker.

## Package layout (little-endian)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `RWF1` |
| 4 | 2 | format version, 1 |
| 6 | 2 | header size, 64 |
| 8 | 4 | total package size (≤ 65536) |
| 12 | 20 | zero |
| 32 | 32 | SHA-256 of bytes 64..total |
| 64 | 48 | id, NUL-padded: `[a-z][a-z0-9]*(\.[a-z][a-z0-9]*)+` |
| 112 | 32 | display name, UTF-8, NUL-padded |
| 144 | 6 | version major, minor, patch |
| 150 | 2+2 | ABI major, minor (1, 0) |
| 154 | 1 | backend: 1 declarative; 2 native (refused) |
| 155 | 1 | preview resource index or 0xFF |
| 156 | 4 | required host capabilities (`RWF_HOST_CAP_*`) |
| 160 | 4+4 | definition offset and size (≤ 4096) |
| 168 | 2 | resource count (≤ 8), then 6 zero bytes |
| 176 | 16 × n | resource table: format (1 RGB565, 2 A8), width, height, zero, offset, length |
| … | | definition (`RWFD`): settings, string pool, elements |
| … | | resource bytes (≤ 48 KiB in all), packed in table order, nothing after |

Limits are in `components/roundie_watchface/include/roundie_face_format.h`:
32 elements, depth 4, 4 settings of up to 8 options, 1024 bytes of strings,
63-byte templates. `tools/roundie_face.py` produces this format from XML,
reproducibly (same source, same bytes; tested).

## Refusal reasons

Errors are short stable strings in the store status (`error`), on RF1 and in
the desktop `face-status` line. Tested cases (`tests/test_watchface.py` and
`tests/test_watchface.c`):

| Reason | Cause |
|---|---|
| `package-size` | shorter than the prefix, larger than 64 KiB, or a size mismatch |
| `package-envelope` | magic, version, header size, total or reserved bytes |
| `package-digest` | SHA-256 mismatch (corruption on storage or in transfer) |
| `package-trailing-bytes` | bytes after the last resource |
| `face-abi` | another ABI major |
| `native-face-unsupported` | backend 2 |
| `manifest-id`, `manifest-name` | id syntax (no paths), name not terminated |
| `definition-*`, `element-*`, `label-*`, `setting-*`, `strings-*` | malformed definition |
| `resource-bounds`, `resource-format`, `resource-budget` | resource table |
| `face-storage-reserve` | the install would cut into the storage reserve |
| `face-id`, `face-open` | bad id in a request; face not installed |
| `face-admission`, `fallback-only`, `storage-unavailable` | store not admitted (trial boot, Safe Mode, before STABLE) |
| `transfer-cancelled-or-expired` | RF1 transfer aborted or idle for 10 s |

## Installation

1. **Stage:** `BEGIN session size sha256`, `DATA session offset hex` (512-byte
   chunks, strictly sequential), `END session`. The file is written to
   `/storage/faces/.transfer`, then fully validated; phase READY. An idle
   transfer expires after 10 s; a stale `.transfer` from a power loss is
   removed at start.
2. **Install:** `INSTALL sha256` commits the READY file by rename to
   `<id>.rwf`. Installing the same or an older version of an installed face
   is refused, unless the installed file no longer validates (repair). The
   capacity check keeps the package store's reserve.
3. Nothing is shown. The face appears in the list and the gallery.

The desktop installs with `--install-face FILE.rwf` through the same
`roundie_watchface_install_file` path.

## Activation and persistence

- `SELECT id` (RF1), `face-select id` (desktop script) or **Ustaw** in the
  gallery load and verify the face and offer it to the GUI. The GUI switches
  (create → activate → first update) and reports success; only then does the
  store write the preference: `active.tmp`, `fsync`, rename to `active`. A
  power loss leaves either the old or the new preference.
- `SELECT system` clears the preference.
- At start the stored preference is offered once (BOOT). A corrupt or missing
  preferred face is not shown: the system face stays, `error` names the
  reason, `load_failures` counts it, and the preference is kept so that a
  reinstall repairs it.
- `REMOVE id` deletes the package; removing the preferred face clears the
  preference, and removing the shown face returns to the system face.
- Gallery previews are never persisted.

## RF1 development transport

Line protocol over the USB Serial/JTAG console, enabled with
`CONFIG_ROUNDIE_DEV_PACKAGES` (`main/face_diagnostics.c`, same framing and
parser as RP1):

```
!RF1 <8 hex request id> STATUS | BEGIN s n sha | DATA s off hex | END s | ABORT s | INSTALL sha | SELECT id | REMOVE id
```

Reply: `!RF1 <id> <esp_err> {json}` with `format` 1, transfer `phase`
(0 idle, 1 receiving, 2 ready), `received`, `total`, `count`, GUI figures
(`objects`, `pool`, `bytes`, `home`, `parked`, `gallery`), `fallback_only`,
`loads`, `load_failures`, nine `counters` (creates, destroys, activations,
deactivations, updates, switches, failures, refused, resumes after
light sleep; a return to Home from the list is an activation), five `us`
timings (load, activate, activate max, update max, resume), `preferred`,
`active`, `failed`, `error` and `faces` (`[id, version, healthy]`). The host
client validates every field and bound before use (`parse_reply`).

```bash
python3 tools/roundie_face.py build faces/minimal_digital/face.xml --output minimal.rwf
```

```bash
python3 tools/roundie_face.py --port /dev/ttyACM0 upload minimal.rwf --install
```

```bash
python3 tools/roundie_face.py --port /dev/ttyACM0 select org.roundie.face.minimal
```
