#include "fake_vfs.h"

#include <stdbool.h>
#include <unistd.h>

#ifndef FAKE_VFS_SHORT
#define FAKE_VFS_SHORT 0
#endif

static size_t short_limit = FAKE_VFS_SHORT;
static bool eof_armed;
static size_t eof_left;
static bool write_armed;
static size_t write_left;
static size_t read_total;

void fake_vfs_set_short(size_t limit)
{
    short_limit = limit;
}

void fake_vfs_eof_after(size_t bytes)
{
    eof_armed = true;
    eof_left = bytes;
}

void fake_vfs_write_fail_after(size_t bytes)
{
    write_armed = true;
    write_left = bytes;
}

void fake_vfs_disarm(void)
{
    eof_armed = false;
    write_armed = false;
}

size_t fake_vfs_read_total(void)
{
    return read_total;
}

void fake_vfs_reset_read_total(void)
{
    read_total = 0;
}

ssize_t fake_read(int file, void *out, size_t size)
{
    if (short_limit != 0 && size > short_limit) {
        size = short_limit;
    }
    if (eof_armed) {
        if (eof_left == 0) {
            return 0;
        }
        if (size > eof_left) {
            size = eof_left;
        }
    }
    ssize_t count = read(file, out, size);
    if (count > 0) {
        read_total += (size_t)count;
        if (eof_armed) {
            eof_left -= (size_t)count;
        }
    }
    return count;
}

ssize_t fake_write(int file, const void *data, size_t size)
{
    if (short_limit != 0 && size > short_limit) {
        size = short_limit;
    }
    if (write_armed) {
        if (write_left == 0) {
            return 0;
        }
        if (size > write_left) {
            size = write_left;
        }
    }
    ssize_t count = write(file, data, size);
    if (count > 0 && write_armed) {
        write_left -= (size_t)count;
    }
    return count;
}
