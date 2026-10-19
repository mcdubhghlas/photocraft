#include "ui/point_curve.h"
#include <math.h>
#include <string.h>
static double clamp(double v, double lo, double hi)
{
    return fmin(hi, fmax(lo, v));
}
pc_status pc_ui_point_curve_load(pc_ui_point_curve *curve, const yyjson_mut_val *values, pc_error *err)
{
    pc_ui_point_curve next = {0};
    if (!values || yyjson_mut_is_null(values)) {
        next.count = 2;
        next.points[1][0] = next.points[1][1] = 255;
    } else {
        size_t count = yyjson_mut_arr_size(values);
        if (!yyjson_mut_is_arr(values) || count < 2 || count > PC_UI_CURVE_CAPACITY)
            return pc_fail(err, PC_ERR_INVALID, "a point curve needs 2 to 19 points");
        next.count = count;
        for (size_t i = 0; i < count; i++) {
            yyjson_mut_val *pair = yyjson_mut_arr_get(values, i);
            if (!yyjson_mut_is_arr(pair) || yyjson_mut_arr_size(pair) != 2)
                return pc_fail(err, PC_ERR_INVALID, "invalid curve point");
            for (size_t a = 0; a < 2; a++) {
                yyjson_mut_val *n = yyjson_mut_arr_get(pair, a);
                double v = yyjson_mut_get_num(n);
                if (!yyjson_mut_is_num(n) || !isfinite(v) || v < 0 || v > 255)
                    return pc_fail(err, PC_ERR_INVALID, "curve values must be between 0 and 255");
                next.points[i][a] = v;
            }
            if (i && next.points[i][0] <= next.points[i - 1][0])
                return pc_fail(err, PC_ERR_INVALID, "curve inputs must increase");
        }
    }
    *curve = next;
    return PC_OK;
}
pc_status pc_ui_point_curve_json(const pc_ui_point_curve *c, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                 pc_error *err)
{
    if (c->count < 2 || c->count > PC_UI_CURVE_CAPACITY)
        return pc_fail(err, PC_ERR_INVALID, "invalid point curve");
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    bool ok = array != NULL;
    for (size_t i = 0; ok && i < c->count; i++) {
        yyjson_mut_val *pair = yyjson_mut_arr(doc);
        ok = pair && isfinite(c->points[i][0]) && isfinite(c->points[i][1]) &&
             yyjson_mut_arr_add_real(doc, pair, c->points[i][0]) &&
             yyjson_mut_arr_add_real(doc, pair, c->points[i][1]) && yyjson_mut_arr_add_val(array, pair);
    }
    if (!ok)
        return pc_fail(err, PC_ERR_NOMEM, "curve points");
    *out = array;
    return PC_OK;
}
bool pc_ui_point_curve_insert(pc_ui_point_curve *c, double x, double y, size_t *index)
{
    if (!isfinite(x) || !isfinite(y) || c->count > PC_UI_CURVE_CAPACITY)
        return false;
    x = clamp(x, 0, 255);
    y = clamp(y, 0, 255);
    for (size_t i = 0; i < c->count; i++)
        if (fabs(c->points[i][0] - x) < 2) {
            *index = i;
            return true;
        }
    if (c->count >= PC_UI_CURVE_INSERT_LIMIT)
        return false;
    size_t i = 0;
    while (i < c->count && c->points[i][0] < x)
        i++;
    memmove(c->points + i + 1, c->points + i, (c->count - i) * sizeof c->points[0]);
    c->points[i][0] = x;
    c->points[i][1] = y;
    c->count++;
    *index = i;
    return true;
}
bool pc_ui_point_curve_delete(pc_ui_point_curve *c, size_t index)
{
    if (c->count > PC_UI_CURVE_CAPACITY || c->count <= 2 || index == 0 || index >= c->count - 1)
        return false;
    memmove(c->points + index, c->points + index + 1, (c->count - index - 1) * sizeof c->points[0]);
    c->count--;
    return true;
}
bool pc_ui_point_curve_move(pc_ui_point_curve *c, size_t i, double x, double y)
{
    if (c->count > PC_UI_CURVE_CAPACITY || i >= c->count || !isfinite(x) || !isfinite(y))
        return false;
    double lo = i ? c->points[i - 1][0] + 1 : 0, hi = i + 1 < c->count ? c->points[i + 1][0] - 1 : 255;
    if (!isfinite(lo) || !isfinite(hi))
        return false;
    x = clamp(round(x), lo, fmax(lo, hi));
    y = clamp(round(y), 0, 255);
    bool changed = x != c->points[i][0] || y != c->points[i][1];
    c->points[i][0] = x;
    c->points[i][1] = y;
    return changed;
}
static size_t hit(const pc_ui_point_curve *c, const float r[4], float x, float y)
{
    size_t index = SIZE_MAX;
    double closest = 9;
    for (size_t i = 0; i < c->count; i++) {
        double sx = r[0] + c->points[i][0] / 255 * r[2], sy = r[1] + r[3] - c->points[i][1] / 255 * r[3],
               d = hypot(sx - x, sy - y);
        if (d <= closest) {
            closest = d;
            index = i;
        }
    }
    return index;
}
pc_ui_curve_edit pc_ui_point_curve_pointer(pc_ui_point_curve *c, const float r[4], unsigned phase, float x,
                                           float y, unsigned mods)
{
    pc_ui_curve_edit edit = {0};
    if (c->count < 2 || c->count > PC_UI_CURVE_CAPACITY || phase > 3 || !isfinite(x) || !isfinite(y))
        return edit;
    for (size_t i = 0; i < 4; i++)
        if (!isfinite(r[i]))
            return edit;
    if (r[2] <= 0 || r[3] <= 0)
        return edit;
    if (c->has_selected && c->selected >= c->count)
        c->has_selected = false;
    if (c->has_drag_index && c->drag_index >= c->count)
        c->dragging = false;
    double vx = clamp((x - r[0]) / r[2] * 255, 0, 255), vy = clamp((r[1] + r[3] - y) / r[3] * 255, 0, 255);
    bool inside = x >= r[0] && x <= r[0] + r[2] && y >= r[1] && y <= r[1] + r[3];
    if (phase == 0 || phase == 3) {
        size_t index = hit(c, r, x, y);
        c->dragging = false;
        c->removed = false;
        bool remove = phase == 3 || (mods & (PC_UI_MOD_COMMAND | PC_UI_MOD_CTRL));
        if (remove) {
            if (index != SIZE_MAX)
                edit.changed = edit.commit = pc_ui_point_curve_delete(c, index);
            c->has_selected = false;
            return edit;
        }
        if (index == SIZE_MAX && inside) {
            size_t before = c->count;
            if (pc_ui_point_curve_insert(c, vx, vy, &index))
                edit.changed = c->count != before;
        }
        if (index != SIZE_MAX) {
            c->has_selected = c->dragging = c->has_drag_index = true;
            c->selected = c->drag_index = index;
            c->offset[0] = c->points[index][0] - vx;
            c->offset[1] = c->points[index][1] - vy;
        }
        return edit;
    }
    if (!c->dragging)
        return edit;
    bool outside = x < r[0] - 12 || x > r[0] + r[2] + 12 || y < r[1] - 12 || y > r[1] + r[3] + 12;
    if (c->has_drag_index && outside && pc_ui_point_curve_delete(c, c->drag_index)) {
        c->has_drag_index = c->has_selected = false;
        c->removed = true;
        edit.changed = true;
    } else if (c->has_drag_index)
        edit.changed = pc_ui_point_curve_move(c, c->drag_index, vx + c->offset[0], vy + c->offset[1]);
    else if (c->removed && !outside) {
        size_t index;
        if (pc_ui_point_curve_insert(c, vx + c->offset[0], vy + c->offset[1], &index)) {
            c->has_selected = c->has_drag_index = true;
            c->selected = c->drag_index = index;
            edit.changed = true;
        }
    }
    if (phase == 2) {
        c->dragging = false;
        edit.commit = true;
    }
    return edit;
}
pc_ui_curve_edit pc_ui_point_curve_key(pc_ui_point_curve *c, pc_ui_key key, unsigned mods)
{
    pc_ui_curve_edit edit = {0};
    if (!c->has_selected || c->selected >= c->count || (mods & ~(unsigned)PC_UI_MOD_SHIFT))
        return edit;
    if (!mods && (key == PC_KEY_DELETE || key == PC_KEY_BACKSPACE)) {
        edit.changed = edit.commit = pc_ui_point_curve_delete(c, c->selected);
        if (edit.changed)
            c->has_selected = false;
        return edit;
    }
    double step = mods & PC_UI_MOD_SHIFT ? 10 : 1, x = c->points[c->selected][0],
           y = c->points[c->selected][1];
    if (key == PC_KEY_ARROWLEFT)
        x -= step;
    else if (key == PC_KEY_ARROWRIGHT)
        x += step;
    else if (key == PC_KEY_ARROWUP)
        y += step;
    else if (key == PC_KEY_ARROWDOWN)
        y -= step;
    else
        return edit;
    edit.changed = edit.commit = pc_ui_point_curve_move(c, c->selected, x, y);
    return edit;
}
