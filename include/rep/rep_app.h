#ifndef REP_APP_H
#define REP_APP_H

#include <stddef.h>
#include <stdint.h>

/* ABI 1: Xtensa LX7 windowed ABI, little endian, 32-bit pointers.
 * UTF-8 strings have explicit byte lengths; services borrow pointers only for
 * the call. All calls are executor-only, during entry/on_start, with the current
 * instance ID. No tasks, waits, persistent callbacks, TLS, errno or libc imports.
 * A table/descriptor stays valid until unload; applications must return.
 * Trusted native code shares firmware memory; this is not a sandbox.
 */
#define REP_ABI_VERSION UINT32_C(1)
#define REP_CAP_LOG UINT32_C(1)
#define REP_CAP_MONOTONIC UINT32_C(2)
#define REP_CAP_ALLOC UINT32_C(4)
#define REP_EVENT_START UINT32_C(1)
#define REP_APP_METADATA \
    __attribute__((used, section(".rep.abi"))) \
    const uint32_t rep_abi_metadata[3] = {UINT32_C(0x31504552), REP_ABI_VERSION, 12}
#define REP_APP_EXPORT __attribute__((visibility("default")))

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t instance;
    uint32_t event;
} rep_app_context_t;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t capabilities;
    int32_t (*log)(uint32_t instance, const char *text, uint32_t length);
    uint64_t (*monotonic_ms)(uint32_t instance);
    void *(*alloc)(uint32_t instance, uint32_t bytes);
    int32_t (*release)(uint32_t instance, void *pointer);
} rep_api_t;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    int32_t (*on_start)(const rep_app_context_t *context);
} rep_app_descriptor_t;

/* The sole named import. Unsupported versions return NULL. */
REP_APP_EXPORT const rep_api_t *rep_abi_v1(uint32_t required_version);
/* The ELF entry address must identify this function. Entry only describes the
 * app. on_start is one bounded run-to-completion START callback in 3A.
 * Return value is application status, never a firmware crash classification.
 */
REP_APP_EXPORT const rep_app_descriptor_t *rep_app_entry(const rep_api_t *api);

_Static_assert(sizeof(rep_app_context_t) == 16, "REP context layout");
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(rep_api_t) == 28, "REP API layout");
_Static_assert(offsetof(rep_api_t, log) == 12, "REP API log offset");
_Static_assert(offsetof(rep_api_t, release) == 24, "REP API release offset");
_Static_assert(sizeof(rep_app_descriptor_t) == 12, "REP descriptor layout");
_Static_assert(offsetof(rep_app_descriptor_t, on_start) == 8, "REP callback offset");
#endif
#endif
