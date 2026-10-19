#include "view_options.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct string_field {
    const char *key, *def;
    size_t offset;
} string_field;
typedef struct bool_field {
    const char *key;
    size_t offset;
} bool_field;
#define STRING_FIELD(name, value) {#name, value, offsetof(pc_ui_view_options, name)}
static const string_field strings[] = {STRING_FIELD(screen_mode, "standard"),
                                       STRING_FIELD(pixel_aspect, "square"), STRING_FIELD(arrange, "tabs"),
                                       STRING_FIELD(font_preview_size, "medium"),
                                       STRING_FIELD(language_features, "defaultFeatures")};
#define BOOL_FIELD(name) {#name, offsetof(pc_ui_view_options, name)}
static const bool_field flags[] = {
    BOOL_FIELD(extras),          BOOL_FIELD(flip_horizontal),   BOOL_FIELD(pixel_aspect_correction),
    BOOL_FIELD(pattern_preview), BOOL_FIELD(pixel_art_preview), BOOL_FIELD(middle_eastern_composer),
    BOOL_FIELD(tile_grid)};
static const char *const show_keys[PC_UI_SHOW_COUNT] = {
    "layer_edges",   "selection_edges", "target_path",    "pixel_grid", "smart_guides",
    "brush_preview", "slices",          "notes",          "mesh",       "edit_pins",
    "count",         "canvas_guides",   "artboard_guides"};
static const char *const show_commands[PC_UI_SHOW_COUNT] = {
    "layerEdges", "selectionEdges", "targetPath", "pixelGrid", "smartGuides",  "brushPreview",  "slices",
    "notes",      "mesh",           "editPins",   "count",     "canvasGuides", "artboardGuides"};
static const char *const snap_keys[PC_UI_SNAP_COUNT] = {"guides", "grid", "layers", "slices",
                                                        "document_bounds"};
static const char *const snap_commands[PC_UI_SNAP_COUNT] = {"guides", "grid", "layers", "slices",
                                                            "documentBounds"};

void pc_ui_view_options_init(pc_ui_view_options *o)
{
    *o = (pc_ui_view_options){0};
    o->extras = true;
    o->pixel_aspect_correction = true;
    for (size_t i = 0; i < PC_UI_SHOW_COUNT; i++) {
        o->show[i] = i != 0 && i != 8;
    }
    for (size_t i = 0; i < PC_UI_SNAP_COUNT; i++) {
        o->snap_to[i] = true;
    }
}

void pc_ui_view_options_fini(pc_ui_view_options *o)
{
    if (!o) {
        return;
    }
    pc_str_fini(&o->screen_mode);
    pc_str_fini(&o->pixel_aspect);
    pc_str_fini(&o->arrange);
    pc_str_fini(&o->font_preview_size);
    pc_str_fini(&o->language_features);
    pc_json_value_fini(&o->guide_layout);
    *o = (pc_ui_view_options){0};
}

static yyjson_mut_val *default_layout(yyjson_mut_doc *doc)
{
    yyjson_mut_val *v = yyjson_mut_obj(doc);
    if (!v || !yyjson_mut_obj_add_uint(doc, v, "columns", 8) ||
        !yyjson_mut_obj_add_uint(doc, v, "gutter", 20) || !yyjson_mut_obj_add_uint(doc, v, "rows", 0) ||
        !yyjson_mut_obj_add_uint(doc, v, "rowGutter", 0) || !yyjson_mut_obj_add_uint(doc, v, "margin", 0) ||
        !yyjson_mut_obj_add_bool(doc, v, "centerColumns", false) ||
        !yyjson_mut_obj_add_bool(doc, v, "clearExisting", false)) {
        return NULL;
    }
    return v;
}

pc_status pc_ui_view_options_to_json(const pc_ui_view_options *o, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                     pc_error *err)
{
    if (!o || !doc || !out) {
        return pc_fail(err, PC_ERR_INVALID, "missing view options");
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc), *show = yyjson_mut_obj(doc), *snap = yyjson_mut_obj(doc);
    bool ok = root && show && snap;
    for (size_t i = 0; ok && i < sizeof strings / sizeof strings[0]; i++) {
        const pc_str *s = (const pc_str *)((const unsigned char *)o + strings[i].offset);
        yyjson_mut_val *v =
            s->ptr ? yyjson_mut_strncpy(doc, s->ptr, s->len) : yyjson_mut_str(doc, strings[i].def);
        ok = v && yyjson_mut_obj_add_val(doc, root, strings[i].key, v);
    }
    for (size_t i = 0; ok && i < sizeof flags / sizeof flags[0]; i++) {
        const bool *b = (const bool *)((const unsigned char *)o + flags[i].offset);
        ok = yyjson_mut_obj_add_bool(doc, root, flags[i].key, *b);
    }
    for (size_t i = 0; ok && i < PC_UI_SHOW_COUNT; i++) {
        ok = yyjson_mut_obj_add_bool(doc, show, show_keys[i], o->show[i]);
    }
    for (size_t i = 0; ok && i < PC_UI_SNAP_COUNT; i++) {
        ok = yyjson_mut_obj_add_bool(doc, snap, snap_keys[i], o->snap_to[i]);
    }
    yyjson_mut_val *layout = o->guide_layout.doc
                                 ? pc_json_normalized_copy(doc, pc_json_value_root(&o->guide_layout))
                                 : default_layout(doc);
    if (!o->guide_layout.doc && layout) {
        layout = pc_json_normalized_copy(doc, layout);
    }
    ok = ok && layout && yyjson_mut_obj_add_val(doc, root, "show", show) &&
         yyjson_mut_obj_add_val(doc, root, "snap_to", snap) &&
         yyjson_mut_obj_add_val(doc, root, "guide_layout", layout);
    if (!ok) {
        return pc_fail(err, PC_ERR_NOMEM, "view options JSON");
    }
    *out = root;
    return PC_OK;
}

static pc_status read_flags(yyjson_val *obj, const char *const *keys, size_t n, bool *out, pc_error *err)
{
    if (!obj) {
        return PC_OK;
    }
    if (!yyjson_is_obj(obj)) {
        return pc_fail(err, PC_ERR_INVALID, "view flags must be an object");
    }
    for (size_t i = 0; i < n; i++) {
        yyjson_val *v = pc_json_get(obj, keys[i]);
        if (v && !pc_json_as_bool(v, &out[i])) {
            return pc_fail(err, PC_ERR_INVALID, "%s must be a boolean", keys[i]);
        }
    }
    return PC_OK;
}

pc_status pc_ui_view_options_from_json(yyjson_val *obj, pc_ui_view_options *out, pc_error *err)
{
    if (!out || (obj && !yyjson_is_obj(obj))) {
        return pc_fail(err, PC_ERR_INVALID, "view must be an object");
    }
    pc_ui_view_options next;
    pc_ui_view_options_init(&next);
    pc_status rc = PC_OK;
    for (size_t i = 0; i < sizeof strings / sizeof strings[0]; i++) {
        yyjson_val *v = pc_json_get(obj, strings[i].key);
        if (!v) {
            continue;
        }
        size_t len = 0;
        const char *text = pc_json_as_str(v, &len);
        if (!text) {
            rc = pc_fail(err, PC_ERR_INVALID, "%s must be a string", strings[i].key);
            goto cleanup;
        }
        pc_str *s = (pc_str *)((unsigned char *)&next + strings[i].offset);
        rc = pc_str_set(s, text, len, err);
        if (rc != PC_OK) {
            goto cleanup;
        }
    }
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
        yyjson_val *v = pc_json_get(obj, flags[i].key);
        bool *b = (bool *)((unsigned char *)&next + flags[i].offset);
        if (v && !pc_json_as_bool(v, b)) {
            rc = pc_fail(err, PC_ERR_INVALID, "%s must be a boolean", flags[i].key);
            goto cleanup;
        }
    }
    rc = read_flags(pc_json_get(obj, "show"), show_keys, PC_UI_SHOW_COUNT, next.show, err);
    if (rc != PC_OK) {
        goto cleanup;
    }
    rc = read_flags(pc_json_get(obj, "snap_to"), snap_keys, PC_UI_SNAP_COUNT, next.snap_to, err);
    if (rc != PC_OK) {
        goto cleanup;
    }
    yyjson_val *layout = pc_json_get(obj, "guide_layout");
    if (layout) {
        rc = pc_json_value_from_val(layout, &next.guide_layout, err);
        if (rc != PC_OK) {
            goto cleanup;
        }
    }
    pc_ui_view_options_fini(out);
    *out = next;
    return PC_OK;
cleanup:
    pc_ui_view_options_fini(&next);
    return rc;
}

bool *pc_ui_view_show_flag(pc_ui_view_options *o, const char *key)
{
    for (size_t i = 0; key && i < PC_UI_SHOW_COUNT; i++) {
        if (strcmp(key, show_commands[i]) == 0) {
            return &o->show[i];
        }
    }
    return NULL;
}
bool *pc_ui_view_snap_flag(pc_ui_view_options *o, const char *key)
{
    for (size_t i = 0; key && i < PC_UI_SNAP_COUNT; i++) {
        if (strcmp(key, snap_commands[i]) == 0) {
            return &o->snap_to[i];
        }
    }
    return NULL;
}

static bool string_is(const pc_str *s, const char *value, const char *def)
{
    return s->ptr ? pc_str_eq_c(s, value) : strcmp(value, def) == 0;
}
bool pc_ui_view_standard(const pc_ui_view_options *o)
{
    return string_is(&o->screen_mode, "standard", "standard");
}
bool pc_ui_view_hides_chrome(const pc_ui_view_options *o)
{
    return string_is(&o->screen_mode, "fullScreen", "standard");
}
const char *pc_ui_view_screen_mode(const pc_ui_view_options *o)
{
    return o->screen_mode.ptr ? pc_str_c(&o->screen_mode) : "standard";
}

float pc_ui_pixel_aspect_ratio(const char *id)
{
    if (!id) {
        return 1.f;
    }
    if (strncmp(id, "custom:", 7) == 0) {
        const char *start = id + 7, *stop = strchr(start, ':');
        size_t n = stop ? (size_t)(stop - start) : strlen(start);
        if (n && n < 128) {
            char buf[128];
            memcpy(buf, start, n);
            buf[n] = '\0';
            char *end = NULL;
            float ratio = strtof(buf, &end);
            if (end == buf + n && end != buf && strspn(buf, "+-0123456789.eE") == n) {
                return ratio;
            }
        }
        return 1.f;
    }
    static const struct {
        const char *id;
        float ratio;
    } table[] = {{"d1DvNtsc", .91f},     {"d1DvPal", 1.09f},           {"d1DvNtscWidescreen", 1.21f},
                 {"hdv1080", 1.33f},     {"d1DvPalWidescreen", 1.46f}, {"dvcproHd1080", 1.5f},
                 {"anamorphic2To1", 2.f}};
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strcmp(id, table[i].id) == 0) {
            return table[i].ratio;
        }
    }
    return 1.f;
}

float pc_ui_view_display_aspect(const pc_ui_view_options *o)
{
    if (!o->pixel_aspect_correction) {
        return 1.f;
    }
    const pc_str *id = &o->pixel_aspect;
    if (id->ptr && memchr(id->ptr, '\0', id->len)) {
        return 1.f;
    }
    float ratio = pc_ui_pixel_aspect_ratio(id->ptr ? pc_str_c(id) : "square");
    return isfinite(ratio) ? fminf(10.f, fmaxf(.1f, ratio)) : 1.f;
}

pc_status pc_ui_view_options_command(pc_ui_view_options *o, const char *id, yyjson_val *params,
                                     pc_json_value *out, pc_error *err)
{
    if (!o || !id || !out) {
        return pc_fail(err, PC_ERR_INVALID, "missing view command");
    }
    bool *flag = strcmp(id, "view.flipHorizontal") == 0               ? &o->flip_horizontal
                 : strcmp(id, "view.pixelAspectRatioCorrection") == 0 ? &o->pixel_aspect_correction
                                                                      : NULL;
    bool group = false, group_value = false, show = false;
    if (strcmp(id, "view.extras") == 0) {
        flag = &o->extras;
    } else if (strncmp(id, "view.show.", 10) == 0) {
        show = true;
        if (strcmp(id + 10, "all") == 0) {
            group = true;
            group_value = true;
            flag = &group_value;
        } else {
            flag = pc_ui_view_show_flag(o, id + 10);
        }
    } else if (strncmp(id, "view.snapTo.", 12) == 0) {
        if (strcmp(id + 12, "all") == 0 || strcmp(id + 12, "none") == 0) {
            group = true;
            group_value = strcmp(id + 12, "all") == 0;
            flag = &group_value;
        } else {
            flag = pc_ui_view_snap_flag(o, id + 12);
        }
    }
    const char *text = NULL;
    pc_str *destination = NULL;
    const char *result_key = NULL;
    static const char *const modes[] = {"standard", "fullScreenWithMenuBar", "fullScreen"};
    static const char *const aspects[] = {
        "square",  "d1DvNtsc",          "d1DvPal",      "d1DvNtscWidescreen",
        "hdv1080", "d1DvPalWidescreen", "dvcproHd1080", "anamorphic2To1"};
    if (strncmp(id, "view.screenMode.", 16) == 0) {
        const char *key = id + 16;
        if (strcmp(key, "cycle") == 0) {
            size_t index = 0;
            for (size_t i = 0; i < 3; i++) {
                if (string_is(&o->screen_mode, modes[i], "standard")) {
                    index = i;
                    break;
                }
            }
            text = modes[(index + 1) % 3];
        } else {
            for (size_t i = 0; i < 3; i++) {
                if (strcmp(key, modes[i]) == 0) {
                    text = modes[i];
                    break;
                }
            }
        }
        destination = &o->screen_mode;
        result_key = "screenMode";
    } else if (strncmp(id, "view.pixelAspectRatio.", 22) == 0) {
        const char *key = id + 22;
        for (size_t i = 0; i < sizeof aspects / sizeof aspects[0]; i++) {
            if (strcmp(key, aspects[i]) == 0) {
                text = aspects[i];
                break;
            }
        }
        destination = &o->pixel_aspect;
        result_key = "pixelAspectRatio";
    }
    if (!flag && !text) {
        return PC_ERR_NOT_FOUND;
    }
    pc_str next = PC_STR_INIT;
    if (text) {
        pc_status rc = pc_str_set_c(&next, text, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    bool on = false;
    if (group) {
        on = group_value;
    } else if (flag && !pc_json_as_bool(pc_json_get(params, "on"), &on)) {
        on = !*flag;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        pc_str_fini(&next);
        return pc_fail(err, PC_ERR_NOMEM, "view result");
    }
    yyjson_mut_val *value = flag ? yyjson_mut_bool(doc, on) : yyjson_mut_obj(doc);
    bool ok = value != NULL;
    if (ok && text) {
        ok = destination == &o->pixel_aspect
                 ? yyjson_mut_obj_add_val(doc, value, result_key,
                                          yyjson_mut_float(doc, pc_ui_pixel_aspect_ratio(text)))
                 : yyjson_mut_obj_add_str(doc, value, result_key, text);
    }
    pc_status rc = ok ? pc_json_value_from_mut(value, out, err) : pc_fail(err, PC_ERR_NOMEM, "view result");
    yyjson_mut_doc_free(doc);
    if (rc == PC_OK) {
        if (flag) {
            *flag = on;
            if (group) {
                bool *values = show ? o->show : o->snap_to;
                size_t count = show ? PC_UI_SHOW_COUNT : PC_UI_SNAP_COUNT;
                for (size_t i = 0; i < count; i++) {
                    values[i] = on;
                }
            }
            if (show && on) {
                o->extras = true;
            }
        } else {
            pc_str_fini(destination);
            *destination = next;
            next = (pc_str)PC_STR_INIT;
        }
    }
    pc_str_fini(&next);
    return rc;
}
