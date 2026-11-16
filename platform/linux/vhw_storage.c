/* Storage faults of the virtual hardware lab (docs/virtual-hardware.md).
 *
 * The desktop links with -Wl,--wrap for the POSIX calls the package manager
 * (components/roundie_packages/manager.c) makes. With "storage-fault io" a
 * call on a path under storage/, or on a descriptor opened there, fails with
 * EIO before it reaches the host file system, as a LittleFS operation failing
 * on the board would; "storage-fault io CALL" fails only that call. Every
 * other call passes through unchanged. The wrappers
 * apply to the whole executable and write() runs in the fault handler, so
 * they use only atomics and the real calls (async-signal-safe).
 *
 * The other storage faults act on the package manager's seams instead
 * (app_runtime.c): "absent" refuses package work as the firmware does without
 * a mounted partition, and "storage-limit" lowers the free space it reports. */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "vhw.h"

#define STORAGE_PREFIX "storage/"
#define TRACKED_DESCRIPTORS 1024

/* Descriptors open on a path under storage/. */
static atomic_bool tracked[TRACKED_DESCRIPTORS];

int __real_open64(const char *path, int flags, ...);
int __real_close(int fd);
ssize_t __real_read(int fd, void *buffer, size_t count);
ssize_t __real_write(int fd, const void *buffer, size_t count);
int __real_fsync(int fd);
int __real_mkdir(const char *path, mode_t mode);
int __real_rmdir(const char *path);
int __real_unlink(const char *path);
int __real_rename(const char *from, const char *to);
int __real_stat64(const char *path, struct stat64 *info);
DIR *__real_opendir(const char *path);

static bool under_storage(const char *path)
{
    return path != NULL && strncmp(path, STORAGE_PREFIX, sizeof(STORAGE_PREFIX) - 1) == 0;
}

static bool path_fails(vhw_storage_call_t call, const char *path)
{
    if (under_storage(path) && vhw_storage_call_fails(call)) {
        errno = EIO;
        return true;
    }
    return false;
}

static bool descriptor_fails(vhw_storage_call_t call, int fd)
{
    if (fd >= 0 && fd < TRACKED_DESCRIPTORS && atomic_load(&tracked[fd]) && vhw_storage_call_fails(call)) {
        errno = EIO;
        return true;
    }
    return false;
}

int __wrap_open64(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if ((flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, unsigned int);
        va_end(arguments);
    }
    if (path_fails(VHW_STORAGE_CALL_OPEN, path)) {
        return -1;
    }
    int fd = __real_open64(path, flags, mode);
    if (fd >= 0 && fd < TRACKED_DESCRIPTORS) {
        atomic_store(&tracked[fd], under_storage(path));
    }
    return fd;
}

int __wrap_close(int fd)
{
    if (fd >= 0 && fd < TRACKED_DESCRIPTORS) {
        atomic_store(&tracked[fd], false);
    }
    return __real_close(fd);
}

ssize_t __wrap_read(int fd, void *buffer, size_t count)
{
    if (descriptor_fails(VHW_STORAGE_CALL_READ, fd)) {
        return -1;
    }
    return __real_read(fd, buffer, count);
}

ssize_t __wrap_write(int fd, const void *buffer, size_t count)
{
    if (descriptor_fails(VHW_STORAGE_CALL_WRITE, fd)) {
        return -1;
    }
    return __real_write(fd, buffer, count);
}

int __wrap_fsync(int fd)
{
    if (descriptor_fails(VHW_STORAGE_CALL_FSYNC, fd)) {
        return -1;
    }
    return __real_fsync(fd);
}

int __wrap_mkdir(const char *path, mode_t mode)
{
    if (path_fails(VHW_STORAGE_CALL_MKDIR, path)) {
        return -1;
    }
    return __real_mkdir(path, mode);
}

int __wrap_rmdir(const char *path)
{
    if (path_fails(VHW_STORAGE_CALL_RMDIR, path)) {
        return -1;
    }
    return __real_rmdir(path);
}

int __wrap_unlink(const char *path)
{
    if (path_fails(VHW_STORAGE_CALL_UNLINK, path)) {
        return -1;
    }
    return __real_unlink(path);
}

int __wrap_rename(const char *from, const char *to)
{
    if (path_fails(VHW_STORAGE_CALL_RENAME, from) || path_fails(VHW_STORAGE_CALL_RENAME, to)) {
        return -1;
    }
    return __real_rename(from, to);
}

int __wrap_stat64(const char *path, struct stat64 *info)
{
    if (path_fails(VHW_STORAGE_CALL_STAT, path)) {
        return -1;
    }
    return __real_stat64(path, info);
}

DIR *__wrap_opendir(const char *path)
{
    if (path_fails(VHW_STORAGE_CALL_OPENDIR, path)) {
        return NULL;
    }
    return __real_opendir(path);
}
