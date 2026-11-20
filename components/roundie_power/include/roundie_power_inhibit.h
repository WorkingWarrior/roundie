#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Sleep inhibitors (REP §17.3, M8-B). Pure C, no IDF and no lock: the caller
 * serialises every call (the firmware and the desktop hold
 * roundie_power_shared.lock). Fixed table, no allocation.
 *
 * Each inhibitor has an owner, a set of kinds, a handle (slot + generation),
 * a state (held or free) and a timeout. A released or expired handle is
 * stale: releasing it again is rejected and counted, never applied to a
 * newer holder of the same slot. An expired inhibitor is released by
 * roundie_power_inhibit_expire() and counted as a leak; owners whose life
 * ends (an application instance, an OTA session) release everything they
 * hold with roundie_power_inhibit_release_owner(). */

#define ROUNDIE_POWER_INHIBIT_SLOTS 8
#define ROUNDIE_POWER_INHIBIT_MAX_TIMEOUT_US INT64_C(600000000) /* 10 min */

typedef enum {
    ROUNDIE_POWER_INHIBIT_CPU_AWAKE = 1U << 0,     /* no light or deep sleep */
    ROUNDIE_POWER_INHIBIT_DISPLAY_AWAKE = 1U << 1, /* no screen-off on timeout */
    ROUNDIE_POWER_INHIBIT_NO_DEEP_SLEEP = 1U << 2, /* light sleep allowed, deep sleep not */
} roundie_power_inhibit_kind_t;

#define ROUNDIE_POWER_INHIBIT_KINDS 0x07U

typedef enum {
    ROUNDIE_POWER_OWNER_NONE,
    ROUNDIE_POWER_OWNER_SYSTEM,
    ROUNDIE_POWER_OWNER_OTA,
    ROUNDIE_POWER_OWNER_STORAGE,
    ROUNDIE_POWER_OWNER_AUDIO,
    ROUNDIE_POWER_OWNER_RUNTIME,
    ROUNDIE_POWER_OWNER_GUI,
    ROUNDIE_POWER_OWNER_DIAGNOSTICS,
    ROUNDIE_POWER_OWNER_COUNT,
} roundie_power_owner_t;

/* 0 is never a valid handle. */
typedef uint32_t roundie_power_inhibit_handle_t;

typedef enum {
    ROUNDIE_POWER_INHIBIT_OK,
    ROUNDIE_POWER_INHIBIT_INVALID, /* bad owner, kinds or timeout */
    ROUNDIE_POWER_INHIBIT_FULL,    /* no free slot: the caller must not start its operation */
    ROUNDIE_POWER_INHIBIT_STALE,   /* released twice, expired, or never issued */
} roundie_power_inhibit_result_t;

typedef struct {
    uint8_t owner;
    uint8_t kinds;
    uint16_t generation;
    bool held;
    int64_t since_us;
    int64_t deadline_us;
} roundie_power_inhibit_slot_t;

typedef struct {
    roundie_power_inhibit_slot_t slots[ROUNDIE_POWER_INHIBIT_SLOTS];
    uint32_t acquired;
    uint32_t released;
    uint32_t stale_releases; /* double or late releases (rejected) */
    uint32_t full;           /* acquisitions refused for lack of a slot */
    uint32_t expired;        /* leaks: released by the timeout */
    uint32_t owner_releases; /* released by release_owner() */
    int64_t longest_hold_us;
} roundie_power_inhibitors_t;

void roundie_power_inhibit_init(roundie_power_inhibitors_t *table);
/* timeout_us: 1 .. ROUNDIE_POWER_INHIBIT_MAX_TIMEOUT_US; every inhibitor ends. */
roundie_power_inhibit_result_t roundie_power_inhibit_acquire(roundie_power_inhibitors_t *table,
    roundie_power_owner_t owner, uint32_t kinds, int64_t now_us, int64_t timeout_us,
    roundie_power_inhibit_handle_t *handle);
roundie_power_inhibit_result_t roundie_power_inhibit_release(roundie_power_inhibitors_t *table,
    roundie_power_inhibit_handle_t handle, int64_t now_us);
/* Returns how many inhibitors of this owner were released. */
uint32_t roundie_power_inhibit_release_owner(roundie_power_inhibitors_t *table, roundie_power_owner_t owner,
    int64_t now_us);
/* Releases every inhibitor past its deadline; returns how many. */
uint32_t roundie_power_inhibit_expire(roundie_power_inhibitors_t *table, int64_t now_us);
/* Union of the held kinds. */
uint32_t roundie_power_inhibit_active(const roundie_power_inhibitors_t *table);
/* Number of held inhibitors, and a bit per owner holding any. */
uint32_t roundie_power_inhibit_held(const roundie_power_inhibitors_t *table, uint32_t *owner_mask);
const char *roundie_power_owner_name(roundie_power_owner_t owner);
