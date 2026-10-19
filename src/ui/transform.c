#include "ui/transform.h"
#include "algo/transform.h"
#include <math.h>
#include <string.h>
static const double UNIT[4] = {0, 0, 1, 1};
void pc_ui_transform_frame_rect(pc_ui_transform_frame *f, const double r[4])
{
    memcpy(f->rect, r, sizeof f->rect);
    const double q[4][2] = {{r[0], r[1]}, {r[2], r[1]}, {r[2], r[3]}, {r[0], r[3]}};
    memcpy(f->quad, q, sizeof q);
    f->pivot[0] = (r[0] + r[2]) / 2;
    f->pivot[1] = (r[1] + r[3]) / 2;
}
static bool finite_frame(const pc_ui_transform_frame *f)
{
    for (size_t i = 0; i < 4; i++) {
        if (!isfinite(f->rect[i]) || fabs(f->rect[i]) > 1000000)
            return false;
        for (size_t a = 0; a < 2; a++)
            if (!isfinite(f->quad[i][a]) || fabs(f->quad[i][a]) > 1000000)
                return false;
    }
    return isfinite(f->pivot[0]) && isfinite(f->pivot[1]) && fabs(f->pivot[0]) <= 1000000 &&
           fabs(f->pivot[1]) <= 1000000 && f->rect[2] > f->rect[0] && f->rect[3] > f->rect[1];
}
bool pc_ui_transform_frame_valid(const pc_ui_transform_frame *frame)
{
    return frame && finite_frame(frame);
}
pc_ui_transform_hit pc_ui_transform_hit_test(const pc_ui_transform_frame *f, const double p[2],
                                             double tolerance)
{
    pc_ui_transform_hit hit = {PC_TRANSFORM_OUTSIDE, 0};
    if (!f || !p || !finite_frame(f) || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(tolerance) ||
        tolerance <= 0)
        return hit;
    double nearest = tolerance;
    for (size_t i = 0; i < 9; i++) {
        double point[2];
        if (i < 4)
            memcpy(point, f->quad[i], sizeof point);
        else if (i < 8)
            for (size_t a = 0; a < 2; a++)
                point[a] = (f->quad[i - 4][a] + f->quad[(i - 3) % 4][a]) / 2;
        else
            memcpy(point, f->pivot, sizeof point);
        double distance = hypot(point[0] - p[0], point[1] - p[1]);
        if (distance < nearest) {
            nearest = distance;
            hit = (pc_ui_transform_hit){i < 4   ? PC_TRANSFORM_CORNER
                                        : i < 8 ? PC_TRANSFORM_EDGE
                                                : PC_TRANSFORM_PIVOT,
                                        i < 4 ? i : (i - 4) % 4};
        }
    }
    if (hit.part != PC_TRANSFORM_OUTSIDE)
        return hit;
    bool inside = false;
    for (size_t i = 0; i < 4; i++) {
        const double *a = f->quad[i], *b = f->quad[(i + 3) % 4];
        if ((a[1] > p[1]) != (b[1] > p[1]) && p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0])
            inside = !inside;
    }
    hit.part = inside ? PC_TRANSFORM_INSIDE : PC_TRANSFORM_OUTSIDE;
    return hit;
}
static void carry(const double from[4][2], const double to[4][2], const double pivot[2], double out[2])
{
    pc_algo_homography h0, h1, inverse;
    memcpy(out, pivot, 2 * sizeof *out);
    if (pc_algo_homography_rect_to_quad(UNIT, from, &h0) && pc_algo_homography_rect_to_quad(UNIT, to, &h1) &&
        pc_algo_homography_inverse(&h0, &inverse)) {
        double u, v, x, y;
        pc_algo_homography_apply(&inverse, pivot[0], pivot[1], &u, &v);
        pc_algo_homography_apply(&h1, u, v, &x, &y);
        if (isfinite(x) && isfinite(y)) {
            out[0] = x;
            out[1] = y;
        }
    }
}
static double turn(const double q[4][2], size_t i)
{
    const double *a = q[i], *b = q[(i + 1) % 4], *c = q[(i + 2) % 4];
    return (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]);
}
static bool convex(const double q[4][2], double sign, double minimum)
{
    for (size_t i = 0; i < 4; i++)
        if (!(turn(q, i) * sign > minimum))
            return false;
    return true;
}
static void keep_convex(const double from[4][2], double to[4][2])
{
    double sign = turn(from, 0) < 0 ? -1 : 1, minimum = INFINITY;
    for (size_t i = 0; i < 4; i++)
        minimum = fmin(minimum, fabs(turn(from, i)));
    minimum *= .001;
    if (!isfinite(minimum) || minimum <= 0 || !convex(from, sign, 0) ||
        convex((const double (*)[2])to, sign, minimum))
        return;
    double low = 0, high = 1, candidate[4][2];
    for (size_t step = 0; step < 40; step++) {
        double t = (low + high) / 2;
        for (size_t i = 0; i < 4; i++)
            for (size_t a = 0; a < 2; a++)
                candidate[i][a] = from[i][a] + (to[i][a] - from[i][a]) * t;
        if (convex((const double (*)[2])candidate, sign, minimum))
            low = t;
        else
            high = t;
    }
    for (size_t i = 0; i < 4; i++)
        for (size_t a = 0; a < 2; a++)
            to[i][a] = from[i][a] + (to[i][a] - from[i][a]) * low;
}
static void raw_drag(pc_ui_transform_frame *f, const pc_ui_transform_drag *g, const double p[2], bool shift,
                     bool alt, bool command, const double previous[2])
{
    const double (*q)[2] = g->before.quad;
    double dx = p[0] - g->start[0], dy = p[1] - g->start[1];
    size_t i = g->hit.index;
    if (g->hit.part == PC_TRANSFORM_INSIDE) {
        if (shift) {
            double angle = round(atan2(dy, dx) / .7853981633974483) * .7853981633974483;
            if (hypot(previous[0], previous[1]) > 0) {
                double previous_angle =
                    round(atan2(previous[1], previous[0]) / .7853981633974483) * .7853981633974483;
                double difference = fabs(remainder(atan2(dy, dx) - previous_angle, 6.283185307179586));
                if (difference < .39269908169872414 + .13962634015954636)
                    angle = previous_angle;
            }
            double projection = fmax(0, dx * cos(angle) + dy * sin(angle));
            dx = projection * cos(angle);
            dy = projection * sin(angle);
            if (fabs(dx) < 1e-9)
                dx = 0;
            if (fabs(dy) < 1e-9)
                dy = 0;
        }
        for (size_t k = 0; k < 4; k++) {
            f->quad[k][0] = q[k][0] + dx;
            f->quad[k][1] = q[k][1] + dy;
        }
        f->pivot[0] = g->before.pivot[0] + dx;
        f->pivot[1] = g->before.pivot[1] + dy;
    } else if (g->hit.part == PC_TRANSFORM_PIVOT)
        memcpy(f->pivot, p, sizeof f->pivot);
    else if (g->hit.part == PC_TRANSFORM_OUTSIDE) {
        const double *c = g->before.pivot;
        double da = atan2(p[1] - c[1], p[0] - c[0]) - atan2(g->start[1] - c[1], g->start[0] - c[0]);
        if (shift) {
            double base = atan2(q[1][1] - q[0][1], q[1][0] - q[0][0]), step = .2617993877991494;
            da = round((base + da) / step) * step - base;
        }
        for (size_t k = 0; k < 4; k++) {
            double x = q[k][0] - c[0], y = q[k][1] - c[1];
            f->quad[k][0] = c[0] + x * cos(da) - y * sin(da);
            f->quad[k][1] = c[1] + x * sin(da) + y * cos(da);
        }
    } else if (g->hit.part == PC_TRANSFORM_CORNER && command) {
        f->quad[i][0] += dx;
        f->quad[i][1] += dy;
        if (alt && shift) {
            bool horizontal = fabs(dx) >= fabs(dy);
            size_t other = horizontal ? (i ^ 1u) : (3u - i);
            f->quad[i][horizontal ? 1 : 0] = q[i][horizontal ? 1 : 0];
            f->quad[other][horizontal ? 0 : 1] -= horizontal ? dx : dy;
        }
        keep_convex(q, f->quad);
        carry(q, (const double (*)[2])f->quad, g->before.pivot, f->pivot);
    } else if (g->hit.part == PC_TRANSFORM_EDGE && command) {
        size_t b = (i + 1) % 4;
        if (shift) {
            double ex = q[b][0] - q[i][0], ey = q[b][1] - q[i][1], l2 = ex * ex + ey * ey;
            double t = l2 > 0 ? (dx * ex + dy * ey) / l2 : 0;
            dx = t * ex;
            dy = t * ey;
        }
        for (size_t k = 0; k < 4; k++) {
            double sign = k == i || k == b ? 1 : alt ? -1 : 0;
            f->quad[k][0] = q[k][0] + sign * dx;
            f->quad[k][1] = q[k][1] + sign * dy;
        }
        carry(q, (const double (*)[2])f->quad, g->before.pivot, f->pivot);
    } else {
        pc_algo_homography h, inverse;
        if (!pc_algo_homography_rect_to_quad(UNIT, q, &h) || !pc_algo_homography_inverse(&h, &inverse))
            return;
        double u, v, pu, pv;
        pc_algo_homography_apply(&inverse, p[0], p[1], &u, &v);
        pc_algo_homography_apply(&inverse, g->before.pivot[0], g->before.pivot[1], &pu, &pv);
        bool corner = g->hit.part == PC_TRANSFORM_CORNER;
        bool left = corner ? (i == 0 || i == 3) : i == 3, right = corner ? (i == 1 || i == 2) : i == 1,
             top = corner ? (i == 0 || i == 1) : i == 0, bottom = corner ? (i == 2 || i == 3) : i == 2;
        double r[4] = {left ? u : 0, top ? v : 0, right ? u : 1, bottom ? v : 1};
        if (alt) {
            if (left)
                r[2] = 2 * pu - r[0];
            if (right)
                r[0] = 2 * pu - r[2];
            if (top)
                r[3] = 2 * pv - r[1];
            if (bottom)
                r[1] = 2 * pv - r[3];
        }
        if (!shift && !corner) {
            double k = fabs((left || right) ? r[2] - r[0] : r[3] - r[1]);
            if (left || right) {
                double c = alt ? pv : .5;
                r[1] = c - k / 2;
                r[3] = c + k / 2;
            } else {
                double c = alt ? pu : .5;
                r[0] = c - k / 2;
                r[2] = c + k / 2;
            }
        }
        if (!shift && corner) {
            double sx = r[2] - r[0], sy = r[3] - r[1], k = fmax(fabs(sx), fabs(sy));
            double nx = sx == 0 ? 0 : copysign(k, sx), ny = sy == 0 ? 0 : copysign(k, sy),
                   ax = alt    ? pu
                        : left ? r[2]
                               : r[0],
                   ay = alt   ? pv
                        : top ? r[3]
                              : r[1];
            r[0] = ax - nx * (alt ? .5 : left ? 1 : 0);
            r[2] = r[0] + nx;
            r[1] = ay - ny * (alt ? .5 : top ? 1 : 0);
            r[3] = r[1] + ny;
        }
        const double local[4][2] = {{r[0], r[1]}, {r[2], r[1]}, {r[2], r[3]}, {r[0], r[3]}};
        for (size_t k = 0; k < 4; k++)
            pc_algo_homography_apply(&h, local[k][0], local[k][1], &f->quad[k][0], &f->quad[k][1]);
        if (!alt)
            carry(q, (const double (*)[2])f->quad, g->before.pivot, f->pivot);
    }
}
bool pc_ui_transform_apply(pc_ui_transform_frame *frame, const pc_ui_transform_drag *drag,
                           const double point[2], pc_ui_transform_mode mode, bool type_layer, bool shift,
                           bool alt, bool command, bool legacy, bool unlinked)
{
    if (!frame || !drag || !point || !finite_frame(frame) || !finite_frame(&drag->before) ||
        !isfinite(drag->start[0]) || !isfinite(drag->start[1]) || !isfinite(point[0]) ||
        !isfinite(point[1]) || fabs(point[0]) > 1000000 || fabs(point[1]) > 1000000 ||
        (unsigned)mode > PC_TRANSFORM_PERSPECTIVE || (unsigned)drag->hit.part > PC_TRANSFORM_OUTSIDE ||
        drag->hit.index >= 4)
        return false;
    double previous[2] = {frame->quad[0][0] - drag->before.quad[0][0],
                          frame->quad[0][1] - drag->before.quad[0][1]};
    pc_ui_transform_frame next = drag->before;
    pc_ui_transform_drag g = *drag;
    bool handle = g.hit.part == PC_TRANSFORM_CORNER || g.hit.part == PC_TRANSFORM_EDGE;
    if ((mode == PC_TRANSFORM_DISTORT &&
         (g.hit.part == PC_TRANSFORM_EDGE || g.hit.part == PC_TRANSFORM_OUTSIDE)) ||
        (mode == PC_TRANSFORM_SKEW && g.hit.part == PC_TRANSFORM_OUTSIDE))
        return false;
    if (type_layer && g.hit.part == PC_TRANSFORM_CORNER) {
        command = false;
        if (mode == PC_TRANSFORM_DISTORT || mode == PC_TRANSFORM_PERSPECTIVE)
            mode = PC_TRANSFORM_FREE;
    }
    if (unlinked && !legacy && !command && handle)
        shift = !shift;
    if (legacy && !command) {
        if (g.hit.part == PC_TRANSFORM_CORNER)
            shift = !shift;
        if (g.hit.part == PC_TRANSFORM_EDGE)
            shift = true;
    }
    if (mode == PC_TRANSFORM_ROTATE && handle)
        g.hit.part = PC_TRANSFORM_OUTSIDE;
    else if (mode == PC_TRANSFORM_DISTORT && g.hit.part == PC_TRANSFORM_CORNER)
        command = true;
    else if (mode == PC_TRANSFORM_PERSPECTIVE && g.hit.part == PC_TRANSFORM_CORNER)
        command = shift = alt = true;
    else if (mode == PC_TRANSFORM_SKEW && g.hit.part == PC_TRANSFORM_EDGE)
        command = shift = true;
    if (mode == PC_TRANSFORM_SKEW && g.hit.part == PC_TRANSFORM_CORNER) {
        static const size_t horizontal[4] = {1, 0, 3, 2}, vertical[4] = {3, 2, 1, 0};
        size_t i = g.hit.index, partners[2] = {horizontal[i], vertical[i]};
        double delta[2] = {point[0] - g.start[0], point[1] - g.start[1]}, projected[2][2];
        for (size_t a = 0; a < 2; a++) {
            const double *q = g.before.quad[i], *p = g.before.quad[partners[a]];
            double ex = q[0] - p[0], ey = q[1] - p[1], len = hypot(ex, ey);
            double t = len > 0 ? (delta[0] * ex + delta[1] * ey) / (len * len) : 0;
            projected[a][0] = t * ex;
            projected[a][1] = t * ey;
        }
        size_t along =
            hypot(projected[0][0], projected[0][1]) >= hypot(projected[1][0], projected[1][1]) ? 0 : 1;
        for (size_t k = 0; k < 4; k++) {
            bool moved = k == i || (type_layer && k == partners[along]);
            double sign = moved ? 1 : alt && (type_layer || k == (i + 2) % 4) ? -1 : 0;
            for (size_t a = 0; a < 2; a++)
                next.quad[k][a] += sign * projected[along][a];
        }
        keep_convex((const double (*)[2])g.before.quad, next.quad);
        carry((const double (*)[2])g.before.quad, (const double (*)[2])next.quad, g.before.pivot, next.pivot);
    } else
        raw_drag(&next, &g, point, shift, alt, command, previous);
    if (!finite_frame(&next))
        return false;
    pc_algo_homography h;
    if (!pc_algo_homography_rect_to_quad(next.rect, (const double (*)[2])next.quad, &h))
        return false;
    *frame = next;
    return true;
}
static void reference_point(const pc_ui_transform_frame *f, unsigned reference, double point[2])
{
    memcpy(point, f->pivot, sizeof f->pivot);
    pc_algo_homography h;
    if (reference < 9 && pc_algo_homography_rect_to_quad(UNIT, (const double (*)[2])f->quad, &h))
        pc_algo_homography_apply(&h, (double)(reference % 3) / 2, (double)(reference / 3) / 2, &point[0],
                                 &point[1]);
}
void pc_ui_transform_measure(const pc_ui_transform_frame *f, unsigned reference, double values[7])
{
    reference_point(f, reference, values);
    double ex = f->quad[1][0] - f->quad[0][0], ey = f->quad[1][1] - f->quad[0][1],
           fx = f->quad[3][0] - f->quad[0][0], fy = f->quad[3][1] - f->quad[0][1];
    values[2] = hypot(ex, ey);
    values[3] = hypot(fx, fy);
    values[4] = atan2(ey, ex) * 180 / 3.14159265358979323846;
    double skew = atan2(ex * fx + ey * fy, ex * fy - ey * fx) * 180 / 3.14159265358979323846;
    values[5] = remainder(skew, 180);
    values[6] = 0;
}
bool pc_ui_transform_numeric(pc_ui_transform_frame *f, unsigned reference, unsigned field, double value,
                             bool linked)
{
    if (!finite_frame(f) || field > 6 || reference > 9 || !isfinite(value) || fabs(value) > 1000000)
        return false;
    double values[7];
    pc_ui_transform_measure(f, reference, values);
    double a = 1, b = 0, c = 0, d = 1, dx = 0, dy = 0, radians = 3.14159265358979323846 / 180;
    if (field < 2) {
        if (field == 0)
            dx = value - values[0];
        else
            dy = value - values[1];
    } else if (field < 4) {
        if (value < .01 || values[field] < .01)
            return false;
        double scale = value / values[field],
               sx = field == 2 ? scale
                    : linked   ? scale
                               : 1,
               sy = field == 3 ? scale
                    : linked   ? scale
                               : 1;
        double co = cos(values[4] * radians), si = sin(values[4] * radians);
        a = sx * co * co + sy * si * si;
        b = (sx - sy) * co * si;
        c = b;
        d = sx * si * si + sy * co * co;
    } else if (field == 4) {
        double angle = (value - values[4]) * radians;
        a = d = cos(angle);
        c = sin(angle);
        b = -c;
    } else {
        if (fabs(value) > 85)
            return false;
        double shear = tan(value * radians) - (field == 5 ? tan(values[5] * radians) : 0);
        double co = cos(values[4] * radians), si = sin(values[4] * radians);
        if (field == 5) {
            a -= shear * co * si;
            b = shear * co * co;
            c = -shear * si * si;
            d += shear * si * co;
        } else {
            a -= shear * si * co;
            b = -shear * si * si;
            c = shear * co * co;
            d += shear * co * si;
        }
    }
    pc_ui_transform_frame next = *f;
    for (size_t i = 0; i < 5; i++) {
        double *point = i < 4 ? next.quad[i] : next.pivot;
        double x = point[0] - values[0], y = point[1] - values[1];
        point[0] = values[0] + a * x + b * y + dx;
        point[1] = values[1] + c * x + d * y + dy;
    }
    pc_algo_homography h, inverse;
    if (!finite_frame(&next) ||
        !pc_algo_homography_rect_to_quad(next.rect, (const double (*)[2])next.quad, &h) ||
        !pc_algo_homography_inverse(&h, &inverse))
        return false;
    *f = next;
    return true;
}
static yyjson_mut_val *array(yyjson_mut_doc *d, const double *values, size_t count)
{
    yyjson_mut_val *a = yyjson_mut_arr(d);
    for (size_t i = 0; a && i < count; i++)
        if (!yyjson_mut_arr_add_real(d, a, values[i]))
            return NULL;
    return a;
}
pc_status pc_ui_transform_to_json(const pc_ui_transform *t, yyjson_mut_doc *d, yyjson_mut_val **out,
                                  pc_error *err)
{
    if (!t->active) {
        *out = yyjson_mut_null(d);
        return *out ? PC_OK : pc_fail(err, PC_ERR_NOMEM, "transform state");
    }
    yyjson_mut_val *root = yyjson_mut_obj(d), *quad = yyjson_mut_arr(d), *target = NULL;
    for (size_t i = 0; quad && i < 4; i++) {
        yyjson_mut_val *point = array(d, t->frame.quad[i], 2);
        if (!point || !yyjson_mut_arr_append(quad, point))
            quad = NULL;
    }
    if (t->target == 3) {
        target = yyjson_mut_obj(d);
        if (!target || !yyjson_mut_obj_add_uint(d, target, "channel", t->alpha))
            target = NULL;
    } else
        target = t->target ? yyjson_mut_str(d, t->target == 1 ? "mask" : "quickMask") : yyjson_mut_null(d);
    bool ok = root && quad && target && yyjson_mut_obj_add_uint(d, root, "session", t->session) &&
              yyjson_mut_obj_add_uint(d, root, "document", t->document) &&
              yyjson_mut_obj_add_uint(d, root, "revision", t->revision) &&
              yyjson_mut_obj_add_uint(d, root, "layer", t->layer) &&
              yyjson_mut_obj_add_bool(d, root, "selection", t->selection) &&
              yyjson_mut_obj_add_bool(d, root, "typeLayer", t->type_layer) &&
              yyjson_mut_obj_add_bool(d, root, "path", t->path) &&
              yyjson_mut_obj_add_bool(d, root, "copy", t->copy) &&
              yyjson_mut_obj_add_strcpy(d, root, "pathName", t->path_name) &&
              yyjson_mut_obj_add_bool(d, root, "canUndo", t->can_undo) &&
              yyjson_mut_obj_add_bool(d, root, "canRedo", t->can_redo) &&
              yyjson_mut_obj_add_str(d, root, "mode", pc_ui_transform_mode_serde(t->mode)) &&
              yyjson_mut_obj_add_strcpy(d, root, "interpolation", t->interpolation) &&
              yyjson_mut_obj_add_val(d, root, "rect", array(d, t->frame.rect, 4)) &&
              yyjson_mut_obj_add_val(d, root, "quad", quad) &&
              yyjson_mut_obj_add_val(d, root, "pivot", array(d, t->frame.pivot, 2)) &&
              yyjson_mut_obj_add_val(d, root, "target", target);
    if (!ok)
        return pc_fail(err, PC_ERR_NOMEM, "transform state");
    *out = root;
    return PC_OK;
}
static bool read_array(yyjson_val *v, double *out, size_t count)
{
    if (!yyjson_is_arr(v) || yyjson_arr_size(v) != count)
        return false;
    for (size_t i = 0; i < count; i++)
        if (!pc_json_as_f64(yyjson_arr_get(v, i), &out[i]) || !isfinite(out[i]) || fabs(out[i]) > 1000000)
            return false;
    return true;
}
pc_status pc_ui_transform_from_json(pc_ui_transform *t, yyjson_val *v, pc_error *err)
{
    if (!v || yyjson_is_null(v)) {
        *t = (pc_ui_transform){0};
        return PC_OK;
    }
    pc_ui_transform next = {.active = true};
    yyjson_val *quad = pc_json_get(v, "quad");
    bool ok = yyjson_is_obj(v) && read_array(pc_json_get(v, "rect"), next.frame.rect, 4) &&
              read_array(pc_json_get(v, "pivot"), next.frame.pivot, 2) && yyjson_is_arr(quad) &&
              yyjson_arr_size(quad) == 4;
    for (size_t i = 0; ok && i < 4; i++)
        ok = read_array(yyjson_arr_get(quad, i), next.frame.quad[i], 2);
    const char *ids[] = {"session", "document", "revision", "layer"};
    uint64_t *values[] = {&next.session, &next.document, &next.revision, &next.layer};
    for (size_t i = 0; ok && i < 4; i++)
        ok = pc_json_as_u64(pc_json_get(v, ids[i]), values[i]);
    const char *mode = pc_json_as_str(pc_json_get(v, "mode"), NULL);
    if (mode) {
        bool found = false;
        for (pc_ui_transform_mode m = PC_TRANSFORM_FREE; m <= PC_TRANSFORM_PERSPECTIVE; m++)
            if (!strcmp(mode, pc_ui_transform_mode_serde(m))) {
                next.mode = m;
                found = true;
            }
        ok = ok && found;
    } else if (pc_json_get(v, "mode"))
        ok = false;
    const char *interpolation = pc_json_as_str(pc_json_get(v, "interpolation"), NULL);
    if (!interpolation)
        interpolation = "bicubic";
    ok = ok && (!strcmp(interpolation, "bicubic") || !strcmp(interpolation, "bilinear") ||
                !strcmp(interpolation, "nearest"));
    if (ok)
        memcpy(next.interpolation, interpolation, strlen(interpolation) + 1);
    const char *flags[] = {"selection", "typeLayer", "canUndo", "canRedo", "path", "copy"};
    bool *booleans[] = {&next.selection, &next.type_layer, &next.can_undo,
                        &next.can_redo,  &next.path,       &next.copy};
    for (size_t i = 0; i < 6; i++) {
        yyjson_val *flag = pc_json_get(v, flags[i]);
        if (flag && !pc_json_as_bool(flag, booleans[i]))
            ok = false;
    }
    size_t path_length = 0;
    const char *path_name = pc_json_as_str(pc_json_get(v, "pathName"), &path_length);
    if (path_name && path_length < sizeof next.path_name && strlen(path_name) == path_length)
        memcpy(next.path_name, path_name, path_length + 1);
    else if (next.path || pc_json_get(v, "pathName"))
        ok = false;
    if (next.path && !next.path_name[0])
        ok = false;
    yyjson_val *target = pc_json_get(v, "target");
    const char *name = yyjson_get_str(target);
    if (yyjson_is_obj(target)) {
        uint64_t index;
        ok = ok && pc_json_as_u64(pc_json_get(target, "channel"), &index) && index <= SIZE_MAX;
        if (ok) {
            next.target = 3;
            next.alpha = (size_t)index;
        }
    } else if (name) {
        if (!strcmp(name, "mask"))
            next.target = 1;
        else if (!strcmp(name, "quickMask"))
            next.target = 2;
        else if (strcmp(name, "pixels"))
            ok = false;
    } else if (target && !yyjson_is_null(target))
        ok = false;
    pc_algo_homography h;
    if (!ok || !finite_frame(&next.frame) ||
        !pc_algo_homography_rect_to_quad(next.frame.rect, (const double (*)[2])next.frame.quad, &h))
        return pc_fail(err, PC_ERR_INVALID, "invalid transform session");
    *t = next;
    return PC_OK;
}
