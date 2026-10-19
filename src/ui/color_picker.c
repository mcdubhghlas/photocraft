/* Color Picker numeric contracts, including retained out-of-gamut Lab/CMYK and grey hue. */
#include "ui/color_picker.h"
#include "base/utf8.h"
#include "color/convert.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static float unit(float v)
{
    return isnan(v) ? 0 : fminf(1, fmaxf(0, v));
}
void pc_ui_picker_hex(const float rgb[3], char out[8])
{
    unsigned r = (unsigned)roundf(unit(rgb[0]) * 255), g = (unsigned)roundf(unit(rgb[1]) * 255),
             b = (unsigned)roundf(unit(rgb[2]) * 255);
    (void)snprintf(out, 8, "#%02x%02x%02x", r, g, b);
}
static int nibble(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}
bool pc_ui_picker_parse_hex(const char *text, size_t len, float out[3])
{
    if (!text || !out) {
        return false;
    }
    size_t start = 0;
    len = pc_utf8_trim(text, len, &start);
    text += start;
    while (len && *text == '#') {
        text++;
        len--;
    }
    if (len != 6) {
        return false;
    }
    float rgb[3];
    for (size_t i = 0; i < 3; i++) {
        int hi = nibble((unsigned char)text[i * 2]), lo = nibble((unsigned char)text[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        rgb[i] = (float)(hi * 16 + lo) / 255;
    }
    memcpy(out, rgb, sizeof rgb);
    return true;
}
void pc_ui_picker_rgb_to_hsv(const float rgb[3], float out[3])
{
    float r = rgb[0], g = rgb[1], b = rgb[2], high = fmaxf(r, fmaxf(g, b)), low = fminf(r, fminf(g, b)),
          d = high - low;
    float h = d <= 0      ? 0
              : high == r ? 60 * fmodf((g - b) / d, 6)
              : high == g ? 60 * ((b - r) / d + 2)
                          : 60 * ((r - g) / d + 4);
    if (h < 0) {
        h += 360;
    }
    out[0] = h;
    out[1] = high <= 0 ? 0 : d / high;
    out[2] = high;
}
void pc_ui_picker_hsv_to_rgb(const float hsv[3], float out[3])
{
    float h = isfinite(hsv[0]) ? fmodf(hsv[0], 360) : 0;
    if (h < 0) {
        h += 360;
    }
    h /= 60;
    float c = hsv[2] * hsv[1], x = c * (1 - fabsf(fmodf(h, 2) - 1)), m = hsv[2] - c;
    float rgb[3];
    if (h < 1) {
        rgb[0] = c;
        rgb[1] = x;
        rgb[2] = 0;
    } else if (h < 2) {
        rgb[0] = x;
        rgb[1] = c;
        rgb[2] = 0;
    } else if (h < 3) {
        rgb[0] = 0;
        rgb[1] = c;
        rgb[2] = x;
    } else if (h < 4) {
        rgb[0] = 0;
        rgb[1] = x;
        rgb[2] = c;
    } else if (h < 5) {
        rgb[0] = x;
        rgb[1] = 0;
        rgb[2] = c;
    } else {
        rgb[0] = c;
        rgb[1] = 0;
        rgb[2] = x;
    }
    for (size_t i = 0; i < 3; i++) {
        out[i] = rgb[i] + m;
    }
}
static bool same_color(const float a[3], const float b[3])
{
    char x[8], y[8];
    pc_ui_picker_hex(a, x);
    pc_ui_picker_hex(b, y);
    return !strcmp(x, y);
}
static bool read_array(const yyjson_mut_val *root, const char *key, float *out, size_t n)
{
    yyjson_mut_val *a = yyjson_mut_obj_get(root, key);
    if (!yyjson_mut_is_arr(a) || yyjson_mut_arr_size(a) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_val *v = yyjson_mut_arr_get(a, i);
        double number = yyjson_mut_get_num(v);
        if (!yyjson_mut_is_num(v) || !isfinite(number) || fabs(number) > 3.402823466e38) {
            return false;
        }
        out[i] = (float)number;
    }
    return true;
}
void pc_ui_picker_current(const yyjson_mut_val *fields, pc_ui_picker_components *out)
{
    *out = (pc_ui_picker_components){0};
    const char *hex = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "color"));
    if (hex) {
        (void)pc_ui_picker_parse_hex(hex, strlen(hex), out->rgb);
    }
    pc_ui_picker_rgb_to_hsv(out->rgb, out->hsv);
    pc_srgb_to_lab(out->rgb, out->lab);
    pc_rgb_to_cmyk(out->rgb, out->cmyk);
    float saved[4], rgb[3];
    if (read_array(fields, "__hsv", saved, 3)) {
        pc_ui_picker_hsv_to_rgb(saved, rgb);
        if (same_color(rgb, out->rgb)) {
            memcpy(out->hsv, saved, sizeof out->hsv);
        }
    }
    if (read_array(fields, "__lab", saved, 3)) {
        pc_lab_to_srgb(saved, rgb);
        if (same_color(rgb, out->rgb)) {
            memcpy(out->lab, saved, sizeof out->lab);
        }
    }
    if (read_array(fields, "__cmyk", saved, 4)) {
        pc_cmyk_to_rgb(saved, rgb);
        if (same_color(rgb, out->rgb)) {
            memcpy(out->cmyk, saved, sizeof out->cmyk);
        }
    }
}
static yyjson_mut_val *array(yyjson_mut_doc *d, const float *values, size_t count)
{
    yyjson_mut_val *a = yyjson_mut_arr(d);
    for (size_t i = 0; i < count; i++) {
        if (!yyjson_mut_arr_add_float(d, a, values[i])) {
            return NULL;
        }
    }
    return a;
}
pc_status pc_ui_picker_set_rgb(yyjson_mut_doc *d, yyjson_mut_val *fields, const float input[3],
                               pc_ui_picker_keep keep, const float *components, pc_error *err)
{
    if (!d || !yyjson_mut_is_obj(fields) || !input || (keep != PC_UI_PICKER_NOTHING && !components) ||
        (unsigned)keep > PC_UI_PICKER_CMYK) {
        return pc_fail(err, PC_ERR_INVALID, "invalid picker colour");
    }
    float rgb[3] = {input[0], input[1], input[2]};
    if (yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "__webOnly"))) {
        for (size_t i = 0; i < 3; i++) {
            rgb[i] = roundf(unit(rgb[i]) * 5) / 5;
        }
        keep = PC_UI_PICKER_NOTHING;
    }
    char hex[8];
    pc_ui_picker_hex(rgb, hex);
    yyjson_mut_val *color = yyjson_mut_strcpy(d, hex), *color_key = yyjson_mut_str(d, "color"),
                   *stored_key = NULL, *stored = NULL;
    const char *keys[] = {NULL, "__hsv", "__lab", "__cmyk"};
    if (keep != PC_UI_PICKER_NOTHING) {
        stored_key = yyjson_mut_str(d, keys[keep]);
        stored = array(d, components, keep == PC_UI_PICKER_CMYK ? 4 : 3);
    }
    if (!color || !color_key || (keep != PC_UI_PICKER_NOTHING && (!stored_key || !stored))) {
        return pc_fail(err, PC_ERR_NOMEM, "picker colour");
    }
    (void)yyjson_mut_obj_put(fields, color_key, color);
    for (size_t i = 1; i < 4; i++) {
        yyjson_mut_obj_remove_key(fields, keys[i]);
    }
    if (stored) {
        (void)yyjson_mut_obj_put(fields, stored_key, stored);
    }
    return PC_OK;
}
yyjson_mut_val *pc_ui_picker_initial(yyjson_mut_doc *d, const char *target, const char *label,
                                     const float rgb[3])
{
    yyjson_mut_val *fields = yyjson_mut_obj(d);
    char hex[8];
    float hsv[3];
    pc_ui_picker_hex(rgb, hex);
    pc_ui_picker_rgb_to_hsv(rgb, hsv);
    yyjson_mut_obj_add_strcpy(d, fields, "__colorPicker", target);
    yyjson_mut_obj_add_strcpy(d, fields, "__label", label);
    yyjson_mut_obj_add_strcpy(d, fields, "color", hex);
    yyjson_mut_obj_add_strcpy(d, fields, "__orig", hex);
    yyjson_mut_obj_add_val(d, fields, "__hsv", array(d, hsv, 3));
    yyjson_mut_obj_add_str(d, fields, "__mode", "h");
    yyjson_mut_obj_add_bool(d, fields, "__webOnly", false);
    return fields;
}
bool pc_ui_picker_field_lab(const char *mode, float z, float x, float y, float out[3])
{
    float l = (1 - y) * 100, a = x * 255 - 128, b = (1 - y) * 255 - 128;
    if (!strcmp(mode, "lab_l")) {
        out[0] = z * 100;
        out[1] = a;
        out[2] = b;
        return true;
    }
    if (!strcmp(mode, "lab_a")) {
        out[0] = l;
        out[1] = z * 255 - 128;
        out[2] = x * 255 - 128;
        return true;
    }
    if (!strcmp(mode, "lab_b")) {
        out[0] = l;
        out[1] = a;
        out[2] = z * 255 - 128;
        return true;
    }
    return false;
}
void pc_ui_picker_field_color(const char *mode, float z, float x, float y, float out[3])
{
    float lab[3];
    if (pc_ui_picker_field_lab(mode, z, x, y, lab)) {
        pc_lab_to_srgb(lab, out);
        return;
    }
    if (!strcmp(mode, "r")) {
        out[0] = z;
        out[1] = 1 - y;
        out[2] = x;
        return;
    }
    if (!strcmp(mode, "g")) {
        out[0] = 1 - y;
        out[1] = z;
        out[2] = x;
        return;
    }
    if (!strcmp(mode, "b")) {
        out[0] = x;
        out[1] = 1 - y;
        out[2] = z;
        return;
    }
    float hsv[3] = {z * 360, x, 1 - y};
    if (!strcmp(mode, "s")) {
        hsv[0] = x * 360;
        hsv[1] = z;
    }
    if (!strcmp(mode, "v")) {
        hsv[0] = x * 360;
        hsv[1] = 1 - y;
        hsv[2] = z;
    }
    pc_ui_picker_hsv_to_rgb(hsv, out);
}
void pc_ui_picker_slider_color(const char *mode, float z, const pc_ui_picker_components *c, float out[3])
{
    if (!strncmp(mode, "lab_", 4)) {
        float lab[3];
        memcpy(lab, c->lab, sizeof lab);
        size_t axis = !strcmp(mode, "lab_l") ? 0 : !strcmp(mode, "lab_a") ? 1 : 2;
        lab[axis] = axis == 0 ? z * 100 : z * 255 - 128;
        pc_lab_to_srgb(lab, out);
        return;
    }
    if (!strcmp(mode, "r") || !strcmp(mode, "g") || !strcmp(mode, "b")) {
        memcpy(out, c->rgb, sizeof c->rgb);
        out[!strcmp(mode, "r") ? 0 : !strcmp(mode, "g") ? 1 : 2] = z;
        return;
    }
    float hsv[3] = {z * 360, 1, 1};
    if (!strcmp(mode, "s")) {
        memcpy(hsv, c->hsv, sizeof hsv);
        hsv[1] = z;
    }
    if (!strcmp(mode, "v")) {
        memcpy(hsv, c->hsv, sizeof hsv);
        hsv[2] = z;
    }
    pc_ui_picker_hsv_to_rgb(hsv, out);
}
void pc_ui_picker_axes(const char *mode, const pc_ui_picker_components *c, float out[3])
{
    float h = c->hsv[0] / 360, s = c->hsv[1], v = c->hsv[2];
    if (!strcmp(mode, "lab_l")) {
        out[0] = unit((c->lab[1] + 128) / 255);
        out[1] = 1 - unit((c->lab[2] + 128) / 255);
        out[2] = unit(c->lab[0] / 100);
    } else if (!strcmp(mode, "lab_a")) {
        out[0] = unit((c->lab[2] + 128) / 255);
        out[1] = 1 - unit(c->lab[0] / 100);
        out[2] = unit((c->lab[1] + 128) / 255);
    } else if (!strcmp(mode, "lab_b")) {
        out[0] = unit((c->lab[1] + 128) / 255);
        out[1] = 1 - unit(c->lab[0] / 100);
        out[2] = unit((c->lab[2] + 128) / 255);
    } else if (!strcmp(mode, "s")) {
        out[0] = h;
        out[1] = 1 - v;
        out[2] = s;
    } else if (!strcmp(mode, "v")) {
        out[0] = h;
        out[1] = 1 - s;
        out[2] = v;
    } else if (!strcmp(mode, "r")) {
        out[0] = c->rgb[2];
        out[1] = 1 - c->rgb[1];
        out[2] = c->rgb[0];
    } else if (!strcmp(mode, "g")) {
        out[0] = c->rgb[2];
        out[1] = 1 - c->rgb[0];
        out[2] = c->rgb[1];
    } else if (!strcmp(mode, "b")) {
        out[0] = c->rgb[0];
        out[1] = 1 - c->rgb[1];
        out[2] = c->rgb[2];
    } else {
        out[0] = s;
        out[1] = 1 - v;
        out[2] = h;
    }
}
bool pc_ui_picker_out_of_gamut(const float rgb[3], float nearest[3])
{
    float inks[4], a[3], b[3];
    pc_rgb_to_cmyk(rgb, inks);
    pc_cmyk_to_rgb(inks, nearest);
    for (size_t i = 0; i < 3; i++) {
        nearest[i] = unit(nearest[i]);
    }
    pc_srgb_to_lab(rgb, a);
    pc_srgb_to_lab(nearest, b);
    float sum = 0;
    for (size_t i = 0; i < 3; i++) {
        float d = a[i] - b[i];
        sum += d * d;
    }
    return sqrtf(sum) > 3;
}
bool pc_ui_picker_not_web_safe(const float rgb[3], float nearest[3])
{
    for (size_t i = 0; i < 3; i++) {
        nearest[i] = roundf(unit(rgb[i]) * 5) / 5;
    }
    return !same_color(rgb, nearest);
}
