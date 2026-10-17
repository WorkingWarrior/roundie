# Roundie — Agent Instructions

## Authority and scope

- `docs/specification.md` (REP v0.4) is the architecture baseline. Read the relevant sections before changing architecture, ABI, storage, boot, or hardware behavior; read §23 and §25 for the current milestone and open decisions. The spec describes a target, not a list of features already implemented.
- Work only on the requested milestone and its validation gate. Preserve working behavior. Do not scaffold future subsystems or add a framework for a logical component merely because the spec names it; a component need not have its own task or singleton.
- Investigate existing code and ESP-IDF/Waveshare BSP facilities before adding a replacement. A proposed change to an established requirement needs an explicit architectural decision and corresponding documentation, not a silent implementation choice.

## Evidence and decisions

- In plans, reports, and design notes, distinguish **verified fact** (source, version, and test or measurement), **architectural decision** (spec section or recorded decision), **assumption** (unverified working premise), and **pending experiment** (question, method, and acceptance criterion).
- Treat the choices in spec §25 as open until measured or explicitly decided. Documentation or a successful build alone does not establish board behavior, loader compatibility, executable PSRAM, wake sources, MTP support, or recovery.
- Report host tests, target compilation, device tests, and long-running or fault-injection tests separately. State failures and unverified behavior. Never call compilation hardware validation.

## C, ESP-IDF, and components

- Implement firmware in C for ESP-IDF v6.1. Pin the exact IDF and critical component revisions after their integration gates; check APIs and Kconfig against those revisions. Waveshare examples target an earlier IDF and require compatibility testing.
- Prefer clear, debuggable code: descriptive identifiers; one statement per line; braces for every `if`, `else`, `for`, and `while`; whitespace around logical operators; explicit state transitions, ownership, and error handling. Avoid clever shortcuts, excessive helper layers, and speculative generalization.
- Keep hardware-independent logic host-testable. Put code in components by responsibility, declare component dependencies correctly, and keep private implementation types out of public headers. Prefer existing ESP-IDF and BSP functions when suitable.
- Use the new I²C master driver for the shared bus; do not mix it with the legacy driver. Check I²S and other migrations, default `gnu23`, and removed APIs. Do not disable `-Werror` or all warnings globally to make a port compile; document any narrow, temporary external-component workaround.

## Runtime, ABI, and ownership

- Preserve the separation between System Core, Application Runtime, SDK, and applications (spec §§4–10). In the MVP, run one external native Xtensa ELF at a time through one Application Executor. Applications receive events and return from callbacks; they have no private main loop, FreeRTOS tasks, background ELF execution, or shared dynamic libraries.
- Native ELF applications are trusted but unisolated code in the firmware address space. A symbol allowlist is an API/compatibility control, not memory isolation. The MVP excludes WASM, interpreters, a Linux-style linker, and processes; changing that scope requires an explicit architectural decision. Do not expose private system functions to applications.
- Expose a versioned public API table and an agreed entry point. Keep LVGL, FreeRTOS, ESP-IDF, and private libc structures out of the public ABI. Use fixed-width fields, explicit lengths/capacities and ownership, UTF-8 text, and target-toolchain layout assertions; keep SDK headers usable with C11 and C23. Do not assume 64-bit host layout matches Xtensa.
- Treat compiler/libc helpers, `errno`, TLS, relocations, and imported symbols as part of ELF compatibility. Verify them with the selected toolchain, libc, and loader before freezing ABI. Reject unsupported imports or relocations rather than partially executing a module; do not export all firmware symbols. Prefer an explicit SDK-managed allocator for application-owned memory.
- Assign every SDK resource to an application instance; check owner, type, and generation on handles. Reject stale events and handles. On stop, reject new work, request cancellation, wait for worker completion or cancellation acknowledgement, finish callbacks, detach subscriptions, release resources, invalidate handles, then unload ELF. No call path into unloaded code may remain. A cancellation request alone does not permit freeing memory a worker still uses.

## Concurrency, UI, and memory

- Keep LVGL object creation, mutation, deletion, rendering, and navigation in the GUI task. Application callbacks run in the Application Executor; long-running storage, network, OTA, package, USB, and audio work belongs in system workers. Do not synchronously wait for GUI work from an application callback.
- Design UI for the round 360 × 360 display from the outset; account for usable width and touch targets near its edges.
- Use bounded event and UI-command queues with explicit overflow/backpressure behavior, instance and operation identity, and stale-work rejection. Keep lifecycle commands serviceable. Treat a newly assigned UI handle as acceptance of a request, not proof that the LVGL object exists. Coalesce commands only when ordering semantics permit it.
- Make callbacks run to completion within a measured budget; prohibit unbounded waits, blocking network calls, and infinite loops. Supervise AppExecutor with the task watchdog. Choose priorities and core affinity from measurements; a lower priority does not isolate faulty native code.
- Budget SRAM, PSRAM, GUI buffers, loader peak memory, and emergency reserve. Do not infer executable ELF memory from ordinary PSRAM `malloc()`. Treat XIP from PSRAM and the final LVGL buffering strategy as measured choices. Flash writes can stall cache, PSRAM access, ELF execution, and GUI; measure latency and avoid waiting for Flash writes while holding conflicting Flash mappings.

## Hardware, storage, and recovery

- Use the **1.85B** board's verified pinout, not the non-B model's. Keep GPIO0 available for ROM Download; do not repurpose PSRAM pins. Do not assume LCD TE, a programmable PWR button, software power-off, or a wake source without the specified schematic and device checks.
- Do not present deep sleep as hardware power-off.
- Keep one managed I²C master for the shared devices. Treat physical behavior, battery-dependent behavior, current draw, and wake reliability as device-test questions.
- Never flash hardware unless the task explicitly calls for it. Never change eFuses, enable Secure Boot/Flash Encryption/hardware anti-rollback, disable ROM Download, or irreversibly change USB recovery capability without a separately tested recovery plan and explicit approval.
- Do not auto-format LittleFS on mount failure or auto-erase NVS on initialization errors. Do not erase NVS, LittleFS, or all Flash without explicit approval. Installation and recovery must preserve user data by default; do not overwrite an active ELF or expose raw system partitions through MTP.
- Use the standard ESP-IDF bootloader with two OTA slots and application-confirmed rollback. Confirm an OTA image only after the critical self-test; do not begin another OTA while confirmation is pending. Safe Mode is in the firmware and must start without LittleFS, apps, registry, MTP, or Wi-Fi. Preserve a manually usable ROM Download recovery path, including when TinyUSB takes over the normal USB interface; verify the procedure on the actual board.

## Validation and documentation

- Follow the spec §23 order and gates: hardware audit; IDF/BSP bring-up; storage/OTA/recovery; ELF proof of concept; runtime; round UI; packages/SDK; MTP; power; stabilization. The ELF proof of concept is a critical gate: if it fails, review the architecture before building the full runtime or stabilizing ABI.
- Use focused host tests for pure logic, target builds for toolchain/layout issues, and actual board tests for hardware, Flash/PSRAM concurrency, OTA, recovery, UI responsiveness, and power. Measure before optimizing; include failure paths, cleanup, cancellation, power interruption, and repeated load/unload where relevant.
- Document significant decisions and behavior changes when they are made. Keep confirmed behavior separate from proposals and pending experiments. Create detailed component documents as implementation reaches them, rather than copying the full specification into new files.
