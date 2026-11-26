/* Host stand-in for main/io_worker.c, which is FreeRTOS-only. There is no
 * worker to quiesce on the host, so parking always succeeds immediately. */
#include <stdbool.h>
#include <stdint.h>

#include "io_worker.h"

bool roundie_io_worker_park(int64_t timeout_us)
{
    (void)timeout_us;
    return true;
}

void roundie_io_worker_resume(void)
{
}

void roundie_io_worker_stall(uint32_t stall_ms)
{
    (void)stall_ms;
}

bool roundie_io_worker_dev_command(const char *operation, const char *id)
{
    (void)operation;
    (void)id;
    return false;
}
