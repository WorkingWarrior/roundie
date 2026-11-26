#pragma once

/* Substituted VFS for host tests: build bounded_io.c with
 * -DROUNDIE_IO_READ=fake_read -DROUNDIE_IO_WRITE=fake_write. Both delegate to
 * the real read()/write(); the knobs below inject short transfers and end of
 * file (a call returning 0) in the middle of a chunk. */

#include <stddef.h>
#include <sys/types.h>

ssize_t fake_read(int file, void *out, size_t size);
ssize_t fake_write(int file, const void *data, size_t size);

/* Every call transfers at most limit bytes; 0 = off. The initial value is the
 * FAKE_VFS_SHORT define (default 0). */
void fake_vfs_set_short(size_t limit);
/* After this many more bytes have been read, in total over all descriptors,
 * every read returns 0; a call that crosses the limit is cut at it. */
void fake_vfs_eof_after(size_t bytes);
/* Same for writes: after this many more bytes, a write returns 0. */
void fake_vfs_write_fail_after(size_t bytes);
/* Disarms both injections; the short limit stays. */
void fake_vfs_disarm(void);
/* Bytes returned by fake_read since the last reset. */
size_t fake_vfs_read_total(void);
void fake_vfs_reset_read_total(void);
