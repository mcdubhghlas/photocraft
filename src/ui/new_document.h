/* File > New presets and physical-size editing. All stored dimensions are whole pixels. */
#ifndef PC_UI_NEW_DOCUMENT_H
#define PC_UI_NEW_DOCUMENT_H
#include "base/json.h"
typedef struct pc_ui_new_unit {
    const char *id, *label;
    float per_inch;
} pc_ui_new_unit;
typedef struct pc_ui_new_preset {
    const char *category, *name;
    uint32_t width, height;
    float ppi;
} pc_ui_new_preset;
pc_status pc_ui_new_offer_clipboard(yyjson_mut_doc *doc, yyjson_mut_val *fields, uint32_t width,
                                    uint32_t height, pc_error *err);
const pc_ui_new_unit *pc_ui_new_units(size_t *count);
const pc_ui_new_preset *pc_ui_new_presets(size_t *count);
float pc_ui_new_to_unit(float pixels, const char *unit, float ppi);
float pc_ui_new_from_unit(float value, const char *unit, float ppi);
uint32_t pc_ui_new_pixel_value(float value);
float pc_ui_new_shown_size(yyjson_mut_val *fields, const char *key, float fallback, const char *unit,
                           float ppi);
yyjson_mut_val *pc_ui_new_settings(yyjson_mut_doc *doc, yyjson_mut_val *fields);
pc_status pc_ui_new_apply_settings(yyjson_mut_doc *doc, yyjson_mut_val *fields, yyjson_mut_val *settings,
                                   pc_error *err);
pc_status pc_ui_new_set_size(yyjson_mut_doc *doc, yyjson_mut_val *fields, const char *key, float value,
                             const char *unit, float ppi, pc_error *err);
pc_status pc_ui_new_swap_size(yyjson_mut_doc *doc, yyjson_mut_val *fields, pc_error *err);
pc_status pc_ui_new_set_resolution(yyjson_mut_doc *doc, yyjson_mut_val *fields, float ppi, pc_error *err);
pc_status pc_ui_new_apply_preset(yyjson_mut_doc *doc, yyjson_mut_val *fields, const pc_ui_new_preset *preset,
                                 float print_ppi, float screen_ppi, pc_error *err);
#endif
