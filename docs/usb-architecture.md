# USB architecture decision — Milestone 3F, 2026-10-20

**Decision: NO-GO for physical OTA.** Keep normal and Safe Mode USB Serial/JTAG
unchanged. Deliver only source analysis and an isolated compile/descriptor
probe. This is a safe feasibility stop under the task's alternative deliverable;
Milestone 3F and REP §23's MTP gate are **not complete**. No 3G work is included.

Authority: REP v0.4 §§2, 11, 13–16, 18–20, 23 and open questions in §25.
Below, **verified source** means inspected pinned code/drawing; **verified
device** means the bounded read-only queries recorded in the acceptance file.
**Design** is a proposed integration, not running firmware. **Pending** needs
the experiment and pass condition stated below.

## Controllers and PHY ownership

**Verified source:** ESP32-S3 has fixed-function USB Serial/JTAG (USJ), a
programmable USB-OTG device controller and a shared internal Full-Speed PHY.
The 1.85B Rev1.1 schematic routes USB-C D−/D+ to GPIO19/20 through 22 ohm
series resistors. It shows no independent external USB PHY. USJ cannot add an
MTP interface; two independent devices cannot use this internal PHY together.
One OTG composite device is the viable controller topology, with CDC replacing
the application diagnostic byte stream while normal OTG owns the PHY.

Inspected IDF: **v6.1**, `fff9895c82d744c7237be8847347bdd1b07c6643`.
`components/esp_hw_support/usb_phy/usb_phy.c:usb_new_phy()` selects OTG/internal
and calls the USB-wrap HAL. In
`components/esp_hal_usb/esp32s3/include/hal/usb_wrap_ll.h:usb_wrap_ll_phy_enable_external()`,
`enable=false` writes `sw_hw_usb_phy_sel=1` and `sw_usb_phy_sel=1`, routing the
internal PHY to USB Wrap/OTG. This is software register selection, **not an
eFuse operation**. `USB_PHY_SEL` must remain unchanged.

esp_tinyusb 3.0.0 `tinyusb_driver_install()` selects OTG/internal/Full-Speed
unless PHY setup is explicitly skipped. It creates PHY ownership before
starting the TinyUSB task. The fixed USJ hardware may still exist internally,
but its existing USB identity and JTAG access are unavailable on this cable
while the PHY belongs to OTG. CDC preserves protocol transport only; it does
not preserve USB JTAG. A disconnect/re-enumeration and new serial node are
expected, not evidence of successful startup.

**Pinned limitation:** `usb_del_phy()` disables pull overrides, releases the
context and clock ownership; it does not explicitly select USJ in the RTC
mux. TinyUSB install failure cleanup/uninstall calls that function. Therefore
“uninstall TinyUSB and keep using the old USJ driver” is not a proven fallback.
Driver allocation bookkeeping also does not establish that an already-running
USJ stream has been quiesced. A live switch would require complete reader,
writer, VFS/logging, interrupt and peripheral ownership retirement first.
No live handover is implemented or attempted here.

## Recovery and boot decision

**Current verified source:** `main/main.c` installs USJ and opens one
`/dev/secondary` RX owner in the existing system worker. Framing/dispatch stays
there. Several diagnostics write USJ directly. All must migrate together for
CDC; enabling TinyUSB alone would strand the current diagnostic path.

**Design:** select transport once per boot. Decide Safe Mode/BootGuard before
normal OTG initialization. Safe Mode starts USJ without TinyUSB, LittleFS,
AppRegistry or launcher dependencies. Normal mode uses one TinyUSB service
and CDC+MTP only after explicit initialization checks. Core confirmation must
require usable diagnostic service initialization, not a Linux host connection;
an unplugged healthy device must boot. MTP storage availability remains
optional. Failed initialization that loses recovery must prevent trial-image
confirmation and allow existing rollback; it must not be silently healthy.

| Failure/state | Required route and current evidence |
|---|---|
| Healthy normal OTG | CDC for unchanged RO1/RS1/RT1/RE1/RP1; **pending**, no deployment |
| MTP unavailable | CDC independent of storage; **design**, not implemented |
| TinyUSB initialization/enumeration fails | Do not depend on another TinyUSB task or a working launcher to recover. No automatic live-USJ fallback is established; **deployment blocked** |
| MTP/USB task crashes | Existing panic/watchdog/rollback mechanisms plus manual ROM; no recovery claim from a merely allocated CDC queue |
| Safe Mode request/failed app | Existing lifecycle quiescence and reset; future Safe Mode must select USJ before OTG and establish mux state |
| Pending OTA fails | Standard bootloader rollback to unchanged A 0.3.8; no subsequent write while USB identity/state is ambiguous |
| Manual ROM Download | BOOT/GPIO0 at a real reset/power-on, no eFuse changes and no dependency on firmware, TinyUSB, filesystem or registry |

The [ROM procedure](rom-recovery.md) was physically verified on 2026-10-18
with battery connected in **USJ firmware**, not after OTG takeover: PWR hold,
USB removal with confirmed dark screen, then BOOT held while reconnecting
USB. It is a historical bounded success, not proof that a CPU-only reset
clears retained RTC mux state. If the board remains powered, do not assume
USB unplugging reset it. ROM identity requires direct no-stub communication;
VID:PID alone does not distinguish USJ firmware from ROM.

**Pending experiment:** establish a repeatable genuine reset/ROM entry from
OTG ownership, observe re-enumeration, obtain ROM chip identity without writes,
and return to the known-good image. A matching hardware/reset analysis and
an independently usable route must precede a deployable OTG candidate.
Physical button/cable steps cannot be performed by this unattended session.
No new ROM cycle, reset or power transition was performed in 3F.

## Battery power and VBUS — unresolved hardware gate

**Verified source:** the manufacturer Rev1.1 PDF hashes to
`d4333dccd12f4c6a6f28351ee329eae08a4251cc61f083a37ef5ee59eff29b6d`.
Both text and rendered schematic were reviewed. The USB connector net
`USB_5V` feeds `VBUS` through D4; battery power also reaches the system power
path. No dedicated USB_5V divider/comparator to an ESP32 GPIO is shown in
the connector, MCU, IO allocation or power blocks. The printed PCB revision
of the attached unit is still unidentified; schematic absence is not an
electrical measurement of that unit.

Espressif requires self-powered devices to monitor host VBUS, detach when it
is absent, and supplies `phy.self_powered`/`vbus_monitor_io` for this purpose.
Battery-backed system voltage is not an independent USB VBUS measurement.
The pinned DWC2 configuration explicitly says VBUS sensing is mandatory for
`tud_umount_cb()`. Treating the board as always bus-powered or tying VBUS
valid permanently high would not establish standards-compatible disconnect
behavior. No GPIO is repurposed, no guessed voltage is attached to a pin,
and no speculative battery-gauge reading is used as a disconnect detector.

**Pending experiment/decision:** identify actual PCB and an existing safe
host-VBUS signal; verify valid/invalid levels and removal timing while battery
keeps the MCU alive, then verify transfer cleanup and reconnect. Pass only
with correct attach/detach behavior and no back-power. If no suitable signal
exists, standards-compatible self-powered OTG may need a hardware change or
an explicit narrower, measured operating scope. Neither is silently approved
here. This blocks an unconditional “MTP on this board without changes” claim.

## Schematic re-check, 2026-11-04

The user supplied the Rev1.1 PDF again; its SHA-256 equals the one reviewed
above (`d4333dcc…9b6d`), so no new revision is involved. Read from the
rendered sheet at 400 dpi (**verified source**, drawing only, no measurement):

- **USB-C.** CC1 (R8) and CC2 (R13) have 5.1 kΩ to GND each: a sink with 5 V
  default current, no Power Delivery. Both D+/D− pairs of the receptacle are
  tied together, so either plug orientation works. D−/D+ go through 22 Ω
  (R24, R25) to GPIO19/GPIO20 and nowhere else. I see no TVS on D+/D−; the
  only ESD part (ESD5451N) is at the PWR key.
- **VBUS.** Connector `USB_5V` → D4 (MBR230LSFT1G) → `VBUS` rail. `VBUS`
  feeds the ETA6098 charger VIN, and through D3 the `VCC` rail. The battery
  reaches `VCC` through D1 and Q2 (a P-FET whose gate is `VBUS`, pulled down by
  10 MΩ). No net from `USB_5V` or `VBUS` goes to an ESP32 pin.
- **Pins for a sense line.** The IO table lists GPIO7 and GPIO8 on header J6
  (3V3, GND beside them); GPIO8 carries a note to remove a 0 Ω resistor before
  use, GPIO7 does not. Header J8 is UART RX/TX. No header carries 5 V.
  Reaching `USB_5V` needs a wire to the connector or D4 pad.
- **Note in the IO table (Chinese, my reading):** if GPIO19/20 are used for
  USB, flashing needs download mode every time. This matches the pinned
  limitation above: no esptool auto-reset after OTG takeover.
- **Other storage.** A microSD slot (TF1) wires SDIO to GPIO12–17, unused today.

**Verified source, IDF v6.1 `usb_phy.c:171-176`:** in device mode the PHY code
forces `iddig`, `bvalid` and `vbusvalid` to constant 1 through the GPIO
matrix. esp_tinyusb 3.0.0 `tinyusb.c:297-322` offers `phy.self_powered` with
`vbus_monitor_io`, which needs a real GPIO. So without a sense line the
stack runs as bus-powered with VBUS always "valid": the host sees attach and
detach itself, but the firmware gets no VBUS-removal event.

**Assumption (not measured):** cable removal during a transfer is then seen
only as suspend or reset, and the MTP session is retired from those. Also
unmeasured: the D+ pull-up staying on with the cable plugged into an
unpowered host, and light sleep while USB is plugged.

**Conclusion for the VBUS gate:** the drawing does not block a USB device, it
blocks a *standards-compliant self-powered* one. Two ways forward, both
pending a decision: (a) accept the bus-powered mode and measure the cases
above on the unit; (b) add a divider from `USB_5V` to GPIO7 (hardware change,
soldering). Neither is approved here.

## OTG recovery probe, 2026-11-04

**Method.** `examples/usb_otg_recovery_probe` is a minimal image (TinyUSB, one
CDC port, event counters) installed through RO1 into the spare OTA slot while
0.3.34 was confirmed in the other. It never confirms itself, so the bootloader
rolls back at the next reset. Authorization: the user chose this test; it
changes no partition, bootloader or eFuse.

**Device results (one run each, user at the board for the manual one):**

| Case | Observation |
|---|---|
| OTG start | After `activate --restart` the host saw `303a:4001` and a CDC port; the probe reported `attached=1`, no resets in 100+ s |
| Manual ROM entry from OTG | Procedure from [rom-recovery.md](rom-recovery.md): PWR 2 s with USB attached (probe kept running), USB removed (host lost the device), BOOT held while reconnecting. esptool `--no-stub chip-id` connected: ESP32-S3 rev v0.2, MAC `28:84:85:87:98:7c`, "Staying in bootloader". Read-only |
| Return from ROM | USB removed, reconnected without BOOT or PWR: 0.3.34 on ota_0 VALID, probe slot ABORTED, reset class power-on, RO1 and RP1 answered over USB Serial/JTAG |
| Software restart from OTG | Twice (once by accident): after `esp_restart()` the board rolled back to 0.3.34 and `303a:1001` reappeared 0.4 s after the OTG device vanished; RO1 answered |

**What this settles.** The recovery gate above is **passed for this unit and
procedure**: ROM Download is reachable from OTG ownership, and a software
restart into a USB Serial/JTAG image restores that path, so the PHY mux does
not stay stuck across a restart in this case. The pinned `usb_del_phy()`
limitation is untouched; the observation says restart works, not why.

**What it does not settle.** Only one unit and one battery state. A hung or
crashed OTG image that never restarts, rollback after a panic inside TinyUSB,
the same sequence with the battery removed, and any behaviour with MTP loaded
are untested. VBUS removal events were not exercised (no cable pull during the
run). Neither the gate on VBUS nor the backend blocker changes.

## Composite endpoint and identity design

**Verified host/target structure:** the compile-only probe uses real pinned
TinyUSB macros, yielding a **105-byte** configuration and three interfaces:

| Interface | Class | Endpoint plan |
|---|---|---|
| 0 | CDC ACM control, IAD covers 0–1 | `0x81` interrupt IN, 8 B |
| 1 | CDC data | `0x02` bulk OUT / `0x82` bulk IN, 64 B |
| 2 | Still Image/MTP 06/01/01 | `0x03` bulk OUT / `0x83` bulk IN, 64 B; `0x84` interrupt IN, 64 B |

Six directional endpoints plus EP0, with **five IN including EP0**. This fits
the documented S3 limit exactly; no spare IN endpoint remains. The two classes
share the same device task/controller. USB is **12 Mbit/s Full-Speed**, not
High-Speed; this is signaling rate, not measured package throughput.

**Pending:** full device descriptor with composite/IAD identity (EF/02/01),
authorized Roundie development VID/PID, language/manufacturer/product/serial
and interface strings, actual descriptor requests and EP0 control/cancel
interoperation. The probe deliberately has **no device descriptor or VID/PID**
and cannot enumerate. No commercial identity is borrowed and no default
Espressif descriptor is deployed. CDC port discovery must use explicit
`--port` or a documented unambiguous identity; the old USJ by-id name is
not a future CDC guarantee. No host-tool migration is enabled before this gate.

## Stack selection and deployment decision

**Verified registry/source:** MTP integration was introduced in esp_tinyusb
2.3.0; 2.4.0 still requires a separate TinyUSB dependency with a broad version
range. 3.0.0 was published on **2026-10-20** and bundles a fixed TinyUSB revision.
The probe locks **3.0.0**, component hash
`0ff29f6113c4922691b3d7eee2f29af5fb98b799ef8b198a2956328f7c2296a9`,
esp-usb commit `68bf7f9613dbb1a31094fdb903209ae3e4bf388d`, bundled TinyUSB
`894ea01409e0407a7dbe0ee29b2d58a4691f9046` (0.21.0~2 line).
Its manifest requires IDF >=5.3; **actual component compilation on pinned
6.1 passes**. This is a probe choice, not production qualification of a new release.

Released 2.4.0 was inspected separately at esp-usb
`7963dbeaddee8798cae953cbcd2f1dc48c7589bf`, component hash
`cfb066b239957ebfba5f7a7764e534b3000d3b400fd42d11bedb6ee5e6eaf3c9`.
Its relevant MTP operations/write/transfer/object-store files match 3.0.0.
Both use FATFS capacity queries and have no public per-storage mutation
admission callbacks. See [backend analysis](usb-mtp.md).

**NO-GO conditions present:** unproven battery/VBUS route, unproven ROM/Safe
Mode recovery from OTG, and a backend that bypasses current Flash admission.
Also pending: authorized USB identity, diagnostic transport migration and
runtime RAM/stack measurements. Do not create/deploy a REP 0.3.10 candidate,
replace B, switch PHY, burn eFuses, format storage or change partitions.
Keep A 0.3.8 and B 0.3.9 intact. Production `dependencies.lock` is unchanged.

## Primary sources

- [ESP-IDF PHY implementation at the pinned commit](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/esp_hw_support/usb_phy/usb_phy.c)
  and [S3 mux HAL](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/esp_hal_usb/esp32s3/include/hal/usb_wrap_ll.h).
- [Exact 1.85B schematic](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B/blob/139e6db584f3737fcfc6a958ee83b79fb69d317c/hardware/ESP32-S3-Touch-LCD-1.85B%20Rev1.1.pdf).
- [Espressif USB guide](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_device.html),
  endpoint limits and self-powered-device section; checked 2026-10-20.
- [Live component registry API](https://components.espressif.com/api/components/espressif/esp_tinyusb),
  manifests and downloaded 2.4.0/3.0.0 releases. The browser's cached 2.4.0
  README still called 3.0.0 prerelease; live registry and resolved package take
  precedence for the current release inventory.
- [Pinned esp_tinyusb source](https://github.com/espressif/esp-usb/tree/68bf7f9613dbb1a31094fdb903209ae3e4bf388d/device/esp_tinyusb),
  `tinyusb.c`, `tinyusb_task.c`, `include/tusb_config.h`, MTP backend and Kconfig.
- [TinyUSB MTP example](https://docs.tinyusb.org/en/latest/examples/device/mtp.html)
  is reference material; it does not establish Roundie integration/recovery.
