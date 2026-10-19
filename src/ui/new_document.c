#include "new_document.h"
#include <math.h>
#include <string.h>
static const pc_ui_new_unit units[] = {{"px", "Pixels", 0},          {"in", "Inches", 1},
                                       {"cm", "Centimeters", 2.54f}, {"mm", "Millimeters", 25.4f},
                                       {"pt", "Points", 72},         {"pica", "Picas", 6}};
static const pc_ui_new_preset presets[] = {{"Recent", "Default PhotoCraft Size", 2100, 1500, 300},
                                           {"Recent", "HDTV 1080p", 1920, 1080, 72},
                                           {"Photo", "Landscape, 6 x 4", 1800, 1200, 300},
                                           {"Photo", "Landscape, 7 x 5", 2100, 1500, 300},
                                           {"Photo", "Landscape, 10 x 8", 3000, 2400, 300},
                                           {"Photo", "Portrait, 4 x 6", 1200, 1800, 300},
                                           {"Photo", "Portrait, 5 x 7", 1500, 2100, 300},
                                           {"Photo", "Square, 5 x 5", 1500, 1500, 300},
                                           {"Print", "Letter", 2550, 3300, 300},
                                           {"Print", "Legal", 2550, 4200, 300},
                                           {"Print", "Tabloid", 3300, 5100, 300},
                                           {"Print", "A4", 2480, 3508, 300},
                                           {"Print", "A3", 3508, 4961, 300},
                                           {"Print", "A5", 1748, 2480, 300},
                                           {"Print", "B3", 4169, 5906, 300},
                                           {"Print", "B4", 2953, 4169, 300},
                                           {"Art & Illustration", "Poster", 5400, 7200, 300},
                                           {"Art & Illustration", "Postcard", 1800, 1200, 300},
                                           {"Art & Illustration", "Comic Book", 1988, 3075, 300},
                                           {"Art & Illustration", "Square, 12 x 12", 3600, 3600, 300},
                                           {"Web", "Web Most Common", 1366, 768, 72},
                                           {"Web", "Web Minimum", 1024, 768, 72},
                                           {"Web", "Web Large", 1920, 1080, 72},
                                           {"Web", "MacBook Pro 16\"", 3456, 2234, 72},
                                           {"Web", "iMac 24\"", 4480, 2520, 72},
                                           {"Mobile", "iPhone 16", 1179, 2556, 72},
                                           {"Mobile", "iPhone 16 Pro Max", 1320, 2868, 72},
                                           {"Mobile", "iPad Pro 13\"", 2064, 2752, 72},
                                           {"Mobile", "Android 1080p", 1080, 1920, 72},
                                           {"Mobile", "Apple Watch 45mm", 396, 484, 72},
                                           {"Film & Video", "HDTV 1080p", 1920, 1080, 72},
                                           {"Film & Video", "HDTV 720p", 1280, 720, 72},
                                           {"Film & Video", "UHD 4K", 3840, 2160, 72},
                                           {"Film & Video", "DCI 4K", 4096, 2160, 72},
                                           {"Film & Video", "UHD 8K", 7680, 4320, 72}};
const pc_ui_new_unit *pc_ui_new_units(size_t *count)
{
    if (count) {
        *count = sizeof units / sizeof *units;
    }
    return units;
}
const pc_ui_new_preset *pc_ui_new_presets(size_t *count)
{
    if (count) {
        *count = sizeof presets / sizeof *presets;
    }
    return presets;
}
static const pc_ui_new_unit *find_unit(const char *name)
{
    for (size_t i = 0; name && i < sizeof units / sizeof *units; i++) {
        if (!strcmp(name, units[i].id)) {
            return &units[i];
        }
    }
    return &units[0];
}
float pc_ui_new_to_unit(float px, const char *unit, float ppi)
{
    float per = find_unit(unit)->per_inch;
    return per > 0 ? px / fmaxf(1, ppi) * per : px;
}
float pc_ui_new_from_unit(float value, const char *unit, float ppi)
{
    float per = find_unit(unit)->per_inch;
    return roundf(per > 0 ? value / per * fmaxf(1, ppi) : value);
}
uint32_t pc_ui_new_pixel_value(float value)
{
    return isfinite(value) ? (uint32_t)fminf(300000, fmaxf(1, roundf(value))) : 1;
}
static float number(yyjson_mut_val *fields, const char *key, float fallback)
{
    yyjson_mut_val *value = yyjson_mut_obj_get(fields, key);
    float n = (float)yyjson_mut_get_num(value);
    return yyjson_mut_is_num(value) && isfinite(n) ? n : fallback;
}
static const char *string(yyjson_mut_val *fields, const char *key, const char *fallback)
{
    const char *s = yyjson_mut_get_str(yyjson_mut_obj_get(fields, key));
    return s ? s : fallback;
}
static const char *typed_key(const char *key)
{
    return !strcmp(key, "width") ? "__widthTyped" : "__heightTyped";
}
float pc_ui_new_shown_size(yyjson_mut_val *fields, const char *key, float fallback, const char *unit,
                           float ppi)
{
    float px = number(fields, key, fallback);
    yyjson_mut_val *typed = yyjson_mut_obj_get(fields, typed_key(key));
    if (unit && strcmp(unit, "px") && !strcmp(string(typed, "unit", ""), unit)) {
        float value = number(typed, "value", NAN);
        if (isfinite(value) && fmaxf(1, pc_ui_new_from_unit(value, unit, ppi)) == px) {
            return value;
        }
    }
    return pc_ui_new_to_unit(px, unit, ppi);
}
typedef struct patch {
    const char *name;
    yyjson_mut_val *key, *value;
} patch;
static pc_status apply(yyjson_mut_doc *doc, yyjson_mut_val *fields, patch *items, size_t count, pc_error *err)
{
    if (!doc || !yyjson_mut_is_obj(fields)) {
        return pc_fail(err, PC_ERR_INVALID, "missing New Document fields");
    }
    for (size_t i = 0; i < count; i++) {
        items[i].key = yyjson_mut_strcpy(doc, items[i].name);
        if (!items[i].key || !items[i].value) {
            return pc_fail(err, PC_ERR_NOMEM, "New Document fields");
        }
    }
    for (size_t i = 0; i < count; i++) {
        (void)yyjson_mut_obj_put(fields, items[i].key, items[i].value);
    }
    return PC_OK;
}
pc_status pc_ui_new_set_size(yyjson_mut_doc *doc, yyjson_mut_val *fields, const char *key, float value,
                             const char *unit, float ppi, pc_error *err)
{
    if (!doc || !key || (strcmp(key, "width") && strcmp(key, "height")) || !isfinite(value) ||
        !isfinite(ppi) || ppi <= 0) {
        return pc_fail(err, PC_ERR_INVALID, "invalid document size");
    }
    yyjson_mut_val *typed = yyjson_mut_obj(doc);
    if (!typed || !yyjson_mut_obj_add_float(doc, typed, "value", value) ||
        !yyjson_mut_obj_add_strcpy(doc, typed, "unit", unit ? unit : "px")) {
        return pc_fail(err, PC_ERR_NOMEM, "typed document size");
    }
    patch items[] = {
        {key, NULL, yyjson_mut_uint(doc, pc_ui_new_pixel_value(pc_ui_new_from_unit(value, unit, ppi)))},
        {typed_key(key), NULL, typed}};
    pc_status status = apply(doc, fields, items, 2, err);
    if (status == PC_OK) {
        (void)yyjson_mut_obj_remove_key(fields, "__preset");
        (void)yyjson_mut_obj_remove_key(fields, "__savedPreset");
    }
    return status;
}
pc_status pc_ui_new_swap_size(yyjson_mut_doc *doc, yyjson_mut_val *fields, pc_error *err)
{
    if (!doc || !fields) {
        return pc_fail(err, PC_ERR_INVALID, "missing document size");
    }
    yyjson_mut_val *w = yyjson_mut_obj_get(fields, "__widthTyped"),
                   *h = yyjson_mut_obj_get(fields, "__heightTyped");
    /* Values belong to an object's linked list; copy them before inserting elsewhere. */
    yyjson_mut_val *tw = w ? yyjson_mut_val_mut_copy(doc, w) : yyjson_mut_null(doc),
                   *th = h ? yyjson_mut_val_mut_copy(doc, h) : yyjson_mut_null(doc);
    patch items[] = {
        {"width", NULL, yyjson_mut_uint(doc, pc_ui_new_pixel_value(number(fields, "height", 1080)))},
        {"height", NULL, yyjson_mut_uint(doc, pc_ui_new_pixel_value(number(fields, "width", 1920)))},
        {"__widthTyped", NULL, th},
        {"__heightTyped", NULL, tw}};
    pc_status status = apply(doc, fields, items, 4, err);
    if (status == PC_OK) {
        if (!h) {
            (void)yyjson_mut_obj_remove_key(fields, "__widthTyped");
        }
        if (!w) {
            (void)yyjson_mut_obj_remove_key(fields, "__heightTyped");
        }
        (void)yyjson_mut_obj_remove_key(fields, "__savedPreset");
    }
    return status;
}
pc_status pc_ui_new_set_resolution(yyjson_mut_doc *doc, yyjson_mut_val *fields, float ppi, pc_error *err)
{
    if (!doc || !isfinite(ppi) || ppi <= 0 || ppi > 30000) {
        return pc_fail(err, PC_ERR_INVALID, "resolution must be 1..30000 pixels/inch");
    }
    float old = number(fields, "resolution", 72);
    bool physical = strcmp(string(fields, "__unit", "px"), "px") && old > 0 && old != ppi;
    patch items[] = {{"resolution", NULL, yyjson_mut_float(doc, ppi)},
                     {"width", NULL,
                      yyjson_mut_uint(doc, pc_ui_new_pixel_value(number(fields, "width", 1920) *
                                                                 (old > 0 ? ppi / old : 1)))},
                     {"height", NULL,
                      yyjson_mut_uint(doc, pc_ui_new_pixel_value(number(fields, "height", 1080) *
                                                                 (old > 0 ? ppi / old : 1)))}};
    pc_status status = apply(doc, fields, items, physical ? 3 : 1, err);
    if (status == PC_OK && old != ppi) {
        (void)yyjson_mut_obj_remove_key(fields, "__savedPreset");
        if (physical) {
            (void)yyjson_mut_obj_remove_key(fields, "__preset");
        }
    }
    return status;
}
pc_status pc_ui_new_apply_preset(yyjson_mut_doc *doc, yyjson_mut_val *fields, const pc_ui_new_preset *preset,
                                 float print_ppi, float screen_ppi, pc_error *err)
{
    if (!doc || !preset || !preset->name || !preset->width || !preset->height) {
        return pc_fail(err, PC_ERR_INVALID, "invalid blank document preset");
    }
    bool clipboard = !strcmp(preset->name, "Clipboard");
    bool print = preset->ppi >= 300;
    float ppi = clipboard ? 72 : print ? print_ppi : screen_ppi;
    if (!isfinite(ppi) || ppi <= 0) {
        ppi = preset->ppi;
    }
    float factor = print ? ppi / preset->ppi : 1;
    uint32_t w = pc_ui_new_pixel_value((float)preset->width * factor),
             h = pc_ui_new_pixel_value((float)preset->height * factor);
    const char *orientation = string(fields, "__orientation", "");
    if (!clipboard && w != h &&
        ((!strcmp(orientation, "portrait") && w > h) || (!strcmp(orientation, "landscape") && h > w))) {
        uint32_t v = w;
        w = h;
        h = v;
    }
    const char *unit = print ? string(fields, "__unit", "in") : "px";
    if (print && (!strcmp(unit, "px") || find_unit(unit) == &units[0])) {
        unit = "in";
    }
    patch items[] = {{"width", NULL, yyjson_mut_uint(doc, w)},
                     {"height", NULL, yyjson_mut_uint(doc, h)},
                     {"resolution", NULL, yyjson_mut_float(doc, ppi)},
                     {"__preset", NULL, yyjson_mut_strcpy(doc, preset->name)},
                     {"__unit", NULL, yyjson_mut_strcpy(doc, unit)}};
    pc_status status = apply(doc, fields, items, 5, err);
    if (status == PC_OK) {
        (void)yyjson_mut_obj_remove_key(fields, "__savedPreset");
    }
    return status;
}

pc_status pc_ui_new_offer_clipboard(yyjson_mut_doc *d, yyjson_mut_val *fields, uint32_t width,
                                    uint32_t height, pc_error *err)
{
    if (!d || !yyjson_mut_is_obj(fields) || !width || !height || width > 300000 || height > 300000)
        return pc_fail(err, PC_ERR_INVALID, "invalid clipboard document dimensions");
    pc_ui_new_preset preset = {"Recent", "Clipboard", width, height, 72};
    yyjson_mut_val *size = yyjson_mut_arr(d);
    if (!size || !yyjson_mut_arr_add_uint(d, size, width) || !yyjson_mut_arr_add_uint(d, size, height))
        return pc_fail(err, PC_ERR_NOMEM, "clipboard preset");
    pc_status status = pc_ui_new_apply_preset(d, fields, &preset, 300, 72, err);
    if (status == PC_OK) {
        patch items[] = {{"__clipboard", NULL, size}, {"__category", NULL, yyjson_mut_str(d, "Recent")}};
        status = apply(d, fields, items, 2, err);
    }
    return status;
}

yyjson_mut_val *pc_ui_new_settings(yyjson_mut_doc *doc, yyjson_mut_val *fields)
{
    if (!doc || !yyjson_mut_is_obj(fields)) {
        return NULL;
    }
    yyjson_mut_val *result = yyjson_mut_obj(doc);
    if (!result) {
        return NULL;
    }
    size_t i, n;
    yyjson_mut_val *key, *value;
    yyjson_mut_obj_foreach(fields, i, n, key, value)
    {
        if (!key || !value) {
            return NULL;
        }
        const char *name = yyjson_mut_get_str(key);
        if (!name || !strncmp(name, "__", 2)) {
            continue;
        }
        yyjson_mut_val *copy = yyjson_mut_val_mut_copy(doc, value);
        yyjson_mut_val *copied_key = yyjson_mut_strcpy(doc, name);
        if (!copy || !copied_key || !yyjson_mut_obj_add(result, copied_key, copy)) {
            return NULL;
        }
    }
    const char *unit = string(fields, "__unit", "px");
    if (find_unit(unit) == &units[0]) {
        unit = "px";
    }
    if (!yyjson_mut_obj_add_strcpy(doc, result, "unit", unit) ||
        !yyjson_mut_obj_add_str(doc, result, "resolutionUnit",
                                !strcmp(string(fields, "__resUnit", "in"), "cm") ? "cm" : "in")) {
        return NULL;
    }
    return result;
}
pc_status pc_ui_new_apply_settings(yyjson_mut_doc *doc, yyjson_mut_val *fields, yyjson_mut_val *settings,
                                   pc_error *err)
{
    if (!doc || !yyjson_mut_is_obj(settings)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid document settings");
    }
    static const char *const keys[] = {"width", "height",     "resolution",     "mode",
                                       "depth", "background", "backgroundColor"};
    patch items[9];
    size_t count = 0;
    for (size_t i = 0; i < 7; i++) {
        yyjson_mut_val *value = yyjson_mut_obj_get(settings, keys[i]);
        if (value) {
            items[count++] = (patch){keys[i], NULL, yyjson_mut_val_mut_copy(doc, value)};
        }
    }
    items[count++] = (patch){"__unit", NULL, yyjson_mut_strcpy(doc, string(settings, "unit", "px"))};
    items[count++] =
        (patch){"__resUnit", NULL, yyjson_mut_strcpy(doc, string(settings, "resolutionUnit", "in"))};
    pc_status status = apply(doc, fields, items, count, err);
    if (status == PC_OK) {
        const char *const remove[] = {"__preset", "__savedPreset", "__widthTyped", "__heightTyped"};
        for (size_t i = 0; i < 4; i++) {
            (void)yyjson_mut_obj_remove_key(fields, remove[i]);
        }
        if (!yyjson_mut_obj_get(settings, "backgroundColor")) {
            (void)yyjson_mut_obj_remove_key(fields, "backgroundColor");
        }
    }
    return status;
}
