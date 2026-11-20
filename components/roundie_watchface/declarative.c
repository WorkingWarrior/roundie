/* Declarative backend (docs/watch-face-platform.md): a validated definition
 * becomes at most ROUNDIE_FACE_ELEMENTS_MAX LVGL objects under the host's
 * surface. No code from the package runs; every value was checked by
 * roundie_face_decode_definition(). GUI task only. */
#include <string.h>
#include "private.h"
#include "roundie_fonts.h"

#define MEMORY_RESERVE 12288U /* LVGL pool left for the launcher and the system after activation */

static const lv_font_t *font_of(uint8_t variant)
{
    static const lv_font_t *const fonts[4] = {&roundie_font_14, &roundie_font_20, &roundie_font_28, &roundie_font_48};
    return fonts[variant & 3U];
}

static bool condition_holds(const roundie_face_declarative_t *face, const roundie_face_element_t *element)
{
    uint32_t flags = face->data.flags;
    switch (element->condition) {
    case ROUNDIE_FACE_WHEN_TIME_VALID: return (flags & RWF_DATA_TIME_VALID) != 0;
    case ROUNDIE_FACE_WHEN_TIME_INVALID: return (flags & RWF_DATA_TIME_VALID) == 0;
    case ROUNDIE_FACE_WHEN_BATTERY_VALID: return (flags & RWF_DATA_BATTERY_VALID) != 0;
    case ROUNDIE_FACE_WHEN_CHARGING: return (flags & RWF_DATA_CHARGING) != 0;
    case ROUNDIE_FACE_WHEN_NOT_CHARGING: return (flags & RWF_DATA_CHARGING) == 0;
    case ROUNDIE_FACE_WHEN_SETTING_IS: return face->settings[element->condition_setting] == element->condition_value;
    case ROUNDIE_FACE_WHEN_SETTING_NOT: return face->settings[element->condition_setting] != element->condition_value;
    default: return true;
    }
}

/* Hand angle; a hand without trusted time points to 12. */
static uint16_t hand_angle(const roundie_face_declarative_t *face, const roundie_face_element_t *element)
{
    if ((face->data.flags & RWF_DATA_TIME_VALID) == 0) {
        return 0;
    }
    return (uint16_t)roundie_face_hand_angle(face->data.ms_of_day, element->variant);
}

static void line_at(lv_layer_t *layer, const lv_area_t *coords, int32_t cx, int32_t cy, uint32_t tenths,
                    int32_t from, int32_t to, int32_t width, uint32_t color)
{
    int16_t degrees = (int16_t)((tenths + 5U) / 10U % 360U);
    int32_t sin_value = lv_trigo_sin(degrees);
    int32_t cos_value = lv_trigo_sin((int16_t)((degrees + 90) % 360));
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(color);
    line.width = width;
    line.round_start = 1;
    line.round_end = 1;
    line.p1.x = coords->x1 + cx + (sin_value * from >> LV_TRIGO_SHIFT);
    line.p1.y = coords->y1 + cy - (cos_value * from >> LV_TRIGO_SHIFT);
    line.p2.x = coords->x1 + cx + (sin_value * to >> LV_TRIGO_SHIFT);
    line.p2.y = coords->y1 + cy - (cos_value * to >> LV_TRIGO_SHIFT);
    lv_draw_line(layer, &line);
}

/* HAND and TICKS draw themselves: one object each, no per-tick objects. */
static void draw_element(lv_event_t *event)
{
    lv_obj_t *object = lv_event_get_target_obj(event);
    roundie_face_declarative_t *face = lv_event_get_user_data(event);
    unsigned index = ROUNDIE_FACE_ELEMENTS_MAX;
    for (unsigned i = 0; i < face->definition->element_count; i++) {
        if (face->objects[i] == object) {
            index = i;
        }
    }
    if (index == ROUNDIE_FACE_ELEMENTS_MAX) {
        return;
    }
    const roundie_face_element_t *element = &face->definition->elements[index];
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t coords;
    lv_obj_get_coords(object, &coords);
    int32_t centre = element->w; /* the object is a square of side 2w+1 around the pivot */
    if (element->type == ROUNDIE_FACE_HAND) {
        line_at(layer, &coords, centre, centre, face->angles[index], -(int32_t)element->extra, element->w,
                element->size, element->color);
        return;
    }
    unsigned count = element->variant;
    unsigned major_every = count == 60 ? 5U : 3U;
    for (unsigned i = 0; i < count; i++) {
        bool major = element->extra > 0 && i % major_every == 0;
        int32_t length = major ? element->extra : element->size;
        line_at(layer, &coords, centre, centre, i * 3600U / count, element->w - length, element->w,
                major ? 3 : 2, major ? element->color2 : element->color);
    }
}

static int32_t create(void *instance, const rwf_host_v1_t *host)
{
    roundie_face_declarative_t *face = instance;
    if (face->definition == NULL || face->created) {
        return RWF_ERR_STATE;
    }
    face->host = host;
    /* Image descriptors point into the verified package bytes; no copy. */
    for (unsigned i = 0; i < face->package->resource_count; i++) {
        const roundie_face_resource_t *resource = &face->package->resources[i];
        lv_image_dsc_t *image = &face->images[i];
        memset(image, 0, sizeof(*image));
        image->header.magic = LV_IMAGE_HEADER_MAGIC;
        image->header.cf = resource->format == RWF_RESOURCE_RGB565 ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_A8;
        image->header.w = resource->width;
        image->header.h = resource->height;
        image->header.stride = resource->width * (resource->format == RWF_RESOURCE_RGB565 ? 2U : 1U);
        image->data_size = resource->length;
        image->data = face->bytes + resource->offset;
    }
    face->created = true;
    return RWF_OK;
}

static void deactivate(void *instance)
{
    roundie_face_declarative_t *face = instance;
    if (face->root != NULL) {
        lv_obj_delete(face->root); /* children with it */
    }
    face->root = NULL;
    memset(face->objects, 0, sizeof(face->objects));
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

static lv_obj_t *build(roundie_face_declarative_t *face, unsigned index, lv_obj_t *parent)
{
    const roundie_face_element_t *element = &face->definition->elements[index];
    lv_obj_t *object = NULL;
    switch (element->type) {
    case ROUNDIE_FACE_LABEL:
        object = lv_label_create(parent);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_font(object, font_of(element->variant), 0);
        lv_obj_set_style_text_color(object, lv_color_hex(element->color), 0);
        lv_obj_set_style_text_opa(object, element->opacity, 0);
        if (element->w > 0) {
            lv_obj_set_width(object, element->w);
            lv_label_set_long_mode(object, LV_LABEL_LONG_MODE_CLIP);
            lv_obj_set_style_text_align(object, element->align == 1 ? LV_TEXT_ALIGN_CENTER :
                element->align == 2 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
        }
        lv_label_set_text_static(object, "");
        lv_obj_set_pos(object, element->x, element->y);
        break;
    case ROUNDIE_FACE_RECT:
        object = plain(parent);
        lv_obj_set_size(object, element->w, element->h);
        lv_obj_set_pos(object, element->x, element->y);
        lv_obj_set_style_bg_color(object, lv_color_hex(element->color), 0);
        lv_obj_set_style_bg_opa(object, element->opacity, 0);
        lv_obj_set_style_radius(object, element->size == ROUNDIE_FACE_RADIUS_CIRCLE ? LV_RADIUS_CIRCLE : element->size, 0);
        if (element->extra > 0) {
            lv_obj_set_style_border_width(object, element->extra, 0);
            lv_obj_set_style_border_color(object, lv_color_hex(element->color2), 0);
            lv_obj_set_style_border_opa(object, LV_OPA_COVER, 0);
        }
        lv_obj_set_style_clip_corner(object, false, 0);
        break;
    case ROUNDIE_FACE_ARC:
        object = lv_arc_create(parent);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_style(object, NULL, LV_PART_KNOB);
        lv_obj_set_size(object, element->w, element->h);
        lv_obj_set_pos(object, element->x, element->y);
        lv_arc_set_rotation(object, 270);
        lv_arc_set_bg_angles(object, 0, 360);
        lv_arc_set_range(object, 0, element->variant == ROUNDIE_FACE_BIND_BATTERY ? 100 :
            element->variant == ROUNDIE_FACE_BIND_HOUR ? 12 : 60);
        lv_obj_set_style_arc_width(object, element->size, LV_PART_MAIN);
        lv_obj_set_style_arc_width(object, element->size, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(object, lv_color_hex(element->color2), LV_PART_MAIN);
        lv_obj_set_style_arc_color(object, lv_color_hex(element->color), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(object, element->opacity, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(object, 0, 0);
        break;
    case ROUNDIE_FACE_IMAGE:
        object = lv_image_create(parent);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
        lv_image_set_src(object, &face->images[element->variant]);
        lv_obj_set_pos(object, element->x, element->y);
        lv_obj_set_style_image_opa(object, element->opacity, 0);
        if (face->package->resources[element->variant].format == RWF_RESOURCE_A8) {
            lv_obj_set_style_image_recolor(object, lv_color_hex(element->color), 0);
            lv_obj_set_style_image_recolor_opa(object, LV_OPA_COVER, 0);
        }
        break;
    case ROUNDIE_FACE_HAND:
    case ROUNDIE_FACE_TICKS:
        object = plain(parent);
        lv_obj_set_size(object, element->w * 2 + 1, element->w * 2 + 1);
        lv_obj_set_pos(object, element->x - element->w, element->y - element->w);
        lv_obj_add_event_cb(object, draw_element, LV_EVENT_DRAW_MAIN, face);
        lv_obj_set_style_opa(object, element->opacity, 0);
        break;
    default:
        break;
    }
    return object;
}

static int32_t activate(void *instance, rwf_surface_t *surface)
{
    roundie_face_declarative_t *face = instance;
    if (!face->created || face->root != NULL || surface == NULL) {
        return RWF_ERR_STATE;
    }
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    if (memory.free_size < MEMORY_RESERVE + (size_t)face->definition->element_count * 512U) {
        return RWF_ERR_NO_MEMORY;
    }
    face->root = plain((lv_obj_t *)surface);
    if (face->root == NULL) {
        return RWF_ERR_NO_MEMORY;
    }
    lv_obj_set_size(face->root, 360, 360);
    lv_obj_set_pos(face->root, 0, 0);
    lv_obj_set_style_bg_color(face->root, lv_color_hex(face->definition->background), 0);
    lv_obj_set_style_bg_opa(face->root, LV_OPA_COVER, 0);
    for (unsigned i = 0; i < face->definition->element_count; i++) {
        uint8_t parent = face->definition->elements[i].parent;
        face->objects[i] = build(face, i, parent == ROUNDIE_FACE_NO_PARENT ? face->root : face->objects[parent]);
        if (face->objects[i] == NULL) {
            return RWF_ERR_NO_MEMORY; /* the host deactivates, which deletes the partial tree */
        }
    }
    lv_mem_monitor(&memory);
    if (memory.free_size < MEMORY_RESERVE) {
        return RWF_ERR_NO_MEMORY;
    }
    return RWF_OK;
}

static void apply(roundie_face_declarative_t *face)
{
    for (unsigned i = 0; i < face->definition->element_count; i++) {
        const roundie_face_element_t *element = &face->definition->elements[i];
        lv_obj_t *object = face->objects[i];
        bool shown = condition_holds(face, element);
        if (shown == lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) {
            if (shown) {
                lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (!shown) {
            continue;
        }
        if (element->type == ROUNDIE_FACE_LABEL) {
            char text[ROUNDIE_FACE_TEXT_MAX];
            roundie_face_expand(roundie_face_text(face->definition, element->text), &face->data, text, sizeof(text));
            if (strcmp(lv_label_get_text(object), text) != 0) {
                lv_label_set_text(object, text);
            }
        } else if (element->type == ROUNDIE_FACE_ARC) {
            int32_t value = 0;
            uint32_t seconds = face->data.ms_of_day / 1000U;
            bool time_valid = (face->data.flags & RWF_DATA_TIME_VALID) != 0;
            switch (element->variant) {
            case ROUNDIE_FACE_BIND_BATTERY:
                value = (face->data.flags & RWF_DATA_BATTERY_VALID) != 0 ? face->data.battery_percent : 0;
                break;
            case ROUNDIE_FACE_BIND_SECOND: value = time_valid ? (int32_t)(seconds % 60U) : 0; break;
            case ROUNDIE_FACE_BIND_MINUTE: value = time_valid ? (int32_t)(seconds / 60U % 60U) : 0; break;
            default: value = time_valid ? (int32_t)(seconds / 3600U % 12U) : 0; break;
            }
            if (lv_arc_get_value(object) != value) {
                lv_arc_set_value(object, value);
            }
        } else if (element->type == ROUNDIE_FACE_HAND) {
            uint16_t angle = hand_angle(face, element);
            if (angle != face->angles[i]) {
                face->angles[i] = angle;
                lv_obj_invalidate(object);
            }
        }
    }
}

/* Topmost shown element with an action under the point. */
static void tap(roundie_face_declarative_t *face, int32_t x, int32_t y)
{
    for (unsigned i = face->definition->element_count; i-- > 0;) {
        const roundie_face_element_t *element = &face->definition->elements[i];
        lv_obj_t *object = face->objects[i];
        if (element->action == ROUNDIE_FACE_ACTION_NONE || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_area_t area;
        lv_obj_get_coords(object, &area);
        if (x < area.x1 || x > area.x2 || y < area.y1 || y > area.y2) {
            continue;
        }
        if (element->action == ROUNDIE_FACE_ACTION_CYCLE_SETTING) {
            unsigned index = element->action_setting;
            uint8_t value = (uint8_t)((face->settings[index] + 1U) % face->definition->settings[index].options);
            face->settings[index] = value;
            face->host->setting_set(face->host->context, index, value);
        } else {
            face->host->request_refresh(face->host->context);
        }
        return;
    }
}

static int32_t update(void *instance, const rwf_event_v1_t *event, const rwf_data_v1_t *data)
{
    roundie_face_declarative_t *face = instance;
    if (face->root == NULL || event == NULL || data == NULL || data->size != sizeof(rwf_data_v1_t)) {
        return RWF_ERR_STATE;
    }
    face->data = *data;
    if (event->kind == RWF_EVENT_TAP) {
        tap(face, event->x, event->y);
    }
    apply(face);
    return RWF_OK;
}

static void destroy(void *instance)
{
    roundie_face_declarative_t *face = instance;
    deactivate(face);
    face->created = false;
    face->host = NULL;
}

void roundie_face_declarative_bind(roundie_face_loaded_t *loaded)
{
    loaded->vtable = (rwf_face_v1_t){
        .size = sizeof(rwf_face_v1_t),
        .abi_major = loaded->package.manifest.abi_major,
        .abi_minor = loaded->package.manifest.abi_minor,
        .requires = loaded->package.manifest.requires,
        .needs = loaded->definition.needs,
        .create = create,
        .activate = activate,
        .update = update,
        .deactivate = deactivate,
        .destroy = destroy,
    };
    loaded->instance = (roundie_face_declarative_t){
        .definition = &loaded->definition,
        .package = &loaded->package,
        .bytes = loaded->bytes,
        .settings = loaded->settings,
    };
}
