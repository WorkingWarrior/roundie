# Milestone 3F acceptance — 2026-10-20

**Result: feasibility gate NO-GO; safe non-deployed proof of concept delivered.**
MTP application deployment is **not complete**. No OTA candidate was built,
no PHY was switched and no firmware was deployed. A 0.3.8 remains selected;
experimental B 0.3.9 remains installed. Milestone 3G was not started.

The initial Git tree was clean at `d92f725`. Changes are documentation and
`examples/usb_mtp_probe/` plus narrowly scoped ignore entries. Production
firmware sources/configuration/dependency lock, ABI, protocols, partitions,
bootloader and installed packages are unchanged.

## Gate findings and decision

1. **Verified source:** pinned IDF v6.1 can assign the internal PHY to OTG
   through software registers, without eFuse changes. USJ loses the cable.
2. **Verified host/target:** a CDC+MTP configuration fits the S3 endpoint budget
   and both released class implementations compile together. This does not
   establish enumeration or concurrent real traffic.
3. **Verified source blocker:** released esp_tinyusb 2.4.0 and 3.0.0 MTP storage
   registration/capacity are FATFS-specific. Direct filesystem mutation has
   no public Roundie admission hook, and runs in the USB service path.
4. **Unresolved hardware gate:** Rev1.1 shows no independent USB VBUS sense
   signal to the MCU for battery-powered attach/detach. Actual PCB revision
   and electrical behavior are pending. Do not claim standards-compatible
   self-powered USB from an always-valid VBUS override.
5. **Unresolved recovery gate:** the prior manual ROM test began from USJ;
   ROM entry/Safe Mode transport after OTG takeover is untested. Pinned
   `usb_del_phy()` does not restore the USJ mux, and disables the peripheral
   without resetting it. CPU-only reset/re-enumeration is not sufficient proof.

See [architecture and exact sources](usb-architecture.md), [storage adaptation,
ownership and memory](usb-mtp.md), and [current deployment availability](application-deployment.md).
The smallest adapter was investigated and documented; no future subsystem
was scaffolded past these failed prerequisites.

## Host and target verification

| Tier | Command/evidence | Result |
|---|---|---|
| Existing host regression | Export pinned IDF, then `python -m unittest discover -s tests -p 'test_*.py' -v` | **82 tests, PASS**, exit 0, 14.616 s; includes existing applicable ASan/UBSan C harnesses and mocks |
| New descriptor host checks | `python examples/usb_mtp_probe/check_descriptors.py -v` | **5 tests, PASS**, exit 0; actual pinned macros compiled as C11 with `-Wall -Wextra -Werror` |
| Dependency resolution | Probe `idf.py ... reconfigure` | PASS, exit 0; exact 3.0.0 registry component and IDF 6.1.0 in isolated lock |
| ESP32-S3 target component compilation | Probe `idf.py ... __idf_main __idf_espressif__esp_tinyusb` | PASS, exit 0; actual CDC, MTP, DWC2 and Espressif backend objects compile, including final descriptor source |
| Probe deployment guard | `cmake --build examples/usb_mtp_probe/build --target probe_flash_forbidden` | **Expected refusal**, exit 1, before any device command |
| Production REP image build | Not repeated: no production code/config changed, and the gate forbids proceeding to an OTA candidate | No new firmware image or full-image validation claim |
| Descriptor completeness | 105 configuration bytes; interfaces 0,1,2; CDC IAD/union; six unique endpoint addresses; legal FS packet sizes | Structure checks pass; full device/string descriptors and VID/PID are deliberately absent |
| Static resource evidence | Target archive section sizes and DWARF sizes in `usb-mtp.md` | Measured object sizes only; runtime budget remains PENDING |

The first configuration command used a relative `-B build` with `-C` and was
refused by CMake because that directory belongs to the repository's original
project (exit 2). No source/configuration was overwritten; the retry uses the
probe's absolute build path. Host descriptor setup initially failed because
`tusb.h` pulls unrelated DeviceInfo configuration; direct class constant and
descriptor headers corrected that. Initial expectations were corrected to the
actual pinned CDC protocol NONE and 105-byte layout before the passing run.
These setup failures are not USB hardware failures.

The probe intentionally has no `app_main`, storage registration, driver startup
or VID/PID. Only component archives were produced, no deployable BIN/ELF.
Build logs/cache and downloaded managed sources are ignored. Root
`dependencies.lock` remains unchanged. No warning mask was added by this work;
upstream's existing narrow `usbd.c` type-limits option remains upstream policy.

## Bounded physical evidence: existing firmware, read-only

The attached unit was queried through its explicitly selected existing USJ
serial path using the current host libraries. Only **STATUS**, RT1 **GET**,
RP1 **INFO** for the two installed IDs and display **PERF** reads were issued.
There was no reboot, mode change, write/erase, eFuse access, ROM operation,
full-Flash read, app launch or physical button/cable interaction.

| Service | Fresh observation |
|---|---|
| RO1 | IDLE; A/ota_0 running and selected 0.3.8 VALID; B/ota_1 VALID, compatible image; rollback enabled; boot checked/passed; error 0 |
| RS1 | Normal, no safe latch/failures, STABLE stage 4, no pending restart; alternate compatible; internal free sample 213,967 B |
| RT1 | OK, RTC OS=0, no driver error. No clock set/sync or accuracy/drift claim |
| RE1 | UNLOADED, busy=0, no callbacks/allocations/resources, loader 1.3.5 |
| RP1 | IDLE, two packages, zero orphans; LittleFS capacity read succeeds; worker minimum stack 5,040 B |
| Display | Existing PERF request parsed successfully; no new display performance experiment |

Running A ELF identity:
`23b9ba7c13083f5bdb21177d6c90722468b458efab55b79d61af749d885c321c`.
Alternate B ELF identity:
`64f714841eccff8e3d5b0fbcfdf33b7a24cf09875354c4a897bdcdd71f8ed68a`.
The current protocol reports the running version, not the alternate's version;
B's 0.3.9 identity is matched against the previously frozen 3E.4 manifest,
not inferred from merely seeing another VALID slot.

Fresh RP1 INFO revalidated these committed packages, matching 3E.4:

| Package | Version / ABI | Full-package SHA-256 |
|---|---|---|
| `org.roundie.hello` | 1.1.0 / 1 | `9a36506f21136344e557235aaa8caa532a52551dd9eb28936ffa3a8a6d054cd1` |
| `org.roundie.ui` | 1.0.0 / 3 | `76ab4c5be328397900de02784f0d5f3f8660a56ba82e974bf5b4c8e17d5bfcd7` |

External frozen images were rehashed without device reads or modification:

- Known-good 0.3.8:
  `/mnt/Dane/Development/roundie-backups/display-3e3-2026-10-20/candidate-c/roundie_rep.bin`,
  **730,576 B**, SHA-256
  `dafdb8ec50a887d3623b6b253a3464160f50dfbaa15207011a3ac90a3d9a48a8`.
- Experimental 0.3.9:
  `/mnt/Dane/Development/roundie-backups/display-3e4-2026-10-20/candidate/roundie_rep.bin`,
  **730,576 B**, SHA-256
  `0639b5d9dee35289a78f047996b0f68162befb3953707a54f2288925e2a81eb5`.

This is an application-level read-only checkpoint, **not** a new deployment
preflight or independent physical Flash/PSRAM/partition/bootloader audit.
Existing 3A–3E.4 and ROM recovery evidence remains intact. Those earlier
tests retain their own evidence scope and dates.

## Explicit answers to the requested final questions

| Question | Answer |
|---|---|
| Can the existing port support MTP without hardware changes? | The OTG controller/pins support MTP in principle without eFuses. Safe standards-compatible use across this battery board's operating states is **not established**; VBUS sensing is unresolved |
| How is internal PHY assigned? | Current firmware uses USJ. Pinned IDF can software-select OTG via RTC mux bits, with no fuse burn; not exercised on hardware here |
| Composite CDC+MTP in use? | **No**. Only the compile/descriptor probe has both enabled |
| Original Serial/JTAG? | Preserved on installed firmware. It would lose the USB cable during normal OTG ownership; CDC replaces diagnostics, not JTAG |
| ROM and firmware recovery? | Existing USJ recovery and A/B preserved; historical BOOT-at-power-on ROM route documented. Recovery after OTG takeover remains PENDING and blocks OTA |
| Implementation/versions? | Non-deployed probe: esp_tinyusb 3.0.0, bundled TinyUSB `894ea014...` (0.21.0~2 line), pinned IDF v6.1 `fff9895...`. Production has no TinyUSB dependency |
| Safe restricted LittleFS backend? | **No shipped adapter selected**. Released FATFS checks and direct mutation require a narrow backend/worker adaptation; design and exact blocker sites are recorded |
| Real Linux `.rpk` transfer? | **Not tested/not available**; no MTP firmware enumerated |
| Installation without RP1? | **Not available**; on-device confirmation is not implemented |
| RO1/RS1/RT1/RE1/RP1 working? | Fresh read-only requests all succeed on existing USJ 0.3.8; operation-preserving CDC migration is unimplemented/unverified |
| Flash/transaction problems? | Source integration hazard identified: direct backend writes bypass admission, and PackageManager BEGIN expects a hash ordinary MTP does not supply. No device mutation/corruption observed or attempted |
| USB memory use? | Probe pre-link archive data+bss 9,967 B; dynamic record arrays 1,556 B; default task stack 4,096 B before overhead. These are partial static costs, **not** live integrated consumption |
| USB throughput? | **Not measured**. Full-Speed 12 Mbit/s is not payload throughput |
| Final OTA A/B? | A 0.3.8 VALID/STABLE selected/running; B 0.3.9 VALID/compatible unselected, unchanged. No 0.3.10 candidate deployed |
| PENDING? | VBUS/actual PCB, post-OTG ROM/Safe Mode, authorized VID/PID/full descriptors, CDC migration/all protocol behavior, restricted backend and confirmation/commit integration, runtime RAM/stack, real MTP/Linux/GUI transfer, cancellation/reconnect, loaded-ELF refusal, native execution after MTP install and genuine OTA acceptance |

## Commit and final status

The probe and documentation are committed as focused Conventional Commits;
exact hashes and final working-tree status are reported in the delivery reply.
No generated artifacts, firmware images, RPK/ELF binaries, raw logs or private
USB identities are committed. No push is performed. The NO-GO decision is a
completed feasibility deliverable, **not a PASS for Milestone 3F**.
