# Audio output — plan and results

**Status, 2026-11-04: A0 and A1 in progress.** The user chose the speaker only
(no microphone in this milestone) and an application API from the start
(`rep_audio_v1`). Audio is named in the specification only as a system worker
(§5.2) and in the hardware list (§3); this page is the design record. Labels
follow AGENTS.md: **fact**, **proposal**, **assumption**, **experiment**.

## Hardware facts (Rev1.1 schematic, hash `d4333dcc…9b6d`, drawing only)

| Part | Fact |
|---|---|
| Codec | ES8311 (U9), I²C address **0x18** (the drawing labels it; CE is pulled low by R35). SCL/SDA on the shared bus GPIO10/11 with 2.2 kΩ pull-ups (R44, R45) |
| I²S | MCLK GPIO2, BCLK GPIO48, LRCK GPIO38, data to the codec GPIO47, data from the codec GPIO39 (not used here); the IO table agrees with `docs/hardware-audit.md` |
| Analog supply | A3V3 from an RT9193-33 LDO (U8) fed from VCC and enabled by `PWR_N_EN`, the same enable as the 3V3 buck |
| Amplifier | NS4150B class-D (U10): differential input from the codec OUTP/OUTN through 100 nF and 150 kΩ; **CTRL = `PA_CTRL` = GPIO9** with a 10 kΩ pull-down (R42), so the amplifier is off until driven high; supply is the VCC rail |
| Speaker | Output VoP/VoN goes to a 2-pin header **H1** (0 Ω links R39/R43, filter capacitors not fitted). No speaker is drawn on the board: one must be connected to H1 |
| Strapping | GPIO2, 9, 38, 39, 47, 48 are not strapping pins on the ESP32-S3 |

## Decisions (proposals until the user objects; defaults in force)

- **Codec driver:** Espressif `esp_codec_dev` (the library the Waveshare board
  support uses) instead of a register-level driver of our own (AGENTS.md: look
  for existing facilities first). Its compile on the pinned IDF 6.1 was checked in
  a scratch project: version **1.6.2** with the ES8311 driver, the I²C control
  interface on an existing `i2c_master_bus_handle_t` and the I²S data interface
  built without warnings (compilation only, not a hardware result).
- **One I²C master:** the codec shares the board's managed bus
  (`roundie_board_i2c_bus`); no second master and no legacy I²C or I²S driver.
- **Playback only,** 16-bit stereo PCM, sample rates fixed per stream. No
  microphone, no ES7210, no recording.
- **A system audio task** feeds the I²S DMA; applications never own a task or the
  codec. An application writes PCM into a bounded ring through `rep_audio_v1`
  (gate A3); the ring and every handle belong to the application instance and
  are released on unload before the ELF is freed.
- **Volume is capped by the system** (`ROUNDIE_AUDIO_MAX_PERCENT`, 50 as a first
  value). The speaker is unknown and the amplifier runs from the battery rail;
  the cap is an assumption to be replaced by a measurement.
- **The amplifier is on only while a stream is open** (`PA_CTRL` high), and light
  sleep is refused while audio plays.

## Gates

| Gate | Content | Acceptance |
|---|---|---|
| A0 | This page: facts, decisions, compile check | Reviewed |
| A1 | `roundie_audio` component: I²S, ES8311 and amplifier bring-up; a tone from a development command | Host test of the tone generator; device: codec ACKs at 0x18 and reports its ID, no I²S or I²C error, touch and gauge reads keep working during a tone; **audible output PENDING the user** (needs a speaker on H1) |
| A2 | Stream engine: ring, volume cap, underrun policy, stop on request; power interplay | Host tests; device: no underruns at the chosen buffer, sleep refused while playing |
| A3 | `rep_audio_v1` for applications: ABI, validator allowlist, per-instance resources, example application | Host and target checks; device: playback from an application, stop on unload with no call into unloaded code |
| A4 | Validation | Repeated start and stop, unload while playing, load and latency measurements |

## Pending experiments

| Question | Method | Acceptance |
|---|---|---|
| Is a speaker connected, and its rating? | Ask the user; read the markings | Known before any loud test |
| Safe maximum volume | Step the codec volume from low with the speaker attached | Documented value, no clipping or audible distortion |
| I²S and the codec across light sleep | Play, sleep, wake, play again | Works or is re-initialised after wake |
| Timing of start and stop (pops) | Listen; scope if needed | No audible pop, or a documented mute sequence |
