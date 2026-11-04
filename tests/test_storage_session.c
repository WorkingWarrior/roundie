#include <assert.h>
#include <stdio.h>
#include "../components/roundie_storage_mode/session.c"

#define S(seconds) (INT64_C(1000000) * (seconds))

int main(void)
{
    roundie_storage_session_t session;

    /* Nobody connects: the mode ends after the no-host timeout. */
    roundie_storage_session_init(&session, S(10));
    assert(session.phase == ROUNDIE_STORAGE_PHASE_WAITING_FOR_HOST);
    assert(roundie_storage_session_seconds_left(&session, S(10)) == 120);
    assert(roundie_storage_session_poll(&session, false, 0, S(129)) == ROUNDIE_STORAGE_ACTION_NONE);
    assert(roundie_storage_session_poll(&session, false, 0, S(130)) == ROUNDIE_STORAGE_ACTION_EXIT);

    /* A host takes the drive; activity postpones the idle timeout. */
    roundie_storage_session_init(&session, 0);
    assert(roundie_storage_session_poll(&session, true, 5, S(5)) == ROUNDIE_STORAGE_ACTION_NONE);
    assert(session.phase == ROUNDIE_STORAGE_PHASE_HOST_OWNS);
    assert(roundie_storage_session_poll(&session, true, 5, S(604)) == ROUNDIE_STORAGE_ACTION_NONE);
    assert(roundie_storage_session_poll(&session, true, 6, S(700)) == ROUNDIE_STORAGE_ACTION_NONE); /* I/O seen */
    assert(roundie_storage_session_poll(&session, true, 6, S(1299)) == ROUNDIE_STORAGE_ACTION_NONE);
    assert(roundie_storage_session_poll(&session, true, 6, S(1300)) == ROUNDIE_STORAGE_ACTION_EXIT);

    /* Eject: the drive goes back to the firmware and a shorter timeout runs. */
    roundie_storage_session_init(&session, 0);
    roundie_storage_session_poll(&session, true, 1, S(1));
    assert(roundie_storage_session_poll(&session, false, 1, S(30)) == ROUNDIE_STORAGE_ACTION_NONE);
    assert(session.phase == ROUNDIE_STORAGE_PHASE_EJECTED);
    assert(roundie_storage_session_seconds_left(&session, S(30)) == 20);
    assert(roundie_storage_session_poll(&session, false, 1, S(49)) == ROUNDIE_STORAGE_ACTION_NONE);
    assert(roundie_storage_session_poll(&session, false, 1, S(50)) == ROUNDIE_STORAGE_ACTION_EXIT);

    /* Re-attach during the ejected window: the host owns the drive again. */
    roundie_storage_session_init(&session, 0);
    roundie_storage_session_poll(&session, true, 1, S(1));
    roundie_storage_session_poll(&session, false, 1, S(2));
    roundie_storage_session_bus_event(&session, ROUNDIE_STORAGE_BUS_ATTACHED, S(5));
    assert(roundie_storage_session_poll(&session, true, 3, S(6)) == ROUNDIE_STORAGE_ACTION_NONE);
    assert(session.phase == ROUNDIE_STORAGE_PHASE_HOST_OWNS);

    /* A pulled cable: suspend without detach. The host loses the drive after the limit. */
    roundie_storage_session_init(&session, 0);
    roundie_storage_session_poll(&session, true, 1, S(1));
    roundie_storage_session_bus_event(&session, ROUNDIE_STORAGE_BUS_SUSPENDED, S(100));
    assert(roundie_storage_session_poll(&session, true, 1, S(119)) == ROUNDIE_STORAGE_ACTION_NONE);
    assert(roundie_storage_session_poll(&session, true, 1, S(120)) == ROUNDIE_STORAGE_ACTION_TAKE_BACK);
    assert(roundie_storage_session_poll(&session, false, 1, S(121)) == ROUNDIE_STORAGE_ACTION_NONE);
    assert(session.phase == ROUNDIE_STORAGE_PHASE_EJECTED && !session.suspended);

    /* Resume before the limit cancels the take-back; a second suspend event does not restart it. */
    roundie_storage_session_init(&session, 0);
    roundie_storage_session_poll(&session, true, 1, S(1));
    roundie_storage_session_bus_event(&session, ROUNDIE_STORAGE_BUS_SUSPENDED, S(10));
    roundie_storage_session_bus_event(&session, ROUNDIE_STORAGE_BUS_SUSPENDED, S(25));
    assert(roundie_storage_session_poll(&session, true, 1, S(30)) == ROUNDIE_STORAGE_ACTION_TAKE_BACK);
    roundie_storage_session_init(&session, 0);
    roundie_storage_session_poll(&session, true, 1, S(1));
    roundie_storage_session_bus_event(&session, ROUNDIE_STORAGE_BUS_SUSPENDED, S(10));
    roundie_storage_session_bus_event(&session, ROUNDIE_STORAGE_BUS_RESUMED, S(15));
    assert(roundie_storage_session_poll(&session, true, 1, S(40)) == ROUNDIE_STORAGE_ACTION_NONE);

    /* Suspend with nobody owning the drive never takes anything back. */
    roundie_storage_session_init(&session, 0);
    roundie_storage_session_bus_event(&session, ROUNDIE_STORAGE_BUS_SUSPENDED, S(1));
    assert(roundie_storage_session_poll(&session, false, 0, S(60)) == ROUNDIE_STORAGE_ACTION_NONE);
    assert(roundie_storage_session_seconds_left(&session, S(500)) == 0);
    puts("storage session: ownership, take-back and timeout policy passed");
    return 0;
}
