/* Parametric shapes (port of crates/affinity/src/shapes.rs): Affinity stores a shape's parameters
 * and its box, never the outline. Formulas were fitted to Affinity's own renders. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "affinity/internal.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Handle length of a quarter circle of radius 1. */
#define KAPPA 0.5522847498307934
#define MAX_SIDES 10000.0
#define PI_ 3.141592653589793
#define FRAC_PI_2_ 1.5707963267948966
#define TAU_ 6.283185307179586

/* The last value of a tag: a shape changed from one kind to another keeps a stale first block. */
static const pc_af_value *last(const ax_reader *r, size_t id, const char *tag)
{
    const pc_af_object *o = ax_object(r->s, id);
    pc_af_tag t = PC_AF_TAG(tag);
    size_t i;
    if (o == NULL) {
        return NULL;
    }
    for (i = o->nfields; i > 0; i--) {
        if (o->fields[i - 1].tag == t) {
            return &o->fields[i - 1].v;
        }
    }
    return NULL;
}

static bool num(const ax_reader *r, size_t id, const char *tag, double *out)
{
    const pc_af_value *v = last(r, id, tag);
    if (v == NULL) {
        return false;
    }
    if (v->kind == PC_AF_FLOAT && isfinite(v->as.f)) {
        *out = v->as.f;
        return true;
    }
    if (v->kind == PC_AF_UINT) {
        *out = (double)v->as.u;
        return true;
    }
    if (v->kind == PC_AF_INT) {
        *out = (double)v->as.i;
        return true;
    }
    return false;
}

static double num_or(const ax_reader *r, size_t id, const char *tag, double d)
{
    double v;
    return num(r, id, tag, &v) ? v : d;
}

static size_t sides(const ax_reader *r, size_t id, const char *tag, double d)
{
    double n = round(num_or(r, id, tag, d));
    return pc_f64_to_size(pc_clampd(n, 3.0, MAX_SIDES));
}

static pc_af_point pt(double x, double y)
{
    pc_af_point p;
    p.x = x;
    p.y = y;
    return p;
}

/* A growable segment list. */
typedef struct segs {
    pc_af_point (*v)[3];
    size_t n;
    size_t cap;
    bool nomem;
} segs;

static void seg_push(segs *s, pc_af_point a, pc_af_point b, pc_af_point c)
{
    if (s->n == s->cap) {
        size_t cap = s->cap > 0 ? s->cap * 2 : 8;
        pc_af_point(*v)[3] = realloc(s->v, cap * sizeof *v);
        if (v == NULL) {
            s->nomem = true;
            return;
        }
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n][0] = a;
    s->v[s->n][1] = b;
    s->v[s->n][2] = c;
    s->n++;
}

static pc_status finish(segs *s, pc_af_point start, bool closed, pc_af_subpath *out, pc_error *err)
{
    if (s->nomem) {
        free(s->v);
        return ax_nomem(err);
    }
    out->start = start;
    out->segments = s->v;
    out->nsegments = s->n;
    out->closed = closed;
    return PC_OK;
}

pc_status ax_rectangle(double x0, double y0, double x1, double y1, pc_af_subpath *out, pc_error *err)
{
    segs s = {NULL, 0, 0, false};
    seg_push(&s, pt(x1, y0), pt(x1, y0), pt(x1, y0));
    seg_push(&s, pt(x1, y1), pt(x1, y1), pt(x1, y1));
    seg_push(&s, pt(x0, y1), pt(x0, y1), pt(x0, y1));
    return finish(&s, pt(x0, y0), true, out, err);
}

static pc_status ellipse(double cx, double cy, double rx, double ry, pc_af_subpath *out, pc_error *err)
{
    double kx = KAPPA * rx;
    double ky = KAPPA * ry;
    segs s = {NULL, 0, 0, false};
    seg_push(&s, pt(cx + rx, cy + ky), pt(cx + kx, cy + ry), pt(cx, cy + ry));
    seg_push(&s, pt(cx - kx, cy + ry), pt(cx - rx, cy + ky), pt(cx - rx, cy));
    seg_push(&s, pt(cx - rx, cy - ky), pt(cx - kx, cy - ry), pt(cx, cy - ry));
    seg_push(&s, pt(cx + kx, cy - ry), pt(cx + rx, cy - ky), pt(cx + rx, cy));
    return finish(&s, pt(cx + rx, cy), true, out, err);
}

static pc_status rounded_rect(const double b[4], double rx, double ry, pc_af_subpath *out, pc_error *err)
{
    double x0 = b[0], y0 = b[1], x1 = b[2], y1 = b[3];
    double kx = (1.0 - KAPPA) * rx;
    double ky = (1.0 - KAPPA) * ry;
    segs s = {NULL, 0, 0, false};
    seg_push(&s, pt(x1 - rx, y0), pt(x1 - rx, y0), pt(x1 - rx, y0));
    seg_push(&s, pt(x1 - kx, y0), pt(x1, y0 + ky), pt(x1, y0 + ry));
    seg_push(&s, pt(x1, y1 - ry), pt(x1, y1 - ry), pt(x1, y1 - ry));
    seg_push(&s, pt(x1, y1 - ky), pt(x1 - kx, y1), pt(x1 - rx, y1));
    seg_push(&s, pt(x0 + rx, y1), pt(x0 + rx, y1), pt(x0 + rx, y1));
    seg_push(&s, pt(x0 + kx, y1), pt(x0, y1 - ky), pt(x0, y1 - ry));
    seg_push(&s, pt(x0, y0 + ry), pt(x0, y0 + ry), pt(x0, y0 + ry));
    seg_push(&s, pt(x0, y0 + ky), pt(x0 + kx, y0), pt(x0 + rx, y0));
    return finish(&s, pt(x0 + rx, y0), true, out, err);
}

/* Elliptical arc from a0 to a1 (radians, y-up), as cubic segments of at most a quarter turn. */
static void arc(double cx, double cy, double rx, double ry, double a0, double a1, segs *out)
{
    size_t n = pc_f64_to_size(pc_clampd(ceil(fabs(a1 - a0) / FRAC_PI_2_), 1.0, 8.0));
    double step = (a1 - a0) / (double)n;
    double k = 4.0 / 3.0 * tan(step / 4.0);
    size_t i;
    for (i = 0; i < n; i++) {
        double s = a0 + step * (double)i;
        double e = a0 + step * (double)(i + 1);
        pc_af_point ps = pt(cx + rx * cos(s), cy - ry * sin(s));
        pc_af_point pe = pt(cx + rx * cos(e), cy - ry * sin(e));
        pc_af_point ds = pt(-rx * sin(s), -ry * cos(s));
        pc_af_point de = pt(-rx * sin(e), -ry * cos(e));
        seg_push(out, pt(ps.x + k * ds.x, ps.y + k * ds.y), pt(pe.x - k * de.x, pe.y - k * de.y), pe);
    }
}

static pc_status pie(double cx, double cy, double rx, double ry, double a0, double a1, double inner,
                     pc_af_subpath *out, pc_error *err)
{
    segs s = {NULL, 0, 0, false};
    pc_af_point start = pt(cx + rx * cos(a0), cy - ry * sin(a0));
    arc(cx, cy, rx, ry, a0, a1, &s);
    if (inner > 0.0) {
        pc_af_point p = pt(cx + inner * rx * cos(a1), cy - inner * ry * sin(a1));
        seg_push(&s, p, p, p);
        arc(cx, cy, inner * rx, inner * ry, a1, a0, &s);
    } else {
        pc_af_point c = pt(cx, cy);
        seg_push(&s, c, c, c);
    }
    return finish(&s, start, true, out, err);
}

/* A closed polygon through pts (n >= 1). */
static pc_status polygon(const pc_af_point *pts, size_t n, pc_af_subpath *out, pc_error *err)
{
    segs s = {NULL, 0, 0, false};
    size_t i;
    for (i = 1; i < n; i++) {
        seg_push(&s, pts[i], pts[i], pts[i]);
    }
    return finish(&s, pts[0], true, out, err);
}

/* ---- special shapes (clouds, hearts, cogs, callouts, arrows, double stars, tears, crescents,
 * circular segments) ---- */

static double clamp_num(const ax_reader *r, size_t id, const char *tag, double d, double lo, double hi)
{
    return pc_clampd(num_or(r, id, tag, d), lo, hi);
}

static pc_status cloud(double cx, double cy, double rx, double ry, size_t bubbles, pc_af_subpath *out,
                       pc_error *err)
{
    size_t n = (bubbles < 3 ? 3 : (bubbles > 128 ? 128 : bubbles)) * 4;
    pc_af_point pts[512];
    segs s = {NULL, 0, 0, false};
    size_t i;
    for (i = 0; i < n; i++) {
        double a = TAU_ * (double)i / (double)n - FRAC_PI_2_;
        double l = 0.5 - 0.5 * cos(TAU_ * (double)i / 4.0);
        double lobe = l * l;
        double radius = 0.78 + 0.22 * lobe;
        pts[i] = pt(cx + rx * radius * cos(a), cy + ry * radius * sin(a));
    }
    for (i = 0; i < n; i++) {
        pc_af_point p0 = pts[(i + n - 1) % n];
        pc_af_point p1 = pts[i];
        pc_af_point p2 = pts[(i + 1) % n];
        pc_af_point p3 = pts[(i + 2) % n];
        seg_push(&s, pt(p1.x + (p2.x - p0.x) / 6.0, p1.y + (p2.y - p0.y) / 6.0),
                 pt(p2.x - (p3.x - p1.x) / 6.0, p2.y - (p3.y - p1.y) / 6.0), p2);
    }
    return finish(&s, pts[0], true, out, err);
}

static pc_af_point heart_pt(const double b[4], double x, double y)
{
    return pt(b[0] + x * (b[2] - b[0]), b[1] + y * (b[3] - b[1]));
}

static pc_status heart(const double b[4], pc_af_subpath *out, pc_error *err)
{
    pc_af_point start = heart_pt(b, 0.5, 0.92);
    segs s = {NULL, 0, 0, false};
    seg_push(&s, heart_pt(b, 0.38, 0.85), heart_pt(b, 0.04, 0.67), heart_pt(b, 0.04, 0.37));
    seg_push(&s, heart_pt(b, 0.04, 0.02), heart_pt(b, 0.39, 0.00), heart_pt(b, 0.5, 0.27));
    seg_push(&s, heart_pt(b, 0.61, 0.00), heart_pt(b, 0.96, 0.02), heart_pt(b, 0.96, 0.37));
    seg_push(&s, heart_pt(b, 0.96, 0.67), heart_pt(b, 0.62, 0.85), start);
    return finish(&s, start, true, out, err);
}

static pc_status cog(const ax_reader *r, size_t shape, double cx, double cy, double rx, double ry,
                     pc_af_subpath *out, pc_error *err)
{
    static const double OFFSET[4] = {-0.5, -0.32, 0.32, 0.5};
    size_t teeth = sides(r, shape, "Teth", 12.0);
    double tooth = clamp_num(r, shape, "TtSz", 0.3, 0.05, 0.9);
    double notch = clamp_num(r, shape, "NtSz", 0.45, 0.05, 0.95);
    double tip = 1.0;
    double root = pc_clampd(1.0 - 0.2 * tooth - 0.08 * notch, 0.55, 0.95);
    pc_af_point *pts = pc_malloc_array(teeth * 4, sizeof *pts);
    pc_status st;
    size_t i;
    int k;
    if (pts == NULL) {
        return ax_nomem(err);
    }
    for (i = 0; i < teeth; i++) {
        double base = TAU_ * (double)i / (double)teeth - FRAC_PI_2_;
        for (k = 0; k < 4; k++) {
            double radius = k == 0 || k == 3 ? root : tip;
            double a = base + OFFSET[k] * TAU_ / (double)teeth;
            pts[4 * i + (size_t)k] = pt(cx + rx * radius * cos(a), cy + ry * radius * sin(a));
        }
    }
    st = polygon(pts, teeth * 4, out, err);
    free(pts);
    return st;
}

static pc_status callout_rect(const ax_reader *r, size_t shape, const double b[4], pc_af_subpath *out,
                              pc_error *err)
{
    double x0 = b[0], y0 = b[1], x1 = b[2], y1 = b[3];
    double w = x1 - x0;
    double h = y1 - y0;
    double tail_h = clamp_num(r, shape, "TlHg", 0.2, 0.0, 0.8) * h;
    double tail_w = clamp_num(r, shape, "TlWd", 0.1, 0.02, 0.8) * w;
    double tip_x = x0 + clamp_num(r, shape, "TlRP", 0.5, 0.0, 1.0) * w;
    double base_x = x0 + clamp_num(r, shape, "TlEP", 0.4, 0.0, 1.0) * w;
    /* The reference's f64::clamp panics on an inverted box (x0 > x1); C clamps to x0 then x1. */
    double a = pc_clampd(base_x - tail_w / 2.0, x0, x1);
    double c = pc_clampd(base_x + tail_w / 2.0, x0, x1);
    double body_bottom = y1 - tail_h;
    double radius = pc_rs_max_k(pc_rs_min(w, body_bottom - y0) * 0.12, 0.0);
    double kr = (1.0 - KAPPA) * radius;
    /* One closed outline joining the tail to the rounded body, so a stroke shows no seam. */
    pc_af_point start = pt(x0 + radius, y0);
    pc_af_point tip = pt(tip_x, y1);
    segs s = {NULL, 0, 0, false};
    seg_push(&s, pt(x1 - radius, y0), pt(x1 - radius, y0), pt(x1 - radius, y0));
    seg_push(&s, pt(x1 - kr, y0), pt(x1, y0 + kr), pt(x1, y0 + radius));
    seg_push(&s, pt(x1, body_bottom - radius), pt(x1, body_bottom - radius), pt(x1, body_bottom - radius));
    seg_push(&s, pt(x1, body_bottom), pt(x1, body_bottom), pt(x1 - radius, body_bottom));
    seg_push(&s, pt(c + radius * 0.2, body_bottom), pt(tip_x, y1 - tail_h * 0.15), tip);
    seg_push(&s, tip, tip, tip);
    seg_push(&s, pt(tip_x, y1 - tail_h * 0.15), pt(a - radius * 0.2, body_bottom), pt(a, body_bottom));
    seg_push(&s, pt(x0 + radius, body_bottom), pt(x0, body_bottom), pt(x0, body_bottom - radius));
    seg_push(&s, pt(x0, y0 + radius), pt(x0, y0 + radius), pt(x0, y0 + radius));
    seg_push(&s, pt(x0, y0 + kr), pt(x0 + kr, y0), start);
    return finish(&s, start, true, out, err);
}

static pc_status callout_ellipse(const ax_reader *r, size_t shape, double cx, double cy, double rx, double ry,
                                 pc_af_subpath *out, pc_error *err)
{
    double tail_h = clamp_num(r, shape, "TlHg", 0.2, 0.0, 0.8);
    double pos = clamp_num(r, shape, "TlEP", 0.25, 0.0, 1.0);
    double angle = num_or(r, shape, "TlAn", 0.0);
    double a0 = pos * TAU_ - angle;
    double a1 = a0 + 0.35;
    pc_af_point start = pt(cx + rx * cos(a1), cy + ry * sin(a1));
    pc_af_point end = pt(cx + rx * cos(a0), cy + ry * sin(a0));
    pc_af_point tip = pt(cx + rx * cos(a0 + 0.18) * 1.08, cy + ry * sin(a0 + 0.18) * (1.0 + tail_h));
    segs s = {NULL, 0, 0, false};
    /* An ellipse-like outline with a short triangular tail. */
    arc(cx, cy, rx, ry, a1, a0 + TAU_, &s);
    seg_push(&s, end, end, tip);
    seg_push(&s, tip, tip, start);
    return finish(&s, start, true, out, err);
}

static pc_status arrow(const ax_reader *r, size_t shape, const double b[4], pc_af_subpath *out, pc_error *err)
{
    double x0 = b[0], y0 = b[1], x1 = b[2], y1 = b[3];
    double h = y1 - y0;
    double thick = clamp_num(r, shape, "Thck", 0.35, 0.05, 0.9) * h;
    double shaft = (y0 + y1 - thick) / 2.0;
    double lower = shaft + thick;
    double head_start = x1 - h * 0.45;
    pc_af_point p[7];
    p[0] = pt(x0, shaft);
    p[1] = pt(head_start, shaft);
    p[2] = pt(head_start, y0);
    p[3] = pt(x1, (y0 + y1) / 2.0);
    p[4] = pt(head_start, y1);
    p[5] = pt(head_start, lower);
    p[6] = pt(x0, lower);
    return polygon(p, 7, out, err);
}

static pc_status double_star(const ax_reader *r, size_t shape, double cx, double cy, double rx, double ry,
                             pc_af_subpath *out, pc_error *err)
{
    size_t pts2 = sides(r, shape, "Pnts", 6.0);
    size_t n = pts2 > 32 ? 64 : (pts2 * 2 < 8 ? 8 : pts2 * 2);
    double inner = clamp_num(r, shape, "IRad", 0.6, 0.05, 0.95);
    double outer = clamp_num(r, shape, "PRad", 0.9, 0.2, 1.0);
    pc_af_point p[128];
    size_t i;
    for (i = 0; i < 2 * n; i++) {
        double a = -FRAC_PI_2_ + PI_ * (double)i / (double)n;
        double radius = i % 2 == 0 ? outer : outer * inner;
        p[i] = pt(cx + rx * radius * cos(a), cy + ry * radius * sin(a));
    }
    return polygon(p, 2 * n, out, err);
}

static pc_status tear(const ax_reader *r, size_t shape, const double b[4], pc_af_subpath *out, pc_error *err)
{
    double x0 = b[0], y0 = b[1], x1 = b[2], y1 = b[3];
    double w = x1 - x0;
    double h = y1 - y0;
    double bulge = clamp_num(r, shape, "Ball", 0.25, 0.0, 0.5);
    double bend = clamp_num(r, shape, "Bend", 0.0, -0.5, 0.5);
    pc_af_point top = pt((x0 + x1) / 2.0, y0);
    pc_af_point bottom = pt((x0 + x1) / 2.0 + bend * w * 0.25, y1);
    segs s = {NULL, 0, 0, false};
    seg_push(&s, pt(x0 + w * 0.25, y0 + h * 0.42), pt(x0 + w * (0.5 - bulge), y1 - h * 0.03), bottom);
    seg_push(&s, pt(x1 - w * (0.5 - bulge), y1 - h * 0.03), pt(x1 - w * 0.25, y0 + h * 0.42), top);
    return finish(&s, top, true, out, err);
}

static pc_status crescent(double cx, double cy, double rx, double ry, pc_af_subpath *out, pc_error *err)
{
    pc_af_point start = pt(cx, cy - ry);
    double ix = cx + rx * 0.72;
    double irx = rx * 0.86;
    double iry = ry * 0.82;
    pc_af_point from = pt(ix, cy + iry);
    segs s = {NULL, 0, 0, false};
    arc(cx, cy, rx, ry, -FRAC_PI_2_, -3.0 * FRAC_PI_2_, &s);
    /* The inner arc returns from the lower tip to the upper one: a crescent open to the right. */
    seg_push(&s, from, from, from);
    arc(ix, cy, irx, iry, FRAC_PI_2_, -FRAC_PI_2_, &s);
    return finish(&s, start, true, out, err);
}

/* Rust's f64::rem_euclid. */
static double rem_euclid(double a, double b)
{
    double r = fmod(a, b);
    return r < 0.0 ? r + fabs(b) : r;
}

static pc_status segment(const ax_reader *r, size_t shape, double cx, double cy, double rx, double ry,
                         pc_af_subpath *out, pc_error *err)
{
    /* Keep hostile but finite angles in a small range before constructing arc controls. */
    double angle = rem_euclid(num_or(r, shape, "Angl", 0.0), TAU_);
    double p0 = clamp_num(r, shape, "Pos0", 0.25, 0.0, 1.0);
    double p1 = clamp_num(r, shape, "Pos1", 0.75, 0.0, 1.0);
    double a0 = angle + (p0 - 0.5) * TAU_;
    double a1 = angle + (p1 - 0.5) * TAU_;
    pc_af_point start;
    pc_af_point end;
    segs s = {NULL, 0, 0, false};
    while (a1 < a0) {
        a1 += TAU_;
    }
    arc(cx, cy, rx, ry, a0, pc_rs_min(a1, a0 + TAU_), &s);
    start = pt(cx + rx * cos(a0), cy + ry * sin(a0));
    end = pt(cx + rx * cos(a1), cy + ry * sin(a1));
    seg_push(&s, end, end, start);
    return finish(&s, start, true, out, err);
}

pc_status ax_shape(ax_reader *r, size_t shape, const double b[4], pc_af_affine world, bool *ok,
                   pc_af_path *out, bool *even_odd, pc_error *err)
{
    double x0 = b[0], y0 = b[1], x1 = b[2], y1 = b[3];
    double w = x1 - x0;
    double h = y1 - y0;
    double cx = (x0 + x1) / 2.0;
    double cy = (y0 + y1) / 2.0;
    double rx = w / 2.0;
    double ry = h / 2.0;
    pc_af_tag cls;
    pc_af_subpath sub;
    pc_status st;
    *ok = false;
    *even_odd = false;
    memset(out, 0, sizeof *out);
    if (!ax_class(r->s, shape, &cls)) {
        return PC_OK;
    }
#define ON_ELLIPSE(a, f) pt(cx + (f)*rx * cos(a), cy + (f)*ry * sin(a))
    if (cls == PC_AF_TAG("ShNR") || cls == PC_AF_TAG("ShRR")) {
        const pc_af_value *ct = last(r, shape, "CTyp");
        const pc_af_value *cr = last(r, shape, "ShCR");
        bool rounded = ct != NULL && ct->kind == PC_AF_ARRAY && ct->as.arr.n > 0;
        bool has_radius = cr != NULL && cr->kind == PC_AF_FLOATS && cr->as.floats.n > 0 &&
                          isfinite(cr->as.floats.v[0]) && cr->as.floats.v[0] > 0.0;
        if (rounded && has_radius) {
            double f = cr->as.floats.v[0];
            double crx;
            double cry;
            bool abs_size;
            size_t i;
            for (i = 0; i < ct->as.arr.n; i++) {
                const pc_af_value *t = &ct->as.arr.v[i];
                if (!(t->kind == PC_AF_ENUM && (t->as.e.id == 0 || t->as.e.id == 4))) {
                    ax_warn(r, "rectangles with straight, concave or other corner types (imported with round "
                               "corners)");
                    break;
                }
            }
            if (ax_bool(r->s, shape, "AbSz", &abs_size) && abs_size) {
                /* An absolute radius is in document pixels, whatever the node's scale. */
                double sx = sqrt(world.m[0] * world.m[0] + world.m[1] * world.m[1]);
                double sy = sqrt(world.m[2] * world.m[2] + world.m[3] * world.m[3]);
                if (sx > 0.0 && sy > 0.0) {
                    crx = f / sx;
                    cry = f / sy;
                } else {
                    crx = 0.0;
                    cry = 0.0;
                }
            } else {
                crx = f * pc_rs_min(w, h);
                cry = f * pc_rs_min(w, h);
            }
            st = rounded_rect(b, pc_rs_min(crx, w / 2.0), pc_rs_min(cry, h / 2.0), &sub, err);
        } else {
            pc_af_point p[4];
            p[0] = pt(x0, y0);
            p[1] = pt(x1, y0);
            p[2] = pt(x1, y1);
            p[3] = pt(x0, y1);
            st = polygon(p, 4, &sub, err);
        }
    } else if (cls == PC_AF_TAG("ShpE")) {
        st = ellipse(cx, cy, rx, ry, &sub, err);
    } else if (cls == PC_AF_TAG("ShPy")) {
        size_t n = sides(r, shape, "Side", 5.0);
        const pc_af_value *sm = last(r, shape, "Smth");
        size_t i;
#define A(i) (-FRAC_PI_2_ + TAU_ * (double)(i) / (double)n)
        if (sm != NULL && sm->kind == PC_AF_BOOL && sm->as.b) {
            /* Smoothed: tangent handles of length curv*4/3*tan(pi/2n) along the inscribed ellipse. */
            double k = num_or(r, shape, "Curv", 0.0) * 4.0 / 3.0 * tan(PI_ / (2.0 * (double)n));
            segs s = {NULL, 0, 0, false};
            for (i = 0; i < n; i++) {
                pc_af_point p = ON_ELLIPSE(A(i), 1.0);
                pc_af_point q = ON_ELLIPSE(A(i + 1), 1.0);
                pc_af_point tp = pt(-rx * sin(A(i)), ry * cos(A(i)));
                pc_af_point tq = pt(-rx * sin(A(i + 1)), ry * cos(A(i + 1)));
                seg_push(&s, pt(p.x + k * tp.x, p.y + k * tp.y), pt(q.x - k * tq.x, q.y - k * tq.y), q);
            }
            st = finish(&s, ON_ELLIPSE(A(0), 1.0), true, &sub, err);
        } else {
            pc_af_point *p = pc_malloc_array(n, sizeof *p);
            if (p == NULL) {
                return ax_nomem(err);
            }
            for (i = 0; i < n; i++) {
                p[i] = ON_ELLIPSE(A(i), 1.0);
            }
            st = polygon(p, n, &sub, err);
            free(p);
        }
#undef A
    } else if (cls == PC_AF_TAG("ShSt")) {
        size_t n = sides(r, shape, "Pnts", 5.0);
        double q = pc_clampd(num_or(r, shape, "IRad", 0.382), 0.0, 1.0);
        static const char *const T[4] = {"CrcI", "CrcO", "CrvL", "CrvR"};
        pc_af_point *p;
        size_t i;
        for (i = 0; i < 4; i++) {
            double v;
            if (num(r, shape, T[i], &v) && fabs(v) > 1e-9) {
                ax_warn(r, "stars with rounded or curved points (imported with sharp points)");
                break;
            }
        }
        p = pc_malloc_array(2 * n, sizeof *p);
        if (p == NULL) {
            return ax_nomem(err);
        }
        for (i = 0; i < 2 * n; i++) {
            p[i] = ON_ELLIPSE(-FRAC_PI_2_ + PI_ * (double)i / (double)n, i % 2 == 0 ? 1.0 : q);
        }
        st = polygon(p, 2 * n, &sub, err);
        free(p);
    } else if (cls == PC_AF_TAG("ShSS")) {
        size_t n = sides(r, shape, "Side", 5.0);
        double ri = pc_clampd(num_or(r, shape, "COut", 0.5), 0.0, 1.0);
        double step = PI_ / (double)n;
        double l = cos(step);
        double hw = ri * sin(step);
        double inner = ri * cos(step);
        pc_af_point *p = pc_malloc_array(4 * n, sizeof *p);
        size_t k;
        if (p == NULL) {
            return ax_nomem(err);
        }
        for (k = 0; k < n; k++) {
            double phi = -FRAC_PI_2_ + step + TAU_ * (double)k / (double)n;
            double c = cos(phi);
            double s = sin(phi);
            double uv[4][2] = {{inner, -hw}, {l, -hw}, {l, hw}, {inner, hw}};
            int j;
            for (j = 0; j < 4; j++) {
                double u = uv[j][0];
                double v = uv[j][1];
                p[4 * k + (size_t)j] = pt(cx + rx * (u * c - v * s), cy + ry * (u * s + v * c));
            }
        }
        st = polygon(p, 4 * n, &sub, err);
        free(p);
    } else if (cls == PC_AF_TAG("ShPi")) {
        double start = num_or(r, shape, "AngS", 0.0);
        double end = num_or(r, shape, "AngE", TAU_);
        double inner = pc_clampd(num_or(r, shape, "IRad", 0.0), 0.0, 1.0);
        /* The reference loops until end >= start; a hostile angle would loop for ever, so the
         * additions stop after a million turns (only such files differ). */
        long guard = 0;
        while (end < start && guard < 1000000) {
            end += TAU_;
            guard++;
        }
        st = pie(cx, cy, rx, ry, start, pc_rs_min(end, start + TAU_), inner, &sub, err);
    } else if (cls == PC_AF_TAG("ShpT")) {
        double p = num_or(r, shape, "Pos ", 0.5);
        pc_af_point v[3];
        v[0] = pt(x0 + p * w, y0);
        v[1] = pt(x1, y1);
        v[2] = pt(x0, y1);
        st = polygon(v, 3, &sub, err);
    } else if (cls == PC_AF_TAG("ShTz")) {
        double l = num_or(r, shape, "PosL", 0.25);
        double rr = num_or(r, shape, "PosR", 0.75);
        pc_af_point v[4];
        v[0] = pt(x0 + l * w, y0);
        v[1] = pt(x0 + rr * w, y0);
        v[2] = pt(x1, y1);
        v[3] = pt(x0, y1);
        st = polygon(v, 4, &sub, err);
    } else if (cls == PC_AF_TAG("ShCl")) {
        st = cloud(cx, cy, rx, ry, sides(r, shape, "Bubl", 12.0), &sub, err);
    } else if (cls == PC_AF_TAG("ShHt")) {
        st = heart(b, &sub, err);
    } else if (cls == PC_AF_TAG("ShCg")) {
        st = cog(r, shape, cx, cy, rx, ry, &sub, err);
    } else if (cls == PC_AF_TAG("ShCR")) {
        st = callout_rect(r, shape, b, &sub, err);
    } else if (cls == PC_AF_TAG("ShCE")) {
        st = callout_ellipse(r, shape, cx, cy, rx, ry, &sub, err);
    } else if (cls == PC_AF_TAG("ShDA")) {
        st = arrow(r, shape, b, &sub, err);
    } else if (cls == PC_AF_TAG("ShDS")) {
        st = double_star(r, shape, cx, cy, rx, ry, &sub, err);
    } else if (cls == PC_AF_TAG("ShTr")) {
        st = tear(r, shape, b, &sub, err);
    } else if (cls == PC_AF_TAG("ShCr")) {
        st = crescent(cx, cy, rx, ry, &sub, err);
    } else if (cls == PC_AF_TAG("ShpD")) {
        pc_af_point v[4];
        v[0] = pt(cx, y0);
        v[1] = pt(x1, cy);
        v[2] = pt(cx, y1);
        v[3] = pt(x0, cy);
        st = polygon(v, 4, &sub, err);
    } else if (cls == PC_AF_TAG("ShSg")) {
        st = segment(r, shape, cx, cy, rx, ry, &sub, err);
    } else {
        ax_warn(r, "an unrecognized parametric shape (imported as its bounding ellipse)");
        st = ellipse(cx, cy, rx, ry, &sub, err);
    }
#undef ON_ELLIPSE
    if (st != PC_OK) {
        return st;
    }
    if ((st = ax_path_push(out, sub, err)) != PC_OK) {
        return st;
    }
    /* A cog's hole is a second subpath: the shape fills even-odd. */
    if (cls == PC_AF_TAG("ShCg")) {
        double hole = clamp_num(r, shape, "Hole", 0.3, 0.0, 0.9);
        if (hole > 0.0) {
            if ((st = ellipse(cx, cy, rx * hole, ry * hole, &sub, err)) != PC_OK ||
                (st = ax_path_push(out, sub, err)) != PC_OK) {
                ax_path_fini(out);
                return st;
            }
        }
        *even_odd = true;
    }
    *ok = true;
    return PC_OK;
}

static pc_af_point unit(pc_af_point from, pc_af_point to, double *len)
{
    double dx = to.x - from.x;
    double dy = to.y - from.y;
    double l = sqrt(dx * dx + dy * dy);
    *len = l > 0.0 ? l : 0.0;
    return l > 0.0 ? pt(dx / l, dy / l) : pt(0.0, 0.0);
}

static bool same(pc_af_point a, pc_af_point b) { return a.x == b.x && a.y == b.y; }

pc_status ax_fillet(const ax_corner_pt *pts, size_t n, bool closed, pc_af_subpath *out, pc_error *err)
{
    pc_af_point(*corners)[4];
    segs s = {NULL, 0, 0, false};
    size_t i;
    pc_af_point cur;
    pc_af_point start;
    if (n == 0) {
        return finish(&s, pt(0.0, 0.0), closed, out, err);
    }
    corners = pc_malloc_array(n, sizeof *corners);
    if (corners == NULL) {
        return ax_nomem(err);
    }
    for (i = 0; i < n; i++) {
        pc_af_point p = pts[i].p;
        double radius = pts[i].radius;
        bool has_prev;
        bool has_next;
        pc_af_point a;
        pc_af_point b;
        pc_af_point u1;
        pc_af_point u2;
        double l1;
        double l2;
        double c;
        double phi;
        double t;
        double rr;
        double k;
        pc_af_point sp;
        pc_af_point ep;
        if (closed) {
            has_prev = true;
            has_next = true;
            a = pts[(i + n - 1) % n].p;
            b = pts[(i + 1) % n].p;
        } else {
            has_prev = i > 0;
            has_next = i + 1 < n;
            a = has_prev ? pts[i - 1].p : p;
            b = has_next ? pts[i + 1].p : p;
        }
        if (!has_prev || !has_next) {
            corners[i][0] = corners[i][1] = corners[i][2] = corners[i][3] = p;
            continue;
        }
        u1 = unit(p, a, &l1);
        u2 = unit(p, b, &l2);
        c = pc_clampd(u1.x * u2.x + u1.y * u2.y, -1.0, 1.0);
        phi = acos(c);
        if (radius <= 0.0 || phi < 1e-6 || (PI_ - phi) < 1e-6) {
            corners[i][0] = corners[i][1] = corners[i][2] = corners[i][3] = p;
            continue;
        }
        t = pc_rs_min(pc_rs_min(radius / tan(phi / 2.0), l1 / 2.0), l2 / 2.0);
        rr = t * tan(phi / 2.0);
        k = 4.0 / 3.0 * tan((PI_ - phi) / 4.0) * rr;
        sp = pt(p.x + u1.x * t, p.y + u1.y * t);
        ep = pt(p.x + u2.x * t, p.y + u2.y * t);
        corners[i][0] = sp;
        corners[i][1] = pt(sp.x - u1.x * k, sp.y - u1.y * k);
        corners[i][2] = pt(ep.x - u2.x * k, ep.y - u2.y * k);
        corners[i][3] = ep;
    }
    cur = corners[0][0];
    start = cur;
    for (i = 0; i < n; i++) {
        if (i > 0) {
            seg_push(&s, cur, corners[i][0], corners[i][0]);
        }
        if (!same(corners[i][0], corners[i][3])) {
            seg_push(&s, corners[i][1], corners[i][2], corners[i][3]);
        }
        cur = corners[i][3];
    }
    if (closed && !same(cur, start)) {
        seg_push(&s, cur, start, start);
    }
    free(corners);
    return finish(&s, start, closed, out, err);
}
