# ESP32-S3 ROM Download recovery validation

## Status and scope

**2026-10-18: PASSED — manual ROM entry, direct ROM identification, and
return to normal Roundie operation with the battery connected.** This closes
the basic Milestone 1 ROM gate for the recorded route. It is one physical
recovery cycle, not a repeated-start reliability or endurance result;
Milestone 1 as a whole remains OPEN.

Target: Waveshare **ESP32-S3-Touch-LCD-1.85B**, battery connected according
to the task context. Printed PCB revision remains **unidentified**; the
previous hardware audit used the manufacturer's Rev1.1 schematic, which is
not proof of this unit's printed revision. USB serial identity is
`28:84:85:87:98:7C`.

This task permits read-only ROM identification and controlled power-state
transitions. No firmware rebuild, Flash write/erase/readback, deployment
script, partition/OTA/NVS/LittleFS change, eFuse change, security-setting
change, wiring modification, or battery disconnection is part of this test.
The existing factory backup must remain untouched.

## Documented behavior and architectural basis

- **Manufacturer documentation:** BOOT pulls GPIO0 low; hold it during
  power-on to select Download Mode. PWR is a hardware power button without
  a custom GPIO function. Waveshare describes approximately two seconds to
  switch power on/off and one second to switch on. Native USB uses GPIO19/20.
  [Waveshare board documentation](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85B),
  checked 2026-10-18.
- **Existing source audit:** the Rev1.1 schematic has a power latch and
  BOOT/GPIO0, without a dedicated RESET pushbutton. See
  [hardware audit](hardware-audit.md#pwr-boot-and-recovery); no new circuit
  audit was performed for this task.
- **Architectural requirement:** specification §§20.1–20.3 requires manual
  ROM access independently of Roundie and the Flash second-stage bootloader;
  §25 keeps battery-powered recovery open until measured. ROM communication
  without a RAM stub is the acceptance evidence for this bounded test.
  Flash corruption is neither required nor permitted.
- **Physical result:** a two-second PWR hold with USB attached left the
  screen running and USB enumerated. Removing USB next made the screen go
  dark. Holding BOOT while reconnecting USB then provided direct ROM access.
  **Unmeasured:** supply rails and the internal power-latch state. These
  observations do not prove that PWR alone reset or powered off the MCU.

## Verified host preparation

Repository baseline: `84b7489`; working tree was clean before this task.
The ESP-IDF checkout reports `v6.1`, commit
`fff9895c82d744c7237be8847347bdd1b07c6643`. No build was run.

Host tools: Linux `lsusb`, `udevadm`, `fuser`, `lsof`, and the existing
ESP-IDF Python environment with **esptool 5.6.0** and pyserial:

```sh
RECOVERY_PY=/home/solve/.espressif/python_env/idf6.1_py3.14_env/bin/python
"$RECOVERY_PY" -m esptool version
```

Installed CLI help confirms `chip-id`, `read-mac`, `flash-id`, `--no-stub`,
`--before no-reset`, and `--after no-reset`. The latter three options select
ROM-only communication without an intentional automatic reset before/after
the command. See [Espressif advanced options](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/advanced-options.html).

### Normal-mode USB baseline

Observed at 19:22–19:24 CEST on 2026-10-18:

| Property | Observed value |
|---|---|
| USB VID:PID | `303a:1001` |
| USB description | Espressif USB JTAG/serial debug unit |
| USB serial | `28:84:85:87:98:7C` |
| Bus/device | Bus 005 Device 021 |
| USB topology | `5-4.3` |
| Serial node | `/dev/ttyACM0` |
| Driver/interface | `cdc_acm`, interface `00` |
| Stable alias | `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_28:84:85:87:98:7C-if00` |

`fuser -v /dev/ttyACM0` and `lsof /dev/ttyACM0` listed no owner. Process
inspection found no active serial monitor, esptool, IDF flashing, or build
process. The short status request below completed and closed its port before
requesting physical interaction. No interfering process needed stopping.

```sh
"$RECOVERY_PY" tools/roundie_time.py \
  --port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_28:84:85:87:98:7C-if00 status
```

Exit **0**, normal Roundie diagnostic response:

```text
RTC available: yes; state: valid
RTC UTC: 2026-10-18T17:23:45+00:00; OS: 0
System UTC: 2026-10-18T17:23:45+00:00
Response: OK; device error: 0; round trip: 0.051s
RTC minus host UTC: approximately -2.57s (seconds resolution)
```

This used `GET` only. No synchronization or software restart was requested.

## Physically tested entry procedure

Starting condition: normal Roundie diagnostic image running, USB connected,
battery left connected throughout. The operator performed each step after
an individual Polish instruction and confirmed its outcome.

1. Close serial connections. Hold PWR for approximately two seconds and
   release. On this unit the display **remained running** with USB attached.
   Do not regard that PWR action as an ESP32 reset.
2. Disconnect USB without touching the battery connector. The operator
   confirmed that the screen went dark and stayed off. At 19:27:52 CEST the
   host had no matching Espressif USB or serial device. No additional PWR
   press was needed or performed after disconnecting USB.
3. Hold BOOT. Reconnect USB while holding BOOT, wait approximately two
   seconds, then release BOOT. Do not press PWR. The operator confirmed that
   the screen stayed dark. At 19:28:33 CEST the host observed a new USB
   enumeration, and the ROM identification below succeeded.
4. Enumerate again; do not assume the previous serial node survived:

   ```sh
   lsusb -d 303a:
   ls -l /dev/serial/by-id/ /dev/serial/by-path/
   udevadm info -q property -n /dev/ttyACM0
   ```

   Substitute the newly discovered node in `udevadm` and subsequent commands.
   VID:PID alone is not proof of ROM mode; normal firmware already uses
   USB Serial/JTAG.
5. Set `ROM_PORT` to the newly observed port. In this test it was again
   `/dev/ttyACM0`, independently rediscovered rather than assumed:

   ```sh
   ROM_PORT=/dev/ttyACM0
   "$RECOVERY_PY" -m esptool --chip esp32s3 --port "$ROM_PORT" \
     --before no-reset --after no-reset --no-stub chip-id
   ```

This is the tested alternative to treating PWR as an effective power cycle
while USB remains attached. The initial PWR action is included because it
was part of the successful sequence; simply reconnecting USB from any
arbitrary battery/latch state has not been established. If disconnecting
USB leaves the board running, stop and reassess rather than assuming the
same state or introducing another untested button sequence.

### USB evidence

| State | Host observation |
|---|---|
| Normal before entry | Bus 005 Device 021, `303a:1001`, `/dev/ttyACM0` |
| USB removed before entry | No matching USB or serial device |
| Manual ROM entry | Bus 005 Device 025, `303a:1001`, `/dev/ttyACM0` |
| USB removed to leave ROM | No matching USB or serial device at 19:29:44 CEST |
| Normal after exit | Bus 005 Device 026, `303a:1001`, `/dev/ttyACM0` |

The connected states retained serial `28:84:85:87:98:7C`, topology `5-4.3`,
the same by-id alias, and the `cdc_acm` interface. Normal and ROM VID:PID are
identical here. Direct esptool communication, not a changed USB product ID
or a dark LCD, distinguishes the verified ROM state. `fuser` found no port
owner immediately before ROM communication.

### Direct ROM identification — PASSED

The `chip-id` command above exited **0** with the following output (blank
lines omitted):

```text
esptool v5.6.0
Serial port /dev/ttyACM0:
NOTE: Pre-connection option "no-reset" was selected. Connection may fail if the chip is not in bootloader or flasher stub mode.
Connecting...
Connected to ESP32-S3 on /dev/ttyACM0:
Chip type:          ESP32-S3 (QFN56) (revision v0.2)
Features:           Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded PSRAM 8MB (AP_3v3)
Crystal frequency:  40MHz
USB mode:           USB-Serial/JTAG
MAC:                28:84:85:87:98:7c
WARNING: ESP32-S3 has no chip ID. Reading MAC address instead.
MAC:                28:84:85:87:98:7c
Staying in bootloader.
```

ESP32-S3's lack of a separate chip-ID operation is the CLI's documented
fallback at execution, not a failed connection. No stub was uploaded or run;
the command retained `--no-stub` and both `no-reset` options. No automatic
reset into ROM was used, and the device remained there after identification.
This proves the manual recovery communication path for this scope without
executing Roundie or relying on its second-stage bootloader. No Flash region
was intentionally damaged to test that architectural independence.

Optional Flash identification also exited **0**, retaining the same options:

```sh
"$RECOVERY_PY" -m esptool --chip esp32s3 --port "$ROM_PORT" \
  --before no-reset --after no-reset --no-stub flash-id
```

After the same chip/USB identification header, it printed:

```text
Enabling default SPI flash mode...
Flash Memory Information:
=========================
Manufacturer: 20
Device: 4018
Detected flash size: 16MB
Flash type set in eFuse: quad (4 data lines)
Flash voltage set by eFuse: 3.3V
Staying in bootloader.
```

This identification command did not program or erase Flash or write eFuses;
the output describes the detected existing configuration. No Flash contents
were read back.

## Return to normal operation — PASSED

1. Leave BOOT released and disconnect USB. The operator confirmed both;
   the host observed no matching device at 19:29:44 CEST. Leave the battery
   connected and do not press PWR.
2. Reconnect USB **without pressing BOOT or PWR**. The operator confirmed
   that the normal Roundie diagnostic display returned. The host detected
   the by-id device at 19:29:59 CEST and enumerated Bus 005 Device 026.
3. A passive 12-second serial capture used the existing `open_serial()`
   helper's established idle line states, without reset or diagnostic
   commands, then closed the port. It captured startup from approximately
   728 ms onward; the ROM/early boot reset-reason line was not captured.
   No panic or watchdog report occurred within this short captured window.
4. Run the same `roundie_time.py ... status` command as the baseline. It
   exited **0**, using `GET` only, without a host time synchronization:

   ```text
   RTC available: yes; state: valid
   RTC UTC: 2026-10-18T17:30:23+00:00; OS: 0
   System UTC: 2026-10-18T17:30:22+00:00
   Response: OK; device error: 0; round trip: 0.013s
   RTC minus host UTC: approximately -3.22s (seconds resolution)
   ```

The startup log identified `roundie 0.1.0`, built `Oct 18 2026 10:50:24`,
ELF SHA-256 prefix `27c13741c`, ESP-IDF `v6.1`, 16,777,216 B Flash and
8,388,608 B PSRAM. Relevant actual startup results:

```text
I (868) roundie_board: I2C probe CST816S 0x15: ESP_ERR_NOT_FOUND
I (878) roundie_board: I2C probe PCF85063 0x51: ESP_OK
I (878) roundie_board: I2C probe BQ27220 0x55: ESP_OK
I (1288) CST816S: IC id: 181
I (1288) roundie_board: CST816S driver: ESP_OK
I (1308) roundie_board: LCD ID: 00 02 7f 7f
I (1568) roundie_board: ST77916 variant 2 initialized at 3 MHz QSPI
I (1578) roundie: RTC startup: valid, OS=0, error=ESP_OK
I (1578) roundie: Gauge startup: valid, present=1, 3975 mV, SoC valid=1 (84%), error=ESP_OK
I (1578) roundie: System wall clock synchronized from valid RTC (UTC)
I (1618) roundie: Development RTC protocol RT1 enabled on USB Serial/JTAG
```

The initial touch probe NACK was followed by successful driver identification;
this test did not require touch input or establish a fresh full touch pass.
Gauge readings remain uncalibrated device estimates. RTC stayed valid and
advanced across the recovery interval; host offset values at seconds
resolution are not a drift/calibration measurement. Restoring system UTC
from the valid RTC is the existing normal boot behavior, not a host resync.

No configuration-changing command, rebuild, or deployment was performed.
There was no independent persistent configuration snapshot or byte comparison;
bitwise configuration preservation is not claimed. The factory backup was
not opened or modified by this test. All short serial connections were
closed after use.

## Troubleshooting and limits

- Missing USB: confirm the known data cable and topology; account for
  battery power keeping the MCU running after USB removal. Collect evidence
  before the one alternative sequence.
- Busy port: identify its owner and close the applicable monitor. Do not
  reset the device or kill unrelated processes to obtain access.
- No ROM response: confirm BOOT was held at actual power-on. Do not replace
  `no-reset` with an automatic reset and report it as manual recovery proof.
- Different USB device number or tty suffix: rediscover by USB identity and
  topology. A re-enumerated `303a:1001` can still require ROM communication
  to distinguish its operating state.
- Normal boot fails: stop and diagnose non-destructively; no automatic Flash
  writes, erasures, recovery-image installation, or time synchronization.
- One successful route is sufficient for the basic gate. USB-only operation,
  other battery conditions, TinyUSB/USB-OTG takeover, ten cold starts,
  endurance, and fault injection remain outside this test. No battery
  disconnection is required to extend coverage.

## Check under XIP, 2026-11-02

**Verified:** with normal 0.3.20 running under XIP from PSRAM, esptool 5.6.0 entered
ROM Download through its host reset (`--before default-reset --after hard-reset
--no-stub flash-id`). It identified the chip and Flash (manufacturer 20, device 4018,
16 MB) without writing, and the firmware restarted with reset class USB, normal and
STABLE. This host reset depends on a responding USB Serial/JTAG peripheral.
On 2026-11-03 the same command worked from Safe Mode, reason=core: the firmware
restarted with reset class USB and stayed in Safe Mode, and a following REBOOT
returned to normal.

**PENDING:** the manual BOOT and USB sequence above has not been repeated under XIP.
It needs a person at the board.

## BOOT at runtime, 2026-11-03

Since 0.3.21 normal firmware reads GPIO0 as an input with pull-up for the power
menu and as a light-sleep wake source. It never drives the pin, and Safe Mode does
not configure it. The user reported that BOOT held while waking from light sleep did
not enter ROM Download. Light sleep is not a reset, so this says nothing about the
strap at power-on. The manual route above is unchanged: BOOT held while power or USB
is applied. See [PowerManager](power-manager.md).

## Restore after foreign firmware, 2026-11-03

**Situation.** The user had installed another firmware. It had replaced the
partition table with nvs/phy_init/factory (3 MB at 0x10000), and the NVS and
otadata regions read as all 0xFF. Roundie's RP1 protocol did not answer.

**Read-only check first.**
- The first 64 KB were saved to
  `../roundie-backups/restore-0.3.30-2026-11-03/`.
- The LittleFS superblock at 0x710000 was still present.

**Write, with explicit user approval for repartitioning.** esptool 5.x, with
`--flash-mode keep --flash-freq keep --flash-size keep`, wrote three images,
each with its hash verified:
- the verified M2B bootloader (`f7c33fe8…`) at 0x0;
- Roundie's partition table at 0x8000;
- 0.3.30 at 0x10000.

LittleFS and ota_1 were not written and NVS was not erased.

**Result.** 0.3.30 runs from `ota_0`, VALID, self-test passed, with no Safe
Mode. All five packages survived, including the watch's stored theme file.
NVS settings (time zone) were already lost to the other firmware.

**Verified reset behaviour on this board:**
- `--after hard-reset` left the chip in ROM Download
  (`boot:0x2 DOWNLOAD(USB/UART0)`).
- `--before no-reset --after watchdog-reset` started the application normally.
- Opening the port with DTR and RTS forced low also reset the chip into
  Download. Use the idle pair from `tools/roundie_time.py` (both asserted).
