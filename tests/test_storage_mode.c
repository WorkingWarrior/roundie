#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../components/roundie_storage_mode/storage_mode.c"

static roundie_storage_conditions_t ready(void)
{
    return (roundie_storage_conditions_t){.stable = true, .ota_idle = true, .image_valid = true};
}

int main(void)
{
    uint8_t image[32];
    uint8_t other[32];
    memset(image, 0x5a, sizeof(image));
    memset(other, 0xa5, sizeof(other));

    /* No request: nothing starts, and it stays inactive. */
    assert(!roundie_storage_mode_begin(true, image) && !roundie_storage_mode_active());

    /* One request survives exactly one software restart. */
    roundie_storage_mode_request(image);
    assert(roundie_storage_mode_begin(true, image) && roundie_storage_mode_active());
    assert(!roundie_storage_mode_begin(true, image) && !roundie_storage_mode_active());

    /* A power-on or other reset class clears the request and does not start. */
    roundie_storage_mode_request(image);
    assert(!roundie_storage_mode_begin(false, image));
    assert(!roundie_storage_mode_begin(true, image));

    /* A different firmware image never inherits the request. */
    roundie_storage_mode_request(image);
    assert(!roundie_storage_mode_begin(true, other));
    assert(!roundie_storage_mode_begin(true, image));

    /* A corrupted record, or one with the wrong marker, is ignored and cleared. */
    roundie_storage_mode_request(image);
    record.image[3] ^= 1;
    assert(!roundie_storage_mode_begin(true, image));
    roundie_storage_mode_request(image);
    record.request = 0;
    record.check = checksum();
    assert(!roundie_storage_mode_begin(true, image));
    roundie_storage_mode_request(image);
    record.version = 2;
    record.check = checksum();
    assert(!roundie_storage_mode_begin(true, image));

    /* Uninitialised memory after a power loss is not a request. */
    memset(&record, 0xff, sizeof(record));
    assert(!roundie_storage_mode_begin(true, image));

    /* Admission: every condition refuses, in the documented order. */
    roundie_storage_conditions_t conditions = ready();
    assert(roundie_storage_mode_refusal(&conditions) == ROUNDIE_STORAGE_ALLOWED);
    conditions = ready();
    conditions.storage_mode = true;
    conditions.safe = true;
    assert(roundie_storage_mode_refusal(&conditions) == ROUNDIE_STORAGE_REFUSED_ACTIVE);
    conditions = ready();
    conditions.safe = true;
    assert(roundie_storage_mode_refusal(&conditions) == ROUNDIE_STORAGE_REFUSED_SAFE_MODE);
    conditions = ready();
    conditions.stable = false;
    assert(roundie_storage_mode_refusal(&conditions) == ROUNDIE_STORAGE_REFUSED_NOT_STABLE);
    conditions = ready();
    conditions.ota_idle = false;
    assert(roundie_storage_mode_refusal(&conditions) == ROUNDIE_STORAGE_REFUSED_OTA);
    conditions = ready();
    conditions.image_valid = false;
    assert(roundie_storage_mode_refusal(&conditions) == ROUNDIE_STORAGE_REFUSED_UNCONFIRMED);
    conditions = ready();
    conditions.app_busy = true;
    assert(roundie_storage_mode_refusal(&conditions) == ROUNDIE_STORAGE_REFUSED_APP);
    assert(strcmp(roundie_storage_refusal_name(ROUNDIE_STORAGE_REFUSED_OTA), "ota") == 0);
    puts("storage mode: one-shot request and admission gates passed");
    return 0;
}
