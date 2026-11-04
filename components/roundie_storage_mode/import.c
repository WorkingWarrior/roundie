#include "roundie_storage_import.h"
#include <string.h>

static bool name_char_ok(char ch)
{
    if (ch < 0x20 || ch > 0x7e) {
        return false;
    }
    return strchr("/\\:*?\"<>|", ch) == NULL;
}

static bool has_rpk_extension(const char *name, size_t length)
{
    if (length < 5) {
        return false;
    }
    const char *extension = name + length - 4;
    return extension[0] == '.' && (extension[1] == 'r' || extension[1] == 'R') &&
        (extension[2] == 'p' || extension[2] == 'P') && (extension[3] == 'k' || extension[3] == 'K');
}

roundie_import_class_t roundie_import_classify(const char *name, size_t size, bool regular, size_t max_size)
{
    size_t length = strnlen(name, ROUNDIE_IMPORT_NAME);
    if (!regular || length == 0 || length >= ROUNDIE_IMPORT_NAME || name[0] == '.' || !has_rpk_extension(name, length)) {
        return ROUNDIE_IMPORT_IGNORE;
    }
    for (size_t index = 0; index < length; index++) {
        if (!name_char_ok(name[index])) {
            return ROUNDIE_IMPORT_IGNORE;
        }
    }
    if (size == 0) {
        return ROUNDIE_IMPORT_EMPTY;
    }
    return size > max_size ? ROUNDIE_IMPORT_TOO_LARGE : ROUNDIE_IMPORT_ACCEPT;
}

const char *roundie_import_result_name(roundie_import_result_t result)
{
    switch (result) {
    case ROUNDIE_IMPORT_RESULT_NONE:
        return "none";
    case ROUNDIE_IMPORT_RESULT_INSTALLED:
        return "installed";
    case ROUNDIE_IMPORT_RESULT_UPDATED:
        return "updated";
    case ROUNDIE_IMPORT_RESULT_SKIPPED:
        return "skipped";
    case ROUNDIE_IMPORT_RESULT_TIMEOUT:
        return "timeout";
    case ROUNDIE_IMPORT_RESULT_EMPTY:
        return "empty";
    case ROUNDIE_IMPORT_RESULT_TOO_LARGE:
        return "too_large";
    case ROUNDIE_IMPORT_RESULT_BAD_PACKAGE:
        return "bad_package";
    case ROUNDIE_IMPORT_RESULT_REFUSED:
        return "refused";
    case ROUNDIE_IMPORT_RESULT_READ_ERROR:
        return "read_error";
    case ROUNDIE_IMPORT_RESULT_CANCELLED:
        return "cancelled";
    }
    return "unknown";
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
    size_t length = strnlen(source, capacity - 1);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

void roundie_import_init(roundie_import_t *import, const roundie_import_ops_t *ops, size_t max_package_bytes)
{
    memset(import, 0, sizeof(*import));
    import->ops = *ops;
    import->max_size = max_package_bytes;
}

bool roundie_import_active(const roundie_import_t *import)
{
    return import->status.phase == ROUNDIE_IMPORT_WORKING ||
        import->status.phase == ROUNDIE_IMPORT_AWAITING_CONFIRMATION;
}

bool roundie_import_start(roundie_import_t *import)
{
    if (roundie_import_active(import)) {
        return false;
    }
    uint32_t counter = import->session_counter;
    uint32_t runs = import->run_counter;
    roundie_import_ops_t ops = import->ops;
    size_t max_size = import->max_size;
    roundie_import_init(import, &ops, max_size);
    import->session_counter = counter;
    import->run_counter = runs + 1;
    import->status.run = import->run_counter;
    import->status.phase = ROUNDIE_IMPORT_WORKING;
    import->step = ROUNDIE_IMPORT_STEP_SCAN;
    return true;
}

void roundie_import_decide(roundie_import_t *import, bool install)
{
    if (import->status.phase == ROUNDIE_IMPORT_AWAITING_CONFIRMATION && import->decision == 0) {
        import->decision = install ? 1 : 2;
    }
}

void roundie_import_cancel(roundie_import_t *import)
{
    if (roundie_import_active(import)) {
        import->cancel = true;
    }
}

static void record(roundie_import_t *import, const char *file, const char *name, const uint16_t version[3],
                   roundie_import_result_t result, const char *reason)
{
    if (import->status.results_count >= sizeof(import->status.results) / sizeof(import->status.results[0])) {
        return;
    }
    roundie_import_file_result_t *entry = &import->status.results[import->status.results_count++];
    memset(entry, 0, sizeof(*entry));
    copy_text(entry->file, sizeof(entry->file), file);
    if (name != NULL) {
        copy_text(entry->name, sizeof(entry->name), name);
    }
    if (version != NULL) {
        memcpy(entry->version, version, sizeof(entry->version));
    }
    entry->result = result;
    if (reason != NULL) {
        copy_text(entry->reason, sizeof(entry->reason), reason);
    }
}

static void release_file(roundie_import_t *import)
{
    if (import->file_open) {
        import->ops.close(import->ops.context);
        import->file_open = false;
    }
}

static void abort_session(roundie_import_t *import)
{
    if (import->session_open) {
        import->ops.abort(import->ops.context, import->session);
        import->session_open = false;
    }
}

/* Ends the current file with a result and moves on. */
static void finish_file(roundie_import_t *import, roundie_import_result_t result, const char *reason)
{
    release_file(import);
    if (result != ROUNDIE_IMPORT_RESULT_INSTALLED && result != ROUNDIE_IMPORT_RESULT_UPDATED) {
        abort_session(import);
    }
    const roundie_import_entry_t *entry = &import->queue[import->status.index];
    record(import, entry->name, import->status.name, import->status.version, result, reason);
    import->status.index++;
    import->status.phase = ROUNDIE_IMPORT_WORKING;
    import->step = ROUNDIE_IMPORT_STEP_NEXT;
}

static void finish_run(roundie_import_t *import)
{
    release_file(import);
    abort_session(import);
    import->status.phase = ROUNDIE_IMPORT_DONE;
    import->status.file[0] = '\0';
}

static void scan(roundie_import_t *import)
{
    roundie_import_entry_t entries[ROUNDIE_IMPORT_LIST_MAX];
    unsigned count = 0;
    if (import->ops.list(import->ops.context, entries, ROUNDIE_IMPORT_LIST_MAX, &count) != 0 ||
        count > ROUNDIE_IMPORT_LIST_MAX) {
        copy_text(import->status.error, sizeof(import->status.error), "Inbox unavailable");
        finish_run(import);
        return;
    }
    for (unsigned index = 0; index < count; index++) {
        roundie_import_class_t kind = roundie_import_classify(entries[index].name, entries[index].size,
                                                              entries[index].regular, import->max_size);
        if (kind == ROUNDIE_IMPORT_IGNORE) {
            import->status.ignored++;
        } else if (kind == ROUNDIE_IMPORT_EMPTY) {
            record(import, entries[index].name, NULL, NULL, ROUNDIE_IMPORT_RESULT_EMPTY, "empty file");
        } else if (kind == ROUNDIE_IMPORT_TOO_LARGE) {
            record(import, entries[index].name, NULL, NULL, ROUNDIE_IMPORT_RESULT_TOO_LARGE, "larger than a package can be");
        } else if (import->queue_count < ROUNDIE_IMPORT_MAX_FILES) {
            import->queue[import->queue_count++] = entries[index];
        } else {
            import->status.over_limit++;
        }
    }
    import->status.total = import->queue_count;
    import->step = ROUNDIE_IMPORT_STEP_NEXT;
}

static void next_file(roundie_import_t *import)
{
    if (import->status.index >= import->queue_count) {
        finish_run(import);
        return;
    }
    const roundie_import_entry_t *entry = &import->queue[import->status.index];
    copy_text(import->status.file, sizeof(import->status.file), entry->name);
    import->status.name[0] = '\0';
    memset(import->status.version, 0, sizeof(import->status.version));
    import->prefix_length = 0;
    import->offset = 0;
    import->step = ROUNDIE_IMPORT_STEP_HASH;
}

static void hash_file(roundie_import_t *import)
{
    const roundie_import_entry_t *entry = &import->queue[import->status.index];
    if (!import->file_open) {
        if (import->ops.open(import->ops.context, entry->name) != 0) {
            finish_file(import, ROUNDIE_IMPORT_RESULT_READ_ERROR, "cannot open");
            return;
        }
        import->file_open = true;
        import->ops.sha256_start(import->ops.context);
    }
    for (unsigned chunk = 0; chunk < ROUNDIE_IMPORT_SLICE_CHUNKS; chunk++) {
        size_t length = 0;
        if (import->ops.read(import->ops.context, import->buffer, sizeof(import->buffer), &length) != 0 ||
            length > sizeof(import->buffer)) {
            finish_file(import, ROUNDIE_IMPORT_RESULT_READ_ERROR, "read failed");
            return;
        }
        if (length == 0) {
            release_file(import);
            if (import->offset != entry->size) {
                finish_file(import, ROUNDIE_IMPORT_RESULT_READ_ERROR, "size changed");
                return;
            }
            import->ops.sha256_finish(import->ops.context, import->digest);
            import->step = ROUNDIE_IMPORT_STEP_DESCRIBE;
            return;
        }
        if (import->offset + length > entry->size) {
            finish_file(import, ROUNDIE_IMPORT_RESULT_READ_ERROR, "size changed");
            return;
        }
        import->ops.sha256_update(import->ops.context, import->buffer, length);
        if (import->prefix_length < sizeof(import->prefix)) {
            size_t room = sizeof(import->prefix) - import->prefix_length;
            size_t take = length < room ? length : room;
            memcpy(import->prefix + import->prefix_length, import->buffer, take);
            import->prefix_length += take;
        }
        import->offset += length;
    }
}

static void describe(roundie_import_t *import)
{
    const roundie_import_entry_t *entry = &import->queue[import->status.index];
    roundie_import_candidate_t candidate;
    char reason[ROUNDIE_IMPORT_REASON] = "";
    memset(&candidate, 0, sizeof(candidate));
    if (!import->ops.describe(import->ops.context, import->prefix, import->prefix_length, entry->size,
                              &candidate, reason)) {
        finish_file(import, ROUNDIE_IMPORT_RESULT_BAD_PACKAGE, reason);
        return;
    }
    copy_text(import->status.name, sizeof(import->status.name), candidate.name);
    memcpy(import->status.version, candidate.version, sizeof(import->status.version));
    import->was_installed = import->ops.installed(import->ops.context, candidate.id);
    import->step = ROUNDIE_IMPORT_STEP_BEGIN;
}

static void begin_transfer(roundie_import_t *import, int64_t now_us)
{
    const roundie_import_entry_t *entry = &import->queue[import->status.index];
    char reason[ROUNDIE_IMPORT_REASON] = "";
    if (import->ops.ready != NULL && !import->ops.ready(import->ops.context)) {
        if (import->ready_since_us == 0) {
            import->ready_since_us = now_us > 0 ? now_us : 1;
        }
        if (now_us - import->ready_since_us >= ROUNDIE_IMPORT_READY_WAIT_US) {
            import->ready_since_us = 0;
            finish_file(import, ROUNDIE_IMPORT_RESULT_REFUSED, "system not ready");
        }
        return;
    }
    import->ready_since_us = 0;
    import->session = ++import->session_counter;
    if (import->ops.begin(import->ops.context, import->session, entry->size, import->digest) != 0) {
        import->ops.last_error(import->ops.context, reason);
        finish_file(import, ROUNDIE_IMPORT_RESULT_REFUSED, reason);
        return;
    }
    import->session_open = true;
    if (import->ops.open(import->ops.context, entry->name) != 0) {
        finish_file(import, ROUNDIE_IMPORT_RESULT_READ_ERROR, "cannot reopen");
        return;
    }
    import->file_open = true;
    import->offset = 0;
    import->step = ROUNDIE_IMPORT_STEP_WRITE;
}

static void write_file(roundie_import_t *import)
{
    const roundie_import_entry_t *entry = &import->queue[import->status.index];
    char reason[ROUNDIE_IMPORT_REASON] = "";
    for (unsigned chunk = 0; chunk < ROUNDIE_IMPORT_SLICE_CHUNKS; chunk++) {
        size_t length = 0;
        if (import->ops.read(import->ops.context, import->buffer, sizeof(import->buffer), &length) != 0 ||
            length > sizeof(import->buffer)) {
            finish_file(import, ROUNDIE_IMPORT_RESULT_READ_ERROR, "read failed");
            return;
        }
        if (length == 0) {
            release_file(import);
            if (import->offset != entry->size) {
                finish_file(import, ROUNDIE_IMPORT_RESULT_READ_ERROR, "size changed");
                return;
            }
            import->step = ROUNDIE_IMPORT_STEP_FINISH;
            return;
        }
        if (import->offset + length > entry->size) {
            finish_file(import, ROUNDIE_IMPORT_RESULT_READ_ERROR, "size changed");
            return;
        }
        if (import->ops.write(import->ops.context, import->session, import->offset, import->buffer, length) != 0) {
            import->ops.last_error(import->ops.context, reason);
            finish_file(import, ROUNDIE_IMPORT_RESULT_REFUSED, reason);
            return;
        }
        import->offset += length;
    }
}

static void finish_transfer(roundie_import_t *import, int64_t now_us)
{
    char reason[ROUNDIE_IMPORT_REASON] = "";
    if (import->ops.finish(import->ops.context, import->session) != 0) {
        import->ops.last_error(import->ops.context, reason);
        finish_file(import, ROUNDIE_IMPORT_RESULT_BAD_PACKAGE, reason);
        return;
    }
    import->decision = 0;
    import->confirm_deadline_us = now_us + ROUNDIE_IMPORT_CONFIRM_US;
    import->status.phase = ROUNDIE_IMPORT_AWAITING_CONFIRMATION;
    import->step = ROUNDIE_IMPORT_STEP_CONFIRM;
}

static void confirm(roundie_import_t *import, int64_t now_us)
{
    if (import->decision == 1) {
        import->status.phase = ROUNDIE_IMPORT_WORKING;
        import->step = ROUNDIE_IMPORT_STEP_INSTALL;
    } else if (import->decision == 2) {
        finish_file(import, ROUNDIE_IMPORT_RESULT_SKIPPED, NULL);
    } else if (now_us >= import->confirm_deadline_us) {
        finish_file(import, ROUNDIE_IMPORT_RESULT_TIMEOUT, "no answer");
    }
}

static void install(roundie_import_t *import)
{
    const roundie_import_entry_t *entry = &import->queue[import->status.index];
    char reason[ROUNDIE_IMPORT_REASON] = "";
    if (import->ops.install(import->ops.context, import->digest) != 0) {
        import->ops.last_error(import->ops.context, reason);
        finish_file(import, ROUNDIE_IMPORT_RESULT_REFUSED, reason);
        return;
    }
    import->session_open = false; /* installed: nothing left to abort */
    /* The file is processed. Removing it keeps the next eject from offering it again. */
    import->ops.remove(import->ops.context, entry->name);
    finish_file(import, import->was_installed ? ROUNDIE_IMPORT_RESULT_UPDATED : ROUNDIE_IMPORT_RESULT_INSTALLED, NULL);
}

bool roundie_import_step(roundie_import_t *import, int64_t now_us)
{
    if (!roundie_import_active(import)) {
        return false;
    }
    if (import->cancel) {
        /* The file in progress, if any, ends as cancelled; the rest of the run is dropped. */
        if (import->step != ROUNDIE_IMPORT_STEP_SCAN && import->step != ROUNDIE_IMPORT_STEP_NEXT &&
            import->status.index < import->queue_count) {
            finish_file(import, ROUNDIE_IMPORT_RESULT_CANCELLED, "volume handed back");
        }
        import->status.cancelled = true;
        finish_run(import);
        return false;
    }
    switch (import->step) {
    case ROUNDIE_IMPORT_STEP_SCAN:
        scan(import);
        break;
    case ROUNDIE_IMPORT_STEP_NEXT:
        next_file(import);
        break;
    case ROUNDIE_IMPORT_STEP_HASH:
        hash_file(import);
        break;
    case ROUNDIE_IMPORT_STEP_DESCRIBE:
        describe(import);
        break;
    case ROUNDIE_IMPORT_STEP_BEGIN:
        begin_transfer(import, now_us);
        break;
    case ROUNDIE_IMPORT_STEP_WRITE:
        write_file(import);
        break;
    case ROUNDIE_IMPORT_STEP_FINISH:
        finish_transfer(import, now_us);
        break;
    case ROUNDIE_IMPORT_STEP_CONFIRM:
        confirm(import, now_us);
        break;
    case ROUNDIE_IMPORT_STEP_INSTALL:
        install(import);
        break;
    }
    return roundie_import_active(import);
}
