# USB mass-storage probe: a FAT volume in PSRAM (lab image)

Answers one question from [the MSC note](../../docs/usb-msc.md): can the board
show the host an ordinary drive with folders, and hand the dropped files back
to the firmware. A 4 MB FAT volume lives in PSRAM behind an `esp_blockdev`,
exposed through esp_tinyusb 3.0.0 next to one CDC port. The probe never marks
itself valid, so the next reset returns to the previous confirmed firmware, and
it resets itself after 2400 s. `RESETNOW` on the CDC port resets it at once.

- **Project name, USB identity, install route:** as in
  [usb_otg_recovery_probe](../usb_otg_recovery_probe/README.md): `roundie_rep`,
  default Espressif descriptors (303a:4003) for the lab only, RO1 install into
  the spare slot, direct flash targets refused.
- **Report.** The CDC port prints one `MSCPROBE` line a second: USB events,
  the owner of the volume (`USB` or `APP`), MSC events, block reads and writes,
  free PSRAM and, while the application owns the volume, the files in Inbox.
- **Host client.** `tools/msc_host.py` is a userspace Bulk-Only/SCSI client
  (pyusb) for hosts that cannot mount the drive: it reads the volume, adds a
  file with mtools, writes the changed sectors and sends the eject command.
  It needs write access to the USB device node.
