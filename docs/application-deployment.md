# Application deployment

**2026-10-20:** ordinary MTP copy followed by on-device Install is the Milestone
3F target, **not an available feature**. Its [feasibility gate](usb-architecture.md)
is NO-GO; there is no package inbox, install dialog or CDC composite firmware
installed. See [acceptance and remaining work](usb-mtp-milestone-3f-acceptance.md).

The current development path remains [RP1/PackageManager](package-manager.md)
using `tools/roundie_package.py` with explicit `--port`. Native artifacts use
RPK1, authoritative package and ELF validation, transactional active selection
and explicit launch. Checksums establish integrity, not publisher identity.
Do not copy packages directly into internal artifact paths or overwrite an
active package. There is no supported arbitrary writable filesystem export.

Future MTP deployment must separate upload, validation and user confirmation:
copy a bounded `.rpk` to the logical inbox, inspect actual validated metadata
and update/current version, then tap Install or Cancel. Commit runs in the
system worker; success refreshes AppRegistry/launcher and never launches code
automatically. A copied or partially received object is not an installed app.
The [backend design](usb-mtp.md) records why direct registration of LittleFS
does not provide this behavior in the inspected released component.
