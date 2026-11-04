# USB-OTG recovery probe (lab image, never confirms itself)

Answers one question from [the USB architecture gate](../../docs/usb-architecture.md):
can the board still be recovered after the application takes the internal PHY
for USB-OTG. The image starts TinyUSB with one CDC port, counts bus events and
prints them once a second. It never marks itself valid, so the standard
bootloader rolls it back to the previous confirmed firmware on the next reset.
It also resets itself after 1500 s of uptime. Sending the word `RESETNOW` over
the port resets it at once.

- **Project name.** `roundie_rep`, because the firmware's OTA path accepts no
  other name. Install it only through RO1 (`tools/roundie_ota.py upload`,
  then `activate --restart`), into the spare slot; direct flash targets are
  refused by the CMake file.
- **USB identity.** It uses the component's default Espressif descriptors
  (303a:4001) for the lab test only. It is not a deployable identity.
- **Build.** `idf.py -B <scratch dir> build`, with IDF 6.1 exported. The
  dependency lock is not committed; esp_tinyusb is pinned to 3.0.0.
- **Host note.** Linux sent about 25 bytes to the new CDC port before anyone
  opened it. Do not use a one-letter reset command.
