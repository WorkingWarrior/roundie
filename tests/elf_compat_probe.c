#include <stdint.h>
#include "rep/rep_app.h"
#if REP_COMPAT_PROBE == 1
#include <errno.h>
#endif
REP_APP_METADATA;
static volatile uint64_t numerator = UINT64_C(0x1234567890abcdef);
static volatile uint64_t denominator = 7;
static volatile uint64_t result;
#if REP_COMPAT_PROBE == 2
extern _Thread_local int probe_tls;
#endif
REP_APP_EXPORT const rep_app_descriptor_t *rep_app_entry(const rep_api_t *api)
{
    (void)api;
    (void)rep_abi_v1(1);
#if REP_COMPAT_PROBE == 1
    result = (uint64_t)errno;
#elif REP_COMPAT_PROBE == 2
    result = (uint64_t)probe_tls;
#else
    result = numerator / denominator;
#endif
    return (const rep_app_descriptor_t *)(uintptr_t)result;
}
