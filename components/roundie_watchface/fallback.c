/* The built-in fallback face (docs/watch-face-platform.md): firmware only,
 * no package, no storage, no ELF. Time, date and battery, or "--:--" and
 * "Brak czasu" without trusted time. GUI task only. */
#include <string.h>
#include "private.h"
#include "roundie_fonts.h"

typedef struct {
    const rwf_host_v1_t *host;
    lv_obj_t *root;
    lv_obj_t *time;
    lv_obj_t *date;
    lv_obj_t *battery;
} fallback_t;

static fallback_t fallback;

static lv_obj_t *line(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) {
        return NULL;
    }
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(label, 280);
    lv_obj_set_pos(label, 40, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text_static(label, "");
    return label;
}

static int32_t create(void *instance, const rwf_host_v1_t *host)
{
    fallback_t *face = instance;
    face->host = host;
    return RWF_OK;
}

static void deactivate(void *instance)
{
    fallback_t *face = instance;
    if (face->root != NULL) {
        lv_obj_delete(face->root);
    }
    face->root = NULL;
    face->time = NULL;
    face->date = NULL;
    face->battery = NULL;
}

static int32_t activate(void *instance, rwf_surface_t *surface)
{
    fallback_t *face = instance;
    if (face->root != NULL || surface == NULL) {
        return RWF_ERR_STATE;
    }
    face->root = lv_obj_create((lv_obj_t *)surface);
    if (face->root == NULL) {
        return RWF_ERR_NO_MEMORY;
    }
    lv_obj_remove_style_all(face->root);
    lv_obj_remove_flag(face->root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(face->root, 360, 360);
    lv_obj_set_style_bg_color(face->root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(face->root, LV_OPA_COVER, 0);
    face->time = line(face->root, &roundie_font_48, 0xffffff, 128);
    face->date = line(face->root, &roundie_font_20, 0xb8c4d0, 196);
    face->battery = line(face->root, &roundie_font_20, 0x7f8c99, 236);
    if (face->time == NULL || face->date == NULL || face->battery == NULL) {
        return RWF_ERR_NO_MEMORY; /* the host deactivates */
    }
    return RWF_OK;
}

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static int32_t update(void *instance, const rwf_event_v1_t *event, const rwf_data_v1_t *data)
{
    fallback_t *face = instance;
    (void)event;
    if (face->root == NULL || data == NULL || data->size != sizeof(rwf_data_v1_t)) {
        return RWF_ERR_STATE;
    }
    char text[ROUNDIE_FACE_TEXT_MAX];
    bool valid = (data->flags & RWF_DATA_TIME_VALID) != 0;
    roundie_face_expand("{HH}:{MM}", data, text, sizeof(text));
    set_text(face->time, valid ? text : "--:--");
    roundie_face_expand("{WEEKDAY}, {D} {MONTH}", data, text, sizeof(text));
    set_text(face->date, valid ? text : "Brak czasu");
    if ((data->flags & RWF_DATA_BATTERY_VALID) != 0) {
        roundie_face_expand((data->flags & RWF_DATA_CHARGING) != 0 ? "Bateria {BAT}% • ładowanie" : "Bateria {BAT}%",
                            data, text, sizeof(text));
        set_text(face->battery, text);
    } else {
        set_text(face->battery, "");
    }
    return RWF_OK;
}

static void destroy(void *instance)
{
    fallback_t *face = instance;
    deactivate(face);
    face->host = NULL;
}

const rwf_face_v1_t roundie_face_fallback = {
    .size = sizeof(rwf_face_v1_t),
    .abi_major = RWF_ABI_MAJOR,
    .abi_minor = RWF_ABI_MINOR,
    .requires = 0,
    .needs = RWF_NEED_BATTERY,
    .create = create,
    .activate = activate,
    .update = update,
    .deactivate = deactivate,
    .destroy = destroy,
};

void *const roundie_face_fallback_instance = &fallback;
