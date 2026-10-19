# RPK1 development package format — Milestone 3D

**Architectural decision:** REP v0.4 §§13.3–13.5, 21.3–21.4 and open choices
§25.12/13/15 permit a small binary manifest and atomic activation record.
RPK1 chooses an uncompressed deterministic `.rpk` containing one fixed envelope,
one fixed manifest, and one native ELF. It is an internal storage/transport
format, not a public C structure or a change to application ABI 1/2/3.
No scripts, hooks, paths, assets, icon, dependencies or extra sections exist.

All integers are unsigned little endian, decoded bytewise. No native alignment
or padding is serialized. Exact total length is mandatory; extra/truncated bytes
are rejected. All reserved bytes and unused string padding must be zero.

| Envelope offset | Bytes | Meaning |
|---|---:|---|
| 0 | 4 | ASCII `RPK1` |
| 4 | 2 | Format version = 1 |
| 6 | 2 | Envelope length = 64 |
| 8 | 4 | Total file length = 224 + ELF length |
| 12 | 4 | Manifest length = 160 |
| 16 | 4 | ELF length, 52..65536 |
| 20 | 4 | Reserved zero |
| 24 | 32 | SHA-256 of exact ELF bytes |
| 56 | 8 | Reserved zero |

The manifest begins at file offset 64. Fixed field positions eliminate duplicate
keys, contradictory aliases, variable parser allocation and extraction paths.
Changing schema requires another explicitly supported format version.

| Manifest offset | Bytes | Meaning |
|---|---:|---|
| 0 | 64 | ID: NUL terminated and zero padded |
| 64 | 64 | Display name: canonical UTF-8, 1..63 bytes, NUL/zero padding |
| 128 | 6 | Version: three uint16 components, major/minor/patch |
| 134 | 2 | Required REP ABI: 1, 2 or 3 |
| 136 | 16 | Platform: `esp32s3.rep1`, NUL/zero padded |
| 152 | 2 | Required platform API generation = 1 |
| 154 | 6 | Reserved zero |

ELF starts at file offset 224. Maximum package size is **65760 bytes**.
Version text on the host/wire is canonical decimal `MAJOR.MINOR.PATCH`, each
0..65535, no leading zero except `0`, no suffix or prerelease syntax. Comparison
is numeric tuple order. Display name is independent of ID/version; controls,
invalid/overlong UTF-8 and surrogates are rejected. JSON diagnostics escape
quotes/backslashes. ID grammar is `[a-z][a-z0-9]*(\.[a-z][a-z0-9]*)+`, 3..63
ASCII bytes. Segments start with a letter. Case folding, Unicode, hyphens,
empty segments, separators, `..`, absolute paths and controls are unavailable.

The C decoder is shared by host inspection and firmware. Installation and launch
both verify persisted full-package SHA-256, decode the envelope/manifest, require
the current platform/API/ABI, verify payload SHA-256, invoke the **existing**
`roundie_elf_validate()`, and require matching ELF `.rep.abi`. Entrypoint,
relocations, imports and memory limits remain the Milestone 3A contract; the
existing ELFManager then validates again before accepting execution. No second
ELF validator or broad symbol export was added.

No capability identifiers are declared in revision 1: the fixed ABI defines
the entire available development service surface. There is no capability
permission system. Platform API 1 identifies the package-enabled REP contract;
ABI remains independently explicit. Optional description/build ID/minimum
firmware strings were omitted to keep this revision bounded.

## Offline tools and reproducibility

Use the pinned GCC 15.2.0 environment for standalone ELF builds. Host inspection
requires a C compiler and OpenSSL development headers/libcrypto; device hashing
uses pinned IDF PSA Crypto. There are no package parser dependencies.

```sh
cmake -S examples/hello_elf -B examples/hello_elf/build
cmake --build examples/hello_elf/build
python tools/roundie_package.py build examples/hello_elf/build/hello.app.elf \
  --output /tmp/hello.rpk --id org.roundie.hello --name 'Roundie Hello' --version 1.0.0
python tools/roundie_package.py inspect /tmp/hello.rpk
```

The builder derives ABI from the real preflight, embeds exact ELF bytes/digest,
and validates the complete candidate before publishing the output. There are
no timestamps, random IDs, host paths or mutable build metadata. Identical ELF
bytes and metadata produce identical package bytes. Full file SHA-256 is the
transfer and activation identity; ELF SHA-256 identifies only its payload.

## Development trust boundary

**Decision:** this is the development profile described by REP §21.4. Unsigned
packages are trusted development artifacts accepted only through the existing,
physically connected, unauthenticated USB diagnostic transport. Install and
launch are separate explicit requests. SHA-256 proves byte integrity relative
to a declaration, not publisher identity, safety or trust. Native ELF runs in
the firmware address space without process isolation. The import allowlist
controls API compatibility and does not sandbox native code.

The REP production signing requirement is preserved. Production package
distribution, signature algorithms/key provisioning and disabling all
development paths remain outside 3D; this format does not claim production
trust. No eFuse, Secure Boot, Flash Encryption or recovery changes are made.
