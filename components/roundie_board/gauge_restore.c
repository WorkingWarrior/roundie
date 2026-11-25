#include "roundie_gauge.h"

#include <stddef.h>

bool roundie_gauge_restore_due(roundie_gauge_restore_t *restore, uint16_t target_mah,
                               bool present, bool ready, bool design_valid,
                               uint16_t design_mah)
{
    if (restore == NULL || target_mah == 0) {
        return false;
    }
    if (!present) {
        /* A new battery means a new gauge power-on reset: one new attempt. */
        restore->attempted = false;
        return false;
    }
    if (!ready || !design_valid) {
        return false;
    }
    if (design_mah == target_mah) {
        restore->attempted = false;
        return false;
    }
    if (restore->attempted) {
        return false;
    }
    restore->attempted = true;
    restore->attempts++;
    return true;
}

void roundie_gauge_restore_done(roundie_gauge_restore_t *restore, esp_err_t error)
{
    if (restore != NULL) {
        restore->last_error = error;
    }
}
