#ifndef REP_STORAGE_V1_H
#define REP_STORAGE_V1_H
#include "rep_app_v3.h"

/* App storage extension of ABI 3: one opaque blob per installed package, in an
 * app-defined format, kept across launches, reboots and package updates and
 * deleted with the package. A second import beside rep_abi_v3; firmware
 * without it rejects the ELF before loading, so no capability bit is needed.
 * rep_storage_v1 returns NULL when this launch has no storage (a development
 * upload without a package); the app then runs with its defaults.
 *
 * read and write touch system RAM only and are callable from any callback,
 * including stop and destroy. The system writes the blob to Flash after the
 * application has unloaded, so changes made since launch are lost if power
 * fails while the application runs. See docs/app-storage.md. */
#define REP_STORAGE_V1 UINT32_C(1)
#define REP_STORAGE_CAPACITY 512U

typedef struct {
    uint32_t version; /* 1 */
    uint32_t struct_size;
    uint32_t capacity; /* REP_STORAGE_CAPACITY */
    /* Copies the blob into buffer and returns its length (0 = nothing stored);
     * -1 if buffer is not app memory or capacity is below the stored length. */
    int32_t (*read)(uint32_t instance, void *buffer, uint32_t capacity);
    /* Replaces the blob; length 0 clears it. 0 on success, -1 on failure. */
    int32_t (*write)(uint32_t instance, const void *data, uint32_t length);
} rep_storage_v1_t;

REP2_EXPORT const rep_storage_v1_t *rep_storage_v1(uint32_t required_version);
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(rep_storage_v1_t) == 20, "REP storage API");
_Static_assert(offsetof(rep_storage_v1_t, write) == 16, "REP storage write");
#elif defined(__x86_64__) && UINTPTR_MAX == UINT64_MAX /* linux-x64.rep1, System V LP64 */
_Static_assert(sizeof(rep_storage_v1_t) == 32, "REP storage API");
_Static_assert(offsetof(rep_storage_v1_t, write) == 24, "REP storage write");
#endif
#endif
