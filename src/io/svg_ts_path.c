/* tiny-skia-path 0.11.4's Point length helpers, Rect / IntRect constructors, PathBuilder (path.rs,
 * path_builder.rs, rect.rs, lib.rs) and Path::transform, on malloc'd storage, for the SVG
 * rasteriser (see svg_ts.h). A builder that runs out of memory or past TS_MAX_POINTS stops
 * growing and reports `failed`; its callers then draw nothing. */
#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "io/svg_ts.h"

/* ---------------------------------------------------------------- points */

float ts_length(sv_pt p)
{
    float mag2 = p.x * p.x + p.y * p.y;
    double xx;
    double yy;
    if (isfinite(mag2)) {
        return sqrtf(mag2);
    }
    xx = (double)p.x;
    yy = (double)p.y;
    return pc_f64_to_f32(sqrt(xx * xx + yy * yy));
}

bool ts_set_length_from(sv_pt *p, float x, float y, float length)
{
    double xx = (double)x;
    double yy = (double)y;
    double dmag = sqrt(xx * xx + yy * yy);
    double dscale = (double)length / dmag;
    x *= pc_f64_to_f32(dscale);
    y *= pc_f64_to_f32(dscale);
    if (!isfinite(x) || !isfinite(y) || (x == 0.0f && y == 0.0f)) {
        *p = ts_pt(0.0f, 0.0f);
        return false;
    }
    *p = ts_pt(x, y);
    return true;
}

/* ---------------------------------------------------------------- rects */

bool ts_rect_ltrb(float l, float t, float r, float b, sv_rect *out)
{
    double w;
    double h;
    if (!isfinite(l) || !isfinite(t) || !isfinite(r) || !isfinite(b) || !(l <= r && t <= b)) {
        return false;
    }
    /* checked_f32_sub: width and height must fit f32 */
    w = (double)r - (double)l;
    h = (double)b - (double)t;
    if (!(w > -(double)FLT_MAX && w < (double)FLT_MAX) || !(h > -(double)FLT_MAX && h < (double)FLT_MAX)) {
        return false;
    }
    out->l = l;
    out->t = t;
    out->r = r;
    out->b = b;
    return true;
}

bool ts_rect_from_points(const sv_pt *p, size_t n, sv_rect *out)
{
    float mn[4];
    float mx[4];
    float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    size_t off;
    int k;
    if (n == 0) {
        return false;
    }
    if (n & 1u) {
        mn[0] = mn[2] = p[0].x;
        mn[1] = mn[3] = p[0].y;
        off = 1;
    } else {
        mn[0] = p[0].x;
        mn[1] = p[0].y;
        mn[2] = p[1].x;
        mn[3] = p[1].y;
        off = 2;
    }
    memcpy(mx, mn, sizeof mx);
    while (off + 2 <= n) {
        float xy[4];
        xy[0] = p[off].x;
        xy[1] = p[off].y;
        xy[2] = p[off + 1].x;
        xy[3] = p[off + 1].y;
        for (k = 0; k < 4; k++) {
            acc[k] *= xy[k];
            mn[k] = pc_rs_minf(mn[k], xy[k]);
            mx[k] = pc_rs_maxf(mx[k], xy[k]);
        }
        off += 2;
    }
    for (k = 0; k < 4; k++) {
        if (!(acc[k] * 0.0f == 0.0f)) {
            return false;
        }
    }
    return ts_rect_ltrb(pc_rs_minf(mn[0], mn[2]), pc_rs_minf(mn[1], mn[3]), pc_rs_maxf(mx[0], mx[2]),
                        pc_rs_maxf(mx[1], mx[3]), out);
}

bool ts_irect_xywh(int32_t x, int32_t y, uint32_t w, uint32_t h, ts_irect *out)
{
    if (w == 0 || h == 0 || w > INT32_MAX || h > INT32_MAX || (int64_t)x + w > INT32_MAX ||
        (int64_t)y + h > INT32_MAX) {
        return false;
    }
    out->x = x;
    out->y = y;
    out->w = w;
    out->h = h;
    return true;
}

bool ts_irect_ltrb(int32_t l, int32_t t, int32_t r, int32_t b, ts_irect *out)
{
    int64_t w = (int64_t)r - l;
    int64_t h = (int64_t)b - t;
    if (w < INT32_MIN || w > INT32_MAX || h < INT32_MIN || h > INT32_MAX || w < 0 || h < 0) {
        return false;
    }
    return ts_irect_xywh(l, t, (uint32_t)w, (uint32_t)h, out);
}

bool ts_irect_intersect(ts_irect a, ts_irect b, ts_irect *out)
{
    int32_t l = a.x > b.x ? a.x : b.x;
    int32_t t = a.y > b.y ? a.y : b.y;
    int32_t r = ts_irect_r(a) < ts_irect_r(b) ? ts_irect_r(a) : ts_irect_r(b);
    int32_t bb = ts_irect_b(a) < ts_irect_b(b) ? ts_irect_b(a) : ts_irect_b(b);
    return ts_irect_ltrb(l, t, r, bb, out);
}

bool ts_irect_contains(ts_irect a, ts_irect b)
{
    return a.x <= b.x && a.y <= b.y && ts_irect_r(a) >= ts_irect_r(b) && ts_irect_b(a) >= ts_irect_b(b);
}

/* i32::saturate_from(f32) */
static int32_t sat_i32(float x)
{
    x = x < 2147483520.0f ? x : 2147483520.0f;
    x = x > -2147483520.0f ? x : -2147483520.0f;
    return (int32_t)x;
}

bool ts_rect_round_out(sv_rect r, ts_irect *out)
{
    int32_t w = sat_i32(ceilf(r.r - r.l));
    int32_t h = sat_i32(ceilf(r.b - r.t));
    uint32_t uw = (uint32_t)w;
    uint32_t uh = (uint32_t)h;
    return ts_irect_xywh(sat_i32(floorf(r.l)), sat_i32(floorf(r.t)), uw > 1 ? uw : 1, uh > 1 ? uh : 1, out);
}

/* ---------------------------------------------------------------- PathBuilder */

void ts_pb_init(ts_pb *b)
{
    memset(b, 0, sizeof *b);
    b->move_to_required = true;
}

void ts_pb_fini(ts_pb *b)
{
    free(b->verbs);
    free(b->pts);
    ts_pb_init(b);
}

void ts_pb_clear(ts_pb *b)
{
    b->nv = 0;
    b->np = 0;
    b->last_move_to = 0;
    b->move_to_required = true;
}

static bool reserve(ts_pb *b, size_t nv, size_t np)
{
    if (b->failed) {
        return false;
    }
    if (b->np + np > TS_MAX_POINTS) {
        b->failed = true;
        b->limit = true;
        return false;
    }
    if (b->nv + nv > b->capv) {
        size_t cap = b->capv;
        void *v = pc_grow_array(b->verbs, &cap, b->nv + nv, 1);
        if (v == NULL) {
            b->failed = true;
            return false;
        }
        b->verbs = v;
        b->capv = cap;
    }
    if (b->np + np > b->capp) {
        size_t cap = b->capp;
        void *v = pc_grow_array(b->pts, &cap, b->np + np, sizeof *b->pts);
        if (v == NULL) {
            b->failed = true;
            return false;
        }
        b->pts = v;
        b->capp = cap;
    }
    return true;
}

void ts_pb_move_to(ts_pb *b, float x, float y)
{
    if (b->nv > 0 && b->verbs[b->nv - 1] == V_MOVE && b->np > 0) {
        b->pts[b->np - 1] = ts_pt(x, y);
        return;
    }
    if (!reserve(b, 1, 1)) {
        return;
    }
    b->last_move_to = b->np;
    b->move_to_required = false;
    b->verbs[b->nv++] = V_MOVE;
    b->pts[b->np++] = ts_pt(x, y);
}

static void inject_move_to(ts_pb *b)
{
    if (b->move_to_required) {
        if (b->last_move_to < b->np) {
            sv_pt p = b->pts[b->last_move_to];
            ts_pb_move_to(b, p.x, p.y);
        } else {
            ts_pb_move_to(b, 0.0f, 0.0f);
        }
    }
}

void ts_pb_line_to(ts_pb *b, float x, float y)
{
    inject_move_to(b);
    if (!reserve(b, 1, 1)) {
        return;
    }
    b->verbs[b->nv++] = V_LINE;
    b->pts[b->np++] = ts_pt(x, y);
}

void ts_pb_quad_to(ts_pb *b, float x1, float y1, float x, float y)
{
    inject_move_to(b);
    if (!reserve(b, 1, 2)) {
        return;
    }
    b->verbs[b->nv++] = V_QUAD;
    b->pts[b->np++] = ts_pt(x1, y1);
    b->pts[b->np++] = ts_pt(x, y);
}

void ts_pb_cubic_to(ts_pb *b, float x1, float y1, float x2, float y2, float x, float y)
{
    inject_move_to(b);
    if (!reserve(b, 1, 3)) {
        return;
    }
    b->verbs[b->nv++] = V_CUBIC;
    b->pts[b->np++] = ts_pt(x1, y1);
    b->pts[b->np++] = ts_pt(x2, y2);
    b->pts[b->np++] = ts_pt(x, y);
}

/* ---- conics (path_geometry.rs Conic, AutoConicToQuads) ---- */

static float subdivide_weight(float w)
{
    return sqrtf(0.5f + w * 0.5f);
}

static bool between(float a, float b, float c)
{
    return (a - b) * (c - b) <= 0.0f;
}

static void conic_chop(const ts_conic *c, ts_conic *d0, ts_conic *d1)
{
    float scale = 1.0f / (1.0f + c->w);
    float nw = subdivide_weight(c->w);
    sv_pt p0 = c->p[0];
    sv_pt p1 = c->p[1];
    sv_pt p2 = c->p[2];
    sv_pt wp1 = ts_pt(c->w * p1.x, c->w * p1.y);
    sv_pt m;
    m.x = (p0.x + (wp1.x + wp1.x) + p2.x) * scale * 0.5f;
    m.y = (p0.y + (wp1.y + wp1.y) + p2.y) * scale * 0.5f;
    if (!ts_pt_is_finite(m)) {
        double wd = (double)c->w;
        double w2 = wd * 2.0;
        double sh = 1.0 / (1.0 + wd) * 0.5;
        m.x = pc_f64_to_f32(((double)p0.x + w2 * (double)p1.x + (double)p2.x) * sh);
        m.y = pc_f64_to_f32(((double)p0.y + w2 * (double)p1.y + (double)p2.y) * sh);
    }
    d0->p[0] = p0;
    d0->p[1] = ts_pt((p0.x + wp1.x) * scale, (p0.y + wp1.y) * scale);
    d0->p[2] = m;
    d0->w = nw;
    d1->p[0] = m;
    d1->p[1] = ts_pt((wp1.x + p2.x) * scale, (wp1.y + p2.y) * scale);
    d1->p[2] = p2;
    d1->w = nw;
}

/* Writes the quads' points after the start into pts (2 per quad); level <= 4 (depth bound). */
static sv_pt *conic_subdivide(const ts_conic *src, sv_pt *pts, int level)
{
    ts_conic d0;
    ts_conic d1;
    float sy;
    float ey;
    if (level <= 0) {
        pts[0] = src->p[1];
        pts[1] = src->p[2];
        return pts + 2;
    }
    conic_chop(src, &d0, &d1);
    sy = src->p[0].y;
    ey = src->p[2].y;
    if (between(sy, src->p[1].y, ey)) {
        float my = d0.p[2].y;
        if (!between(sy, my, ey)) {
            float closer = fabsf(my - sy) < fabsf(my - ey) ? sy : ey;
            d0.p[2].y = closer;
            d1.p[0].y = closer;
        }
        if (!between(sy, d0.p[1].y, d0.p[2].y)) {
            d0.p[1].y = sy;
        }
        if (!between(d1.p[0].y, d1.p[1].y, ey)) {
            d1.p[1].y = ey;
        }
    }
    pts = conic_subdivide(&d0, pts, level - 1);
    return conic_subdivide(&d1, pts, level - 1);
}

/* AutoConicToQuads::compute: the number of quads written to pts (1 + 2n points), 0 = None. */
static size_t conic_to_quads(sv_pt p0, sv_pt p1, sv_pt p2, float w, sv_pt pts[64])
{
    ts_conic c;
    float a;
    float k;
    float x;
    float y;
    float err;
    int pow2 = 0;
    int i;
    size_t nq;
    size_t npt;
    size_t j;
    const float tol = 0.25f;
    c.p[0] = p0;
    c.p[1] = p1;
    c.p[2] = p2;
    c.w = w;
    if (!ts_pt_is_finite(p0) || !ts_pt_is_finite(p1) || !ts_pt_is_finite(p2)) {
        return 0;
    }
    a = w - 1.0f;
    k = a / (4.0f * (2.0f + a));
    x = k * (p0.x - 2.0f * p1.x + p2.x);
    y = k * (p0.y - 2.0f * p1.y + p2.y);
    err = sqrtf(x * x + y * y);
    for (i = 0; i < 4; i++) {
        if (err <= tol) {
            break;
        }
        err *= 0.25f;
        pow2++;
    }
    pow2 = pow2 > 1 ? pow2 : 1;
    pts[0] = p0;
    (void)conic_subdivide(&c, pts + 1, pow2);
    nq = (size_t)1 << pow2;
    npt = 2 * nq + 1;
    for (j = 0; j < npt; j++) {
        if (!ts_pt_is_finite(pts[j])) {
            for (j = 1; j + 1 < npt; j++) {
                pts[j] = p1;
            }
            break;
        }
    }
    return nq;
}

void ts_pb_conic_to(ts_pb *b, float x1, float y1, float x, float y, float w)
{
    sv_pt last;
    sv_pt q[64];
    size_t n;
    size_t i;
    if (!(w > 0.0f)) {
        ts_pb_line_to(b, x, y);
    } else if (!isfinite(w)) {
        ts_pb_line_to(b, x1, y1);
        ts_pb_line_to(b, x, y);
    } else if (w == 1.0f) {
        ts_pb_quad_to(b, x1, y1, x, y);
    } else {
        inject_move_to(b);
        if (!ts_pb_last_point(b, &last)) {
            return;
        }
        n = conic_to_quads(last, ts_pt(x1, y1), ts_pt(x, y), w, q);
        for (i = 0; i < n; i++) {
            ts_pb_quad_to(b, q[1 + 2 * i].x, q[1 + 2 * i].y, q[2 + 2 * i].x, q[2 + 2 * i].y);
        }
    }
}

void ts_pb_close(ts_pb *b)
{
    if (b->nv > 0 && b->verbs[b->nv - 1] != V_CLOSE && reserve(b, 1, 0)) {
        b->verbs[b->nv++] = V_CLOSE;
    }
    b->move_to_required = true;
}

bool ts_pb_last_point(const ts_pb *b, sv_pt *out)
{
    if (b->np == 0) {
        return false;
    }
    *out = b->pts[b->np - 1];
    return true;
}

void ts_pb_set_last_point(ts_pb *b, sv_pt p)
{
    if (b->np > 0) {
        b->pts[b->np - 1] = p;
    } else {
        ts_pb_move_to(b, p.x, p.y);
    }
}

bool ts_pb_zero_length_since(const ts_pb *b, size_t start)
{
    size_t i;
    if (start > b->np || b->np - start < 2) {
        return true;
    }
    for (i = start + 1; i < b->np; i++) {
        if (!ts_pt_eq(b->pts[start], b->pts[i])) {
            return false;
        }
    }
    return true;
}

void ts_pb_push_circle(ts_pb *b, float x, float y, float r)
{
    sv_rect o;
    float cx;
    float cy;
    sv_pt op[4];
    sv_pt rp[4];
    int i;
    if (!ts_rect_ltrb(x - r, y - r, (r + r) + (x - r), (r + r) + (y - r), &o)) {
        return;
    }
    /* push_oval */
    cx = o.l * 0.5f + o.r * 0.5f;
    cy = o.t * 0.5f + o.b * 0.5f;
    op[0] = ts_pt(cx, o.b);
    op[1] = ts_pt(o.l, cy);
    op[2] = ts_pt(cx, o.t);
    op[3] = ts_pt(o.r, cy);
    rp[0] = ts_pt(o.r, o.b);
    rp[1] = ts_pt(o.l, o.b);
    rp[2] = ts_pt(o.l, o.t);
    rp[3] = ts_pt(o.r, o.t);
    ts_pb_move_to(b, op[3].x, op[3].y);
    for (i = 0; i < 4; i++) {
        ts_pb_conic_to(b, rp[i].x, rp[i].y, op[i].x, op[i].y, TS_ROOT_2_OVER_2);
    }
    ts_pb_close(b);
}

void ts_pb_push_builder(ts_pb *b, const ts_pb *other)
{
    if (other->nv == 0) {
        return;
    }
    if (b->last_move_to != 0) {
        b->last_move_to = b->np + other->last_move_to;
    }
    if (!reserve(b, other->nv, other->np)) {
        return;
    }
    memcpy(b->verbs + b->nv, other->verbs, other->nv);
    memcpy(b->pts + b->np, other->pts, other->np * sizeof *b->pts);
    b->nv += other->nv;
    b->np += other->np;
}

void ts_pb_reverse_path_to(ts_pb *b, const ts_pb *other)
{
    size_t off;
    size_t i;
    if (other->nv == 0 || other->np == 0) {
        return;
    }
    off = other->np - 1;
    for (i = other->nv; i-- > 0;) {
        uint8_t v = other->verbs[i];
        if (v == V_MOVE) {
            break;
        }
        if (v == V_LINE && off >= 1) {
            sv_pt p = other->pts[off - 1];
            off -= 1;
            ts_pb_line_to(b, p.x, p.y);
        } else if (v == V_QUAD && off >= 2) {
            sv_pt p1 = other->pts[off - 1];
            sv_pt p2 = other->pts[off - 2];
            off -= 2;
            ts_pb_quad_to(b, p1.x, p1.y, p2.x, p2.y);
        } else if (v == V_CUBIC && off >= 3) {
            sv_pt p1 = other->pts[off - 1];
            sv_pt p2 = other->pts[off - 2];
            sv_pt p3 = other->pts[off - 3];
            off -= 3;
            ts_pb_cubic_to(b, p1.x, p1.y, p2.x, p2.y, p3.x, p3.y);
        } else if (v != V_CLOSE) {
            break; /* inconsistent points (cannot happen for builders of this file) */
        }
    }
}

bool ts_pb_finish(const ts_pb *b, ts_path *out)
{
    if (b->failed || b->nv <= 1) {
        return false;
    }
    if (!ts_rect_from_points(b->pts, b->np, &out->bounds)) {
        return false;
    }
    out->verbs = b->verbs;
    out->pts = b->pts;
    out->nv = b->nv;
    out->np = b->np;
    return true;
}

/* ---------------------------------------------------------------- transforms */

void ts_map_points(sv_ts t, sv_pt *p, size_t n)
{
    size_t i;
    if (ts_is_identity(t)) {
        return;
    }
    if (t.sx == 1.0f && t.sy == 1.0f && t.kx == 0.0f && t.ky == 0.0f) {
        for (i = 0; i < n; i++) {
            p[i].x += t.tx;
            p[i].y += t.ty;
        }
    } else if (t.kx == 0.0f && t.ky == 0.0f) {
        for (i = 0; i < n; i++) {
            p[i].x = p[i].x * t.sx + t.tx;
            p[i].y = p[i].y * t.sy + t.ty;
        }
    } else {
        for (i = 0; i < n; i++) {
            float x = p[i].x * t.sx + p[i].y * t.kx + t.tx;
            float y = p[i].x * t.ky + p[i].y * t.sy + t.ty;
            p[i].x = x;
            p[i].y = y;
        }
    }
}

bool ts_path_transform(const ts_path *p, sv_ts t, ts_pb *b, ts_path *out)
{
    ts_pb_clear(b);
    if (!reserve(b, p->nv, p->np)) {
        return false;
    }
    if (p->nv > 0) {
        memcpy(b->verbs, p->verbs, p->nv);
    }
    if (p->np > 0) {
        memcpy(b->pts, p->pts, p->np * sizeof *b->pts);
    }
    b->nv = p->nv;
    b->np = p->np;
    out->verbs = b->verbs;
    out->pts = b->pts;
    out->nv = b->nv;
    out->np = b->np;
    out->bounds = p->bounds;
    if (ts_is_identity(t)) {
        return true;
    }
    ts_map_points(t, b->pts, b->np);
    return ts_rect_from_points(b->pts, b->np, &out->bounds);
}

/* ---------------------------------------------------------------- segments */

void ts_iter_init(ts_iter *it, const ts_path *p)
{
    memset(it, 0, sizeof *it);
    it->path = p;
}

bool ts_iter_next(ts_iter *it, ts_seg *out)
{
    const ts_path *p = it->path;
    uint8_t v;
    if (it->vi >= p->nv) {
        return false;
    }
    v = p->verbs[it->vi++];
    out->verb = (sv_verb)v;
    switch (v) {
    case V_MOVE:
        if (it->pi + 1 > p->np) {
            return false;
        }
        it->pi += 1;
        it->last_move_to = p->pts[it->pi - 1];
        it->last_point = it->last_move_to;
        out->p[0] = it->last_move_to;
        return true;
    case V_LINE:
        if (it->pi + 1 > p->np) {
            return false;
        }
        it->pi += 1;
        it->last_point = p->pts[it->pi - 1];
        out->p[0] = it->last_point;
        return true;
    case V_QUAD:
        if (it->pi + 2 > p->np) {
            return false;
        }
        it->pi += 2;
        it->last_point = p->pts[it->pi - 1];
        out->p[0] = p->pts[it->pi - 2];
        out->p[1] = it->last_point;
        return true;
    case V_CUBIC:
        if (it->pi + 3 > p->np) {
            return false;
        }
        it->pi += 3;
        it->last_point = p->pts[it->pi - 1];
        out->p[0] = p->pts[it->pi - 3];
        out->p[1] = p->pts[it->pi - 2];
        out->p[2] = it->last_point;
        return true;
    case V_CLOSE:
        if (it->auto_close && !ts_pt_eq(it->last_point, it->last_move_to)) {
            it->vi -= 1;
            out->verb = V_LINE;
            out->p[0] = it->last_move_to;
        }
        it->last_point = it->last_move_to;
        return true;
    default:
        return false;
    }
}

int ts_iter_next_verb(const ts_iter *it)
{
    return it->vi < it->path->nv ? (int)it->path->verbs[it->vi] : -1;
}
