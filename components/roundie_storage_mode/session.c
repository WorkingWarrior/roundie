#include "roundie_storage_session.h"
#include <stdint.h>

static int64_t deadline_us(const roundie_storage_session_t *session)
{
    switch (session->phase) {
    case ROUNDIE_STORAGE_PHASE_WAITING_FOR_HOST:
        return session->phase_since_us + ROUNDIE_STORAGE_NO_HOST_EXIT_US;
    case ROUNDIE_STORAGE_PHASE_HOST_OWNS:
        return session->last_activity_us + ROUNDIE_STORAGE_HOST_IDLE_EXIT_US;
    case ROUNDIE_STORAGE_PHASE_EJECTED:
        return session->phase_since_us + ROUNDIE_STORAGE_EJECTED_EXIT_US;
    case ROUNDIE_STORAGE_PHASE_IMPORTING:
        return INT64_MAX;
    case ROUNDIE_STORAGE_PHASE_RESULT:
        return session->phase_since_us + ROUNDIE_STORAGE_RESULT_EXIT_US;
    }
    return session->phase_since_us;
}

void roundie_storage_session_init(roundie_storage_session_t *session, int64_t now_us)
{
    *session = (roundie_storage_session_t){
        .phase = ROUNDIE_STORAGE_PHASE_WAITING_FOR_HOST,
        .phase_since_us = now_us,
        .last_activity_us = now_us,
    };
}

void roundie_storage_session_set_phase(roundie_storage_session_t *session, roundie_storage_phase_t phase,
                                       int64_t now_us)
{
    if (session->phase != phase) {
        session->phase = phase;
        session->phase_since_us = now_us;
    }
}

void roundie_storage_session_bus_event(roundie_storage_session_t *session,
                                       roundie_storage_bus_event_t event, int64_t now_us)
{
    switch (event) {
    case ROUNDIE_STORAGE_BUS_ATTACHED:
        session->suspended = false;
        session->last_activity_us = now_us;
        break;
    case ROUNDIE_STORAGE_BUS_SUSPENDED:
        if (!session->suspended) {
            session->suspended = true;
            session->suspended_since_us = now_us;
        }
        break;
    case ROUNDIE_STORAGE_BUS_RESUMED:
        session->suspended = false;
        session->last_activity_us = now_us;
        break;
    }
}

roundie_storage_action_t roundie_storage_session_poll(roundie_storage_session_t *session,
                                                      bool host_owns, uint32_t io_calls, int64_t now_us)
{
    if (io_calls != session->io_calls) {
        session->io_calls = io_calls;
        session->last_activity_us = now_us;
    }
    if (host_owns && session->phase != ROUNDIE_STORAGE_PHASE_HOST_OWNS) {
        session->phase = ROUNDIE_STORAGE_PHASE_HOST_OWNS;
        session->phase_since_us = now_us;
        session->last_activity_us = now_us;
    } else if (!host_owns && session->phase == ROUNDIE_STORAGE_PHASE_HOST_OWNS) {
        /* The host ejected the drive, or it was taken back after a silent host. */
        session->phase = ROUNDIE_STORAGE_PHASE_EJECTED;
        session->phase_since_us = now_us;
        session->suspended = false;
    }
    /* No detach event exists on this board: a pulled cable shows as suspend
     * only, so a suspended host that still owns the drive loses it. */
    if (host_owns && session->suspended &&
        now_us - session->suspended_since_us >= ROUNDIE_STORAGE_SUSPEND_TAKE_BACK_US) {
        return ROUNDIE_STORAGE_ACTION_TAKE_BACK;
    }
    if (now_us >= deadline_us(session)) {
        return ROUNDIE_STORAGE_ACTION_EXIT;
    }
    return ROUNDIE_STORAGE_ACTION_NONE;
}

uint32_t roundie_storage_session_seconds_left(const roundie_storage_session_t *session, int64_t now_us)
{
    if (session->phase == ROUNDIE_STORAGE_PHASE_IMPORTING) {
        return 0;
    }
    int64_t left = deadline_us(session) - now_us;
    return left > 0 ? (uint32_t)(left / 1000000) : 0;
}
