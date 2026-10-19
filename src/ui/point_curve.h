#ifndef PC_UI_POINT_CURVE_H
#define PC_UI_POINT_CURVE_H
#include "ui/shortcuts.h"
#include "base/json.h"
enum { PC_UI_CURVE_CAPACITY = 19, PC_UI_CURVE_INSERT_LIMIT = 16 };
typedef struct pc_ui_point_curve {
    double points[PC_UI_CURVE_CAPACITY][2];
    size_t count, selected, drag_index;
    bool has_selected, dragging, has_drag_index, removed;
    double offset[2];
} pc_ui_point_curve;
typedef struct pc_ui_curve_edit {
    bool changed, commit;
} pc_ui_curve_edit;
pc_status pc_ui_point_curve_load(pc_ui_point_curve *curve, const yyjson_mut_val *points, pc_error *err);
pc_status pc_ui_point_curve_json(const pc_ui_point_curve *curve, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                 pc_error *err);
bool pc_ui_point_curve_move(pc_ui_point_curve *curve, size_t index, double x, double y);
bool pc_ui_point_curve_delete(pc_ui_point_curve *curve, size_t index);
bool pc_ui_point_curve_insert(pc_ui_point_curve *curve, double x, double y, size_t *index);
/* phase: 0 press,1 drag,2 release,3 secondary click. Rect and pointer are screen pixels. */
pc_ui_curve_edit pc_ui_point_curve_pointer(pc_ui_point_curve *curve, const float rect[4], unsigned phase,
                                           float x, float y, unsigned modifiers);
pc_ui_curve_edit pc_ui_point_curve_key(pc_ui_point_curve *curve, pc_ui_key key, unsigned modifiers);
#endif
