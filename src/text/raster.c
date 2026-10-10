/* Type rasteriser (port of crates/text/src/raster.rs; see raster.h). Arithmetic follows the
 * reference expression by expression (f32 vs f64, evaluation order, Rust min/max/clamp and
 * saturating casts) so coverage is bit-identical. */
#include "text/raster.h"

#include <math.h>
#include <stdlib.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

void pc_xform_apply(const pc_xform *t, double x, double y, double *ox, double *oy)
{
    const double *m = t->m;
    *ox = m[0] * x + m[2] * y + m[4];
    *oy = m[1] * x + m[3] * y + m[5];
}

pc_xform pc_xform_mul(const pc_xform *t, const pc_xform *o)
{
    const double *s = t->m, *p = o->m;
    pc_xform r;
    r.m[0] = s[0] * p[0] + s[2] * p[1];
    r.m[1] = s[1] * p[0] + s[3] * p[1];
    r.m[2] = s[0] * p[2] + s[2] * p[3];
    r.m[3] = s[1] * p[2] + s[3] * p[3];
    r.m[4] = s[0] * p[4] + s[2] * p[5] + s[4];
    r.m[5] = s[1] * p[4] + s[3] * p[5] + s[5];
    return r;
}

double pc_xform_max_scale(const pc_xform *t)
{
    const double *m = t->m;
    return pc_rs_max(sqrt(m[0] * m[0] + m[1] * m[1]), sqrt(m[2] * m[2] + m[3] * m[3]));
}

void pc_bounds_add(pc_bounds *b, double x, double y)
{
    if (!(isfinite(x) && isfinite(y))) {
        return;
    }
    if (!b->has) {
        b->has = true;
        b->r[0] = x;
        b->r[1] = y;
        b->r[2] = x;
        b->r[3] = y;
        return;
    }
    b->r[0] = pc_rs_min(b->r[0], x);
    b->r[1] = pc_rs_min(b->r[1], y);
    b->r[2] = pc_rs_max(b->r[2], x);
    b->r[3] = pc_rs_max(b->r[3], y);
}

static void bounds_line(void *ctx, double x0, double y0, double x1, double y1)
{
    pc_bounds_add(ctx, x0, y0);
    pc_bounds_add(ctx, x1, y1);
}

pc_line_sink pc_bounds_sink(pc_bounds *b)
{
    return (pc_line_sink){bounds_line, b};
}

void pc_rect_to(const pc_line_sink *sink, const pc_xform *t, double x0, double y0, double x1, double y1)
{
    double p[4][2];
    int i;
    pc_xform_apply(t, x0, y0, &p[0][0], &p[0][1]);
    pc_xform_apply(t, x1, y0, &p[1][0], &p[1][1]);
    pc_xform_apply(t, x1, y1, &p[2][0], &p[2][1]);
    pc_xform_apply(t, x0, y1, &p[3][0], &p[3][1]);
    for (i = 0; i < 4; i++) {
        sink->line(sink->ctx, p[i][0], p[i][1], p[(i + 1) % 4][0], p[(i + 1) % 4][1]);
    }
}

/* ------------------------------------------------------------------------------------------ */

pc_status pc_coverage_init(pc_coverage *c, size_t width, size_t height, pc_error *err)
{
    size_t n;
    if (!pc_size_mul(width, height, &n) || !pc_size_add(n, 2, &n) || n > PC_MAX_ALLOC / sizeof(float)) {
        return pc_fail(err, PC_ERR_LIMIT, "coverage buffer too large");
    }
    c->acc = pc_calloc_array(n, sizeof(float));
    if (c->acc == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a %zux%zu coverage buffer", width, height);
    }
    c->width = width;
    c->height = height;
    return PC_OK;
}

void pc_coverage_fini(pc_coverage *c)
{
    if (c != NULL) {
        free(c->acc);
        c->acc = NULL;
    }
}

/* f32::clamp: NaN stays NaN. */
static float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        v = lo;
    }
    if (v > hi) {
        v = hi;
    }
    return v;
}

void pc_coverage_line(pc_coverage *c, double p0x, double p0y, double p1x, double p1y)
{
    float w = (float)c->width, h = (float)c->height, dir, x0, y0, x1, y1, dxdy, x, ys, ye, y;
    size_t stride = c->width, row;
    float *acc = c->acc;
    if (p0y == p1y || !(isfinite(p0x) && isfinite(p0y) && isfinite(p1x) && isfinite(p1y))) {
        return;
    }
    if (p0y < p1y) {
        dir = 1.0f;
        x0 = (float)p0x;
        y0 = (float)p0y;
        x1 = (float)p1x;
        y1 = (float)p1y;
    } else {
        dir = -1.0f;
        x0 = (float)p1x;
        y0 = (float)p1y;
        x1 = (float)p0x;
        y1 = (float)p0y;
    }
    if (y1 <= 0.0f || y0 >= h) {
        return;
    }
    dxdy = (x1 - x0) / (y1 - y0);
    x = x0;
    ys = pc_rs_maxf_k(y0, 0.0f);
    if (y0 < 0.0f) {
        x -= y0 * dxdy;
    }
    ye = pc_rs_minf(y1, h);
    y = ys;
    row = pc_f32_to_size(floorf(ys));
    while (y < ye) {
        float ynext = pc_rs_minf((float)(row + 1), ye);
        float dy = ynext - y;
        float xnext = x + dxdy * dy;
        float d = dy * dir;
        float xa = x < xnext ? x : xnext, xb = x < xnext ? xnext : x;
        size_t ls, x0i, x1i;
        float x0f, x1c;
        xa = clampf(xa, 0.0f, w);
        xb = clampf(xb, 0.0f, w);
        ls = row * stride;
        x0f = floorf(xa);
        x0i = pc_f32_to_size(x0f);
        x1c = ceilf(xb);
        x1i = pc_f32_to_size(x1c);
        if (row >= c->height || x0i > c->width || x1i > c->width) {
            break; /* can't happen for finite input (the reference would panic); stay in bounds */
        }
        if (x1i <= x0i + 1) {
            float xmf = 0.5f * (xa + xb) - x0f;
            acc[ls + x0i] += d - d * xmf;
            acc[ls + x0i + 1] += d * xmf;
        } else {
            float s = 1.0f / (xb - xa);
            float x0frac = xa - x0f;
            float a0 = 0.5f * s * (1.0f - x0frac) * (1.0f - x0frac);
            float x1frac = xb - x1c + 1.0f;
            float am = 0.5f * s * x1frac * x1frac;
            acc[ls + x0i] += d * a0;
            if (x1i == x0i + 2) {
                acc[ls + x0i + 1] += d * (1.0f - a0 - am);
            } else {
                float a1 = s * (1.5f - x0frac);
                size_t xi;
                float a2;
                acc[ls + x0i + 1] += d * (a1 - a0);
                for (xi = x0i + 2; xi < x1i - 1; xi++) {
                    acc[ls + xi] += d * s;
                }
                a2 = a1 + (float)(x1i - x0i - 3) * s;
                acc[ls + x1i - 1] += d * (1.0f - a2 - am);
            }
            acc[ls + x1i] += d * am;
        }
        x = xnext;
        y = ynext;
        row++;
    }
}

static void coverage_line(void *ctx, double x0, double y0, double x1, double y1)
{
    pc_coverage_line(ctx, x0, y0, x1, y1);
}

pc_line_sink pc_coverage_sink(pc_coverage *c)
{
    return (pc_line_sink){coverage_line, c};
}

void pc_coverage_finish(const pc_coverage *c, float *out)
{
    size_t n = c->width * c->height, i;
    float sum = 0.0f;
    for (i = 0; i < n; i++) {
        sum += c->acc[i];
        out[i] = pc_rs_minf_k(fabsf(sum), 1.0f);
    }
}

/* ------------------------------------------------------------------------------------------ */

void pc_flatten_pen_init(pc_flatten_pen *p, const pc_line_sink *sink, pc_xform xf)
{
    p->sink = sink;
    p->xf = xf;
    p->sx = p->sy = p->lx = p->ly = 0.0;
    p->tol = 0.03;
}

static void pen_to(pc_flatten_pen *p, double x, double y)
{
    p->sink->line(p->sink->ctx, p->lx, p->ly, x, y);
    p->lx = x;
    p->ly = y;
}

static size_t segments(const pc_flatten_pen *p, double ax, double ay, double bx, double by, double cx,
                       double cy)
{
    /* Deviation of the control polygon from the chord bounds the flattening error. */
    double ex = ax - 2.0 * bx + cx, ey = ay - 2.0 * by + cy;
    double dd = sqrt(ex * ex + ey * ey);
    size_t n = pc_f64_to_size(ceil(sqrt(0.75 * dd / p->tol)));
    return n < 1 ? 1 : n > 100 ? 100 : n;
}

void pc_flatten_pen_close(pc_flatten_pen *p)
{
    if (p->lx != p->sx || p->ly != p->sy) {
        pen_to(p, p->sx, p->sy);
    }
}

void pc_flatten_pen_move_to(pc_flatten_pen *p, float x, float y)
{
    double px, py;
    pc_flatten_pen_close(p);
    pc_xform_apply(&p->xf, (double)x, (double)y, &px, &py);
    p->sx = p->lx = px;
    p->sy = p->ly = py;
}

void pc_flatten_pen_line_to(pc_flatten_pen *p, float x, float y)
{
    double px, py;
    pc_xform_apply(&p->xf, (double)x, (double)y, &px, &py);
    pen_to(p, px, py);
}

void pc_flatten_pen_quad_to(pc_flatten_pen *p, float cx0, float cy0, float x, float y)
{
    double ax = p->lx, ay = p->ly, bx, by, cx, cy;
    size_t n, i;
    pc_xform_apply(&p->xf, (double)cx0, (double)cy0, &bx, &by);
    pc_xform_apply(&p->xf, (double)x, (double)y, &cx, &cy);
    n = segments(p, ax, ay, bx, by, cx, cy);
    for (i = 1; i <= n; i++) {
        double t = (double)i / (double)n, mt = 1.0 - t;
        pen_to(p, mt * mt * ax + 2.0 * mt * t * bx + t * t * cx,
               mt * mt * ay + 2.0 * mt * t * by + t * t * cy);
    }
}

void pc_flatten_pen_curve_to(pc_flatten_pen *p, float c0x, float c0y, float c1x, float c1y, float x, float y)
{
    double ax = p->lx, ay = p->ly, bx, by, cx, cy, dx, dy;
    size_t n, n2, i;
    pc_xform_apply(&p->xf, (double)c0x, (double)c0y, &bx, &by);
    pc_xform_apply(&p->xf, (double)c1x, (double)c1y, &cx, &cy);
    pc_xform_apply(&p->xf, (double)x, (double)y, &dx, &dy);
    n = segments(p, ax, ay, bx, by, cx, cy);
    n2 = segments(p, bx, by, cx, cy, dx, dy);
    if (n2 > n) {
        n = n2;
    }
    for (i = 1; i <= n; i++) {
        double t = (double)i / (double)n, mt = 1.0 - t;
        double k0 = mt * mt * mt, k1 = 3.0 * mt * mt * t, k2 = 3.0 * mt * t * t, k3 = t * t * t;
        pen_to(p, k0 * ax + k1 * bx + k2 * cx + k3 * dx, k0 * ay + k1 * by + k2 * cy + k3 * dy);
    }
}

static void op_move(void *c, float x, float y)
{
    pc_flatten_pen_move_to(c, x, y);
}
static void op_line(void *c, float x, float y)
{
    pc_flatten_pen_line_to(c, x, y);
}
static void op_quad(void *c, float a, float b, float x, float y)
{
    pc_flatten_pen_quad_to(c, a, b, x, y);
}
static void op_curve(void *c, float a, float b, float d, float e, float x, float y)
{
    pc_flatten_pen_curve_to(c, a, b, d, e, x, y);
}
static void op_close(void *c)
{
    pc_flatten_pen_close(c);
}

pc_outline_pen pc_flatten_pen_outline(pc_flatten_pen *p)
{
    return (pc_outline_pen){op_move, op_line, op_quad, op_curve, op_close, p};
}
