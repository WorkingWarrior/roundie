# Roundie Manager

Local TypeScript application for the existing REP Web Serial diagnostics and
RP1 PackageManager. **No firmware modifications or deployment are required.**
The original Milestone 3F USB MTP gate remains **NO-GO / OPEN**.

From the repository root:

```sh
cd tools/roundie-manager
npm install
npm run dev
```

Open the printed **http://127.0.0.1:5173** address in a browser exposing
`navigator.serial`. Click Connect and select ESP32-S3 USB Serial/JTAG. Close
Python clients, IDF monitors and other serial applications first. Browser
selection and Linux serial-device permission are both necessary. `file://`
is not a supported launch path.

The tested development host is CachyOS Linux with Node 26.11.1/npm 12.2.0.
Use a current Node release supported by the pinned Vite (Node 24+ recommended).
There are no production npm dependencies, remote fonts, telemetry or backend
serial proxy. Dependency installation needs npm access; builds and operation
work offline afterward. The lockfile pins the dependency graph; use `npm ci`
for a frozen clean install.

```sh
npm run typecheck
npm test
npx playwright install chromium
npm run test:ui
npm run build
npm run preview
```

`npm test` generates ignored vectors through the **unchanged** production
Python clients, compiles the firmware transfer parser for host validation,
and builds an example ELF in a temporary directory. These reference tests
require Python, CMake, a host C compiler, OpenSSL development files and the
repository's pinned Xtensa compiler. The frontend itself needs none of those
tools to run. Browser tests use an explicitly injected mock serial API,
not a physical Roundie.

`dist/` is a local static build, ignored by Git. Serve it from localhost or a
trusted HTTPS origin. Relative asset paths permit hosting below a URL prefix.
The production content policy permits only local scripts/styles; the dev
server adds a nonce for Vite's injected styles. No service worker is installed.

See [architecture, protocol and recovery](../../docs/roundie-manager.md) and
[acceptance / pending physical checks](../../docs/roundie-manager-milestone-3f-w-acceptance.md).

For connection failures, open **http://127.0.0.1:5173/?debug=serial**. Connection
details retain a bounded, local diagnostic trace; `window.roundieDiagnostics()`
returns the latest session. The debug-only A–D buttons compare passive raw input,
raw RO1 STATUS, transport RO1 STATUS and ProtocolClient RO1 STATUS. Ordinary
Connect is level E (complete status/registry/INFO handshake). These probes never
install, reset or retry a transaction. Passive probe A may legitimately be idle.
See the [3F-W.1 investigation and read-only acceptance procedure](../../docs/roundie-manager-milestone-3f-w1-diagnostics.md).
Physical connectivity remains PENDING until this procedure succeeds on Roundie.
