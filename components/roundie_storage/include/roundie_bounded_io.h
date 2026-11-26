#pragma once

/* Bounded /storage transfers (docs/storage-io.md, P1 "4 KiB VFS calls").
 *
 * One VFS call holds the LittleFS lock; 4 KiB bounds that hold (E0 §14:
 * chunk_max 58-80 ms at 4 KiB, 184-240 ms at 16 KiB). Portable C, shared by
 * the firmware and the desktop build. */

#include <stdbool.h>
#include <stddef.h>

#define ROUNDIE_IO_CHUNK 4096U

/* Reads exactly size bytes in calls of at most ROUNDIE_IO_CHUNK. A short read
 * continues with the rest; end of file (0) or an error fails. */
bool roundie_io_read_exact(int file, void *out, size_t size);

/* Writes exactly size bytes in calls of at most ROUNDIE_IO_CHUNK. A short write
 * continues with the rest; a call that writes nothing or fails fails. */
bool roundie_io_write_exact(int file, const void *data, size_t size);

/* Called between two calls of one transfer: the 4 KiB chunk boundary where
 * io_worker may park before light sleep. NULL (the default) does nothing. */
void roundie_io_set_chunk_hook(void (*hook)(void));
