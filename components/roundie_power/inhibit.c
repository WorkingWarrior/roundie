#include "roundie_power_inhibit.h"

#include <string.h>

/* Handle layout: bits 0..7 slot + 1, bits 16..31 generation (never 0). */
static roundie_power_inhibit_handle_t make_handle(uint32_t slot, uint16_t generation)
{
    return ((uint32_t)generation << 16) | (slot + 1U);
}

static void free_slot(roundie_power_inhibitors_t *table, roundie_power_inhibit_slot_t *slot, int64_t now_us)
{
    int64_t held_us = now_us - slot->since_us;
    if (held_us > table->longest_hold_us) {
        table->longest_hold_us = held_us;
    }
    slot->held = false;
    slot->owner = ROUNDIE_POWER_OWNER_NONE;
    slot->kinds = 0;
    /* A later holder of this slot gets a new generation, so the old handle
     * stays stale. 0 is skipped: a handle is never 0. */
    slot->generation++;
    if (slot->generation == 0U) {
        slot->generation = 1U;
    }
}

void roundie_power_inhibit_init(roundie_power_inhibitors_t *table)
{
    memset(table, 0, sizeof(*table));
    for (uint32_t index = 0; index < ROUNDIE_POWER_INHIBIT_SLOTS; index++) {
        table->slots[index].generation = 1U;
    }
}

roundie_power_inhibit_result_t roundie_power_inhibit_acquire(roundie_power_inhibitors_t *table,
    roundie_power_owner_t owner, uint32_t kinds, int64_t now_us, int64_t timeout_us,
    roundie_power_inhibit_handle_t *handle)
{
    *handle = 0;
    if (owner <= ROUNDIE_POWER_OWNER_NONE || owner >= ROUNDIE_POWER_OWNER_COUNT || kinds == 0U ||
        (kinds & ~ROUNDIE_POWER_INHIBIT_KINDS) != 0U || timeout_us <= 0 ||
        timeout_us > ROUNDIE_POWER_INHIBIT_MAX_TIMEOUT_US) {
        return ROUNDIE_POWER_INHIBIT_INVALID;
    }
    for (uint32_t index = 0; index < ROUNDIE_POWER_INHIBIT_SLOTS; index++) {
        roundie_power_inhibit_slot_t *slot = &table->slots[index];
        if (!slot->held) {
            slot->held = true;
            slot->owner = (uint8_t)owner;
            slot->kinds = (uint8_t)kinds;
            slot->since_us = now_us;
            slot->deadline_us = now_us + timeout_us;
            table->acquired++;
            *handle = make_handle(index, slot->generation);
            return ROUNDIE_POWER_INHIBIT_OK;
        }
    }
    table->full++;
    return ROUNDIE_POWER_INHIBIT_FULL;
}

roundie_power_inhibit_result_t roundie_power_inhibit_release(roundie_power_inhibitors_t *table,
    roundie_power_inhibit_handle_t handle, int64_t now_us)
{
    uint32_t slot_number = handle & 0xffU;
    uint16_t generation = (uint16_t)(handle >> 16);
    if (slot_number == 0U || slot_number > ROUNDIE_POWER_INHIBIT_SLOTS || (handle & 0xff00U) != 0U) {
        table->stale_releases++;
        return ROUNDIE_POWER_INHIBIT_STALE;
    }
    roundie_power_inhibit_slot_t *slot = &table->slots[slot_number - 1U];
    if (!slot->held || slot->generation != generation) {
        table->stale_releases++;
        return ROUNDIE_POWER_INHIBIT_STALE;
    }
    free_slot(table, slot, now_us);
    table->released++;
    return ROUNDIE_POWER_INHIBIT_OK;
}

uint32_t roundie_power_inhibit_release_owner(roundie_power_inhibitors_t *table, roundie_power_owner_t owner,
    int64_t now_us)
{
    uint32_t count = 0;
    for (uint32_t index = 0; index < ROUNDIE_POWER_INHIBIT_SLOTS; index++) {
        roundie_power_inhibit_slot_t *slot = &table->slots[index];
        if (slot->held && slot->owner == (uint8_t)owner) {
            free_slot(table, slot, now_us);
            count++;
        }
    }
    table->owner_releases += count;
    return count;
}

uint32_t roundie_power_inhibit_expire(roundie_power_inhibitors_t *table, int64_t now_us)
{
    uint32_t count = 0;
    for (uint32_t index = 0; index < ROUNDIE_POWER_INHIBIT_SLOTS; index++) {
        roundie_power_inhibit_slot_t *slot = &table->slots[index];
        if (slot->held && now_us >= slot->deadline_us) {
            free_slot(table, slot, now_us);
            count++;
        }
    }
    table->expired += count;
    return count;
}

uint32_t roundie_power_inhibit_active(const roundie_power_inhibitors_t *table)
{
    uint32_t kinds = 0;
    for (uint32_t index = 0; index < ROUNDIE_POWER_INHIBIT_SLOTS; index++) {
        if (table->slots[index].held) {
            kinds |= table->slots[index].kinds;
        }
    }
    return kinds;
}

uint32_t roundie_power_inhibit_held(const roundie_power_inhibitors_t *table, uint32_t *owner_mask)
{
    uint32_t count = 0;
    uint32_t owners = 0;
    for (uint32_t index = 0; index < ROUNDIE_POWER_INHIBIT_SLOTS; index++) {
        if (table->slots[index].held) {
            count++;
            owners |= 1U << table->slots[index].owner;
        }
    }
    if (owner_mask != NULL) {
        *owner_mask = owners;
    }
    return count;
}

const char *roundie_power_owner_name(roundie_power_owner_t owner)
{
    switch (owner) {
    case ROUNDIE_POWER_OWNER_NONE: return "none";
    case ROUNDIE_POWER_OWNER_SYSTEM: return "system";
    case ROUNDIE_POWER_OWNER_OTA: return "ota";
    case ROUNDIE_POWER_OWNER_STORAGE: return "storage";
    case ROUNDIE_POWER_OWNER_AUDIO: return "audio";
    case ROUNDIE_POWER_OWNER_RUNTIME: return "runtime";
    case ROUNDIE_POWER_OWNER_GUI: return "gui";
    case ROUNDIE_POWER_OWNER_DIAGNOSTICS: return "diagnostics";
    case ROUNDIE_POWER_OWNER_COUNT: break;
    }
    return "unknown";
}
