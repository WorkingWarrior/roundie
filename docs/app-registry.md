# AppRegistry — Milestone 3D

**3F status, 2026-10-20:** there is no MTP inbox integration. The
[feasibility stop](usb-mtp-milestone-3f-acceptance.md) preserves both installed
package identities/digests. Future completed uploads must remain absent from
this registry until explicit confirmation and successful PackageManager commit.

The registry is the bounded discovery/metadata portion of `roundie_packages`.
It has no boot task, separate persistent database, ELF mappings or autostart.
Each `active` selection is the persistent record; the in-memory array is rebuilt
from those records on explicit LIST/INFO/LAUNCH and after install/remove.
The wire never uses stale cached metadata. Internal `get/find` read only the
most recent successful scan and copy records, not filesystem pointers.

Discovery visits at most 32 root entries and admits at most 8 active records,
sorted by validated ID. Overflow fails closed and clears the returned array
instead of selecting an arbitrary prefix. Incomplete transfers, directories
without `active`, `active.tmp` and inactive artifacts do not enumerate as
installed apps. Orphan directories are counted separately. No registry file
is rewritten during list, lookup, launch validation or reboot.

Each active record copies ID, display name, numeric version, ABI, platform/API,
ELF size/digest, package digest, internal artifact-slot number and health.
LIST returns `[id, version, abi, health]`; INFO adds display name, full-package
digest and ELF size. Health is 0 = compatible/integrity checked,
1 = corrupt/missing committed bytes/invalid selection, 2 = incompatible
platform/API/ABI. A corrupt record can retain only its directory ID and zeroed
untrusted metadata. Bad IDs and unknown root files are not registry records.

Each scan reopens selection/artifact, checks exact envelope/manifest and streams
the entire selected file through SHA-256 against the committed selection.
It does not allocate an ELF-sized buffer or run the loader to list apps.
Full ELF preflight was required at commit and is repeated on **every launch**.
A valid old commit is evidence of earlier validation, not current execution
admission. Missing artifacts and manifest/digest changes are reported as corrupt;
compatible package files are preserved even when another record is damaged.
Future ABI/platform/API metadata is reported incompatible without deletion.

Launch-by-ID resolves only a healthy committed selection, then reopens and
validates the complete package and copies the ELF into a separately owned
buffer. Existing ELFManager routes ABI 1 to one-shot RUN and ABI 2/3 to START.
Only `rep_executor` executes entrypoints/callbacks. A second loaded instance,
active OTA, pending verification, Safe Mode or package transfer denies launch.
Unload retires runtime/UI/events/timers/resources and input buffer while keeping
the installed package. Stop/suspend do not permit update/removal until unload.

**Scope boundary against REP §13.1:** this foundation exposes one explicitly
active version per ID and retains at most one inactive artifact for replacement
safety. It does not offer arbitrary retained-version selection, a full history,
data migrations or a graphical launcher. Missing activation metadata is an
error/orphan, never an implicit version election. This preserves REP §13.4's
separate durable activation mechanism without creating a multi-version database.

No application autostarts after normal restart, OTA, rollback or Safe Mode.
See [package format](package-format.md), [transactions](package-manager.md) and
[acceptance evidence](packages-milestone-3d-acceptance.md).

## Milestone 3E presentation

Successful scans now advance a volatile cache revision; persistence is unchanged.
The [system launcher](app-launcher.md) copies bounded metadata on the same worker
and observes revision changes after installation, update, removal or diagnostic
scans. Failed refresh preserves its previous display but closes selection until
controlled retry. Selection includes exact ID and presentation revision;
PackageManager still rescans and fully validates every launch. No GUI filesystem
access or second persistent registry is introduced.
