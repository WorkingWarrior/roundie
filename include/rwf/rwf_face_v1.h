#pragma once
/* Roundie Watch Face ABI v1 (docs/watch-face-abi.md).
 *
 * A separate, versioned interface between WatchFaceHost and a watch face
 * backend. It is not REP ABI 1/2/3 and shares no table, symbol or version
 * number with it. Fixed-width fields, explicit sizes, UTF-8 text; usable with
 * C11 and C23. No LVGL, FreeRTOS, ESP-IDF or libc structure appears here.
 *
 * Roles:
 *   rwf_host_v1_t  what the host offers a face: data, refresh, update needs,
 *                  package resources, per-face settings. Nothing else: a face
 *                  cannot reach the RTC, OTA, BootGuard, the Power Manager,
 *                  navigation, storage paths or another face.
 *   rwf_face_v1_t  what a face implements: create, activate, update,
 *                  deactivate, destroy. The host calls every entry from the
 *                  GUI task only, never while suspended, and never after
 *                  destroy.
 *
 * Negotiation: a face states the major/minor it was built for and the host
 * capabilities it requires. The host refuses a different major, a newer minor
 * than its own, or a required capability it lacks. */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RWF_ABI_MAJOR 1U
#define RWF_ABI_MINOR 0U

/* Results. Negative values are failures. */
#define RWF_OK 0
#define RWF_ERR_INVALID (-1)     /* bad argument or definition */
#define RWF_ERR_NO_MEMORY (-2)   /* allocation or object budget */
#define RWF_ERR_UNSUPPORTED (-3) /* ABI, capability or backend */
#define RWF_ERR_STATE (-4)       /* lifecycle order */

/* rwf_data_v1_t.flags */
#define RWF_DATA_TIME_VALID (1U << 0)    /* local and UTC time are trusted */
#define RWF_DATA_BATTERY_VALID (1U << 1) /* battery_percent is a reading */
#define RWF_DATA_CHARGING (1U << 2)
#define RWF_DATA_VISIBLE (1U << 3) /* the screen shows the face */

/* One consistent sample, taken by the host. Without RWF_DATA_TIME_VALID the
 * time fields are zero and a face shows that time is unavailable. */
typedef struct {
    uint32_t size;          /* sizeof(rwf_data_v1_t) */
    uint32_t flags;         /* RWF_DATA_* */
    int64_t utc_seconds;    /* seconds since 1970-01-01 UTC */
    uint32_t local_days;    /* local days since 1970-01-01 */
    uint32_t ms_of_day;     /* local milliseconds since midnight */
    int32_t utc_offset_s;   /* local minus UTC, includes any DST in effect */
    uint32_t time_revision; /* changes on a sync, a zone change or a jump */
    uint8_t battery_percent;
    uint8_t reserved[3];
    uint32_t battery_revision; /* changes when percent or charging changes */
    char zone[48];             /* POSIX TZ rule in effect, NUL-terminated */
} rwf_data_v1_t;

/* rwf_event_v1_t.kind; one event per update call. */
typedef enum {
    RWF_EVENT_ACTIVATED = 1, /* first update after activate (also after resume) */
    RWF_EVENT_SECOND = 2,
    RWF_EVENT_MINUTE = 3,
    RWF_EVENT_DATE = 4,      /* the local date changed */
    RWF_EVENT_ZONE = 5,      /* time_revision changed: sync, zone or jump */
    RWF_EVENT_BATTERY = 6,
    RWF_EVENT_VISIBLE = 7,   /* RWF_DATA_VISIBLE changed */
    RWF_EVENT_RESUMED = 8,   /* after light sleep: the current sample only */
    RWF_EVENT_TAP = 9,       /* a tap on the face, x/y in face pixels */
    RWF_EVENT_SETTING = 10,  /* a setting changed; index in value */
    RWF_EVENT_REFRESH = 11,  /* the face asked for a refresh */
} rwf_event_kind_v1_t;

typedef struct {
    uint32_t size; /* sizeof(rwf_event_v1_t) */
    uint32_t kind; /* rwf_event_kind_v1_t */
    int16_t x;
    int16_t y;
    uint32_t value;
} rwf_event_v1_t;

/* Update needs (rwf_face_v1_t.needs and set_needs). The host schedules only
 * what a face asks for; nothing renders continuously. */
#define RWF_NEED_SECOND (1U << 0)  /* else minute updates only */
#define RWF_NEED_BATTERY (1U << 1)
#define RWF_NEED_TAP (1U << 2)

/* Host capabilities (rwf_host_v1_t.caps, rwf_face_v1_t.requires). */
#define RWF_HOST_CAP_RESOURCES (1U << 0)
#define RWF_HOST_CAP_SETTINGS (1U << 1)
#define RWF_HOST_CAP_BATTERY (1U << 2)

/* rwf_resource_v1_t.format */
#define RWF_RESOURCE_RGB565 1U /* width * height * 2 bytes, little-endian */
#define RWF_RESOURCE_A8 2U     /* width * height bytes of alpha, tinted */

/* A package resource. data stays valid until destroy; read-only. */
typedef struct {
    uint32_t size;
    uint16_t format;
    uint16_t width;
    uint16_t height;
    uint16_t reserved;
    uint32_t length;
    const uint8_t *data;
} rwf_resource_v1_t;

/* Opaque drawing surface handed to activate. In firmware backends it is the
 * host's root object; the ABI does not say what it is. */
typedef struct rwf_surface rwf_surface_t;

typedef struct rwf_host_v1 {
    uint32_t size; /* sizeof(rwf_host_v1_t) */
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t caps; /* RWF_HOST_CAP_* */
    void *context; /* host-owned; passed back unchanged */
    /* Copies the latest sample; false before the host has one. */
    bool (*data)(void *context, rwf_data_v1_t *out);
    /* Asks for one RWF_EVENT_REFRESH update; coalesced, never re-entrant. */
    void (*request_refresh)(void *context);
    /* Replaces the face's update needs (RWF_NEED_*). */
    void (*set_needs)(void *context, uint32_t needs);
    /* Package resource by index; false when absent. */
    bool (*resource)(void *context, uint32_t index, rwf_resource_v1_t *out);
    /* Per-face setting by index: the option index, or a negative error. */
    int32_t (*setting_get)(void *context, uint32_t index);
    /* Stores an option index. The host persists it outside the GUI task. */
    int32_t (*setting_set)(void *context, uint32_t index, uint32_t value);
} rwf_host_v1_t;

typedef struct rwf_face_v1 {
    uint32_t size; /* sizeof(rwf_face_v1_t) */
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t requires; /* RWF_HOST_CAP_* the face cannot work without */
    uint32_t needs;    /* initial RWF_NEED_* */
    /* No drawing: validate, allocate, bind resources. */
    int32_t (*create)(void *instance, const rwf_host_v1_t *host);
    /* Build the visible face under surface. */
    int32_t (*activate)(void *instance, rwf_surface_t *surface);
    /* Apply one event with the current sample. Bounded work, no waiting. */
    int32_t (*update)(void *instance, const rwf_event_v1_t *event, const rwf_data_v1_t *data);
    /* Remove everything activate built. Cannot fail. */
    void (*deactivate)(void *instance);
    /* Release everything create allocated. Cannot fail. */
    void (*destroy)(void *instance);
} rwf_face_v1_t;

#ifdef __cplusplus
}
#endif
