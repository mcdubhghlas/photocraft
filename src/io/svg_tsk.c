/* tiny-skia-path 0.11 geometry in f32, as usvg uses it: Transform (concat, invert, validity),
 * Rect / NonZeroRect, usvg's BBox, PathBuilder (move/line/quad/cubic/close semantics), path
 * transform, segment iteration and tight bounds (curve extrema). */
#include <math.h>
#include <string.h>

#include "io/svg_internal.h"

/* ---------------------------------------------------------------- transforms */

sv_ts ts_row(float sx, float ky, float kx, float sy, float tx, float ty)
{
    sv_ts t;
    t.sx = sx;
    t.ky = ky;
    t.kx = kx;
    t.sy = sy;
    t.tx = tx;
    t.ty = ty;
    return t;
}

sv_ts ts_identity(void)
{
    return ts_row(1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
}

sv_ts ts_translate(float tx, float ty)
{
    return ts_row(1.0f, 0.0f, 0.0f, 1.0f, tx, ty);
}

sv_ts ts_scale(float sx, float sy)
{
    return ts_row(sx, 0.0f, 0.0f, sy, 0.0f, 0.0f);
}

bool ts_is_identity(sv_ts t)
{
    return t.sx == 1.0f && t.ky == 0.0f && t.kx == 0.0f && t.sy == 1.0f && t.tx == 0.0f && t.ty == 0.0f;
}

static bool has_scale(sv_ts t)
{
    return t.sx != 1.0f || t.sy != 1.0f;
}

bool ts_has_skew(sv_ts t)
{
    return t.kx != 0.0f || t.ky != 0.0f;
}

bool ts_is_finite(sv_ts t)
{
    return isfinite(t.sx) && isfinite(t.ky) && isfinite(t.kx) && isfinite(t.sy) && isfinite(t.tx) &&
           isfinite(t.ty);
}

void ts_get_scale(sv_ts t, float *sx, float *sy)
{
    *sx = sqrtf(t.sx * t.sx + t.kx * t.kx);
    *sy = sqrtf(t.ky * t.ky + t.sy * t.sy);
}

bool ts_is_valid(sv_ts t)
{
    float sx;
    float sy;
    if (!ts_is_finite(t)) {
        return false;
    }
    ts_get_scale(t, &sx, &sy);
    return !(fabsf(sx) <= 1.19209290e-7f || fabsf(sy) <= 1.19209290e-7f);
}

static float mul_add_mul(float a, float b, float c, float d)
{
    return (float)((double)a * (double)b + (double)c * (double)d);
}

static sv_ts concat(sv_ts a, sv_ts b)
{
    if (ts_is_identity(a)) {
        return b;
    }
    if (ts_is_identity(b)) {
        return a;
    }
    if (!ts_has_skew(a) && !ts_has_skew(b)) {
        return ts_row(a.sx * b.sx, 0.0f, 0.0f, a.sy * b.sy, a.sx * b.tx + a.tx, a.sy * b.ty + a.ty);
    }
    return ts_row(mul_add_mul(a.sx, b.sx, a.kx, b.ky), mul_add_mul(a.ky, b.sx, a.sy, b.ky),
                  mul_add_mul(a.sx, b.kx, a.kx, b.sy), mul_add_mul(a.ky, b.kx, a.sy, b.sy),
                  mul_add_mul(a.sx, b.tx, a.kx, b.ty) + a.tx, mul_add_mul(a.ky, b.tx, a.sy, b.ty) + a.ty);
}

sv_ts ts_pre_concat(sv_ts a, sv_ts b)
{
    return concat(a, b);
}

sv_ts ts_post_concat(sv_ts a, sv_ts b)
{
    return concat(b, a);
}

sv_ts ts_pre_translate(sv_ts a, float tx, float ty)
{
    return concat(a, ts_translate(tx, ty));
}

bool ts_invert(sv_ts t, sv_ts *out)
{
    double det;
    double inv;
    sv_ts r;
    if (ts_is_identity(t)) {
        *out = t;
        return true;
    }
    if (!ts_has_skew(t)) {
        if (has_scale(t)) {
            float ix = 1.0f / t.sx;
            float iy = 1.0f / t.sy;
            *out = ts_row(ix, 0.0f, 0.0f, iy, -t.tx * ix, -t.ty * iy);
        } else {
            *out = ts_translate(-t.tx, -t.ty);
        }
        return true;
    }
    det = (double)t.sx * (double)t.sy - (double)t.kx * (double)t.ky;
    {
        const float nz = 1.0f / 4096.0f;
        if (fabsf((float)det) <= nz * nz * nz) {
            return false;
        }
    }
    inv = 1.0 / det;
    r = ts_row((float)((double)t.sy * inv), (float)(-(double)t.ky * inv), (float)(-(double)t.kx * inv),
               (float)((double)t.sx * inv),
               (float)(((double)t.kx * (double)t.ty - (double)t.sy * (double)t.tx) * inv),
               (float)(((double)t.ky * (double)t.tx - (double)t.sx * (double)t.ty) * inv));
    if (!ts_is_finite(r)) {
        return false;
    }
    *out = r;
    return true;
}

void ts_map_point(sv_ts t, float *x, float *y)
{
    if (ts_is_identity(t)) {
        return;
    }
    if (!has_scale(t) && !ts_has_skew(t)) {
        *x += t.tx;
        *y += t.ty;
    } else if (!ts_has_skew(t)) {
        *x = *x * t.sx + t.tx;
        *y = *y * t.sy + t.ty;
    } else {
        float nx = *x * t.sx + *y * t.kx + t.tx;
        float ny = *x * t.ky + *y * t.sy + t.ty;
        *x = nx;
        *y = ny;
    }
}

sv_ts ts_from_bbox(sv_rect r)
{
    return ts_row(rect_w(r), 0.0f, 0.0f, rect_h(r), r.l, r.t);
}

/* ---------------------------------------------------------------- rects */

static bool checked_sub(float a, float b)
{
    double n = (double)a - (double)b;
    return n > -3.40282347e38 && n < 3.40282347e38;
}

bool rect_from_ltrb(float l, float t, float r, float b, sv_rect *out)
{
    if (!(isfinite(l) && isfinite(t) && isfinite(r) && isfinite(b))) {
        return false;
    }
    if (!(l <= r && t <= b) || !checked_sub(r, l) || !checked_sub(b, t)) {
        return false;
    }
    out->l = l;
    out->t = t;
    out->r = r;
    out->b = b;
    return true;
}

bool rect_from_xywh(float x, float y, float w, float h, sv_rect *out)
{
    return rect_from_ltrb(x, y, w + x, h + y, out);
}

bool nzrect_from_ltrb(float l, float t, float r, float b, sv_rect *out)
{
    if (!(isfinite(l) && isfinite(t) && isfinite(r) && isfinite(b))) {
        return false;
    }
    if (!(l < r && t < b) || !checked_sub(r, l) || !checked_sub(b, t)) {
        return false;
    }
    out->l = l;
    out->t = t;
    out->r = r;
    out->b = b;
    return true;
}

bool nzrect_from_xywh(float x, float y, float w, float h, sv_rect *out)
{
    return nzrect_from_ltrb(x, y, w + x, h + y, out);
}

float rect_w(sv_rect r)
{
    return r.r - r.l;
}

float rect_h(sv_rect r)
{
    return r.b - r.t;
}

static bool bounds_of(const sv_pt *p, size_t n, sv_rect *out)
{
    float l;
    float t;
    float r;
    float b;
    size_t i;
    if (n == 0) {
        return false;
    }
    l = r = p[0].x;
    t = b = p[0].y;
    for (i = 0; i < n; i++) {
        if (!isfinite(p[i].x) || !isfinite(p[i].y)) {
            return false;
        }
        l = p[i].x < l ? p[i].x : l;
        r = p[i].x > r ? p[i].x : r;
        t = p[i].y < t ? p[i].y : t;
        b = p[i].y > b ? p[i].y : b;
    }
    return rect_from_ltrb(l, t, r, b, out);
}

bool rect_transform(sv_rect r, sv_ts t, sv_rect *out)
{
    sv_pt p[4];
    size_t i;
    if (ts_is_identity(t)) {
        *out = r;
        return true;
    }
    p[0].x = r.l;
    p[0].y = r.t;
    p[1].x = r.r;
    p[1].y = r.t;
    p[2].x = r.r;
    p[2].y = r.b;
    p[3].x = r.l;
    p[3].y = r.b;
    for (i = 0; i < 4; i++) {
        ts_map_point(t, &p[i].x, &p[i].y);
    }
    return bounds_of(p, 4, out);
}

bool nzrect_transform(sv_rect r, sv_ts t, sv_rect *out)
{
    sv_rect b;
    if (ts_is_identity(t)) {
        *out = r;
        return true;
    }
    if (!rect_transform(r, t, &b)) {
        return false;
    }
    return rect_to_nz(b, out);
}

sv_rect rect_bbox_transform(sv_rect r, sv_rect bbox)
{
    float x = r.l * rect_w(bbox) + bbox.l;
    float y = r.t * rect_h(bbox) + bbox.t;
    float w = rect_w(r) * rect_w(bbox);
    float h = rect_h(r) * rect_h(bbox);
    sv_rect o;
    if (!nzrect_from_xywh(x, y, w, h, &o)) {
        /* The reference unwraps here; keep the input rather than abort. */
        return r;
    }
    return o;
}

bool rect_to_nz(sv_rect r, sv_rect *out)
{
    return nzrect_from_xywh(r.l, r.t, rect_w(r), rect_h(r), out);
}

sv_bbox bbox_default(void)
{
    sv_bbox b = {3.40282347e38f, 3.40282347e38f, -3.40282347e38f, -3.40282347e38f};
    return b;
}

/* f32::min / max: a NaN operand yields the other. */
static float fmin_rs(float a, float b)
{
    return isnan(a) ? b : (b < a ? b : a);
}

static float fmax_rs(float a, float b)
{
    return isnan(a) ? b : (b > a ? b : a);
}

sv_bbox bbox_expand(sv_bbox b, sv_rect r)
{
    sv_bbox o;
    o.l = fmin_rs(b.l, r.l);
    o.t = fmin_rs(b.t, r.t);
    o.r = fmax_rs(b.r, r.r);
    o.b = fmax_rs(b.b, r.b);
    return o;
}

static bool bbox_is_default(sv_bbox b)
{
    return b.l == 3.40282347e38f && b.t == 3.40282347e38f && b.r == -3.40282347e38f && b.b == -3.40282347e38f;
}

bool bbox_to_rect(sv_bbox b, sv_rect *out)
{
    return !bbox_is_default(b) && rect_from_ltrb(b.l, b.t, b.r, b.b, out);
}

bool bbox_to_nz(sv_bbox b, sv_rect *out)
{
    return !bbox_is_default(b) && nzrect_from_ltrb(b.l, b.t, b.r, b.b, out);
}

/* ---------------------------------------------------------------- path builder */

void pb_init(sv_pb *b, sv_arena *a)
{
    memset(b, 0, sizeof *b);
    b->a = a;
    b->move_to_required = true;
}

static bool push_verb(sv_pb *b, sv_verb v)
{
    void *p = b->verbs;
    if (b->failed || !sv_grow(b->a, &p, &b->capverbs, b->nverbs + 1, 1)) {
        b->failed = true;
        return false;
    }
    b->verbs = p;
    b->verbs[b->nverbs++] = (uint8_t)v;
    return true;
}

static bool push_pt(sv_pb *b, float x, float y)
{
    void *p = b->pts;
    if (b->failed || !sv_grow(b->a, &p, &b->cappts, b->npts + 1, sizeof(sv_pt))) {
        b->failed = true;
        return false;
    }
    b->pts = p;
    b->pts[b->npts].x = x;
    b->pts[b->npts].y = y;
    b->npts++;
    return true;
}

void pb_move_to(sv_pb *b, float x, float y)
{
    if (b->nverbs > 0 && b->verbs[b->nverbs - 1] == V_MOVE) {
        b->pts[b->npts - 1].x = x;
        b->pts[b->npts - 1].y = y;
        return;
    }
    b->last_move_to = b->npts;
    b->move_to_required = false;
    if (push_verb(b, V_MOVE)) {
        (void)push_pt(b, x, y);
    }
}

static void inject_move_to(sv_pb *b)
{
    if (b->move_to_required) {
        if (b->last_move_to < b->npts) {
            sv_pt p = b->pts[b->last_move_to];
            pb_move_to(b, p.x, p.y);
        } else {
            pb_move_to(b, 0.0f, 0.0f);
        }
    }
}

void pb_line_to(sv_pb *b, float x, float y)
{
    inject_move_to(b);
    if (push_verb(b, V_LINE)) {
        (void)push_pt(b, x, y);
    }
}

void pb_quad_to(sv_pb *b, float x1, float y1, float x, float y)
{
    inject_move_to(b);
    if (push_verb(b, V_QUAD) && push_pt(b, x1, y1)) {
        (void)push_pt(b, x, y);
    }
}

void pb_cubic_to(sv_pb *b, float x1, float y1, float x2, float y2, float x, float y)
{
    inject_move_to(b);
    if (push_verb(b, V_CUBIC) && push_pt(b, x1, y1) && push_pt(b, x2, y2)) {
        (void)push_pt(b, x, y);
    }
}

void pb_close(sv_pb *b)
{
    if (b->nverbs > 0 && b->verbs[b->nverbs - 1] != V_CLOSE) {
        (void)push_verb(b, V_CLOSE);
    }
    b->move_to_required = true;
}

bool pb_last_point(const sv_pb *b, sv_pt *out)
{
    if (b->npts == 0) {
        return false;
    }
    *out = b->pts[b->npts - 1];
    return true;
}

sv_path *pb_finish(sv_pb *b)
{
    sv_path *p;
    sv_rect r;
    if (b->failed || b->nverbs <= 1) {
        return NULL;
    }
    if (!bounds_of(b->pts, b->npts, &r)) {
        return NULL;
    }
    p = sv_alloc(b->a, sizeof *p);
    if (p == NULL) {
        b->failed = true;
        return NULL;
    }
    p->verbs = b->verbs;
    p->nverbs = b->nverbs;
    p->pts = b->pts;
    p->npts = b->npts;
    p->bounds = r;
    return p;
}

sv_path *path_from_rect(sv_arena *a, sv_rect r)
{
    sv_path *p = sv_alloc(a, sizeof *p);
    if (p == NULL) {
        return NULL;
    }
    p->verbs = sv_alloc(a, 5);
    p->pts = sv_alloc_array(a, 4, sizeof(sv_pt));
    if (p->verbs == NULL || p->pts == NULL) {
        return NULL;
    }
    p->verbs[0] = V_MOVE;
    p->verbs[1] = V_LINE;
    p->verbs[2] = V_LINE;
    p->verbs[3] = V_LINE;
    p->verbs[4] = V_CLOSE;
    p->nverbs = 5;
    p->pts[0].x = r.l;
    p->pts[0].y = r.t;
    p->pts[1].x = r.r;
    p->pts[1].y = r.t;
    p->pts[2].x = r.r;
    p->pts[2].y = r.b;
    p->pts[3].x = r.l;
    p->pts[3].y = r.b;
    p->npts = 4;
    p->bounds = r;
    return p;
}

const sv_path *path_transform(sv_arena *a, const sv_path *p, sv_ts t)
{
    sv_path *o;
    size_t i;
    if (ts_is_identity(t)) {
        return p;
    }
    o = sv_alloc(a, sizeof *o);
    if (o == NULL) {
        return NULL;
    }
    *o = *p;
    o->pts = sv_alloc_array(a, p->npts > 0 ? p->npts : 1, sizeof(sv_pt));
    if (o->pts == NULL) {
        return NULL;
    }
    for (i = 0; i < p->npts; i++) {
        o->pts[i] = p->pts[i];
        ts_map_point(t, &o->pts[i].x, &o->pts[i].y);
    }
    if (!bounds_of(o->pts, o->npts, &o->bounds)) {
        return NULL;
    }
    return o;
}

void piter_init(sv_piter *it, const sv_path *p)
{
    memset(it, 0, sizeof *it);
    it->path = p;
}

bool piter_next(sv_piter *it, sv_pseg *out)
{
    const sv_path *p = it->path;
    sv_verb v;
    if (it->vi >= p->nverbs) {
        return false;
    }
    v = (sv_verb)p->verbs[it->vi++];
    out->verb = v;
    switch (v) {
    case V_MOVE:
        if (it->pi + 1 > p->npts) {
            return false;
        }
        it->pi += 1;
        it->last_move_to = p->pts[it->pi - 1];
        it->last_point = it->last_move_to;
        out->p[0] = it->last_move_to;
        break;
    case V_LINE:
        if (it->pi + 1 > p->npts) {
            return false;
        }
        it->pi += 1;
        it->last_point = p->pts[it->pi - 1];
        out->p[0] = it->last_point;
        break;
    case V_QUAD:
        if (it->pi + 2 > p->npts) {
            return false;
        }
        it->pi += 2;
        out->p[0] = p->pts[it->pi - 2];
        it->last_point = p->pts[it->pi - 1];
        out->p[1] = it->last_point;
        break;
    case V_CUBIC:
        if (it->pi + 3 > p->npts) {
            return false;
        }
        it->pi += 3;
        out->p[0] = p->pts[it->pi - 3];
        out->p[1] = p->pts[it->pi - 2];
        it->last_point = p->pts[it->pi - 1];
        out->p[2] = it->last_point;
        break;
    case V_CLOSE:
    default:
        out->verb = V_CLOSE;
        it->last_point = it->last_move_to;
        break;
    }
    return true;
}

/* ---------------------------------------------------------------- tight bounds */

static bool unit_divide(float numer, float denom, float *out)
{
    float r;
    if (numer < 0.0f) {
        numer = -numer;
        denom = -denom;
    }
    if (denom == 0.0f || numer == 0.0f || numer >= denom) {
        return false;
    }
    r = numer / denom;
    if (!(r > 0.0f && r < 1.0f)) {
        return false;
    }
    *out = r;
    return true;
}

static size_t unit_quad_roots(float a, float b, float c, float roots[2])
{
    double dr;
    float r;
    float q;
    size_t n = 0;
    if (a == 0.0f) {
        return unit_divide(-c, b, &roots[0]) ? 1 : 0;
    }
    dr = (double)b * (double)b - 4.0 * (double)a * (double)c;
    if (dr < 0.0) {
        return 0;
    }
    dr = sqrt(dr);
    r = (float)dr;
    if (!isfinite(r)) {
        return 0;
    }
    q = b < 0.0f ? -(b - r) / 2.0f : -(b + r) / 2.0f;
    if (unit_divide(q, a, &roots[n])) {
        n++;
    }
    if (unit_divide(c, q, &roots[n])) {
        n++;
    }
    if (n == 2) {
        if (roots[0] > roots[1]) {
            float t = roots[0];
            roots[0] = roots[1];
            roots[1] = t;
        } else if (roots[0] == roots[1]) {
            n = 1;
        }
    }
    return n;
}

static sv_pt eval_quad(const sv_pt s[3], float t)
{
    sv_pt o;
    float bx = 2.0f * (s[1].x - s[0].x);
    float by = 2.0f * (s[1].y - s[0].y);
    float ax = s[2].x - (s[1].x + s[1].x) + s[0].x;
    float ay = s[2].y - (s[1].y + s[1].y) + s[0].y;
    o.x = (ax * t + bx) * t + s[0].x;
    o.y = (ay * t + by) * t + s[0].y;
    return o;
}

static sv_pt eval_cubic(const sv_pt s[4], float t)
{
    sv_pt o;
    float ax = s[3].x + 3.0f * (s[1].x - s[2].x) - s[0].x;
    float ay = s[3].y + 3.0f * (s[1].y - s[2].y) - s[0].y;
    float bx = 3.0f * (s[2].x - (s[1].x + s[1].x) + s[0].x);
    float by = 3.0f * (s[2].y - (s[1].y + s[1].y) + s[0].y);
    float cx = 3.0f * (s[1].x - s[0].x);
    float cy = 3.0f * (s[1].y - s[0].y);
    o.x = ((ax * t + bx) * t + cx) * t + s[0].x;
    o.y = ((ay * t + by) * t + cy) * t + s[0].y;
    return o;
}

static size_t cubic_extrema(float a, float b, float c, float d, float t[2])
{
    float aa = d - a + 3.0f * (b - c);
    float bb = 2.0f * (a - b - b + c);
    float cc = b - a;
    return unit_quad_roots(aa, bb, cc, t);
}

bool path_tight_bounds(const sv_path *p, sv_rect *out)
{
    sv_piter it;
    sv_pseg seg;
    sv_pt mn;
    sv_pt mx;
    sv_pt last = {0.0f, 0.0f};
    if (p->npts == 0) {
        return false;
    }
    mn = mx = p->pts[0];
    piter_init(&it, p);
    while (piter_next(&it, &seg)) {
        sv_pt ext[5];
        size_t n = 0;
        size_t i;
        switch (seg.verb) {
        case V_MOVE:
        case V_LINE:
            ext[0] = seg.p[0];
            n = 1;
            break;
        case V_QUAD: {
            sv_pt s[3];
            float t;
            s[0] = last;
            s[1] = seg.p[0];
            s[2] = seg.p[1];
            if (unit_divide(s[0].x - s[1].x, s[0].x - s[1].x - s[1].x + s[2].x, &t)) {
                ext[n++] = eval_quad(s, t);
            }
            if (unit_divide(s[0].y - s[1].y, s[0].y - s[1].y - s[1].y + s[2].y, &t)) {
                ext[n++] = eval_quad(s, t);
            }
            ext[n++] = s[2];
            break;
        }
        case V_CUBIC: {
            sv_pt s[4];
            float t0[2];
            float t1[2];
            size_t n0;
            size_t n1;
            s[0] = last;
            s[1] = seg.p[0];
            s[2] = seg.p[1];
            s[3] = seg.p[2];
            n0 = cubic_extrema(s[0].x, s[1].x, s[2].x, s[3].x, t0);
            n1 = cubic_extrema(s[0].y, s[1].y, s[2].y, s[3].y, t1);
            for (i = 0; i < n0; i++) {
                ext[n++] = eval_cubic(s, t0[i]);
            }
            for (i = 0; i < n1; i++) {
                ext[n++] = eval_cubic(s, t1[i]);
            }
            ext[n++] = s[3];
            break;
        }
        case V_CLOSE:
        default:
            break;
        }
        last = it.last_point;
        for (i = 0; i < n; i++) {
            mn.x = fmin_rs(mn.x, ext[i].x);
            mn.y = fmin_rs(mn.y, ext[i].y);
            mx.x = fmax_rs(mx.x, ext[i].x);
            mx.y = fmax_rs(mx.y, ext[i].y);
        }
    }
    return rect_from_ltrb(mn.x, mn.y, mx.x, mx.y, out);
}
