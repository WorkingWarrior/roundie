#pragma once

/* Virtual hardware lab of the Linux desktop (docs/virtual-hardware.md).
 *
 * A functional model of the board's peripherals below the board API
 * (roundie_board.h): battery and its gauge or PMIC, the power path with USB
 * and the PWR latch, and the faults of the display, touch, audio and storage.
 * Commands of the desktop script and of the control panel change it; the
 * desktop BSP reads it. It models what the firmware can observe, not
 * electrical behaviour; every number is a working assumption unless
 * docs/virtual-hardware.md says otherwise.
 *
 * One machine per process, under one lock: every function may be called from
 * any thread. Times are roundie_time_us() values passed in by the caller, so
 * the model is host-testable. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "roundie_board.h"

/* Which chip reports the battery. */
typedef enum {
    VHW_CHIP_GAUGE, /* 1.85B: BQ27220 fuel gauge */
    VHW_CHIP_PMIC,  /* 1.75C: AXP2101, voltage and presence only */
} vhw_chip_t;

/* An I2C device's answers. */
typedef enum {
    VHW_BUS_OK,
    VHW_BUS_TIMEOUT, /* transactions time out: ROUNDIE_SENSOR_COMM_ERROR */
    VHW_BUS_ABSENT,  /* no acknowledge: ROUNDIE_SENSOR_NOT_FOUND */
} vhw_bus_t;

typedef enum {
    VHW_TOUCH_OK,
    VHW_TOUCH_TIMEOUT, /* reads fail with ESP_ERR_TIMEOUT */
    VHW_TOUCH_NOISE,   /* reads succeed with spurious points, also off the panel */
    VHW_TOUCH_ABSENT,  /* no controller: ESP_ERR_NOT_FOUND */
} vhw_touch_t;

typedef enum {
    VHW_DISPLAY_OK,
    VHW_DISPLAY_RESTORE, /* re-initialisation fails and panel memory is lost */
    VHW_DISPLAY_INIT,    /* the panel does not start: bring-up stops at boot */
} vhw_display_t;

typedef enum {
    VHW_AUDIO_OK,
    VHW_AUDIO_UNAVAILABLE, /* no output device: start fails, writes fail */
} vhw_audio_t;

typedef enum {
    VHW_STORAGE_OK,
    VHW_STORAGE_ABSENT, /* not mounted: ENOENT */
    VHW_STORAGE_IO,     /* operations fail with EIO */
} vhw_storage_t;

/* With VHW_STORAGE_IO: the wrapped call that fails (vhw_storage.c), or all. */
typedef enum {
    VHW_STORAGE_CALL_ALL,
    VHW_STORAGE_CALL_OPEN,
    VHW_STORAGE_CALL_READ,
    VHW_STORAGE_CALL_WRITE,
    VHW_STORAGE_CALL_FSYNC,
    VHW_STORAGE_CALL_MKDIR,
    VHW_STORAGE_CALL_RMDIR,
    VHW_STORAGE_CALL_UNLINK,
    VHW_STORAGE_CALL_RENAME,
    VHW_STORAGE_CALL_STAT,
    VHW_STORAGE_CALL_OPENDIR,
} vhw_storage_call_t;

/* What the machine does next, for the desktop to carry out. */
typedef enum {
    VHW_ACTION_NONE,
    VHW_ACTION_POWER_OFF, /* power lost: stop at once, no shutdown */
    VHW_ACTION_POWER_ON,  /* power returned while off: power-on reset */
    VHW_ACTION_RESET,     /* a requested reset (vhw_request_reset) */
} vhw_action_t;

/* Why the machine is off. */
typedef enum {
    VHW_OFF_NONE,
    VHW_OFF_CUT,      /* power-cut: supply removed */
    VHW_OFF_DEPLETED, /* battery empty, no USB */
    VHW_OFF_LATCH,    /* PWR held: the latch opened */
} vhw_off_t;

typedef struct {
    vhw_chip_t chip;
    bool powered;
    vhw_off_t off_reason;
    bool usb;
    bool battery_present;
    uint32_t full_mah;
    uint32_t charge_mah_x1000; /* stored charge, micro-ampere-hours */
    uint32_t soc_percent;      /* the true state of charge, rounded */
    uint16_t open_circuit_mv;
    int32_t current_ma; /* battery: positive = charging */
    uint32_t load_ma;
    uint32_t sleep_load_ma;
    uint32_t charge_ma;
    bool sleeping;
    bool rtc_backup; /* the RTC keeps time while the board is off */
    vhw_bus_t gauge;
    vhw_touch_t touch;
    vhw_display_t display;
    vhw_audio_t audio;
    vhw_storage_t storage;
    vhw_storage_call_t storage_call;
    uint64_t storage_limit_bytes; /* 0: the host file system's free space */
    uint32_t audio_underruns;     /* requested, not yet delivered */
    uint32_t power_cycles;
} vhw_state_t;

/* Initial machine: powered, battery at 80 %, USB off, no faults. */
void vhw_init(vhw_chip_t chip, int64_t now_us);
void vhw_get(vhw_state_t *state, int64_t now_us);

/* ---- Lab controls ---- */
void vhw_set_usb(bool connected, int64_t now_us);
void vhw_set_battery_present(bool present, int64_t now_us);
/* Sets the stored charge; soc_percent 0..100. */
void vhw_set_soc(uint32_t soc_percent, int64_t now_us);
void vhw_set_capacity(uint32_t full_mah, int64_t now_us);
/* System draw while awake and in light sleep, and the charger's current. */
void vhw_set_load(uint32_t awake_ma, uint32_t sleep_ma, int64_t now_us);
void vhw_set_charge_current(uint32_t ma, int64_t now_us);
void vhw_set_gauge(vhw_bus_t bus);
void vhw_set_rtc_backup(bool kept);
void vhw_set_touch(vhw_touch_t fault);
void vhw_set_display(vhw_display_t fault);
void vhw_set_audio(vhw_audio_t fault);
void vhw_set_storage(vhw_storage_t fault);
/* Which call VHW_STORAGE_IO fails. */
void vhw_set_storage_call(vhw_storage_call_t call);
void vhw_set_storage_limit(uint64_t bytes);
/* The storage fault without the lock: async-signal-safe. */
vhw_storage_t vhw_storage_fault(void);
/* Whether the wrapped call fails now, without the lock: async-signal-safe. */
bool vhw_storage_call_fails(vhw_storage_call_t call);
void vhw_request_underrun(void);
/* The supply goes away (power-cut) or comes back (power-restore: USB and a
 * charged battery as they were). */
void vhw_power_cut(int64_t now_us);
void vhw_power_restore(int64_t now_us);
/* A reset the desktop then carries out: sw, panic, wdt, brownout or pin. */
void vhw_request_reset(int kind);

/* ---- The desktop ---- */
/* The PWR key, sampled by the GUI loop. Assumption from Waveshare's 1.85B
 * description, unmeasured: held 1 s on battery while off switches on, held
 * 2 s while on battery switches off. With USB the board stays on. */
void vhw_set_pwr(bool pressed, int64_t now_us);
void vhw_set_sleeping(bool sleeping, int64_t now_us);
/* Integrates the battery up to now and returns what the desktop must do;
 * *reset_kind is set for VHW_ACTION_RESET. Each action is returned once. */
vhw_action_t vhw_poll(int64_t now_us, int *reset_kind);
/* The board call: what the gauge (1.85B) or PMIC (1.75C) reports. */
void vhw_battery_sample(roundie_battery_sample_t *sample, int64_t now_us);
/* One underrun requested by the lab, consumed once. */
bool vhw_take_underrun(void);

/* ---- Restart of the desktop process (reboot, power) ---- */
/* Text lines "vhw.KEY VALUE"; load ignores other keys. Load re-anchors time at now_us. */
void vhw_save(FILE *file, int64_t now_us);
bool vhw_load_line(const char *key, const char *value, int64_t now_us);

const char *vhw_bus_name(vhw_bus_t bus);
const char *vhw_off_name(vhw_off_t reason);
