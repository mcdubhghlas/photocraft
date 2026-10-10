/* Parametric ("live") shapes -> paths (port of crates/vector/src/shapes.rs): rectangles with
 * corner radii, ellipses, polygons, stars and lines. Knots run clockwise on screen (y down),
 * starting at the top. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "vector/internal.h"

/* One closed or open subpath from n knots (copied) as a whole path. */
static pc_status path_of_knots(const pc_knot *knots, size_t n, bool closed, pc_path *out, pc_error *err)
{
    pc_subpath *sp = pc_calloc_array(1, sizeof *sp);
    if (sp == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    if (n > 0) {
        sp->knots = pc_malloc_array(n, sizeof *sp->knots);
        if (sp->knots == NULL) {
            free(sp);
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        memcpy(sp->knots, knots, n * sizeof *knots);
    }
    sp->nknots = n;
    sp->closed = closed;
    sp->op = PC_PATH_COMBINE;
    pc_vec_path_from(sp, 1, out);
    return PC_OK;
}

static pc_status polygon_path(const double (*pts)[2], size_t n, pc_path *out, pc_error *err)
{
    pc_knot k[200];
    size_t i;
    for (i = 0; i < n; i++) {
        k[i] = pc_knot_corner(pts[i][0], pts[i][1]);
    }
    return path_of_knots(k, n, true, out, err);
}

pc_status pc_vec_shape_rect(double x, double y, double w, double h, pc_path *out, pc_error *err)
{
    double pts[4][2];
    pts[0][0] = x;
    pts[0][1] = y;
    pts[1][0] = x + w;
    pts[1][1] = y;
    pts[2][0] = x + w;
    pts[2][1] = y + h;
    pts[3][0] = x;
    pts[3][1] = y + h;
    return polygon_path((const double (*)[2])pts, 4, out, err);
}

void pc_vec_clamp_radii(double w, double h, const double radii[4], double out[4])
{
    double r[4];
    double f = 1.0;
    double side[4];
    int i;
    for (i = 0; i < 4; i++) {
        r[i] = pc_rs_max_k(radii[i], 0.0);
    }
    side[0] = w;
    side[1] = h;
    side[2] = w;
    side[3] = h;
    for (i = 0; i < 4; i++) {
        double a = r[i];
        double b = r[(i + 1) % 4];
        if (a + b > side[i] && a + b > 0.0) {
            f = pc_rs_min(f, side[i] / (a + b));
        }
    }
    for (i = 0; i < 4; i++) {
        out[i] = r[i] * f;
    }
}

static pc_knot knot3(double ax, double ay, double ix, double iy, double ox, double oy, bool smooth)
{
    pc_knot k;
    k.anchor = pc_point_make(ax, ay);
    k.in_ctrl = pc_point_make(ix, iy);
    k.out_ctrl = pc_point_make(ox, oy);
    k.smooth = smooth;
    return k;
}

/* Corner arc from the end of one edge to the start of the next (clockwise). */
static void push_arc(pc_knot *k, size_t *n, const double start[2], const double end[2], const double cs[2],
                     const double ce[2], double r)
{
    if (r <= 0.0) {
        k[(*n)++] = pc_knot_corner(start[0], start[1]);
        return;
    }
    k[(*n)++] = knot3(start[0], start[1], start[0], start[1], cs[0], cs[1], false);
    k[(*n)++] = knot3(end[0], end[1], ce[0], ce[1], end[0], end[1], false);
}

pc_status pc_vec_shape_rounded_rect(double x, double y, double w, double h, const double radii[4],
                                    pc_path *out, pc_error *err)
{
    double cr[4];
    double tl;
    double tr;
    double br;
    double bl;
    double x1;
    double y1;
    pc_knot k[8];
    pc_knot rot[8];
    pc_knot o[8];
    size_t n = 0;
    size_t no = 0;
    size_t i;
    w = fabs(w);
    h = fabs(h);
    pc_vec_clamp_radii(w, h, radii, cr);
    tl = cr[0];
    tr = cr[1];
    br = cr[2];
    bl = cr[3];
    if (tl <= 0.0 && tr <= 0.0 && br <= 0.0 && bl <= 0.0) {
        return pc_vec_shape_rect(x, y, w, h, out, err);
    }
    x1 = x + w;
    y1 = y + h;
    {
        const double s0[2] = {x1 - tr, y};
        const double e0[2] = {x1, y + tr};
        const double c0[2] = {x1 - tr + PC_VEC_KAPPA * tr, y};
        const double d0[2] = {x1, y + tr - PC_VEC_KAPPA * tr};
        const double s1[2] = {x1, y1 - br};
        const double e1[2] = {x1 - br, y1};
        const double c1[2] = {x1, y1 - br + PC_VEC_KAPPA * br};
        const double d1[2] = {x1 - br + PC_VEC_KAPPA * br, y1};
        const double s2[2] = {x + bl, y1};
        const double e2[2] = {x, y1 - bl};
        const double c2[2] = {x + bl - PC_VEC_KAPPA * bl, y1};
        const double d2[2] = {x, y1 - bl + PC_VEC_KAPPA * bl};
        const double s3[2] = {x, y + tl};
        const double e3[2] = {x + tl, y};
        const double c3[2] = {x, y + tl - PC_VEC_KAPPA * tl};
        const double d3[2] = {x + tl - PC_VEC_KAPPA * tl, y};
        push_arc(k, &n, s0, e0, c0, d0, tr);
        push_arc(k, &n, s1, e1, c1, d1, br);
        push_arc(k, &n, s2, e2, c2, d2, bl);
        push_arc(k, &n, s3, e3, c3, d3, tl);
    }
    /* Start at the top-left arc end so the first knot is on the top edge (rotate_left(n-1)). */
    for (i = 0; i < n; i++) {
        rot[i] = k[(i + n - 1) % n];
    }
    /* Drop duplicated consecutive anchors (zero-length straight edges). */
    for (i = 0; i < n; i++) {
        if (no > 0 && fabs(o[no - 1].anchor.x - rot[i].anchor.x) < 1e-12 &&
            fabs(o[no - 1].anchor.y - rot[i].anchor.y) < 1e-12) {
            o[no - 1].out_ctrl = rot[i].out_ctrl;
            continue;
        }
        o[no++] = rot[i];
    }
    if (no > 1) {
        pc_knot f = o[0];
        pc_knot l = o[no - 1];
        if (fabs(f.anchor.x - l.anchor.x) < 1e-12 && fabs(f.anchor.y - l.anchor.y) < 1e-12) {
            o[0].in_ctrl = l.in_ctrl;
            no--;
        }
    }
    return path_of_knots(o, no, true, out, err);
}

pc_status pc_vec_shape_ellipse(double x, double y, double w, double h, pc_path *out, pc_error *err)
{
    double rx = w / 2.0;
    double ry = h / 2.0;
    double cx = x + rx;
    double cy = y + ry;
    double kx = rx * PC_VEC_KAPPA;
    double ky = ry * PC_VEC_KAPPA;
    pc_knot k[4];
    k[0] = knot3(cx, cy - ry, cx - kx, cy - ry, cx + kx, cy - ry, true);
    k[1] = knot3(cx + rx, cy, cx + rx, cy - ky, cx + rx, cy + ky, true);
    k[2] = knot3(cx, cy + ry, cx + kx, cy + ry, cx - kx, cy + ry, true);
    k[3] = knot3(cx - rx, cy, cx - rx, cy + ky, cx - rx, cy - ky, true);
    return path_of_knots(k, 4, true, out, err);
}

pc_status pc_vec_shape_polygon(double x, double y, double w, double h, uint32_t sides, double star_ratio,
                               pc_path *out, pc_error *err)
{
    size_t ns = sides < 3 ? 3 : sides > 100 ? 100 : sides;
    double rx = w / 2.0;
    double ry = h / 2.0;
    double cx = x + rx;
    double cy = y + ry;
    bool star = star_ratio > 0.0 && star_ratio < 1.0;
    size_t count = star ? ns * 2 : ns;
    double pts[200][2];
    size_t i;
    for (i = 0; i < count; i++) {
        double a = -PC_VEC_FRAC_PI_2 + PC_VEC_TAU * (double)i / (double)count;
        double k = star && i % 2 == 1 ? star_ratio : 1.0;
        pts[i][0] = cx + rx * k * cos(a);
        pts[i][1] = cy + ry * k * sin(a);
    }
    return polygon_path((const double (*)[2])pts, count, out, err);
}

pc_status pc_vec_shape_line(const double a[2], const double b[2], double weight, pc_path *out, pc_error *err)
{
    double dx = b[0] - a[0];
    double dy = b[1] - a[1];
    double l = hypot(dx, dy);
    double hw = pc_rs_max_k(weight, 0.0) / 2.0;
    double nx;
    double ny;
    double pts[4][2];
    if (l > 0.0) {
        nx = -dy / l * hw;
        ny = dx / l * hw;
    } else {
        nx = 0.0;
        ny = hw;
    }
    pts[0][0] = a[0] + nx;
    pts[0][1] = a[1] + ny;
    pts[1][0] = b[0] + nx;
    pts[1][1] = b[1] + ny;
    pts[2][0] = b[0] - nx;
    pts[2][1] = b[1] - ny;
    pts[3][0] = a[0] - nx;
    pts[3][1] = a[1] - ny;
    return polygon_path((const double (*)[2])pts, 4, out, err);
}

pc_status pc_vec_live_path(const pc_live_shape *s, pc_path *out, pc_error *err)
{
    switch (s->kind) {
    case PC_LIVE_ELLIPSE:
        return pc_vec_shape_ellipse(s->rect[0], s->rect[1], s->rect[2], s->rect[3], out, err);
    case PC_LIVE_POLYGON:
        return pc_vec_shape_polygon(s->rect[0], s->rect[1], s->rect[2], s->rect[3], s->sides, s->star_ratio,
                                    out, err);
    case PC_LIVE_LINE:
        return pc_vec_shape_line(s->from, s->to, s->weight, out, err);
    case PC_LIVE_RECT:
    default:
        return pc_vec_shape_rounded_rect(s->rect[0], s->rect[1], s->rect[2], s->rect[3], s->radii, out, err);
    }
}
