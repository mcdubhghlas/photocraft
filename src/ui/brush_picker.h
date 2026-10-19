#ifndef PC_UI_BRUSH_PICKER_H
#define PC_UI_BRUSH_PICKER_H
#include "base/json.h"
typedef struct pc_ui_brush_picker {
    bool open, show_name, show_stroke, show_tip;
    float point[2], size[2], scale;
} pc_ui_brush_picker;
void pc_ui_brush_picker_default(pc_ui_brush_picker *picker);
/* Validates the picker subset of ui.set without changing any state on failure. */
pc_status pc_ui_brush_picker_configure(pc_ui_brush_picker *picker, yyjson_val *params, bool *grid,
                                       bool *has_brush_size, double *brush_size, pc_error *err);
pc_status pc_ui_brush_picker_write(const pc_ui_brush_picker *picker, bool grid, yyjson_mut_doc *doc,
                                   yyjson_mut_val *root, pc_error *err);
pc_status pc_ui_brush_picker_read(pc_ui_brush_picker *picker, bool *grid, yyjson_val *root, pc_error *err);
#endif
