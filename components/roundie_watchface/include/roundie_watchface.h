#pragma once
/* WatchFaceHost: the system Home (docs/watch-face-platform.md).
 *
 * Two halves. The store runs on the sole system worker: it owns
 * /storage/faces (transfer, validation, install, removal, the active-face
 * preference and per-face settings) and is the only code that touches those
 * files. The GUI half runs on the GUI task only: it owns the face screen, the
 * gallery and the WatchFaceHost lifecycle, and never touches storage. They
 * meet in a locked mailbox that carries owned, fully validated faces from the
 * worker to the GUI and small requests back. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "roundie_elf.h"
#include "roundie_face_format.h"

#define ROUNDIE_WATCHFACE_MAX 8U        /* installed faces; separate from ROUNDIE_PACKAGES_MAX */
#define ROUNDIE_WATCHFACE_SYSTEM "system" /* the built-in fallback face */

typedef struct {
    bool (*allowed)(void); /* storage admission, as roundie_packages_allowed() */
    bool (*capacity)(size_t *free_bytes);
    void *(*allocate)(size_t size); /* face buffers: PSRAM on the board */
    void (*free)(void *memory);
    int64_t (*clock)(void); /* monotonic microseconds */
} roundie_watchface_store_platform_t;

typedef struct {
    char id[ROUNDIE_FACE_ID];
    char name[ROUNDIE_FACE_NAME];
    uint16_t version[3];
    uint32_t size;
    bool healthy; /* envelope, digest and definition valid at the last scan */
    bool preview; /* has a preview image */
} roundie_watchface_entry_t;

typedef enum {
    ROUNDIE_WATCHFACE_IDLE,
    ROUNDIE_WATCHFACE_RECEIVING,
    ROUNDIE_WATCHFACE_READY, /* staged and validated; install commits it */
} roundie_watchface_phase_t;

typedef struct {
    /* Store (worker). */
    uint32_t revision; /* changes with the list */
    uint32_t count;
    roundie_watchface_entry_t entries[ROUNDIE_WATCHFACE_MAX];
    char preferred[ROUNDIE_FACE_ID]; /* stored preference; "" means the system face */
    char error[ROUNDIE_FACE_ERROR];  /* last store refusal or failure */
    uint32_t phase;
    uint32_t session;
    uint32_t received;
    uint32_t total;
    bool fallback_only; /* Safe Mode, recovery or a failed previous boot: no packages */
    uint32_t load_us;   /* last read and validation */
    uint32_t loads;
    uint32_t load_failures;
    uint32_t pending; /* faces offered to the GUI, not yet taken */
    /* GUI (reported by the GUI task). */
    char active[ROUNDIE_FACE_ID]; /* face shown or ready to show; "system" for the fallback */
    char failed[ROUNDIE_FACE_ID]; /* last face that failed in the host */
    int32_t host_error;
    bool home;    /* the face screen is the Home screen now */
    bool parked;  /* light sleep or Home left: no face is active */
    bool visible; /* the panel is on */
    bool gallery;
    uint32_t creates;
    uint32_t destroys;
    uint32_t activations;
    uint32_t deactivations;
    uint32_t updates;
    uint32_t switches;
    uint32_t failures;
    uint32_t refused;
    uint32_t resumes;
    uint32_t objects;   /* LVGL objects under the face screen */
    uint32_t pool_free; /* LVGL pool after the last activation */
    uint32_t face_bytes; /* package bytes held by the GUI */
    uint32_t activate_us;     /* last switch: create + activate + first update */
    uint32_t activate_max_us;
    uint32_t update_max_us;   /* slowest sample delivery */
    uint32_t resume_us;       /* last resume to a painted-ready face */
} roundie_watchface_status_t;

/* --- System worker ------------------------------------------------------ */

/* Once, before the worker polls. fallback_only keeps every package unused
 * (the GUI shows the system face); the files stay as they are. */
void roundie_watchface_init(const roundie_watchface_store_platform_t *platform, bool fallback_only);
/* Each worker iteration: GUI requests, transfer expiry. */
void roundie_watchface_poll(int64_t now_us);
/* Transfer (RF1 BEGIN/DATA/END/ABORT): staged in .transfer, validated at
 * finish. Nothing is installed or activated by a transfer. */
esp_err_t roundie_watchface_begin(uint32_t session, size_t size, const uint8_t digest[32]);
esp_err_t roundie_watchface_write(uint32_t session, size_t offset, const void *data, size_t size);
esp_err_t roundie_watchface_finish(uint32_t session);
esp_err_t roundie_watchface_abort(uint32_t session);
/* Validates the staged package again and renames it into place. Installing
 * never activates; replacing the active face takes effect at the next select
 * or restart. */
esp_err_t roundie_watchface_install(const uint8_t digest[32]);
/* Installs a file directly (desktop --install-face; same validation). */
esp_err_t roundie_watchface_install_file(const char *path);
/* Reads and validates the face, then offers it to the GUI; the preference is
 * written only after the GUI reports a successful activation.
 * ROUNDIE_WATCHFACE_SYSTEM selects the fallback and clears the preference. */
esp_err_t roundie_watchface_select(const char *id);
/* Removes an installed face; removing the active one shows the fallback. */
esp_err_t roundie_watchface_remove(const char *id);
void roundie_watchface_status(roundie_watchface_status_t *out);

/* --- GUI task ------------------------------------------------------------ */

typedef struct {
    bool (*time)(roundie_elf_clock_t *local, int64_t *utc_seconds); /* false: no trusted time */
    void (*zone)(char out[48]);
    bool (*battery)(uint8_t *percent, bool *charging); /* false: no reading */
} roundie_watchface_gui_platform_t;

/* Once, on the GUI task, before the first pump; platform stays the caller's. */
void roundie_watchface_gui_bind(const roundie_watchface_gui_platform_t *platform);
/* Each launcher pump. home: Home is the foreground. Loads the face screen,
 * takes offered faces and delivers updates that are due (at most a time
 * check per second; the face is updated only when its data changed). */
void roundie_watchface_gui_pump(bool home);
/* M8-B light sleep: park before the GUI parks; resume after the display is
 * restored and the power manager has resumed. */
void roundie_watchface_gui_park(void);
void roundie_watchface_gui_resume(void);
/* The panel was switched off or on (screen-off without sleep). */
void roundie_watchface_gui_visible(bool visible);
/* Safe Mode cleans the screen tree: never touch face objects again. */
void roundie_watchface_gui_abandon(void);
