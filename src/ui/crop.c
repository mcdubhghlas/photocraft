#include "crop.h"
#include <math.h>
#include <string.h>
static const double PI = 3.14159265358979323846;
bool pc_ui_crop_valid(const pc_ui_crop_frame *f)
{
    if (!f || !f->present || !isfinite(f->angle) || fabs(f->angle) > 3600)
        return false;
    for (size_t i = 0; i < 4; i++)
        if (!isfinite(f->rect[i]) || fabs(f->rect[i]) > 1000000)
            return false;
    return f->rect[2] - f->rect[0] >= 1 && f->rect[3] - f->rect[1] >= 1;
}
void pc_ui_crop_quad(const pc_ui_crop_frame *f, pc_ui_transform_frame *out)
{
    pc_ui_transform_frame_rect(out, f->rect);
    double co = cos(f->angle * PI / 180), si = sin(f->angle * PI / 180);
    for (size_t i = 0; i < 4; i++) {
        double x = out->quad[i][0] - out->pivot[0], y = out->quad[i][1] - out->pivot[1];
        out->quad[i][0] = out->pivot[0] + co * x - si * y;
        out->quad[i][1] = out->pivot[1] + si * x + co * y;
    }
}
static bool point_valid(const double point[2])
{
    return point && isfinite(point[0]) && isfinite(point[1]) && fabs(point[0]) <= 1000000 &&
           fabs(point[1]) <= 1000000;
}
bool pc_ui_crop_begin(pc_ui_crop_drag *d, const pc_ui_crop_frame *f, const double point[2], double tolerance,
                      bool default_frame, bool classic)
{
    if (!d || !pc_ui_crop_valid(f) || !point_valid(point) || !isfinite(tolerance) || tolerance <= 0)
        return false;
    pc_ui_transform_frame transform;
    pc_ui_crop_quad(f, &transform);
    pc_ui_transform_hit hit = pc_ui_transform_hit_test(&transform, point, tolerance);
    if (hit.part == PC_TRANSFORM_PIVOT)
        hit.part = PC_TRANSFORM_INSIDE;
    *d = (pc_ui_crop_drag){.active = true,
                           .before = *f,
                           .draw = default_frame && hit.part == PC_TRANSFORM_INSIDE,
                           .image = !classic &&
                                    (hit.part == PC_TRANSFORM_INSIDE || hit.part == PC_TRANSFORM_OUTSIDE),
                           .transform = {.hit = hit, .before = transform}};
    memcpy(d->anchor, point, sizeof d->anchor);
    memcpy(d->last, point, sizeof d->last);
    memcpy(d->transform.start, point, sizeof d->transform.start);
    if (d->draw)
        d->image = false;
    return true;
}
static bool from_quad(const pc_ui_transform_frame *q, pc_ui_crop_frame *f)
{
    double cx = 0, cy = 0;
    for (size_t i = 0; i < 4; i++) {
        cx += q->quad[i][0] / 4;
        cy += q->quad[i][1] / 4;
    }
    double w = hypot(q->quad[1][0] - q->quad[0][0], q->quad[1][1] - q->quad[0][1]),
           h = hypot(q->quad[3][0] - q->quad[0][0], q->quad[3][1] - q->quad[0][1]);
    pc_ui_crop_frame next = {.present = true,
                             .rect = {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2},
                             .angle = atan2(q->quad[1][1] - q->quad[0][1], q->quad[1][0] - q->quad[0][0]) *
                                      180 / PI};
    if (!pc_ui_crop_valid(&next))
        return false;
    *f = next;
    return true;
}
static bool motion_impl(pc_ui_crop_frame *f, pc_ui_crop_drag *d, const double point[2], double ratio,
                        bool shift, bool alt, bool space)
{
    if (!f || !d || !d->active || !point_valid(point) || !isfinite(ratio) || ratio < 0 || ratio > 1000000)
        return false;
    if (d->draw) {
        if (space && d->drawn) {
            double dx = point[0] - d->last[0], dy = point[1] - d->last[1];
            pc_ui_crop_frame next = *f;
            for (size_t i = 0; i < 4; i++)
                next.rect[i] += i % 2 ? dy : dx;
            if (!pc_ui_crop_valid(&next))
                return false;
            d->anchor[0] += dx;
            d->anchor[1] += dy;
            *f = next;
        } else {
            double dx = point[0] - d->anchor[0], dy = point[1] - d->anchor[1], w = fabs(dx), h = fabs(dy),
                   r = ratio > 0 ? ratio
                       : shift   ? 1
                                 : 0;
            if (r > 0) {
                if (w > h * r)
                    h = w / r;
                else
                    w = h * r;
            }
            if (alt) {
                w *= 2;
                h *= 2;
            }
            double x = alt ? d->anchor[0] - w / 2 : d->anchor[0] - (dx < 0 ? w : 0),
                   y = alt ? d->anchor[1] - h / 2 : d->anchor[1] - (dy < 0 ? h : 0);
            pc_ui_crop_frame next = {.present = true, .rect = {x, y, x + w, y + h}};
            if (!pc_ui_crop_valid(&next))
                return false;
            *f = next;
            d->drawn = true;
        }
    } else {
        pc_ui_transform_frame q;
        pc_ui_crop_quad(f, &q);
        double p[2] = {point[0], point[1]};
        if (d->transform.hit.part == PC_TRANSFORM_CORNER || d->transform.hit.part == PC_TRANSFORM_EDGE) {
            size_t i = d->transform.hit.index % 4;
            for (size_t axis = 0; axis < 2; axis++) {
                double handle =
                    d->transform.hit.part == PC_TRANSFORM_CORNER
                        ? d->transform.before.quad[i][axis]
                        : (d->transform.before.quad[i][axis] + d->transform.before.quad[(i + 1) % 4][axis]) /
                              2;
                p[axis] += handle - d->transform.start[axis];
            }
        }
        bool outside = d->transform.hit.part == PC_TRANSFORM_OUTSIDE;
        if (d->image) {
            if (outside) {
                const double *center = d->transform.before.pivot;
                double start = atan2(d->transform.start[1] - center[1], d->transform.start[0] - center[0]);
                double angle = 2 * start - atan2(p[1] - center[1], p[0] - center[0]),
                       radius = hypot(p[0] - center[0], p[1] - center[1]);
                p[0] = center[0] + cos(angle) * radius;
                p[1] = center[1] + sin(angle) * radius;
            } else {
                p[0] = 2 * d->transform.start[0] - p[0];
                p[1] = 2 * d->transform.start[1] - p[1];
            }
        }
        bool constrained = ratio > 0 || shift;
        if (!pc_ui_transform_apply(&q, &d->transform, p, PC_TRANSFORM_FREE, false,
                                   d->transform.hit.part == PC_TRANSFORM_CORNER ||
                                           d->transform.hit.part == PC_TRANSFORM_EDGE
                                       ? !constrained
                                       : shift,
                                   alt, false, false, false) ||
            !from_quad(&q, f))
            return false;
        if (ratio > 0 &&
            (d->transform.hit.part == PC_TRANSFORM_CORNER || d->transform.hit.part == PC_TRANSFORM_EDGE)) {
            pc_ui_crop_quad(f, &q);
            const unsigned opposite_corner[] = {8, 6, 0, 2}, opposite_edge[] = {7, 3, 1, 5};
            bool corner = d->transform.hit.part == PC_TRANSFORM_CORNER;
            unsigned reference = alt      ? 4
                                 : corner ? opposite_corner[d->transform.hit.index]
                                          : opposite_edge[d->transform.hit.index];
            double width = f->rect[2] - f->rect[0], height = f->rect[3] - f->rect[1];
            unsigned field = !corner && !(d->transform.hit.index & 1u) ? 2 : 3;
            double value = field == 2 ? height * ratio : width / ratio;
            if (!pc_ui_transform_numeric(&q, reference, field, value, false) || !from_quad(&q, f))
                return false;
        }
    }
    memcpy(d->last, point, sizeof d->last);
    return true;
}
bool pc_ui_crop_motion(pc_ui_crop_frame *frame, pc_ui_crop_drag *drag, const double point[2], double ratio,
                       bool shift, bool alt, bool space)
{
    if (!frame || !drag || !pc_ui_crop_valid(frame) || !pc_ui_crop_valid(&drag->before))
        return false;
    pc_ui_crop_frame next = *frame;
    pc_ui_crop_drag next_drag = *drag;
    if (!motion_impl(&next, &next_drag, point, ratio, shift, alt, space))
        return false;
    *frame = next;
    *drag = next_drag;
    return true;
}
bool pc_ui_crop_swap(pc_ui_crop_frame *f)
{
    if (!pc_ui_crop_valid(f))
        return false;
    double cx = (f->rect[0] + f->rect[2]) / 2, cy = (f->rect[1] + f->rect[3]) / 2,
           w = f->rect[2] - f->rect[0], h = f->rect[3] - f->rect[1];
    pc_ui_crop_frame next = {
        .present = true, .angle = f->angle, .rect = {cx - h / 2, cy - w / 2, cx + h / 2, cy + w / 2}};
    if (!pc_ui_crop_valid(&next))
        return false;
    *f = next;
    return true;
}
