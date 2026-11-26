#pragma once

/* Bounded /storage transfers (docs/storage-io.md, P1 "4 KiB VFS calls").
 *
 * One VFS call holds the LittleFS lock; 4 KiB bounds that hold (E0 §14:
 * chunk_max 58-80 ms at 4 KiB, 184-240 ms at 16 KiB). Portable C, shared by
 * the firmware and the desktop build. */

#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

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

/* Called before every VFS call of a transfer: development builds check there
 * that the caller owns /storage and holds no claim mutex. NULL does nothing. */
void roundie_io_set_access_guard(void (*guard)(void));

/* Every other /storage VFS operation (docs/storage-io.md, P1 fix 2). Each one
 * calls the access guard, then the libc function of the same name with the
 * same arguments and result. Code outside roundie_storage calls these, never
 * the libc functions, on /storage paths and descriptors (tools/ci/
 * check_storage_vfs.py). roundie_io_read/roundie_io_write are one call
 * without the 4 KiB cap: only for the measured transfer sizes of the
 * development storage bench and for single-byte probes. */
int roundie_io_open(const char *path, int flags, mode_t mode);
int roundie_io_close(int file);
ssize_t roundie_io_read(int file, void *out, size_t size);
ssize_t roundie_io_write(int file, const void *data, size_t size);
off_t roundie_io_lseek(int file, off_t offset, int whence);
int roundie_io_fstat(int file, struct stat *out);
int roundie_io_fsync(int file);
int roundie_io_stat(const char *path, struct stat *out);
int roundie_io_rename(const char *from, const char *to);
int roundie_io_unlink(const char *path);
int roundie_io_mkdir(const char *path, mode_t mode);
int roundie_io_rmdir(const char *path);
DIR *roundie_io_opendir(const char *path);
struct dirent *roundie_io_readdir(DIR *directory);
int roundie_io_closedir(DIR *directory);
