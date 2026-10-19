#include "pen_state.h"
#include "base/mem.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
void pc_ui_pen_fini(pc_ui_pen *pen)
{
    if (pen) {
        free(pen->knots);
        free(pen->trace);
        memset(pen, 0, sizeof *pen);
    }
}
pc_status pc_ui_pen_to_json(const pc_ui_pen *pen, yyjson_mut_doc *doc, yyjson_mut_val **out, pc_error *err)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc), *knots = yyjson_mut_arr(doc), *unlinked = yyjson_mut_arr(doc);
    bool ok = o && knots && unlinked;
    for (size_t i = 0; ok && i < pen->count; i++) {
        yyjson_mut_val *knot = yyjson_mut_arr(doc);
        ok = knot != NULL;
        for (size_t j = 0; ok && j < 3; j++) {
            yyjson_mut_val *point = yyjson_mut_arr(doc);
            ok = point && yyjson_mut_arr_add_real(doc, point, pen->knots[i].points[j][0]) &&
                 yyjson_mut_arr_add_real(doc, point, pen->knots[i].points[j][1]) &&
                 yyjson_mut_arr_add_val(knot, point);
        }
        ok = ok && yyjson_mut_arr_add_val(knots, knot);
        if (pen->knots[i].unlinked) {
            ok = ok && yyjson_mut_arr_add_uint(doc, unlinked, i);
        }
    }
    ok = ok && yyjson_mut_obj_add_val(doc, o, "knots", knots) &&
         yyjson_mut_obj_add_val(doc, o, "unlinked", unlinked) &&
         yyjson_mut_obj_add_bool(doc, o, "curvature", pen->curvature);
    if (!ok) {
        return pc_fail(err, PC_ERR_NOMEM, "pen state");
    }
    *out = o;
    return PC_OK;
}
pc_status pc_ui_pen_from_json(pc_ui_pen *pen, yyjson_val *object, pc_error *err)
{
    yyjson_val *knots = pc_json_get(object, "knots"), *unlinked = pc_json_get(object, "unlinked"),
               *curvature = pc_json_get(object, "curvature");
    if (!yyjson_is_obj(object) || !yyjson_is_arr(knots) || yyjson_arr_size(knots) > 4096 ||
        (unlinked && (!yyjson_is_arr(unlinked) || yyjson_arr_size(unlinked) > 4096))) {
        return pc_fail(err, PC_ERR_INVALID, "invalid pen state");
    }
    pc_ui_pen next = {0};
    next.count = yyjson_arr_size(knots);
    next.capacity = next.count;
    if (curvature && !pc_json_as_bool(curvature, &next.curvature)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid curvature pen state");
    }
    next.knots = pc_calloc_array(next.count ? next.count : 1, sizeof *next.knots);
    if (!next.knots) {
        return pc_fail(err, PC_ERR_NOMEM, "pen knots");
    }
    pc_status status = PC_OK;
    for (size_t i = 0; status == PC_OK && i < next.count; i++) {
        yyjson_val *knot = yyjson_arr_get(knots, i);
        if (!yyjson_is_arr(knot) || yyjson_arr_size(knot) != 3) {
            status = pc_fail(err, PC_ERR_INVALID, "invalid pen knot");
            break;
        }
        for (size_t j = 0; j < 3; j++) {
            yyjson_val *point = yyjson_arr_get(knot, j);
            if (!yyjson_is_arr(point) || yyjson_arr_size(point) != 2) {
                status = pc_fail(err, PC_ERR_INVALID, "invalid pen point");
                break;
            }
            for (size_t axis = 0; axis < 2; axis++) {
                double value = 0;
                if (!pc_json_as_f64(yyjson_arr_get(point, axis), &value) || !isfinite(value) ||
                    fabs(value) > 1e6) {
                    status = pc_fail(err, PC_ERR_INVALID, "pen coordinate out of range");
                    break;
                }
                next.knots[i].points[j][axis] = value;
            }
        }
    }
    for (size_t i = 0; status == PC_OK && i < yyjson_arr_size(unlinked); i++) {
        uint64_t index = 0;
        if (!pc_json_as_u64(yyjson_arr_get(unlinked, i), &index) || index >= next.count) {
            status = pc_fail(err, PC_ERR_INVALID, "invalid pen corner index");
            break;
        }
        next.knots[index].unlinked = true;
    }
    if (status == PC_OK) {
        pc_ui_pen_fini(pen);
        *pen = next;
    } else {
        pc_ui_pen_fini(&next);
    }
    return status;
}
