#include "ui/tool_options.h"
#include "doc/json.h"
#include "base/mem.h"
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef enum field_type { BOOL, FLOAT, U32, U8, STRING, BLEND, STROKE, SHIELD, OVERLAY, SHOW } field_type;
typedef struct field {
    const char *name;
    size_t offset, size;
    field_type type;
} field;
static const field fields[] = {
#define PC_UI_OPTION(kind, name, value)                                                                      \
    {#name, offsetof(pc_ui_tool_options, name), sizeof(((pc_ui_tool_options *)0)->name), kind},
#include "ui/tool_options_fields.inc"
#undef PC_UI_OPTION
};
static const char *const overlays[] = {"thirds",   "grid",        "diagonal",
                                       "triangle", "goldenRatio", "goldenSpiral"};
static const char *const shows[] = {"auto", "always", "never"};
const char *pc_ui_crop_overlay_id(pc_ui_crop_overlay overlay)
{
    return (unsigned)overlay < 6 ? overlays[overlay] : overlays[0];
}
unsigned pc_ui_crop_overlay_orientations(pc_ui_crop_overlay overlay)
{
    return overlay == PC_UI_CROP_TRIANGLE ? 2u : overlay == PC_UI_CROP_GOLDEN_SPIRAL ? 4u : 1u;
}
void pc_ui_crop_shield_default(pc_ui_crop_shield *s)
{
    *s = (pc_ui_crop_shield){.auto_center_preview = true,
                             .show_cropped_area = true,
                             .enabled = true,
                             .auto_adjust = true,
                             .opacity = 75};
}
bool pc_ui_crop_shield_fill(const pc_ui_crop_shield *s, pc_ui_color canvas, bool editing, pc_ui_color *out)
{
    if (!s || !out) {
        return false;
    }
    if (!s->classic_mode && !s->show_cropped_area) {
        canvas.a = 255;
        *out = canvas;
        return true;
    }
    if (!s->enabled) {
        return false;
    }
    float opacity = isfinite(s->opacity) ? s->opacity : 75;
    float alpha = fminf(100, fmaxf(0, opacity)) / 100 * (s->auto_adjust && editing ? .5f : 1);
    uint8_t a = (uint8_t)roundf(alpha * 255);
    if (!a) {
        return false;
    }
    *out = s->custom ? (pc_ui_color){s->custom_color[0], s->custom_color[1], s->custom_color[2], a} : canvas;
    out->a = a;
    return true;
}
#define INIT_BOOL(o, n, v) (o)->n = v
#define INIT_FLOAT(o, n, v) (o)->n = v
#define INIT_U32(o, n, v) (o)->n = v
#define INIT_U8(o, n, v) (o)->n = v
#define INIT_BLEND(o, n, v) (o)->n = v
#define INIT_OVERLAY(o, n, v) (o)->n = PC_UI_CROP_THIRDS
#define INIT_SHOW(o, n, v) (o)->n = PC_UI_OVERLAY_AUTO
#define INIT_STRING(o, n, v)                                                                                 \
    do {                                                                                                     \
        if (pc_str_set(&(o)->n, v, sizeof(v) - 1, err) != PC_OK) {                                           \
            rc = PC_ERR_NOMEM;                                                                               \
        }                                                                                                    \
    } while (0)
#define INIT_STROKE(o, n, v) pc_shape_stroke_default(&(o)->n)
#define INIT_SHIELD(o, n, v) pc_ui_crop_shield_default(&(o)->n)
static pc_status init_options(pc_ui_tool_options *o, pc_error *err)
{
    pc_status rc = PC_OK;
    *o = (pc_ui_tool_options){0};
#define PC_UI_OPTION(kind, name, value) INIT_##kind(o, name, value);
#include "ui/tool_options_fields.inc"
#undef PC_UI_OPTION
    return rc;
}
void pc_ui_tool_options_init(pc_ui_tool_options *o)
{
    (void)init_options(o, NULL);
}
void pc_ui_tool_options_fini(pc_ui_tool_options *o)
{
    if (!o) {
        return;
    }
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
        const field *f = &fields[i];
        void *value = (char *)o + f->offset;
        if (f->type == STRING) {
            pc_str_fini(value);
        }
        if (f->type == STROKE) {
            pc_shape_stroke_fini(value);
        }
    }
    *o = (pc_ui_tool_options){0};
}
pc_status pc_ui_tool_options_clone(const pc_ui_tool_options *src, pc_ui_tool_options *out, pc_error *err)
{
    if (!src || !out) {
        return pc_fail(err, PC_ERR_INVALID, "missing tool options");
    }
    pc_ui_tool_options next = {0};
    pc_status rc = PC_OK;
    for (size_t i = 0; i < sizeof fields / sizeof *fields && rc == PC_OK; i++) {
        const field *f = &fields[i];
        const void *from = (const char *)src + f->offset;
        void *to = (char *)&next + f->offset;
        if (f->type == STRING) {
            const pc_str *s = from;
            rc = pc_str_set(to, pc_str_c(s), s->len, err);
        } else if (f->type == STROKE) {
            rc = pc_shape_stroke_clone(from, to, err);
        } else {
            memcpy(to, from, f->size);
        }
    }
    if (rc != PC_OK) {
        pc_ui_tool_options_fini(&next);
        return rc;
    }
    pc_ui_tool_options_fini(out);
    *out = next;
    return PC_OK;
}
static yyjson_mut_val *shield_json(yyjson_mut_doc *d, const pc_ui_crop_shield *s)
{
    yyjson_mut_val *o = yyjson_mut_obj(d), *rgb = yyjson_mut_arr(d);
    yyjson_mut_obj_add_bool(d, o, "classic_mode", s->classic_mode);
    yyjson_mut_obj_add_bool(d, o, "auto_center_preview", s->auto_center_preview);
    yyjson_mut_obj_add_bool(d, o, "show_cropped_area", s->show_cropped_area);
    yyjson_mut_obj_add_bool(d, o, "enabled", s->enabled);
    yyjson_mut_obj_add_str(d, o, "color", s->custom ? "custom" : "matchCanvas");
    for (size_t i = 0; i < 3; i++) {
        yyjson_mut_arr_add_uint(d, rgb, s->custom_color[i]);
    }
    yyjson_mut_obj_add_val(d, o, "custom_color", rgb);
    yyjson_mut_obj_add_float(d, o, "opacity", s->opacity);
    yyjson_mut_obj_add_bool(d, o, "auto_adjust", s->auto_adjust);
    return o && yyjson_mut_obj_size(o) == 8 && yyjson_mut_arr_size(rgb) == 3 ? o : NULL;
}
static yyjson_mut_val *stroke_json(yyjson_mut_doc *d, const pc_shape_stroke *stroke)
{
    yyjson_mut_val *full = pc_shape_stroke_to_json(d, stroke, 0), *o = yyjson_mut_obj(d);
    const char *const names[] = {"align", "cap", "join", "miter_limit", "dashes", "dash_offset", "opacity"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        yyjson_mut_val *v = yyjson_mut_obj_get(full, names[i]);
        if (!v || !yyjson_mut_obj_add_val(d, o, names[i], pc_json_normalized_copy(d, v))) {
            return NULL;
        }
    }
    return o;
}
pc_status pc_ui_tool_options_to_json(const pc_ui_tool_options *o, yyjson_mut_doc *d, yyjson_mut_val **out,
                                     pc_error *err)
{
    if (!o || !d || !out) {
        return pc_fail(err, PC_ERR_INVALID, "missing tool options");
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
        const field *f = &fields[i];
        const void *value = (const char *)o + f->offset;
        yyjson_mut_val *v = NULL;
        switch (f->type) {
        case BOOL:
            v = yyjson_mut_bool(d, *(const bool *)value);
            break;
        case FLOAT:
            v = yyjson_mut_float(d, *(const float *)value);
            break;
        case U8:
            v = yyjson_mut_uint(d, *(const uint8_t *)value);
            break;
        case U32:
            v = yyjson_mut_uint(d, *(const uint32_t *)value);
            break;
        case STRING:
            v = yyjson_mut_strcpy(d, pc_str_c(value));
            break;
        case BLEND:
            v = yyjson_mut_str(d, pc_blend_name(*(const pc_blend_mode *)value));
            break;
        case OVERLAY:
            v = yyjson_mut_str(d, pc_ui_crop_overlay_id(*(const pc_ui_crop_overlay *)value));
            break;
        case SHOW:
            v = yyjson_mut_str(d, (unsigned)*(const pc_ui_overlay_show *)value < 3
                                      ? shows[*(const pc_ui_overlay_show *)value]
                                      : shows[0]);
            break;
        case STROKE:
            v = stroke_json(d, value);
            break;
        case SHIELD:
            v = shield_json(d, value);
            break;
        }
        if (!v || !yyjson_mut_obj_add_val(d, root, f->name, v)) {
            return pc_fail(err, PC_ERR_NOMEM, "tool options JSON");
        }
    }
    if (!root) {
        return pc_fail(err, PC_ERR_NOMEM, "tool options JSON");
    }
    *out = root;
    return PC_OK;
}
static pc_status invalid(pc_error *err, const char *field_name)
{
    return pc_fail(err, PC_ERR_INVALID, "invalid tool option `%s`", field_name);
}
static pc_status shield_patch(pc_ui_crop_shield *s, yyjson_val *o, pc_error *err)
{
    if (!yyjson_is_obj(o)) {
        return invalid(err, "crop_shield");
    }
    const char *names[] = {"classic_mode", "auto_center_preview", "show_cropped_area", "enabled",
                           "auto_adjust"};
    bool *values[] = {&s->classic_mode, &s->auto_center_preview, &s->show_cropped_area, &s->enabled,
                      &s->auto_adjust};
    for (size_t i = 0; i < 5; i++) {
        yyjson_val *v = pc_json_get(o, names[i]);
        if (v && !pc_json_as_bool(v, values[i])) {
            return invalid(err, names[i]);
        }
    }
    yyjson_val *v = pc_json_get(o, "color");
    if (v) {
        const char *name = pc_json_as_str(v, NULL);
        if (!name || (strcmp(name, "custom") && strcmp(name, "matchCanvas"))) {
            return invalid(err, "crop_shield.color");
        }
        s->custom = strcmp(name, "custom") == 0;
    }
    if ((v = pc_json_get(o, "opacity"))) {
        double value;
        if (!pc_json_as_f64(v, &value) || !isfinite(value) || fabs(value) > FLT_MAX) {
            return invalid(err, "crop_shield.opacity");
        }
        s->opacity = (float)value;
    }
    if ((v = pc_json_get(o, "custom_color"))) {
        if (!yyjson_is_arr(v) || yyjson_arr_size(v) != 3) {
            return invalid(err, "crop_shield.custom_color");
        }
        for (size_t i = 0; i < 3; i++) {
            uint64_t c;
            if (!pc_json_as_u64(yyjson_arr_get(v, i), &c) || c > 255) {
                return invalid(err, "crop_shield.custom_color");
            }
            s->custom_color[i] = (uint8_t)c;
        }
    }
    return PC_OK;
}
static pc_status stroke_patch(pc_shape_stroke *stroke, yyjson_val *o, pc_error *err)
{
    if (!yyjson_is_obj(o)) {
        return invalid(err, "shape_stroke");
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d) {
        return pc_fail(err, PC_ERR_NOMEM, "shape stroke options");
    }
    yyjson_mut_val *full = pc_shape_stroke_to_json(d, stroke, 0);
    const char *const names[] = {"align", "cap", "join", "miter_limit", "dashes", "dash_offset", "opacity"};
    pc_status rc = PC_OK;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        yyjson_val *v = pc_json_get(o, names[i]);
        if (!v) {
            continue;
        }
        yyjson_mut_val *copy = yyjson_val_mut_copy(d, v);
        yyjson_mut_val *key = yyjson_mut_str(d, names[i]);
        if (!copy || !key || !yyjson_mut_obj_put(full, key, copy)) {
            rc = pc_fail(err, PC_ERR_NOMEM, "shape stroke JSON");
            break;
        }
    }
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *read = NULL;
    pc_shape_stroke next = {0};
    if (rc == PC_OK) {
        rc = pc_json_write(full, 0, &text, err);
    }
    if (rc == PC_OK) {
        rc = pc_json_parse(pc_buf_cstr(&text), text.len, &read, err);
    }
    if (rc == PC_OK) {
        rc = pc_shape_stroke_from_json(yyjson_doc_get_root(read), &next, err);
    }
    if (rc == PC_OK) {
        rc = pc_shape_stroke_validate(&next, err);
    }
    if (rc == PC_OK) {
        pc_shape_stroke_fini(stroke);
        *stroke = next;
        next = (pc_shape_stroke){0};
    }
    pc_shape_stroke_fini(&next);
    yyjson_doc_free(read);
    pc_buf_fini(&text);
    yyjson_mut_doc_free(d);
    return rc;
}
static pc_status apply(pc_ui_tool_options *o, yyjson_val *object, pc_error *err)
{
    if (!yyjson_is_obj(object)) {
        return invalid(err, "options");
    }
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
        const field *f = &fields[i];
        yyjson_val *v = pc_json_get(object, f->name);
        if (!v) {
            continue;
        }
        void *value = (char *)o + f->offset;
        double number = 0;
        uint64_t integer = 0;
        const char *text;
        switch (f->type) {
        case BOOL:
            if (!pc_json_as_bool(v, value)) {
                return invalid(err, f->name);
            }
            break;
        case FLOAT:
            if (!pc_json_as_f64(v, &number) || !isfinite(number) || fabs(number) > FLT_MAX) {
                return invalid(err, f->name);
            }
            *(float *)value = (float)number;
            break;
        case U32:
        case U8:
            if (!pc_json_as_u64(v, &integer) || integer > (f->type == U8 ? UINT8_MAX : UINT32_MAX)) {
                return invalid(err, f->name);
            }
            if (f->type == U8) {
                *(uint8_t *)value = (uint8_t)integer;
            } else {
                *(uint32_t *)value = (uint32_t)integer;
            }
            break;
        case STRING: {
            size_t len;
            text = pc_json_as_str(v, &len);
            if (!text || len > 65536) {
                return invalid(err, f->name);
            }
            pc_status rc = pc_str_set(value, text, len, err);
            if (rc != PC_OK) {
                return rc;
            }
            break;
        }
        case BLEND:
            text = pc_json_as_str(v, NULL);
            if (!text || !pc_blend_from_name(text, strlen(text), value)) {
                return invalid(err, f->name);
            }
            break;
        case OVERLAY:
        case SHOW: {
            text = pc_json_as_str(v, NULL);
            unsigned found = 0;
            const char *const *names = f->type == OVERLAY ? overlays : shows;
            unsigned n = f->type == OVERLAY ? 6 : 3;
            while (found < n && (!text || strcmp(text, names[found]))) {
                found++;
            }
            if (found == n) {
                return invalid(err, f->name);
            }
            if (f->type == OVERLAY) {
                *(pc_ui_crop_overlay *)value = (pc_ui_crop_overlay)found;
            } else {
                *(pc_ui_overlay_show *)value = (pc_ui_overlay_show)found;
            }
            break;
        }
        case STROKE: {
            pc_status rc = stroke_patch(value, v, err);
            if (rc != PC_OK) {
                return rc;
            }
            break;
        }
        case SHIELD: {
            pc_status rc = shield_patch(value, v, err);
            if (rc != PC_OK) {
                return rc;
            }
            break;
        }
        }
    }
    return PC_OK;
}
pc_status pc_ui_tool_options_from_json(pc_ui_tool_options *o, yyjson_val *object, pc_error *err)
{
    if (!o) {
        return invalid(err, "options");
    }
    pc_ui_tool_options next;
    pc_status rc = init_options(&next, err);
    if (rc == PC_OK) {
        rc = apply(&next, object, err);
    }
    if (rc != PC_OK) {
        pc_ui_tool_options_fini(&next);
        return rc;
    }
    pc_ui_tool_options_fini(o);
    *o = next;
    return PC_OK;
}
pc_status pc_ui_tool_options_patch(pc_ui_tool_options *o, yyjson_val *object, pc_error *err)
{
    if (!o) {
        return invalid(err, "options");
    }
    pc_ui_tool_options next = {0};
    pc_status rc = pc_ui_tool_options_clone(o, &next, err);
    if (rc == PC_OK) {
        rc = apply(&next, object, err);
    }
    if (rc != PC_OK) {
        pc_ui_tool_options_fini(&next);
        return rc;
    }
    pc_ui_tool_options_fini(o);
    *o = next;
    return PC_OK;
}
