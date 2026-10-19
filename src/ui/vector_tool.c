#include "vector_tool.h"
#include "ui/shortcuts.h"
#include "engine/vector_cmds.h"
#include <math.h>
#include <string.h>
static yyjson_mut_val *array(yyjson_mut_doc *doc, const double *values, size_t count)
{
    yyjson_mut_val *out = yyjson_mut_arr(doc);
    if (!out) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        if (!yyjson_mut_arr_add_real(doc, out, values[i])) {
            return NULL;
        }
    }
    return out;
}
static yyjson_mut_val *color(yyjson_mut_doc *doc, const float values[4])
{
    double rgba[4] = {values[0], values[1], values[2], values[3]};
    return array(doc, rgba, 4);
}
pc_status pc_ui_shape_drag_params(pc_ui_tool tool, const pc_ui_tool_options *o, const float foreground[4],
                                  const float background[4], const float start[2], const float end[2],
                                  unsigned modifiers, pc_json_value *out, pc_error *err)
{
    if (!o || !start || !end || !foreground || !background || !out) {
        return pc_fail(err, PC_ERR_INVALID, "missing shape drag");
    }
    if (!isfinite(o->stroke_width) || !isfinite(o->corner_radius) || !isfinite(o->line_weight)) {
        return pc_fail(err, PC_ERR_INVALID, "shape dimensions must be finite");
    }
    bool line = tool == PC_TOOL_LINE;
    const char *kind = line                            ? "line"
                       : tool == PC_TOOL_RECTANGLE     ? (o->corner_radius > 0 ? "roundedRect" : "rect")
                       : tool == PC_TOOL_ELLIPSE_SHAPE ? "ellipse"
                       : tool == PC_TOOL_TRIANGLE || tool == PC_TOOL_POLYGON ? "polygon"
                                                                             : NULL;
    if (!kind) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "this shape requires a selected custom preset");
    }
    for (size_t i = 0; i < 2; i++) {
        if (!isfinite(start[i]) || !isfinite(end[i]) || fabsf(start[i]) > 1e6f || fabsf(end[i]) > 1e6f) {
            return pc_fail(err, PC_ERR_INVALID, "shape coordinates are out of range");
        }
    }
    for (size_t i = 0; i < 4; i++) {
        if (!isfinite(foreground[i]) || !isfinite(background[i])) {
            return pc_fail(err, PC_ERR_INVALID, "shape colors must be finite");
        }
    }
    double dx = (double)end[0] - start[0], dy = (double)end[1] - start[1], x = start[0], y = start[1];
    bool shift = (modifiers & PC_UI_MOD_SHIFT) != 0, alt = (modifiers & PC_UI_MOD_ALT) != 0;
    if (line) {
        if (hypot(dx, dy) < 1) {
            return PC_ERR_NOT_FOUND;
        }
        if (shift) {
            double length = hypot(dx, dy),
                   angle = round(atan2(dy, dx) / .7853981633974483) * .7853981633974483;
            dx = length * cos(angle);
            dy = length * sin(angle);
        }
    } else {
        if (shift) {
            double size = fmax(fabs(dx), fabs(dy));
            dx = copysign(size, dx);
            dy = copysign(size, dy);
        }
        if (alt) {
            x -= dx;
            y -= dy;
            dx *= 2;
            dy *= 2;
        }
        x = round(fmin(x, x + dx));
        y = round(fmin(y, y + dy));
        dx = round(fabs(dx));
        dy = round(fabs(dy));
        if (dx < 1 || dy < 1) {
            return PC_ERR_NOT_FOUND;
        }
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = doc ? yyjson_mut_obj(doc) : NULL;
    bool ok = p && yyjson_mut_obj_add_str(doc, p, "kind", kind);
    if (line) {
        double a[2] = {start[0], start[1]}, b[2] = {start[0] + dx, start[1] + dy};
        ok = ok && yyjson_mut_obj_add_val(doc, p, "from", array(doc, a, 2)) &&
             yyjson_mut_obj_add_val(doc, p, "to", array(doc, b, 2)) &&
             yyjson_mut_obj_add_real(doc, p, "weight", fmax(1, o->line_weight));
    } else {
        double box[4] = {x, y, dx, dy};
        ok = ok && yyjson_mut_obj_add_val(doc, p, "rect", array(doc, box, 4));
        if (tool == PC_TOOL_RECTANGLE && o->corner_radius > 0) {
            double radii[4] = {o->corner_radius, o->corner_radius, o->corner_radius, o->corner_radius};
            ok = ok && yyjson_mut_obj_add_val(doc, p, "radii", array(doc, radii, 4));
        }
        if (tool == PC_TOOL_TRIANGLE || tool == PC_TOOL_POLYGON) {
            ok = ok && yyjson_mut_obj_add_uint(doc, p, "sides",
                                               tool == PC_TOOL_TRIANGLE ? 3
                                               : o->polygon_sides < 3   ? 3
                                                                        : o->polygon_sides);
        }
    }
    ok = ok && yyjson_mut_obj_add_val(doc, p, "fill",
                                      o->shape_fill ? color(doc, foreground) : yyjson_mut_null(doc));
    if (o->stroke_width > 0) {
        const pc_shape_stroke *s = &o->shape_stroke;
        const char *align[] = {"inside", "center", "outside"}, *cap[] = {"butt", "round", "square"},
                   *join[] = {"miter", "round", "bevel"};
        if ((unsigned)s->align >= 3 || (unsigned)s->cap >= 3 || (unsigned)s->join >= 3 ||
            s->ndashes > 100000 || (s->ndashes && !s->dashes)) {
            yyjson_mut_doc_free(doc);
            return pc_fail(err, PC_ERR_INVALID, "invalid shape stroke");
        }
        yyjson_mut_val *stroke = yyjson_mut_obj(doc), *dashes = yyjson_mut_arr(doc);
        ok = ok && stroke && dashes && yyjson_mut_obj_add_real(doc, stroke, "width", o->stroke_width) &&
             yyjson_mut_obj_add_str(doc, stroke, "align", align[s->align]) &&
             yyjson_mut_obj_add_str(doc, stroke, "cap", cap[s->cap]) &&
             yyjson_mut_obj_add_str(doc, stroke, "join", join[s->join]) &&
             yyjson_mut_obj_add_real(doc, stroke, "miterLimit", s->miter_limit) &&
             yyjson_mut_obj_add_real(doc, stroke, "dashOffset", s->dash_offset) &&
             yyjson_mut_obj_add_real(doc, stroke, "opacity", s->opacity * 100) &&
             yyjson_mut_obj_add_val(doc, stroke, "color", color(doc, background));
        for (size_t i = 0; ok && i < s->ndashes; i++) {
            ok = yyjson_mut_arr_add_real(doc, dashes, s->dashes[i]);
        }
        ok = ok && yyjson_mut_obj_add_val(doc, stroke, "dashes", dashes) &&
             yyjson_mut_obj_add_val(doc, p, "stroke", stroke);
    } else {
        ok = ok && yyjson_mut_obj_add_null(doc, p, "stroke");
    }
    pc_status status =
        ok ? pc_json_value_from_mut(p, out, err) : pc_fail(err, PC_ERR_NOMEM, "shape drag parameters");
    yyjson_mut_doc_free(doc);
    return status;
}
pc_status pc_ui_shape_gesture(pc_ui_tool_gesture *gesture, pc_engine_session *session, bool preview,
                              pc_json_value *out, pc_error *err)
{
    if (!gesture->count) {
        return PC_OK;
    }
    const pc_ui_tool_point *first = &gesture->points[0], *last = &gesture->points[gesture->count - 1];
    float start[2] = {first->x, first->y}, end[2] = {last->x, last->y};
    pc_json_value params = PC_JSON_VALUE_INIT;
    pc_status status =
        pc_ui_shape_drag_params(gesture->tool, &gesture->options, gesture->colors[0], gesture->colors[1],
                                start, end, gesture->modifiers, &params, err);
    if (status == PC_ERR_NOT_FOUND) {
        pc_shape_layer_fini(&gesture->shape_preview);
        gesture->has_shape_preview = false;
        return PC_OK;
    }
    pc_buf json = PC_BUF_INIT;
    yyjson_doc *parsed = NULL;
    if (status == PC_OK) {
        status = pc_json_write(pc_json_value_root(&params), 0, &json, err);
    }
    if (status == PC_OK) {
        status = pc_json_parse(pc_buf_cstr(&json), json.len, &parsed, err);
    }
    if (status == PC_OK) {
        if (preview) {
            pc_shape_layer shape = {0};
            status =
                pc_engine_shape_from_params(gesture->colors[0], yyjson_doc_get_root(parsed), &shape, err);
            if (status == PC_OK) {
                pc_shape_layer_fini(&gesture->shape_preview);
                gesture->shape_preview = shape;
                gesture->has_shape_preview = true;
            }
        } else {
            status =
                pc_engine_session_execute(session, "shape.create", yyjson_doc_get_root(parsed), out, err);
        }
    }
    yyjson_doc_free(parsed);
    pc_buf_fini(&json);
    pc_json_value_fini(&params);
    return status;
}
