#include "ui/brush_picker.h"
#include <math.h>
#include <string.h>
void pc_ui_brush_picker_default(pc_ui_brush_picker *p)
{
    *p = (pc_ui_brush_picker){
        .show_name = true, .show_stroke = true, .show_tip = true, .size = {300, 420}, .scale = 1};
}
static bool pair(yyjson_val *v, float out[2])
{
    if (!yyjson_is_arr(v) || yyjson_arr_size(v) != 2)
        return false;
    for (size_t i = 0; i < 2; i++) {
        yyjson_val *n = yyjson_arr_get(v, i);
        if (!yyjson_is_num(n) || !isfinite(yyjson_get_num(n)) || fabs(yyjson_get_num(n)) > 1000000)
            return false;
        out[i] = (float)yyjson_get_num(n);
    }
    return true;
}
pc_status pc_ui_brush_picker_configure(pc_ui_brush_picker *p, yyjson_val *params, bool *grid, bool *has_size,
                                       double *size, pc_error *err)
{
    pc_ui_brush_picker next = *p;
    bool next_grid = *grid;
    yyjson_val *v = pc_json_get(params, "brushPicker");
    if (v) {
        next.open = !yyjson_is_null(v);
        if (next.open && !pair(v, next.point))
            return pc_fail(err, PC_ERR_INVALID, "brushPicker must be [x,y] or null");
    }
    const char *names[] = {"brushPickerName", "brushPickerStroke", "brushPickerTip"};
    bool *flags[] = {&next.show_name, &next.show_stroke, &next.show_tip};
    for (size_t i = 0; i < 3; i++) {
        v = pc_json_get(params, names[i]);
        if (v && !pc_json_as_bool(v, flags[i]))
            return pc_fail(err, PC_ERR_INVALID, "%s must be boolean", names[i]);
    }
    if (!next.show_name && !next.show_stroke && !next.show_tip)
        return pc_fail(err, PC_ERR_INVALID, "at least one brush picker card part must stay on");
    v = pc_json_get(params, "brushPickerScale");
    if (v) {
        double scale;
        if (!pc_json_as_f64(v, &scale) || !isfinite(scale) || scale < .15 || scale > 2)
            return pc_fail(err, PC_ERR_INVALID, "brushPickerScale must be between 0.15 and 2");
        next.scale = (float)scale;
    }
    v = pc_json_get(params, "brushesView");
    if (v) {
        const char *view = yyjson_get_str(v);
        if (!view || (strcmp(view, "list") && strcmp(view, "grid")))
            return pc_fail(err, PC_ERR_INVALID, "brushesView must be list or grid");
        next_grid = !strcmp(view, "grid");
    }
    v = pc_json_get(params, "brushSize");
    double value = 0;
    if (v && (!pc_json_as_f64(v, &value) || !isfinite(value) || value < 1 || value > 5000))
        return pc_fail(err, PC_ERR_INVALID, "brushSize must be between 1 and 5000 pixels");
    *has_size = v != NULL;
    *size = value;
    *grid = next_grid;
    *p = next;
    return PC_OK;
}
pc_status pc_ui_brush_picker_write(const pc_ui_brush_picker *p, bool grid, yyjson_mut_doc *d,
                                   yyjson_mut_val *root, pc_error *err)
{
    yyjson_mut_val *point = p->open ? yyjson_mut_arr(d) : yyjson_mut_null(d), *size = yyjson_mut_arr(d),
                   *view = yyjson_mut_obj(d), *brushes = yyjson_mut_obj(d);
    bool ok = point && size && view && brushes &&
              (!p->open || (yyjson_mut_arr_add_real(d, point, p->point[0]) &&
                            yyjson_mut_arr_add_real(d, point, p->point[1]))) &&
              yyjson_mut_arr_add_real(d, size, p->size[0]) && yyjson_mut_arr_add_real(d, size, p->size[1]) &&
              yyjson_mut_obj_add_bool(d, view, "show_name", p->show_name) &&
              yyjson_mut_obj_add_bool(d, view, "show_stroke", p->show_stroke) &&
              yyjson_mut_obj_add_bool(d, view, "show_tip", p->show_tip) &&
              yyjson_mut_obj_add_real(d, view, "scale", p->scale) &&
              yyjson_mut_obj_add_str(d, brushes, "view", grid ? "grid" : "list") &&
              yyjson_mut_obj_add_val(d, root, "brushes", brushes) &&
              yyjson_mut_obj_add_val(d, root, "brush_picker", point) &&
              yyjson_mut_obj_add_val(d, root, "brush_picker_size", size) &&
              yyjson_mut_obj_add_val(d, root, "brush_picker_list", view);
    return ok ? PC_OK : pc_fail(err, PC_ERR_NOMEM, "brush picker state");
}
pc_status pc_ui_brush_picker_read(pc_ui_brush_picker *p, bool *grid, yyjson_val *root, pc_error *err)
{
    pc_ui_brush_picker next;
    pc_ui_brush_picker_default(&next);
    yyjson_val *point = pc_json_get(root, "brush_picker");
    if (point && !yyjson_is_null(point)) {
        if (!pair(point, next.point))
            return pc_fail(err, PC_ERR_INVALID, "invalid saved brush picker position");
        next.open = true;
    }
    yyjson_val *size = pc_json_get(root, "brush_picker_size");
    if (size && !yyjson_is_null(size) &&
        (!pair(size, next.size) || next.size[0] < 230 || next.size[1] < 170 || next.size[0] > 4000 ||
         next.size[1] > 4000))
        return pc_fail(err, PC_ERR_INVALID, "invalid saved brush picker size");
    yyjson_val *view = pc_json_get(root, "brush_picker_list");
    if (view && !yyjson_is_obj(view))
        return pc_fail(err, PC_ERR_INVALID, "invalid saved brush picker view");
    const char *names[] = {"show_name", "show_stroke", "show_tip"};
    bool *flags[] = {&next.show_name, &next.show_stroke, &next.show_tip};
    for (size_t i = 0; i < 3; i++) {
        yyjson_val *v = pc_json_get(view, names[i]);
        if (v && !pc_json_as_bool(v, flags[i]))
            return pc_fail(err, PC_ERR_INVALID, "invalid saved brush picker card part");
    }
    yyjson_val *scale = pc_json_get(view, "scale");
    double value = next.scale;
    if (scale && (!pc_json_as_f64(scale, &value) || !isfinite(value) || value < .15 || value > 2))
        return pc_fail(err, PC_ERR_INVALID, "invalid saved brush picker scale");
    next.scale = (float)value;
    if (!next.show_name && !next.show_stroke && !next.show_tip)
        return pc_fail(err, PC_ERR_INVALID, "invalid empty brush picker card");
    yyjson_val *mode_value = pc_json_get(pc_json_get(root, "brushes"), "view");
    const char *mode = pc_json_as_str(mode_value, NULL);
    if (mode_value && !mode)
        return pc_fail(err, PC_ERR_INVALID, "invalid saved brushes view");
    if (mode && strcmp(mode, "list") && strcmp(mode, "grid"))
        return pc_fail(err, PC_ERR_INVALID, "invalid saved brushes view");
    *grid = mode && !strcmp(mode, "grid");
    *p = next;
    return PC_OK;
}
