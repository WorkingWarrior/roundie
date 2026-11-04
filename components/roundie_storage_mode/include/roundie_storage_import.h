#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Importer of USB storage mode (docs/usb-storage-mode-plan.md, gate G3): reads
 * the .rpk files the host left in Inbox, hashes each, stages it through
 * PackageManager, asks the user, and installs. A non-blocking state machine
 * that the system worker steps; the file system, the hash and PackageManager
 * are injected, so the same code runs under host tests with mocks. No IDF
 * calls, no allocation, no locks. */

#define ROUNDIE_IMPORT_MAX_FILES 8U
#define ROUNDIE_IMPORT_NAME 64U
#define ROUNDIE_IMPORT_REASON 48U
#define ROUNDIE_IMPORT_CHUNK 512U       /* PackageManager's chunk bound */
#define ROUNDIE_IMPORT_SLICE_CHUNKS 8U  /* chunks per step, so the worker stays responsive */
#define ROUNDIE_IMPORT_PREFIX 224U      /* package header plus manifest */
#define ROUNDIE_IMPORT_CONFIRM_US (INT64_C(45) * 1000000) /* inside PackageManager's 60 s READY window */
#define ROUNDIE_IMPORT_LIST_MAX 24U
#define ROUNDIE_IMPORT_READY_WAIT_US (INT64_C(30) * 1000000) /* for PackageManager to be admitted */

typedef enum {
    ROUNDIE_IMPORT_IGNORE,    /* not a candidate: junk, a folder, another extension */
    ROUNDIE_IMPORT_EMPTY,
    ROUNDIE_IMPORT_TOO_LARGE,
    ROUNDIE_IMPORT_ACCEPT,
} roundie_import_class_t;

typedef enum {
    ROUNDIE_IMPORT_RESULT_NONE,
    ROUNDIE_IMPORT_RESULT_INSTALLED,
    ROUNDIE_IMPORT_RESULT_UPDATED,
    ROUNDIE_IMPORT_RESULT_SKIPPED,    /* the user chose Skip */
    ROUNDIE_IMPORT_RESULT_TIMEOUT,    /* no answer in time */
    ROUNDIE_IMPORT_RESULT_EMPTY,
    ROUNDIE_IMPORT_RESULT_TOO_LARGE,
    ROUNDIE_IMPORT_RESULT_BAD_PACKAGE,
    ROUNDIE_IMPORT_RESULT_REFUSED,    /* PackageManager refused: version, space, admission */
    ROUNDIE_IMPORT_RESULT_READ_ERROR, /* the volume vanished or changed */
    ROUNDIE_IMPORT_RESULT_CANCELLED,
} roundie_import_result_t;

typedef enum {
    ROUNDIE_IMPORT_IDLE,
    ROUNDIE_IMPORT_WORKING,
    ROUNDIE_IMPORT_AWAITING_CONFIRMATION,
    ROUNDIE_IMPORT_DONE,
} roundie_import_phase_t;

typedef struct {
    char name[ROUNDIE_IMPORT_NAME];
    size_t size;
    bool regular;
} roundie_import_entry_t;

typedef struct {
    char id[ROUNDIE_IMPORT_NAME];
    char name[ROUNDIE_IMPORT_NAME];
    uint16_t version[3];
} roundie_import_candidate_t;

typedef struct {
    char file[ROUNDIE_IMPORT_NAME];
    char name[ROUNDIE_IMPORT_NAME];
    uint16_t version[3];
    roundie_import_result_t result;
    char reason[ROUNDIE_IMPORT_REASON];
} roundie_import_file_result_t;

/* Everything the GUI reads: a plain copy, published by the worker. */
typedef struct {
    roundie_import_phase_t phase;
    uint32_t run;                   /* counts runs, so a reader can tell a new one from an old result */
    unsigned index;                 /* file in progress, from 0 */
    unsigned total;                 /* files taken into the run */
    char file[ROUNDIE_IMPORT_NAME]; /* current file */
    char name[ROUNDIE_IMPORT_NAME]; /* current package name once decoded */
    uint16_t version[3];
    unsigned ignored;               /* entries that were not candidates */
    unsigned over_limit;            /* candidates beyond the file limit */
    unsigned results_count;
    roundie_import_file_result_t results[ROUNDIE_IMPORT_MAX_FILES + ROUNDIE_IMPORT_LIST_MAX];
    char error[ROUNDIE_IMPORT_REASON]; /* run-level failure, e.g. the volume could not be listed */
    bool cancelled;
} roundie_import_status_t;

/* Operations return 0 on success. read gives *length 0 at the end of the file. */
typedef struct {
    void *context;
    int (*list)(void *context, roundie_import_entry_t *entries, unsigned capacity, unsigned *count);
    int (*open)(void *context, const char *name);
    int (*read)(void *context, void *buffer, size_t capacity, size_t *length);
    void (*close)(void *context);
    int (*remove)(void *context, const char *name);
    void (*sha256_start)(void *context);
    void (*sha256_update)(void *context, const void *data, size_t length);
    void (*sha256_finish)(void *context, uint8_t digest[32]);
    /* Decode the first bytes of the package. Structural check only. */
    bool (*describe)(void *context, const uint8_t *prefix, size_t available, size_t total,
                     roundie_import_candidate_t *out, char reason[ROUNDIE_IMPORT_REASON]);
    bool (*installed)(void *context, const char *package_id);
    /* NULL means always ready. False while the system is not yet stable enough to install:
     * the run waits instead of failing, up to the ready wait. */
    bool (*ready)(void *context);
    int (*begin)(void *context, uint32_t session, size_t size, const uint8_t digest[32]);
    int (*write)(void *context, uint32_t session, size_t offset, const void *data, size_t length);
    int (*finish)(void *context, uint32_t session);
    int (*install)(void *context, const uint8_t digest[32]);
    int (*abort)(void *context, uint32_t session);
    /* Text of the last PackageManager failure, copied into the result. */
    void (*last_error)(void *context, char reason[ROUNDIE_IMPORT_REASON]);
} roundie_import_ops_t;

typedef enum {
    ROUNDIE_IMPORT_STEP_SCAN,
    ROUNDIE_IMPORT_STEP_NEXT,
    ROUNDIE_IMPORT_STEP_HASH,
    ROUNDIE_IMPORT_STEP_DESCRIBE,
    ROUNDIE_IMPORT_STEP_BEGIN,
    ROUNDIE_IMPORT_STEP_WRITE,
    ROUNDIE_IMPORT_STEP_FINISH,
    ROUNDIE_IMPORT_STEP_CONFIRM,
    ROUNDIE_IMPORT_STEP_INSTALL,
} roundie_import_step_t;

typedef struct {
    roundie_import_ops_t ops;
    size_t max_size;
    roundie_import_step_t step;
    roundie_import_status_t status;
    roundie_import_entry_t queue[ROUNDIE_IMPORT_MAX_FILES];
    unsigned queue_count;
    uint32_t session_counter;
    uint32_t run_counter;
    uint32_t session;
    bool session_open;
    bool file_open;
    bool was_installed;
    uint8_t digest[32];
    uint8_t prefix[ROUNDIE_IMPORT_PREFIX];
    size_t prefix_length;
    size_t offset;
    int64_t confirm_deadline_us;
    int64_t ready_since_us;
    int decision; /* 0 none, 1 install, 2 skip */
    bool cancel;
    uint8_t buffer[ROUNDIE_IMPORT_CHUNK];
} roundie_import_t;

roundie_import_class_t roundie_import_classify(const char *name, size_t size, bool regular, size_t max_size);
void roundie_import_init(roundie_import_t *import, const roundie_import_ops_t *ops, size_t max_package_bytes);
/* Begins a run when idle or done. Returns false while a run is active. */
bool roundie_import_start(roundie_import_t *import);
/* One bounded slice of work. Returns true while the run is active. */
bool roundie_import_step(roundie_import_t *import, int64_t now_us);
/* From the GUI, through the worker's mailbox: the user's answer. Ignored unless awaiting. */
void roundie_import_decide(roundie_import_t *import, bool install);
/* Ends the run at the next step: aborts an open session and closes the file. */
void roundie_import_cancel(roundie_import_t *import);
bool roundie_import_active(const roundie_import_t *import);
const char *roundie_import_result_name(roundie_import_result_t result);
