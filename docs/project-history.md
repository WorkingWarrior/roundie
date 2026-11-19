# Project history: REP stages on the 1.85B

Before the shared Roundie Core ([roundie-core.md](roundie-core.md)), Roundie
was firmware for the Waveshare ESP32-S3-Touch-LCD-1.85B alone. Its stages
follow the specification (§23). This page keeps the checkpoint that the README
used to carry. It records earlier device tests, not a fresh check of any board.
The detailed evidence is in the linked documents.

## Checkpoint, device results recorded on 2026-11-04

- Normal firmware 0.3.48 was VALID in `ota_0`, running under XIP from PSRAM
  ([audio A3 results](audio.md#gate-a3-results-2026-11-04-firmware-0348-orgroundiesound-100)).
- REP stages 0–4 are done.
- Stage 5 has the launcher and the [Round UI widgets](round-ui.md)
  (RoundList, ArcSlider, EdgeButton, RoundDialog, shadow state). They were
  verified on the device, by injected input and by a person's touch.
- Stage 6's example is a [watch face](watch-face.md) with local time, a time
  zone and clock hands instead of the calculator, plus a
  [stopwatch](stopwatch.md). Stage 6 has packages, but no SDK calculator
  example.
- Stage 7 uses a [USB mass-storage boot mode](usb-storage-mode-plan.md)
  instead of MTP: copy `.rpk` files to `Inbox`, eject, and confirm the import
  on the device.
  - Gates G1–G4 have recorded results.
  - G5 stabilisation and the Windows, macOS and Android checks remain pending.
  - Roundie Manager over Web Serial is also available.
- [Audio](audio.md) has a PCM stream engine, `rep_audio_v1` and a sound
  application, up to gate A3. A4 validation remains pending.
- Stage 8 has screen off, the BOOT power menu and light sleep, verified on the
  device. Current measurements and deep sleep remain open
  ([PowerManager](power-manager.md)).

## Flash layout and OTA history

- The connected 1.85B was migrated to the REP v1 layout in Milestone 2B.
- The build's default profile is still `legacy`, so `rep` must be selected
  explicitly.
- The approved Milestone 2C.1 A-to-B OTA passed, and so did the separately
  approved automatic rollback.
- An earlier authorized restoration on 2026-10-19 left both slots with normal
  firmware 0.2.1 and the armed rollback test disabled. The later results above
  supersede that state.
- The historical bring-up updater (`tools/flash_bringup.py`) rejects the
  physical REP table.

Deployment limits and recovery: [flash-migration.md](flash-migration.md#rep-development-updates).
