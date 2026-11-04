#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "roundie_storage_import.h"

/* Device side of the importer (docs/usb-storage-mode-plan.md, gate G3). The
 * state machine is stepped by the system worker, the only caller of
 * PackageManager. The GUI task talks to it through a small mailbox and reads
 * a published copy of the status, so it never waits for storage work. */

/* Worker, once per loop: handles the mailbox, steps the run, publishes status. */
void roundie_storage_import_poll(int64_t now_us);

/* GUI task. */
void roundie_storage_import_request(void); /* run over Inbox once the firmware owns the volume */
void roundie_storage_import_cancel(void);  /* the host took the volume back */
void roundie_storage_import_decide(bool install);
void roundie_storage_import_snapshot(roundie_import_status_t *out);
