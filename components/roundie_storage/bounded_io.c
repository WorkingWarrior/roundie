#include "roundie_bounded_io.h"

#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>

/* Host tests substitute the VFS (short reads, EOF inside a chunk). */
#ifndef ROUNDIE_IO_READ
#define ROUNDIE_IO_READ read
#endif
#ifndef ROUNDIE_IO_WRITE
#define ROUNDIE_IO_WRITE write
#endif

static void (*_Atomic chunk_hook)(void);

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
        ssize_t count = ROUNDIE_IO_WRITE(file, bytes, step);
        if (count <= 0 || (size_t)count > step) {
            return false;
        }
        bytes += count;
        size -= (size_t)count;
    }
    return true;
}
