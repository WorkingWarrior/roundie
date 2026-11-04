#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Policy of one USB storage session (docs/usb-storage-mode-plan.md, gate G2):
 * who is waiting for what, when a silent host loses the drive, and when the
 * mode ends. Pure logic with an injected clock; the GUI task owns one
 * session. The USB layer reports bus events and the current owner. */

#define ROUNDIE_STORAGE_SUSPEND_TAKE_BACK_US (INT64_C(20) * 1000000)
#define ROUNDIE_STORAGE_NO_HOST_EXIT_US (INT64_C(120) * 1000000)
#define ROUNDIE_STORAGE_HOST_IDLE_EXIT_US (INT64_C(600) * 1000000)
#define ROUNDIE_STORAGE_EJECTED_EXIT_US (INT64_C(20) * 1000000)

typedef enum {
    ROUNDIE_STORAGE_PHASE_WAITING_FOR_HOST,
    ROUNDIE_STORAGE_PHASE_HOST_OWNS,
    ROUNDIE_STORAGE_PHASE_EJECTED,
} roundie_storage_phase_t;

typedef enum {
    ROUNDIE_STORAGE_BUS_ATTACHED,
    ROUNDIE_STORAGE_BUS_SUSPENDED,
    ROUNDIE_STORAGE_BUS_RESUMED,
} roundie_storage_bus_event_t;

typedef enum {
    ROUNDIE_STORAGE_ACTION_NONE,
    ROUNDIE_STORAGE_ACTION_TAKE_BACK, /* the host went silent: give the drive back to the firmware */
    ROUNDIE_STORAGE_ACTION_EXIT,      /* leave storage mode */
} roundie_storage_action_t;

typedef struct {
    roundie_storage_phase_t phase;
    int64_t phase_since_us;
    int64_t last_activity_us;
    int64_t suspended_since_us;
    bool suspended;
    uint32_t io_calls;
} roundie_storage_session_t;

void roundie_storage_session_init(roundie_storage_session_t *session, int64_t now_us);
void roundie_storage_session_bus_event(roundie_storage_session_t *session,
                                       roundie_storage_bus_event_t event, int64_t now_us);
/* host_owns: the drive is currently exposed to the host. io_calls: running
 * count of host block reads and writes, to tell an idle host from a busy one. */
roundie_storage_action_t roundie_storage_session_poll(roundie_storage_session_t *session,
                                                      bool host_owns, uint32_t io_calls, int64_t now_us);
/* Whole seconds until the mode would end on its own; 0 when it is due. */
uint32_t roundie_storage_session_seconds_left(const roundie_storage_session_t *session, int64_t now_us);
