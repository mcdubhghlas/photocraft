#ifndef PC_UI_COLOR_PICKER_H
#define PC_UI_COLOR_PICKER_H
#include "base/json.h"
typedef struct pc_ui_picker_components {
    float rgb[3], hsv[3], lab[3], cmyk[4];
} pc_ui_picker_components;
typedef enum pc_ui_picker_keep {
    PC_UI_PICKER_NOTHING,
    PC_UI_PICKER_HSV,
    PC_UI_PICKER_LAB,
    PC_UI_PICKER_CMYK
} pc_ui_picker_keep;
void pc_ui_picker_hex(const float rgb[3], char out[8]);
bool pc_ui_picker_parse_hex(const char *text, size_t len, float out[3]);
void pc_ui_picker_rgb_to_hsv(const float rgb[3], float out[3]);
void pc_ui_picker_hsv_to_rgb(const float hsv[3], float out[3]);
void pc_ui_picker_current(const yyjson_mut_val *fields, pc_ui_picker_components *out);
pc_status pc_ui_picker_set_rgb(yyjson_mut_doc *doc, yyjson_mut_val *fields, const float rgb[3],
                               pc_ui_picker_keep keep, const float *components, pc_error *err);
yyjson_mut_val *pc_ui_picker_initial(yyjson_mut_doc *doc, const char *target, const char *label,
                                     const float rgb[3]);
void pc_ui_picker_field_color(const char *mode, float z, float x, float y, float out[3]);
void pc_ui_picker_slider_color(const char *mode, float z, const pc_ui_picker_components *current,
                               float out[3]);
void pc_ui_picker_axes(const char *mode, const pc_ui_picker_components *current, float out[3]);
bool pc_ui_picker_field_lab(const char *mode, float z, float x, float y, float out[3]);
bool pc_ui_picker_out_of_gamut(const float rgb[3], float nearest[3]);
bool pc_ui_picker_not_web_safe(const float rgb[3], float nearest[3]);
#endif
