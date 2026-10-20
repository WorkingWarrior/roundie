# Compile-only CDC + MTP feasibility probe

This is a non-deployed Milestone 3F experiment, outside the REP component tree.
The [architecture gate](../../docs/usb-architecture.md) is **NO-GO for OTA**.
There is no `app_main`, device descriptor, VID/PID, storage registration,
PHY initialization, USB callback or package installation. Component archives
are the output; no application image is linked. Generated flash targets are
blocked. Do not add this directory to the firmware's component search path.

The probe pins ESP-IDF **6.1.0** and **esp_tinyusb 3.0.0**, including its bundled
TinyUSB at `894ea01409e0407a7dbe0ee29b2d58a4691f9046` (0.21.0~2 line).
The dependency lock belongs only to this probe. Production dependencies and
USB behavior remain unchanged. The registry published 3.0.0 on 2026-10-20;
2.3.0 introduced Espressif's MTP backend, and 2.4.0 also has it. Both inspected
2.4.0 and 3.0.0 require FATFS in their storage-registration implementation.

Run from the repository root; absolute build paths avoid `idf.py -C` resolving
a relative `-B build` against the calling directory:

```sh
. /home/solve/.espressif/v6.1/esp-idf/export.sh
PROBE_DIR=/mnt/Dane/Development/roundie/examples/usb_mtp_probe
idf.py -C "$PROBE_DIR" -B "$PROBE_DIR/build" reconfigure
idf.py -C "$PROBE_DIR" -B "$PROBE_DIR/build" __idf_main __idf_espressif__esp_tinyusb
python "$PROBE_DIR/check_descriptors.py" -v
```

The host checker compiles the **same C array and upstream descriptor macros**
as the Xtensa probe. Five checks inspect descriptor boundaries/total length,
interface numbering/classes, CDC IAD/union, unique endpoint addresses and the
Full-Speed packet sizes. There are three interfaces, 105 configuration bytes,
four non-control IN endpoints and two OUT endpoints. EP0 brings IN use to the
S3 limit of five. No extra IN endpoint remains for another interface.

`host_config/` selects a host MCU and no RTOS solely for descriptor expansion;
it is not a firmware configuration. No mocked USB session is run. Passing this
probe proves component compilation and configuration-descriptor structure;
it does not prove device/string descriptors, VID/PID authorization, PHY
switching, enumeration, VBUS detach, CDC traffic, MTP transfer, LittleFS safety,
runtime memory use or ROM recovery. See the [acceptance record](../../docs/usb-mtp-milestone-3f-acceptance.md).
