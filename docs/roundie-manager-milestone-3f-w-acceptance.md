# Milestone 3F-W — Roundie Manager acceptance, 2026-10-20

**Result: runnable host application delivered; software gates PASS; physical
Web Serial acceptance PENDING.** This is not a claim of browser-to-board
installation or physical milestone completion. The task explicitly permits
delivery with a manual procedure when actual browser/device access is absent.
The original **Milestone 3F — USB MTP remains NO-GO / OPEN**. Milestone 3G
was not started.

Initial tree: clean `bc5a895`. Implementation: `95cd474`
(`feat(manager): add local Web Serial package management with RP1 parity tests`).
The following documentation commit records this evidence and adds deployment
links. Final commit hashes and Git status are supplied with delivery.
No push, firmware build/deployment, OTA, device serial command, package mutation,
raw Flash operation, PHY change, reboot or recovery action was performed.

## Verified software evidence

Host: **CachyOS Linux, x86_64, kernel 7.2.9-2-cachyos**, Node **26.11.1**, npm
**12.2.0**. Exact direct development dependencies: TypeScript **7.0.2**, Vite
**8.3.4**, Playwright **1.64.1**, Node typings **24.10.1**; lockfile committed.
Browser: Playwright **HeadlessChrome 156.0.8078.4**. Playwright used its
Ubuntu 24.04 fallback browser build because CachyOS is not an officially
supported Playwright distribution. That browser ran successfully on this host.

| Evidence tier | Command / method | Actual result |
|---|---|---|
| Dependency install | `cd tools/roundie-manager && npm install --ignore-scripts` | PASS, 22 packages installed, no reported audit vulnerabilities at install time |
| Frozen dependency install | `npm ci --ignore-scripts` | PASS, 22 packages; committed lockfile consumed |
| TypeScript | `npm run typecheck` | PASS, exit 0, strict production and test type checks |
| Golden/reference and unit tests | `npm test` | **78 tests PASS**, exit 0; final Node run 160 ms, excluding reference generation |
| Existing host regression | `. /home/solve/.espressif/v6.1/esp-idf/export.sh` then `python -m unittest discover -s tests -p 'test_*.py' -v` from repository root | **82 tests PASS**, exit 0, 15.814 s; unchanged suite, applicable C/ASan/UBSan and mocked managers |
| Browser software acceptance | `npm run test:ui` | **15 tests PASS**, exit 0, 3.8 s; serial behavior is injected mock except native feature-detection test |
| Native browser feature detection | Unmocked Chromium on localhost, no Connect click | `isSecureContext=true`, `"serial" in navigator=true`; disconnected page, no permission prompt |
| Static build | `npm run build` | PASS, exit 0; CSS 5.05 kB, JS 25.79 kB, HTML 0.66 kB; JS gzip 9.84 kB |
| Build reproducibility | Rebuild after clean `npm ci`, compare every dist file SHA-256 | All three static artifacts byte-identical on this host |
| Production preview software checks | `MANAGER_PREVIEW=1 npx playwright test --grep 'new install requires\|dark/light theme\|connect, read status\|desktop styles'` | **4 tests PASS**, exit 0, 1.8 s; actual static assets and CSP, simulated serial device |
| Visual inspection | View generated 390px mobile dark and 1280px desktop light screenshots from the actual local application | CSS/layout inspected; metadata shown comes from the explicit test model. No physical LCD or human device observation claimed |
| Whitespace/scope | `git diff --check`, staged diff review, production-source comparison against initial HEAD | PASS; changes limited to frontend/tests and documentation |
| Production target compilation | Not required/repeated for a host-only change | No new firmware artifact or hardware claim |

The final build hashes, which also matched the preceding clean-install build:

```text
09a24c6d0e1113d0b98785f9749ea657e4b0d94b6df70ae26b33c605abddb6b3  assets/index-CEWxKbNK.css
07d10d1b644f3f4e06e528aab5341659ec2052782cc06a11dde7130626c7785f  assets/index-CuY_u3YA.js
54fbf0ed957f36f52ce6fca6434cb9f2c9d8c93e45a0e260ade93c1360f90b63  index.html
```

Raw test output, generated packages/vectors, browser downloads, screenshots,
node_modules and dist are local/ignored and are not committed. Reference tests
build only temporary example ELF artifacts and a host C parser harness; they
do not build or deploy Roundie firmware. The sample reply fixtures are
Python-validated, not captured physical replies.

Initial verification failures were corrected rather than hidden: TypeScript
found a missing CSS module declaration, a nullable connection reference and
unused/test typing issues. The first screenshots revealed that the dev CSP
blocked Vite-injected styles despite functional tests passing. The development
server now supplies a style nonce; tests explicitly check computed CSS, and
production retains external local styles. Final checks above pass after the
corrections. The production Python clients and firmware parser were unchanged.

## Protocol and behavior coverage

Eleven request vectors use the actual Python request/write path: RP1 STATUS,
LIST, INFO, BEGIN, short binary DATA, maximum 512-byte binary DATA, END,
INSTALL, ABORT, RO1 STATUS and RS1 STATUS. The real firmware C transfer parser
accepts the applicable transfer vectors. TypeScript request bytes match
exactly, including the leading LF, lowercase hex and trailing LF. Two response
vectors are validated by the actual Python RP1 parser (success and rejection).
There is no frame CRC in this protocol; integrity uses package SHA-256.

Tests cover fragmented and coalesced replies, single-byte reads, bounded noisy
logs, wrong/late/out-of-order IDs, malformed JSON/length/schema, unknown error
codes, short-write rejection, timeout, disconnect and concurrent-command refusal.
RPK1 tests use the real production builder/C inspector and verify metadata,
full/embedded hashes, header/manifest sizes, truncation, oversized allocation
refusal, identifier/UTF-8/control/reserved fields, target/API/ABI and corruption.

Transaction software tests cover multiple acknowledged chunks, BEGIN/DATA/END/
INSTALL rejections, old-version retention in the model, cancel before BEGIN,
cancel during DATA with ABORT confirmation, failed ABORT, loaded/suspended ELF,
Safe Mode, storage reserve, same-version refusal, concurrent upload refusal,
USB removal during DATA/INSTALL, fresh-session reconciliation, still-busy
reconciliation, mismatched INFO and a device error after active selection that
is independently reconciled as success. The unchanged C regression suite adds
real manager logic coverage; neither tier proves physical power-loss durability.

Browser tests cover native feature detection without permission prompts,
unsupported API, chooser cancellation, permission denial, busy-port guidance,
status/list, reconnect, new/update preview, confirmation, file selection,
drag/drop, hostile filename text handling, progress/cancel, verified completion,
DATA/INSTALL disconnects, commit reconciliation, keyboard focus, 390px layout,
light/dark desktop and maximum-size hashing. Serial permission and busy-port
exceptions are simulated; they are not real OS permission/ownership tests.

## Performance and memory evidence

One final **real browser, local file inspection** sample for a derived maximum
65760-byte RPK1 envelope (not a deployable padded ELF): complete inspectFile
**1.10 ms**, full-package plus embedded SHA-256 portion **0.10 ms**.
With Chromium precise memory reporting enabled, used JS heap changed from
**2,157,280 B to 2,381,566 B**, a **224,286 B** increase. This is a single
before/after heap sample including browser allocation overhead, not peak RSS
or a cross-browser memory guarantee. Input is bounded before File.arrayBuffer;
Web Crypto is not treated as a streaming digest API.

Actual connection/open time, status and listing latency, USB DATA transfer,
INSTALL duration, INFO latency and effective payload throughput are **PENDING**.
No physical speed was inferred from the USB Full-Speed line rate or mock timers.
DeviceService records status/list and full diagnostic handshake timing;
DeploymentCoordinator records hash/transfer/commit/verification and acknowledged
bytes per elapsed transfer second. Those are bounded samples, not worst-case
guarantees; a handshake timer excludes the native chooser/port-open interval.

## Physical acceptance — PENDING and manual procedure

The CUA inventory reported **no connected browsers or native applications**.
The available automated browser was headless and had no user-approved real
serial chooser/device path. No Python serial substitute was used to claim
browser acceptance. No physical browser-to-Roundie operation was attempted.

The task's expected baseline is A/ota_0 **0.3.8 VALID / selected / STABLE**, B
**0.3.9 VALID / unselected**, OTA IDLE, Hello **1.1.0 ABI 1**, UI **1.0.0 ABI 3**.
It is an expected/historical baseline, **not a fresh observation in this run**.
Physical verification of preservation remains pending; our work made no
device writes or installations.

Before any installation, complete these read-only actions in order:

1. Connect the 1.85B by its normal USB route. Open Manager on localhost in an
   interactive browser exposing Web Serial. Confirm the browser detects it.
2. Click Connect and select the actual ESP32-S3 USB Serial/JTAG port. Confirm
   firmware 0.3.8, ota_0 running/selected VALID, BootGuard STABLE, normal mode,
   OTA IDLE, healthy storage and no loaded/reserved ELF. Stop if any differs.
3. Confirm LIST/INFO shows exactly the expected existing Hello/UI identities,
   versions and ABIs; record their full digests. Manager cannot invent an
   alternate-slot version the diagnostics do not return. Use the previously
   approved image identity record if alternate-image verification is needed.
4. Disconnect Manager before running the unchanged Python `list` and `info`
   clients on the actual port. Compare the returned registry metadata/digests.
   Close Python, reconnect Manager and confirm it reads the same values.
5. Verify manual Disconnect/reconnect. Separately hold the port with an ordinary
   read-only serial client and check that a failed browser open gives guidance;
   release that client before proceeding. Do not assume the browser enforces
   cross-process OS exclusivity on every platform.

Only after those checks pass, at most one bounded installation is allowed by
the task, if a vetted harmless dedicated `.rpk` is available:

1. Inspect it with the existing production Python package inspector. Record ID,
   version, ABI, size and **full** SHA-256. Ensure it replaces neither Hello nor
   UI, fits the eight-ID limit and available storage including reserves, and
   the device remains unloaded/idle. Do not improvise direct storage writes.
2. Drop/select that package in Manager. Verify metadata/digest and **New
   installation**. Explicitly confirm trust/installation and click Install.
3. Observe acknowledged bytes, END, INSTALL and independent INFO. Accept success
   only for exact ID/version/ABI/full digest/ELF size with healthy record.
4. Refresh/reconnect, verify the new package plus unchanged Hello/UI digests and
   unchanged firmware/boot state. Do not launch it automatically or initiate a
   second physical upload to test failure injection.
5. Leave the dedicated package installed and report it unless a separately
   verified established PackageManager removal path is safely used. Never
   remove raw LittleFS paths. No reset, OTA, formatting or raw Flash cleanup.

Chooser permission, real browser handshake, package-list parity on the board,
physical stream locking/cleanup, DTR/RTS behavior, port-conflict handling,
drag/drop-to-device installation, device transfer timing and unchanged-device
readback are all **PENDING**. Physical unplug during DATA/INSTALL, long-running
or power-interruption experiments were not performed and must not be inferred
from the deterministic failure tests. There is no request for the user to
perform an interaction before the authorized host work can finish.

## Explicit final answers

| Requested question | Answer |
|---|---|
| UI framework and why? | Framework-free strict TypeScript + Vite: small bounded UI, clear state and no runtime dependency overhead |
| Web Serial implementation? | Native navigator.serial / browser streams, no serial library, WebUSB or proxy |
| Browser and host tested? | CachyOS x86_64, Node 26.11.1/npm 12.2.0, HeadlessChrome 156.0.8078.4; interactive physical browser PENDING |
| Port ownership? | One transport reader/writer pair; one protocol receive pump; one pending command; UI operation gate; cancellation/abort/lock-release teardown |
| Byte-for-byte Python RP1 parity? | PASS against generated authoritative request vectors and C transfer parser; no device-capture claim |
| RPK inspection? | PASS with actual builder/C-validated reference, bounds/malformed inputs and Web Crypto hashes |
| Drag-and-drop installation? | Browser software PASS using mock native serial streams; real device installation PENDING |
| Progress based on ACK bytes? | Yes; exact received/session/total/phase checks; transfer completion remains distinct from commit |
| Independent commit verification? | Yes, fresh INFO checks ID/version/ABI/health/full digest/ELF size before success |
| USB disconnect during DATA? | Reject waits, no duplicate chunk/resume, preserve interruption, cleanup unverified until fresh status |
| USB disconnect during INSTALL? | Preserve unknown outcome; reconnect handshake and INFO, never replay INSTALL; exact match resolves success |
| Loaded-ELF write restrictions? | Preserved by existing firmware admission plus fresh UI preflight, including suspended/stopped-but-loaded; host refusal tests PASS |
| Observed transfer performance? | No physical throughput measured. Maximum envelope inspection 1.10 ms, hashing 0.10 ms in one local browser sample |
| Actual browser-to-Roundie operations? | None |
| Remaining manual items? | All physical chooser/permission/handshake/parity/port-ownership/install/performance/preservation checks above |
| Packages installed or modified? | None on device. Temporary host test package generated in ignored fixtures only |
| Firmware unchanged? | Yes, no production firmware files or device firmware modified/deployed; no fresh device readback claim |
| Future MTP integration? | Separate restricted-staging frontend to the same PackageManager; no requirement to tunnel RP1; MTP NO-GO remains open |
| Outside scope? | Firmware/OTA/USB PHY/descriptors, boot/partitions/eFuses, ABI/format/schema, launcher/LCD, autostart, app store/network deployment, WebUSB/FUSE/proxy, MTP implementation and 3G |

Launch instructions: [Manager README](../tools/roundie-manager/README.md).
Architecture, exact protocol and recovery semantics: [Manager design](roundie-manager.md).
