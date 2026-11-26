#include "roundie_bounded_io.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

/* Host tests substitute the VFS (short reads, EOF inside a chunk). */
#ifdef ROUNDIE_IO_READ
ssize_t ROUNDIE_IO_READ(int file, void *out, size_t size);
#else
#define ROUNDIE_IO_READ read
#endif
#ifdef ROUNDIE_IO_WRITE
ssize_t ROUNDIE_IO_WRITE(int file, const void *data, size_t size);
#else
#define ROUNDIE_IO_WRITE write
#endif

static void (*_Atomic chunk_hook)(void);
static void (*_Atomic access_guard)(void);

void roundie_io_set_access_guard(void (*guard)(void))
{
    atomic_store(&access_guard, guard);
}

static void guard_access(void)
{
    void (*guard)(void) = atomic_load(&access_guard);
    if (guard != NULL) {
        guard();
    }
}

void roundie_io_set_chunk_hook(void (*hook)(void))
{
    atomic_store(&chunk_hook, hook);
}

static void chunk_boundary(void)
{
    void (*hook)(void) = atomic_load(&chunk_hook);
    if (hook != NULL) {
        hook();
    }
}

bool roundie_io_read_exact(int file, void *out, size_t size)
{
    uint8_t *bytes = out;
    bool first = true;
    while (size > 0) {
        if (!first) {
            chunk_boundary();
        }
        first = false;
        size_t step = size < ROUNDIE_IO_CHUNK ? size : ROUNDIE_IO_CHUNK;
        guard_access();
        ssize_t count = ROUNDIE_IO_READ(file, bytes, step);
        if (count <= 0 || (size_t)count > step) {
            return false;
        }
        bytes += count;
        size -= (size_t)count;
    }
    return true;
}

bool roundie_io_write_exact(int file, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    bool first = true;
    while (size > 0) {
        if (!first) {
            chunk_boundary();
        }
        first = false;
        size_t step = size < ROUNDIE_IO_CHUNK ? size : ROUNDIE_IO_CHUNK;
        guard_access();
        ssize_t count = ROUNDIE_IO_WRITE(file, bytes, step);
        if (count <= 0 || (size_t)count > step) {
            return false;
        }
        bytes += count;
        size -= (size_t)count;
    }
    return true;
}

int roundie_io_open(const char *path, int flags, mode_t mode)
{
    guard_access();
    return open(path, flags, mode);
}

int roundie_io_close(int file)
{
    guard_access();
    return close(file);
}

ssize_t roundie_io_read(int file, void *out, size_t size)
{
    guard_access();
    return ROUNDIE_IO_READ(file, out, size);
}

ssize_t roundie_io_write(int file, const void *data, size_t size)
{
    guard_access();
    return ROUNDIE_IO_WRITE(file, data, size);
}

off_t roundie_io_lseek(int file, off_t offset, int whence)
{
    guard_access();
    return lseek(file, offset, whence);
}

int roundie_io_fstat(int file, struct stat *out)
{
    guard_access();
    return fstat(file, out);
}

int roundie_io_fsync(int file)
{
    guard_access();
    return fsync(file);
}

int roundie_io_stat(const char *path, struct stat *out)
{
    guard_access();
    return stat(path, out);
}

int roundie_io_rename(const char *from, const char *to)
{
    guard_access();
    return rename(from, to);
}

int roundie_io_unlink(const char *path)
{
    guard_access();
    return unlink(path);
}

int roundie_io_mkdir(const char *path, mode_t mode)
{
    guard_access();
    return mkdir(path, mode);
}

int roundie_io_rmdir(const char *path)
{
    guard_access();
    return rmdir(path);
}

DIR *roundie_io_opendir(const char *path)
{
    guard_access();
    return opendir(path);
}

struct dirent *roundie_io_readdir(DIR *directory)
{
    guard_access();
    return readdir(directory);
}

int roundie_io_closedir(DIR *directory)
{
    guard_access();
    return closedir(directory);
}
