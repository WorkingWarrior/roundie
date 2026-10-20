# Milestone 3F-W.1 — connection investigation, 2026-10-20

**Status: host defects reproduced and fixed; software gates PASS. Original
physical failure unresolved; browser-to-device acceptance PENDING.**

This report does not claim restored physical connectivity. No accessible
interactive browser was registered with CUA (`apps: [], browsers: []`).
`/dev/ttyACM0` was present, but no physical port was opened by this work.
The automated Chromium uses explicitly injected serial streams. A successful
mocked handshake is software evidence only. MTP stays **NO-GO / OPEN**.

## Evidence and root-cause boundary

The user supplied physical observations: manual open/close succeeds; Manager's
open resolves with readable/writable streams; a later read rejects with
`NetworkError: The device has been lost`; disabling the signal call did not
solve the failure; an independent native read receives 255 bytes beginning
with a recognizable RO1 reply. These are supplied observations, not fresh
captures by this run. They establish neither the first failing operation nor
whether that read exception preceded application teardown. Buffered data from
an earlier request is possible; spontaneous RO1 status is not inferred.

| Required diagnosis | Conclusion and evidence tier |
|---|---|
| First failing physical operation | **UNKNOWN** until a failing debug-session trace is captured. Open success is supplied evidence; there is no ordered trace of subsequent operations. |
| Was device-lost NetworkError primary or secondary? | **UNKNOWN physically.** Deterministic tests separately demonstrate primary native read failure and timeout → teardown → secondary read rejection. |
| What initiated physical teardown? | **UNKNOWN.** Existing source initiators are read EOF/error, write failure, addressed parser/schema failure, response timeout, and explicit close from UI/user. New trace records the winning initiator before cancellation. |
| DTR/RTS involved? | Not established; disabling the call already failed to resolve the supplied case. No new signal experiment was performed on hardware. Production's paired true/true setting is unchanged. |
| Stale RO1 involved? | Possible buffered response per supplied raw read, but involvement in the failure is **unproven**. Valid unrelated request IDs were already ignored. No flushing or speculative stale-session reset was added. |
| Was response matching correct? | ASCII protocol + exact eight-digit request ID matching was correct. A distinct confirmed defect decoded unrelated candidates as strict UTF-8 before matching; malformed unrelated bytes could poison a healthy request. |
| Pump/teardown race? | No promise dependency cycle was found: receive called fail without awaiting transport teardown, and close awaited teardown then pump. Confirmed hazards were partial-acquisition lock leakage, unbounded abort/cancel waits, hidden cleanup failures, and UI completions without full generation checks. No proof links these hazards to the physical observation yet. |
| Can the user connect and list physical apps now? | **Not verified.** Full simulated status/LIST/INFO and reconnect succeed. Actual Hello/UI listing remains PENDING. |

Confirmed defects were fixed, rather than suppressing exceptions or extending
handshake timeouts. These are source/host findings, not a claim that any one
is the root cause of the physical case. The milestone's restored-connectivity
requirement remains open.

## Reconnaissance and lifecycle

Baseline commits supplied by the goal: `95cd474`, `5a82ef2`. The only initial
working-tree modification was the commented `setSignals()` in transport.ts.
Its exact original patch is preserved outside Git at
`/tmp/roundie-3f-w1-original-transport.patch`. The task-related diagnostic edit
was restored to the committed signal behavior for implementation and testing;
raw probes A/B retain the no-signals comparison. No unrelated work was found.

Sources examined: transport.ts, protocol.ts, main.ts, services.ts, existing
unit/browser tests, production Python roundie_time/ota/safe/package clients,
firmware main serial polling, ota_diagnostics.c, existing acceptance records,
and REP specification §§23/25. Firmware has one secondary USB RX owner and
emits LF-delimited RO1 responses including the supplied request ID. Python
reads bounded lines and matches the address before decoding; it does not flush
all buffered input at connect. RP1 encodes binary package bytes as ASCII hex.

Actual Manager order:

1. Connect click sets connecting/generation, calls requestPort directly under
   the user gesture, then opens 115200, 8N1, no flow control, 4096-byte buffers.
2. Apply the existing paired DTR/RTS setting, check streams, acquire reader then
   writer. Roll back reader ownership if writer acquisition fails.
3. Construct ProtocolClient; start the sole receive pump immediately. Associate
   its DeviceService and disconnect callback with this generation.
4. Send RO1 STATUS, RS1 STATUS, RE1 STATUS, RP1 STATUS sequentially. For each,
   establish pending ID/timer **before** writing; successful write is not ACK.
5. Validate the REP fingerprint/coherent firmware state. If package admission
   permits, read RP1 LIST and one INFO per listed app; validate identities.
6. Publish the complete snapshot only for the still-current, connected client.
   Reconcile prior ambiguous installation through existing read-only INFO logic.
7. The first fatal event rejects pending work and starts transport cleanup;
   subsequent errors remain secondary evidence. User close follows the same
   cleanup ordering but is explicitly intentional.

Port opening does not establish a connected REP device. Responses may be split
anywhere across chunks or coalesced with logs/old replies. Matching is by exact
prefix, never by response arrival order, USB identity alone or a session field
inside the JSON. An old client/pump never supplies a new client's requests.

## Changes and closure semantics

- `diagnostics.ts`: phase-bearing ConnectionFailure with original cause;
  expandable exception chain; opt-in local metadata ring (256 events plus
  first error retained separately). Includes monotonic sequence, relative time,
  session, phase, transport/protocol state, pending ID, reader/writer ownership,
  error name/message and teardown initiator. Payloads and USB serial numbers
  are omitted. Diagnostics are disabled by default.
- `transport.ts`: transactional stream acquisition; explicit operation phases;
  observer for unexpected idle output failure; idempotent closing marked before
  cancellation. Cancellation/abort outcomes are observed; original errored-stream
  cancellation rejections are secondary, not reclassified as opening failures.
  Locks release before port close, whose failures remain observable.
- `protocol.ts`: match addresses before strict decoding, retain the first
  termination cause, classify write/read/EOF/parser/timeout separately, and
  ignore late pump results after shutdown. No queue, retry, flushing or wire
  change was introduced. Explicit close waits for cleanup and pump completion;
  there is no receive-loop await of its own shutdown.
- `main.ts`: phase-specific primary messages with expandable details; generation
  and owner checks across connection, refresh, reconcile, and disconnect;
  reconnect waits for prior cleanup. Intentional disconnect cannot be overwritten
  by an abandoned handshake/refresh failure. Preserve ambiguous-INSTALL handling
  and its independent verification message.
- `serial-probe.ts`: debug-only read-only A–D comparison, with E provided by
  the existing full Connect action. No package mutation operation is available
  in the probes.

Cleanup has finite software waiting budgets: 2 s for cancel/abort (in parallel),
then 2 s for port close; the protocol bounds transport cleanup at 5 s and pump
completion at 2 s. The UI blocks reuse after an incomplete cleanup and directs
page reload. A deadline does **not** cancel an arbitrary stuck native sink,
prove OS release, or turn uncertainty into success. Late rejections remain
observed. Response timeouts remain 8 s and RP1 writes are never retried.

The lock/cancellation order follows the official
[Web Serial stream/close contract](https://developer.chrome.com/docs/capabilities/serial)
and [Web Serial standard](https://wicg.github.io/serial/): port closure requires
unlocked streams; native read chunk sizes are arbitrary. This supports the
implementation choice, not a diagnosis of a board/browser failure.

## Regression evidence

Before production edits, the new connection.test.ts had **3 tests, 0 PASS,
3 FAIL**, exit 1 on the initial implementation:

1. NetworkError outside open was incorrectly mapped to Cannot open serial port.
2. getWriter failure left the already acquired reader locked.
3. A stale unrelated RO1 line with invalid UTF-8 killed the current valid RO1
   request. This is a deterministic source-defect reproducer, not a reproduction
   of the physical device's observed payload (which was not supplied in full).

All three pass after the fix. Additional coverage uses native Web Streams for
successful reads/writes, cancellation with pending read, read/write NetworkError
cause retention, idle output failure, missing streams, failed selection/open/
signals, concurrent closes, failed port close, blocked sink cleanup deadline,
late rejection, first-error preservation, timeout preceding secondary read
failure, old pump/new client overlap, and diagnostics bounds. RO1 tests cover
LF framing, fragmented/coalesced replies, delayed delivery, stale session IDs,
wrong protocol/ID, unrelated bytes and strict addressed decoding. Existing
malformed JSON, oversized frame, wrong-ID timeout and RP1/RPK1 tests remain.

Browser tests retain all original deployment/UI checks and add post-open read/
write failure with reconnect, handshake timeout with reconnect, disconnect
during open/handshake/refresh, idle loss, incomplete-close reuse refusal, missing
streams/incompatible diagnostics and debug B/C/D probes. Unexpected page errors
are checked in the new failure/lifecycle cases. Tests inject navigator.serial;
they do not substitute for a real browser/device acceptance trace.

| Gate | Command / scope | Result |
|---|---|---|
| TypeScript | `cd tools/roundie-manager && npm run typecheck` | PASS, exit 0 |
| Manager host/reference tests | `npm test` | **106 PASS**, exit 0; includes unchanged production Python vectors/C parser/RPK1 fixture generation |
| Browser tests | `npm run test:ui` | **26 PASS**, exit 0, 5.9 s; headless Chromium 156.0.8078.4 with injected serial streams |
| Production build | `npm run build` | PASS, exit 0 |
| Existing repository host suite | `. /home/solve/.espressif/v6.1/esp-idf/export.sh` then `python -m unittest discover -s tests -p 'test_*.py' -q` | **82 PASS**, exit 0, 13.905 s; C/mock/ASan/UBSan scope, no board validation |
| Firmware build/flash/OTA | Not performed | Host-only milestone; no firmware edits |
| Physical A/B/C/D/E | No accessible interactive browser | **PENDING** |
| Real connect/list/disconnect/reconnect and timing | Not performed | **PENDING** |
| Reset / USB re-enumeration observation | No physical session captured | **PENDING** |

One intermediate browser run exposed an introduced feedback overwrite after
successful ambiguous-INSTALL reconciliation (21/22 PASS). This was fixed, not
masked; the original test then passed in the next full 22/22 run. Later added
phase/refresh/open tests expand the final browser gate. Existing install,
acknowledged-byte progress, INFO verification, SHA-256, binary parity, ABORT,
loaded-ELF admission, drag/drop, theme and responsive-layout tests are retained.

## Read-only physical acceptance — PENDING

Launch the existing app, with no firmware or package preparation:

```sh
cd /mnt/Dane/Development/roundie/tools/roundie-manager
npm run dev -- --port 5173 --strictPort
```

Open `http://127.0.0.1:5173/?debug=serial` in the browser that successfully ran
the original manual serial test. Close other serial clients normally. Do not
reset the device or change permissions/USB configuration. No flashing, OTA,
package installation/removal, direct storage write or recovery action is part
of this procedure.

1. Attempt ordinary Connect (E) once. Select Espressif USB Serial/JTAG. On
   failure, expand Connection details and preserve the entire trace before
   starting another attempt. In DevTools, obtain the live latest session with:

   ```javascript
   JSON.stringify(window.roundieDiagnostics(), null, 2)
   ```

   The separate firstError must identify the first failure/initiator. Inspect
   preceding PORT_OPENED, REQUEST_SENT, READ/WRITE, RESPONSE_MATCHED and
   TEARDOWN_REQUESTED events to distinguish origin from cleanup symptoms. A
   read rejection after timeout/teardown is secondary; a read rejection before
   those events is primary I/O evidence. Do not infer USB removal from the
   browser exception string alone.
2. When disconnected and cleanup has completed, expand Read-only serial
   diagnostics and run A, B, C, D once each, selecting the same authorized
   device. Preserve each trace before proceeding; only the latest is buffered.
   If cleanup is reported incomplete, stop the sequence; page reload is needed
   before attempting reuse. Do not reset Roundie.

   | Level | Sequence | Success evidence |
   |---|---|---|
   | A | Native minimal open, acquire reader, passive 1.5 s sample, cancel/unlock/close; no request/signals | Byte count or **idle**; input need not be spontaneous and a sampled chunk is not a full handshake |
   | B | Same minimal native open, single addressed RO1 STATUS through native reader/writer, close | Current request-ID reply with actual firmware/slot/phase |
   | C | Production WebSerialTransport (full options/existing signals), same single raw addressed RO1 reader | Same result; transport/options/signal setup differ from B as a group |
   | D | Same transport plus ProtocolClient RO1 STATUS | Matching reply through production pump/scheduler/parser |
   | E | Ordinary Connect | Complete RO1/RS1/RE1/RP1 status, LIST and INFO, actual applications visible |

   Record the first divergent level; a B/C difference alone does not identify
   DTR/RTS, because several setup details differ. No spontaneous status, byte
   flush or protocol retry is assumed. A/B intentionally do not set signals;
   C/D/E retain the existing production paired setting.
3. For E success, record actual firmware/slot/BootGuard/OTA fields and the full
   listed Hello/UI identities, versions and ABIs. Expected historical values:
   0.3.8 / ota_0 / stable, org.roundie.hello 1.1.0 ABI 1 and org.roundie.ui
   1.0.0 ABI 3. These are **not confirmed current values**. Stop and record any
   difference; do not change firmware or packages to force that baseline.
4. Disconnect normally, reconnect and repeat **five ordinary cycles**. Each
   must produce a complete snapshot/list, then clean intentional disconnect,
   with no phantom Connected state, browser error, or page reload required.
   Record `connectionMs` displayed in metrics, or elapsed time from
   HANDSHAKE_STARTED to HANDSHAKE_COMPLETE in each debug trace. No physical
   latency value has been measured yet and no acceptance threshold is invented.
5. During the bounded cycles, a separate read-only `journalctl -kf --no-pager`
   can capture USB kernel events. Record whether re-enumeration occurred, along
   with any observed unintended reset. Do not issue commands that reset it.

The physical gate passes only with full E handshake + actual registry/INFO,
ordinary repeatability, clean close, latency and USB/reset observations. Raw
open, a single RO1 response, or a mocked full handshake is insufficient.

## Preservation and remaining work

Only tools/roundie-manager and its documentation/tests changed. Firmware,
partitions, BootGuard, Safe Mode, SDK/ELF ABI, device protocol, RPK1 format and
PackageManager semantics remain unchanged. No device port was opened; no device
packages were installed, removed or modified. Fresh physical state readback is
pending, so preservation here means no mutating device operation was performed,
not a claim that a fresh package/Flash comparison passed.

Pending: capture the original causal sequence, determine the first divergent
physical level, establish whether the fixes restore full connectivity, complete
the five read-only cycles and list actual Hello/UI identities. If a trace shows
a different cause, fix that demonstrated issue within this host-only milestone;
do not add reconnect loops, increase response timeouts, or change firmware.

Changes are committed locally with a Conventional Commit; no push.
Final commit hash and Git status are reported in the task response.
