# Roundie Manager — Milestone 3F-W

Decision, 2026-10-20: implement a local application management frontend in
strict TypeScript, framework-free DOM and Vite. This bounded interface does
not justify a runtime framework or dependency injection container. There are
no runtime npm dependencies, cloud services, remote fonts or analytics.
The target is the existing 1.85B REP firmware; this milestone makes no firmware,
ABI, boot, partition, schema, USB PHY or descriptor changes.

This is a delivery frontend for **existing** PackageManager policy, not an
alternative package authority. REP v0.4 §§13, 16, 23 and 25 retain their
architecture and open gates. **Milestone 3F — USB MTP: NO-GO / OPEN**. 3G is
outside scope.

## Launch and dependencies

```sh
cd /mnt/Dane/Development/roundie/tools/roundie-manager
npm install
npm run dev
```

Open `http://127.0.0.1:5173` in a browser exposing Web Serial. `npm ci` uses
the committed lockfile for a clean frozen installation. `npm run build`
typechecks and builds `dist/`; `npm run preview` serves it on localhost.
Dependencies are installed once; development, static builds, hashing and
serial operation then work offline. No elevated privilege, Docker, cloud
account, serial daemon or backend proxy is required. Do not open index.html
directly with `file://`.

The UI checks secure context and `"serial" in navigator`; a Chromium-derived
name alone does not establish support. Port selection is exclusively an
explicit Connect click. Browser permission, serial-device permissions on
Linux and an available unoccupied port are separate requirements. For Linux
permission diagnosis inspect the actual port owner/group (`stat` and `id`);
use the host's normal administrator-managed device access policy, rather
than running the browser as root or changing global device modes. Do not
leave `idf.py monitor`, Python serial clients or another browser connected.

The native implementation follows the [Web Serial API lifecycle](https://developer.chrome.com/docs/capabilities/serial).
Port chooser cancellation, permission failure, missing API, unavailable secure
context, busy port, protocol timeout, unexpected device and USB removal have
separate guidance. The hardware chooser remains browser-controlled: canceling
connection in the app invalidates the attempt and closes any port returned
later; it cannot programmatically dismiss a native browser chooser.

## Ownership and layers

| Source | Responsibility |
|---|---|
| `src/transport.ts` | SerialTransport byte-stream contract and native WebSerialTransport |
| `src/protocol.ts` | Bounded line decoder, request correlation, RP1 schema and error decoding |
| `src/package.ts` | Bounded RPK1 metadata inspection and Web Crypto hashing |
| `src/services.ts` | DeviceService, installation plan, DeploymentCoordinator and reconciliation |
| `src/main.ts` | DOM presentation, local confirmation, file selection, drop, responsive progress |
| `tests/mock.ts` | Deterministic transport and device model; never imported into production |

The transport owns one reader lock and one writer lock for the entire session.
Only ProtocolClient consumes bytes; all four read-only diagnostic protocols
share its decoder. There are no extra status/list/upload readers. One pending
command is allowed; another is rejected rather than racing for a reply. The UI
also locks Refresh, file replacement and Install for the whole operation.
Disconnect and cancellation remain serviceable. There is no pipelining,
polling loop, automatic reconnect or blind retry.

Web Serial's WritableStream write consumes a Uint8Array or rejects; it provides
no partial byte count. Protocol completion requires the addressed device reply,
never resolution of stream.write. The test transport rejects short writes.
Receive buffering retains at most a 2048-byte line, discarding oversized
unrelated logs through LF. Transport buffers are 4096 bytes. An addressed
oversize/malformed reply, timeout, write failure or disconnect closes the
session and rejects outstanding waits. Teardown cancels the reader, aborts
the writer, releases locks and closes the port. No failed mutation is replayed.
Native stream/chooser behavior on the actual board remains a physical gate.

## Authoritative contract reconnaissance

Verified **source** at baseline `bc5a895`, unchanged by this milestone:
`tools/roundie_package.py`, `tools/roundie_time.py`, `tools/roundie_ota.py`,
`tools/roundie_safe.py`, `tools/roundie_elf.py`,
`components/roundie_ota/ota_protocol.c`, `main/package_diagnostics.c`,
`main/recovery_diagnostics.c`, `main/ota_diagnostics.c`,
`components/roundie_packages/{format,manager}.c` and their public/internal
headers. Existing package, registry, launcher, OTA, Safe Mode, USB architecture,
MTP acceptance and deployment documents informed the scope. The specification
is a target; the following contract is implemented production behavior.

| Contract point | Existing behavior reproduced by Manager |
|---|---|
| Protocol version | Text `!RP1`; response `format:1`. Not a binary packet protocol |
| Request framing | ASCII `\n!RP1 <8 lowercase hex request digits> <operation>\n` |
| Response framing | `!RP1 <same request> <signed decimal ESP error> <JSON>\n`; leading logs/blank lines ignored |
| Byte order | Decimal counters / lowercase hex octets in wire order; no integer endian conversion in RP1. RPK1 numeric fields are little-endian |
| IDs | Random 32-bit request ID; random **nonzero** 32-bit transaction session. Exactly eight lowercase hex digits. They are not per-command numeric opcodes |
| Bounds | Request line <1100 bytes excluding delimiter; RP1 reply ≤1900 bytes; RE1 ≤2048; RS1/RO1 ≤1024 |
| Chunk maximum | 512 raw bytes encoded as 1024 hex characters; stop and wait |
| Package maximum | 224-byte prefix plus 65536-byte ELF = 65760 bytes; minimum 276 bytes |
| BEGIN | `BEGIN <session> <full size> <64 lowercase hex full-package SHA-256>`; admitted phase RECEIVING=1 |
| DATA | `DATA <session> <exact next decimal offset> <hex bytes>`; require phase 1, same session/total and exact received count before next chunk |
| END | `END <session>`; complete size, full digest, metadata, embedded digest and authoritative ELF checks; READY=4, **not installed** |
| Commit | `INSTALL <full-package digest>`; no literal RP1 COMMIT command. Device selects the transactional active artifact and returns to IDLE |
| ABORT | `ABORT <session>` for current busy transaction; rejects wrong/stale session or loaded runtime; require returned IDLE=0 / busy=0 |
| Read-only | STATUS, LIST, INFO `<canonical ID>`; LIST tuples are `[id,version,abi,health]`; INFO adds name, full package digest, ELF size |
| Other commands | Production also exposes LAUNCH, REMOVE and navigation diagnostics. Manager deliberately does not expose execution/removal controls |
| Timeout | Python per-request default 8 s; Manager uses the same. Device receiving inactivity deadline 10 s; READY retention 60 s |
| Retry | No non-idempotent retry; uncertain commands close the session and require a fresh handshake |
| Admission | Package allowed/busy, loaded ELF, OTA, Safe Mode, storage reserve and registry capacity stay device-authoritative |
| Replacement | Requires a strictly newer version and a non-corrupt selection; same version, downgrade and corrupt health=1 are rejected. Incompatible health=2 is not separately rejected by this comparison. Registry includes at most 8 package/orphan IDs |
| Reconnect | No resume/replay protocol. Reopen, read current diagnostics and registry, then INFO for an uncertain commit |
| Serial configuration | 115200 baud, 8 data bits, 1 stop bit, no parity/flow control; DTR=true and RTS=true in one setSignals call, matching the verified Python idle pair |

RP1 has **no frame checksum** or authenticated product identity. The full-file
SHA-256 supplied to BEGIN and INSTALL is integrity, not publisher authentication.
The app requires coherent RO1/RS1 status (matching running slot and version),
supported RE1 loader diagnostics and RP1 format 1 before enabling writes. It
requires VALID selected/running firmware, BootGuard stage 4/STABLE, zero latch,
failures/trial, available persistence, normal mode, idle OTA and PackageManager,
unloaded/unreserved ELF and available capacity. This protocol fingerprint
identifies a compatible REP session; it is not cryptographic authentication.
The public data does not include a product serial/name string or firmware-wide
ABI feature mask. Only returned fields are shown; ABI 1–3 support comes from
the inspected RPK1/RE1 compatibility contract, not invented live telemetry.

Error replies use signed decimal `esp_err_t` and the actual PackageManager
`error` string. Common codes are -1 FAIL, 257 NO_MEM, 258 INVALID_ARG,
259 INVALID_STATE, 260 INVALID_SIZE, 261 NOT_FOUND, 262 NOT_SUPPORTED and
263 TIMEOUT. Unknown codes retain their numeric value. Messages such as
`package-admission`, `package-storage-reserve`, `transfer-state`,
`transfer-offset-or-length`, `replace-requires-healthy-older-version`,
`selection-commit` and `registry-admission` are preserved for diagnosis.
An error does not authorize bypassing admission or modifying firmware.

## RPK1 inspection and installation

The browser reads a File only after checking its extension and 276–65760-byte
bound. It validates the exact 64-byte header, revision 1, 160-byte manifest,
declared full/ELF sizes, reserved fields, canonical zero-padded UTF-8 fields,
lowercase dotted ASCII ID, numeric 16-bit version, target `esp32s3.rep1`,
API 1 and ABI 1–3. Fixed offsets avoid header-controlled allocation/overflow.
It computes full-package and embedded-ELF SHA-256 using Web Crypto. It does not
execute or interpret ELF, validate Xtensa relocations or create a competing
authoritative loader. PackageManager and the production ELF validator perform
the final acceptance after upload. Native applications remain trusted-only.

Local preview shows actual metadata, size, digest and whether this is a new
installation, update or forbidden same-version/downgrade replacement. The
browser checks known storage/registry limits for useful early feedback:
free storage must include 256 KiB reserve + package size + 16 KiB.
The user must explicitly check the trust/installation confirmation and click
Install. Fresh preflight rechecks device state and version policy before BEGIN.
The device remains authoritative if state changes afterward.

Progress reports acknowledged raw bytes, separate from END validation, INSTALL
commit and INFO verification. At 100% transferred it still states that completion
requires verification. A successful INSTALL is followed by an independent
INFO matching package ID, version, ABI, healthy status, full digest and ELF size.
Only then is completion reported. Registry refresh follows; nothing autostarts.

## Failure, cancellation and reconnect

Milestone 3F-W.1 separates port selection, opening, signal setup, stream
initialization, protocol handshake, invalid/incompatible diagnostics, serial
I/O, unexpected EOF and user disconnect. The browser exception is retained as
an Error cause and shown in expandable Connection details. A NetworkError is
an opening error only when it originated in `SerialPort.open()`.

Reader/writer acquisition rolls back partial ownership. Closing marks the
transport before cancellation, observes both stream completion promises,
releases locks before closing the port, and records secondary cleanup failures.
There is one idempotent close promise per transport/client. A blocked sink is
bounded by a 2 s stream cleanup deadline; port close has a separate 2 s deadline.
Protocol shutdown bounds transport cleanup at 5 s and pump completion at 2 s.
These deadlines report incomplete cleanup and block reconnect until page reload;
they neither extend response timeouts nor establish that the browser released
a stuck native operation. Normal response deadlines remain 8 s, with no retries.

Protocol failure stores the first error before requesting teardown. The pump
does not await teardown from its own receive loop, and ignores data/errors
arriving after termination. Output-stream failure also invalidates an idle
connection. UI callbacks and awaited connection/refresh/reconciliation results
check their generation and device owner before updating visible state. New
Connect attempts wait for previous cleanup; user disconnect is an intentional
shutdown and does not produce a device-loss warning.

As in production Python `read_serial_reply`, incoming lines are matched by
their ASCII protocol/request prefix **before** strict UTF-8/JSON decoding.
Unrelated/stale lines do not complete a request and cannot fail current decoding.
Addressed malformed, oversized or invalid responses still fail closed. No
incoming flush, protocol change or RP1 transaction replay was added.

Opt-in `?debug=serial` records at most 256 metadata events plus a separately
retained first error, with sequence, relative time, session, connection phase,
transport/protocol state, pending request ID, lock ownership and teardown
initiator. No normal-console logging or package payload logging is enabled.
The latest trace is available through `window.roundieDiagnostics()` and the
Connection details panel. Debug-only read-only A–D probes compare native
streams with the Manager layers; ordinary Connect is E. See the
[3F-W.1 report](roundie-manager-milestone-3f-w1-diagnostics.md) for evidence,
limitations and the physical procedure. Software tests do not resolve the
original physical failure without a captured failing session and real acceptance.

| Situation | Behavior |
|---|---|
| Canceled before BEGIN | No transaction |
| Canceled during DATA | Finish the outstanding bounded request, schedule no more chunks, send ABORT, require IDLE acknowledgement |
| ABORT response lost/rejected | Cleanup unverified; preserve context and require fresh status, do not claim cancellation cleanup succeeded |
| USB removed / timeout during BEGIN or DATA | Close session, retain interrupted package identity, report cleanup unknown. Reconnect and check IDLE/registry; never replay a chunk |
| Canceled during INSTALL | Do not reverse or ABORT a commit; finish INFO verification or preserve uncertainty |
| Lost INSTALL reply or malformed final result | Preserve intended ID/version/ABI/digest/size. Reconnect, perform fresh handshake and query INFO. Never resend INSTALL |
| INSTALL returns error | Query INFO before deciding: production can report a scan failure after changing active selection |
| INFO matches intended artifact | Report independently verified completion |
| Fresh INFO/registry differs and PackageManager is IDLE | Report expected artifact not committed and show authoritative current installed state; no automatic retry |
| PackageManager remains busy after reconnect | Keep unresolved warning and block new install; wait for its 10 s / 60 s timeout, then explicit Refresh |

Interrupted operation metadata lives only in the open page's memory. No full
packages or private device identifiers are persisted. Reload/close loses that
context; query the current registry and INFO before any new action. The app
requests the browser's navigation warning while work/uncertainty exists;
browser support for that warning is not guaranteed. All filenames/metadata
use DOM textContent, never untrusted HTML. Refresh and upload cannot create
independent stream consumers.

## Verification and future integration

`npm test` generates deterministic vectors through production Python
request/write functions, compiles the real firmware transfer parser and
checks exact TypeScript bytes, including a maximum 512-byte binary chunk.
Success/error reply samples are validated by the production Python parser;
they are not physical device captures. A real RPK1 is built by the existing
builder/C validator in a temporary directory. Fixtures are ignored/generated
locally so ELF/package artifacts are not committed. Browser UI tests inject
an explicit mock navigator.serial with native streams and use that same
production transport/client/services/UI. This proves browser software behavior,
not physical Web Serial acceptance. See the [acceptance record](roundie-manager-milestone-3f-w-acceptance.md).

Current: `browser → Web Serial → RP1 → existing PackageManager`.

Future: `MTP → restricted staging → existing PackageManager`.

Future MTP need not tunnel RP1. No imaginary MTP browser transport, unused
firmware interface, storage abstraction or new ingestion protocol was added.
The [MTP feasibility blockers](usb-mtp.md) and [NO-GO gate](usb-architecture.md)
remain open. Firmware updates, package launch/removal, online stores, BLE/Wi-Fi
deployment, WebUSB, FUSE, boot/recovery changes and power work remain outside
this milestone.
