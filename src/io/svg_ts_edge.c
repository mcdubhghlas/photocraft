/* tiny-skia 0.11.4 fixed_point.rs, edge.rs, edge_builder.rs, edge_clipper.rs and line_clipper.rs:
 * a path's segments clipped to the target and turned into 16.16 fixed-point edges (lines, and
 * quads / cubics forward-differenced into lines), as scan/path.rs walks them. i32 arithmetic
 * wraps like release Rust; divisions that would trap in C (by zero, INT32_MIN / -1) give the
 * wrapped result instead. */
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "io/svg_ts_edge.h"

/* ---------------------------------------------------------------- fixed point */

static int32_t fdot6_round(int32_t n)
{
    return ts_wadd(n, 32) >> 6;
}

static int32_t fdot6_to_fdot16(int32_t n)
{
    return ts_shl(n, 10);
}

static int32_t idiv(int32_t a, int32_t b)
{
    if (b == 0) {
        return 0; /* unreachable for the reference's inputs */
    }
    if (a == INT32_MIN && b == -1) {
        return INT32_MIN;
    }
    return a / b;
}

int32_t ts_fdot16_div(int32_t numer, int32_t denom)
{
    int64_t v;
    if (denom == 0) {
        return 0;
    }
    v = (int64_t)((uint64_t)(int64_t)numer << 16) / (int64_t)denom;
    return v < INT32_MIN ? INT32_MIN : v > INT32_MAX ? INT32_MAX : (int32_t)v;
}

static int32_t fdot6_div(int32_t a, int32_t b)
{
    if (a >= INT16_MIN && a <= INT16_MAX) {
        return idiv(ts_shl(a, 16), b);
    }
    return ts_fdot16_div(a, b);
}

int32_t ts_fdot16_mul(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * (int64_t)b) >> 16);
}

int32_t ts_fdot16_fast_div(int32_t a, int32_t b)
{
    return idiv(ts_shl(a, 16), b);
}

/* ---------------------------------------------------------------- edges */

static int32_t compute_dy(int32_t top, int32_t y0)
{
    return ts_wsub(ts_wadd(ts_shl(top, 6), 32), y0);
}

static int32_t cheap_distance(int32_t dx, int32_t dy)
{
    dx = ts_abs(dx);
    dy = ts_abs(dy);
    return dx > dy ? ts_wadd(dx, dy >> 1) : ts_wadd(dy, dx >> 1);
}

static int32_t clz32(uint32_t v)
{
    int32_t n = 0;
    if (v == 0) {
        return 32;
    }
    while ((v & 0x80000000u) == 0) {
        v <<= 1;
        n++;
    }
    return n;
}

static int32_t diff_to_shift(int32_t dx, int32_t dy, int32_t shift_aa)
{
    int32_t dist = cheap_distance(dx, dy);
    dist = ts_wadd(dist, 1 << 4) >> (3 + shift_aa);
    return (32 - clz32((uint32_t)dist)) >> 1;
}

static int32_t fdot6_to_fixed_div2(int32_t v)
{
    return ts_shl(v, 16 - 6 - 1);
}

static int32_t cubic_delta_from_line(int32_t a, int32_t b, int32_t c, int32_t d)
{
    int32_t one =
        ts_wmul(ts_wadd(ts_wadd(ts_wsub(ts_wmul(a, 8), ts_wmul(b, 15)), ts_wmul(6, c)), d), 19) >> 9;
    int32_t two =
        ts_wmul(ts_wadd(ts_wsub(ts_wadd(a, ts_wmul(6, b)), ts_wmul(c, 15)), ts_wmul(d, 8)), 19) >> 9;
    int32_t ao = ts_abs(one);
    int32_t at = ts_abs(two);
    return ao > at ? ao : at;
}

/* LineEdge::update (shared by quad / cubic edges) */
static bool line_update(ts_edge *e, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    int32_t top;
    int32_t bottom;
    int32_t slope;
    int32_t dy;
    y0 >>= 10;
    y1 >>= 10;
    top = fdot6_round(y0);
    bottom = fdot6_round(y1);
    if (top == bottom) {
        return false;
    }
    x0 >>= 10;
    x1 >>= 10;
    slope = fdot6_div(ts_wsub(x1, x0), ts_wsub(y1, y0));
    dy = compute_dy(top, y0);
    e->x = fdot6_to_fdot16(ts_wadd(x0, ts_fdot16_mul(slope, dy)));
    e->dx = slope;
    e->first_y = top;
    e->last_y = ts_wsub(bottom, 1);
    return true;
}

static bool line_new(ts_edge *e, sv_pt p0, sv_pt p1, int32_t shift)
{
    float scale = (float)(1 << (shift + 6));
    int32_t x0 = pc_f32_to_i32(p0.x * scale);
    int32_t y0 = pc_f32_to_i32(p0.y * scale);
    int32_t x1 = pc_f32_to_i32(p1.x * scale);
    int32_t y1 = pc_f32_to_i32(p1.y * scale);
    int8_t winding = 1;
    int32_t top;
    int32_t bottom;
    int32_t slope;
    int32_t dy;
    if (y0 > y1) {
        int32_t t = x0;
        x0 = x1;
        x1 = t;
        t = y0;
        y0 = y1;
        y1 = t;
        winding = -1;
    }
    top = fdot6_round(y0);
    bottom = fdot6_round(y1);
    if (top == bottom) {
        return false;
    }
    slope = fdot6_div(ts_wsub(x1, x0), ts_wsub(y1, y0));
    dy = compute_dy(top, y0);
    memset(e, 0, sizeof *e);
    e->kind = TS_EDGE_LINE;
    e->x = fdot6_to_fdot16(ts_wadd(x0, ts_fdot16_mul(slope, dy)));
    e->dx = slope;
    e->first_y = top;
    e->last_y = ts_wsub(bottom, 1);
    e->winding = winding;
    return true;
}

bool ts_quad_update(ts_edge *e)
{
    bool ok;
    int32_t count = e->curve_count;
    int32_t oldx = e->cx;
    int32_t oldy = e->cy;
    int32_t dx = e->cdx;
    int32_t dy = e->cdy;
    int32_t newx;
    int32_t newy;
    int32_t shift = e->curve_shift;
    for (;;) {
        count -= 1;
        if (count > 0) {
            newx = ts_wadd(oldx, dx >> shift);
            dx = ts_wadd(dx, e->cddx);
            newy = ts_wadd(oldy, dy >> shift);
            dy = ts_wadd(dy, e->cddy);
        } else {
            newx = e->last_x;
            newy = e->last_y_fixed;
        }
        ok = line_update(e, oldx, oldy, newx, newy);
        oldx = newx;
        oldy = newy;
        if (count == 0 || ok) {
            break;
        }
    }
    e->cx = newx;
    e->cy = newy;
    e->cdx = dx;
    e->cdy = dy;
    e->curve_count = (int8_t)count;
    return ok;
}

static bool quad_new(ts_edge *e, const sv_pt p[3], int32_t shift)
{
    float scale = (float)(1 << (shift + 6));
    int32_t x0 = pc_f32_to_i32(p[0].x * scale);
    int32_t y0 = pc_f32_to_i32(p[0].y * scale);
    int32_t x1 = pc_f32_to_i32(p[1].x * scale);
    int32_t y1 = pc_f32_to_i32(p[1].y * scale);
    int32_t x2 = pc_f32_to_i32(p[2].x * scale);
    int32_t y2 = pc_f32_to_i32(p[2].y * scale);
    int8_t winding = 1;
    int32_t top;
    int32_t bottom;
    int32_t a;
    int32_t b;
    if (y0 > y2) {
        int32_t t = x0;
        x0 = x2;
        x2 = t;
        t = y0;
        y0 = y2;
        y2 = t;
        winding = -1;
    }
    top = fdot6_round(y0);
    bottom = fdot6_round(y2);
    if (top == bottom) {
        return false;
    }
    {
        int32_t dx = (ts_wsub(ts_wsub(ts_shl(x1, 1), x0), x2)) >> 2;
        int32_t dy = (ts_wsub(ts_wsub(ts_shl(y1, 1), y0), y2)) >> 2;
        shift = diff_to_shift(dx, dy, shift);
    }
    if (shift == 0) {
        shift = 1;
    } else if (shift > 6) {
        shift = 6;
    }
    memset(e, 0, sizeof *e);
    e->kind = TS_EDGE_QUAD;
    e->winding = winding;
    e->curve_count = (int8_t)(1 << shift);
    e->curve_shift = (uint8_t)(shift - 1);
    a = fdot6_to_fixed_div2(ts_wadd(ts_wsub(ts_wsub(x0, x1), x1), x2));
    b = fdot6_to_fdot16(ts_wsub(x1, x0));
    e->cx = fdot6_to_fdot16(x0);
    e->cdx = ts_wadd(b, a >> shift);
    e->cddx = a >> (shift - 1);
    a = fdot6_to_fixed_div2(ts_wadd(ts_wsub(ts_wsub(y0, y1), y1), y2));
    b = fdot6_to_fdot16(ts_wsub(y1, y0));
    e->cy = fdot6_to_fdot16(y0);
    e->cdy = ts_wadd(b, a >> shift);
    e->cddy = a >> (shift - 1);
    e->last_x = fdot6_to_fdot16(x2);
    e->last_y_fixed = fdot6_to_fdot16(y2);
    return ts_quad_update(e);
}

bool ts_cubic_update(ts_edge *e)
{
    bool ok;
    int32_t count = e->curve_count;
    int32_t oldx = e->cx;
    int32_t oldy = e->cy;
    int32_t newx;
    int32_t newy;
    int32_t ddshift = e->curve_shift;
    int32_t dshift = e->dshift;
    for (;;) {
        count += 1;
        if (count < 0) {
            newx = ts_wadd(oldx, e->cdx >> dshift);
            e->cdx = ts_wadd(e->cdx, e->cddx >> ddshift);
            e->cddx = ts_wadd(e->cddx, e->cdddx);
            newy = ts_wadd(oldy, e->cdy >> dshift);
            e->cdy = ts_wadd(e->cdy, e->cddy >> ddshift);
            e->cddy = ts_wadd(e->cddy, e->cdddy);
        } else {
            newx = e->last_x;
            newy = e->last_y_fixed;
        }
        if (newy < oldy) {
            newy = oldy;
        }
        ok = line_update(e, oldx, oldy, newx, newy);
        oldx = newx;
        oldy = newy;
        if (count == 0 || ok) {
            break;
        }
    }
    e->cx = newx;
    e->cy = newy;
    e->curve_count = (int8_t)count;
    return ok;
}

static bool cubic_new(ts_edge *e, const sv_pt p[4], int32_t shift)
{
    float scale = (float)(1 << (shift + 6));
    int32_t x0 = pc_f32_to_i32(p[0].x * scale);
    int32_t y0 = pc_f32_to_i32(p[0].y * scale);
    int32_t x1 = pc_f32_to_i32(p[1].x * scale);
    int32_t y1 = pc_f32_to_i32(p[1].y * scale);
    int32_t x2 = pc_f32_to_i32(p[2].x * scale);
    int32_t y2 = pc_f32_to_i32(p[2].y * scale);
    int32_t x3 = pc_f32_to_i32(p[3].x * scale);
    int32_t y3 = pc_f32_to_i32(p[3].y * scale);
    int8_t winding = 1;
    int32_t top;
    int32_t bot;
    int32_t up;
    int32_t down;
    int32_t b;
    int32_t c;
    int32_t d;
    if (y0 > y3) {
        int32_t t = x0;
        x0 = x3;
        x3 = t;
        t = x1;
        x1 = x2;
        x2 = t;
        t = y0;
        y0 = y3;
        y3 = t;
        t = y1;
        y1 = y2;
        y2 = t;
        winding = -1;
    }
    top = fdot6_round(y0);
    bot = fdot6_round(y3);
    if (top == bot) {
        return false;
    }
    {
        int32_t dx = cubic_delta_from_line(x0, x1, x2, x3);
        int32_t dy = cubic_delta_from_line(y0, y1, y2, y3);
        shift = diff_to_shift(dx, dy, 2) + 1;
    }
    if (shift > 6) {
        shift = 6;
    }
    up = 6;
    down = shift + up - 10;
    if (down < 0) {
        down = 0;
        up = 10 - shift;
    }
    memset(e, 0, sizeof *e);
    e->kind = TS_EDGE_CUBIC;
    e->winding = winding;
    e->curve_count = (int8_t)ts_shl(-1, shift);
    e->curve_shift = (uint8_t)shift;
    e->dshift = (uint8_t)down;
    b = ts_shl(ts_wmul(3, ts_wsub(x1, x0)), up);
    c = ts_shl(ts_wmul(3, ts_wadd(ts_wsub(ts_wsub(x0, x1), x1), x2)), up);
    d = ts_shl(ts_wsub(ts_wadd(x3, ts_wmul(3, ts_wsub(x1, x2))), x0), up);
    e->cx = fdot6_to_fdot16(x0);
    e->cdx = ts_wadd(ts_wadd(b, c >> shift), d >> (2 * shift));
    e->cddx = ts_wadd(ts_wmul(2, c), ts_wmul(3, d) >> (shift - 1));
    e->cdddx = ts_wmul(3, d) >> (shift - 1);
    b = ts_shl(ts_wmul(3, ts_wsub(y1, y0)), up);
    c = ts_shl(ts_wmul(3, ts_wadd(ts_wsub(ts_wsub(y0, y1), y1), y2)), up);
    d = ts_shl(ts_wsub(ts_wadd(y3, ts_wmul(3, ts_wsub(y1, y2))), y0), up);
    e->cy = fdot6_to_fdot16(y0);
    e->cdy = ts_wadd(ts_wadd(b, c >> shift), d >> (2 * shift));
    e->cddy = ts_wadd(ts_wmul(2, c), ts_wmul(3, d) >> (shift - 1));
    e->cdddy = ts_wmul(3, d) >> (shift - 1);
    e->last_x = fdot6_to_fdot16(x3);
    e->last_y_fixed = fdot6_to_fdot16(y3);
    return ts_cubic_update(e);
}

/* ---------------------------------------------------------------- line_clipper.rs */

static float pin_f32(float v, float l0, float l1)
{
    if (l1 < l0) {
        float t = l0;
        l0 = l1;
        l1 = t;
    }
    return v < l0 ? l0 : v > l1 ? l1 : v;
}

static double pin_f64(double v, double l0, double l1)
{
    if (l1 < l0) {
        double t = l0;
        l0 = l1;
        l1 = t;
    }
    return v < l0 ? l0 : v > l1 ? l1 : v;
}

static float sect_with_horizontal(const sv_pt s[2], float y)
{
    float dy = s[1].y - s[0].y;
    if (ts_nearly_zero(dy)) {
        return (s[0].x + s[1].x) * 0.5f;
    } else {
        double x0 = (double)s[0].x;
        double y0 = (double)s[0].y;
        double x1 = (double)s[1].x;
        double y1 = (double)s[1].y;
        double r = x0 + ((double)y - y0) * (x1 - x0) / (y1 - y0);
        return pc_f64_to_f32(pin_f64(r, x0, x1));
    }
}

static float sect_with_vertical(const sv_pt s[2], float x)
{
    float dx = s[1].x - s[0].x;
    if (ts_nearly_zero(dx)) {
        return (s[0].y + s[1].y) * 0.5f;
    } else {
        double x0 = (double)s[0].x;
        double y0 = (double)s[0].y;
        double x1 = (double)s[1].x;
        double y1 = (double)s[1].y;
        return pc_f64_to_f32(y0 + ((double)x - x0) * (y1 - y0) / (x1 - x0));
    }
}

static float sect_clamp_with_vertical(const sv_pt s[2], float x)
{
    return pin_f32(sect_with_vertical(s, x), s[0].y, s[1].y);
}

/* line_clipper::clip: the clipped polyline (up to 4 points) in out; returns the point count. */
static size_t line_clip(const sv_pt src[2], sv_rect clip, bool cull_right, sv_pt out[4])
{
    size_t i0;
    size_t i1;
    sv_pt tmp[2];
    sv_pt res[4];
    const sv_pt *result;
    size_t lines = 1;
    bool reverse;
    size_t k;
    if (src[0].y < src[1].y) {
        i0 = 0;
        i1 = 1;
    } else {
        i0 = 1;
        i1 = 0;
    }
    if (src[i1].y <= clip.t || src[i0].y >= clip.b) {
        return 0;
    }
    tmp[0] = src[0];
    tmp[1] = src[1];
    if (src[i0].y < clip.t) {
        tmp[i0] = ts_pt(sect_with_horizontal(src, clip.t), clip.t);
    }
    if (tmp[i1].y > clip.b) {
        tmp[i1] = ts_pt(sect_with_horizontal(src, clip.b), clip.b);
    }
    if (src[0].x < src[1].x) {
        i0 = 0;
        i1 = 1;
        reverse = false;
    } else {
        i0 = 1;
        i1 = 0;
        reverse = true;
    }
    if (tmp[i1].x <= clip.l) {
        tmp[0].x = clip.l;
        tmp[1].x = clip.l;
        reverse = false;
        result = tmp;
    } else if (tmp[i0].x >= clip.r) {
        if (cull_right) {
            return 0;
        }
        tmp[0].x = clip.r;
        tmp[1].x = clip.r;
        reverse = false;
        result = tmp;
    } else {
        size_t off = 0;
        if (tmp[i0].x < clip.l) {
            res[off++] = ts_pt(clip.l, tmp[i0].y);
            res[off] = ts_pt(clip.l, sect_clamp_with_vertical(tmp, clip.l));
        } else {
            res[off] = tmp[i0];
        }
        off++;
        if (tmp[i1].x > clip.r) {
            res[off++] = ts_pt(clip.r, sect_clamp_with_vertical(tmp, clip.r));
            res[off] = ts_pt(clip.r, tmp[i1].y);
        } else {
            res[off] = tmp[i1];
        }
        lines = off;
        result = res;
    }
    if (reverse) {
        for (k = 0; k <= lines; k++) {
            out[lines - k] = result[k];
        }
    } else {
        for (k = 0; k <= lines; k++) {
            out[k] = result[k];
        }
    }
    return lines + 1;
}

static bool nested_lt(float a, float b, float dim)
{
    return a <= b && (a < b || dim > 0.0f);
}

bool ts_line_intersect(const sv_pt src[2], sv_rect clip, sv_pt dst[2])
{
    sv_rect bounds;
    size_t i0;
    size_t i1;
    sv_pt tmp[2];
    if (ts_rect_ltrb(pc_rs_minf(src[0].x, src[1].x), pc_rs_minf(src[0].y, src[1].y),
                     pc_rs_maxf(src[0].x, src[1].x), pc_rs_maxf(src[0].y, src[1].y), &bounds)) {
        if (clip.l <= bounds.l && clip.t <= bounds.t && clip.r >= bounds.r && clip.b >= bounds.b) {
            dst[0] = src[0];
            dst[1] = src[1];
            return true;
        }
        if (nested_lt(bounds.r, clip.l, bounds.r - bounds.l) ||
            nested_lt(clip.r, bounds.l, bounds.r - bounds.l) ||
            nested_lt(bounds.b, clip.t, bounds.b - bounds.t) ||
            nested_lt(clip.b, bounds.t, bounds.b - bounds.t)) {
            return false;
        }
    }
    if (src[0].y < src[1].y) {
        i0 = 0;
        i1 = 1;
    } else {
        i0 = 1;
        i1 = 0;
    }
    tmp[0] = src[0];
    tmp[1] = src[1];
    if (tmp[i0].y < clip.t) {
        tmp[i0] = ts_pt(sect_with_horizontal(src, clip.t), clip.t);
    }
    if (tmp[i1].y > clip.b) {
        tmp[i1] = ts_pt(sect_with_horizontal(src, clip.b), clip.b);
    }
    if (tmp[0].x < tmp[1].x) {
        i0 = 0;
        i1 = 1;
    } else {
        i0 = 1;
        i1 = 0;
    }
    if (tmp[i1].x <= clip.l || tmp[i0].x >= clip.r) {
        if (tmp[0].x != tmp[1].x || tmp[0].x < clip.l || tmp[0].x > clip.r) {
            return false;
        }
    }
    if (tmp[i0].x < clip.l) {
        tmp[i0] = ts_pt(clip.l, sect_with_vertical(src, clip.l));
    }
    if (tmp[i1].x > clip.r) {
        tmp[i1] = ts_pt(clip.r, sect_with_vertical(src, clip.r));
    }
    dst[0] = tmp[0];
    dst[1] = tmp[1];
    return true;
}

/* ---------------------------------------------------------------- edge_clipper.rs */

/* ArrayVec<PathEdge, 18> in the reference (which would panic past 18; 27 is the true maximum). */
#define CLIP_MAX 32

typedef struct pedge {
    int n; /* 2 line, 3 quad, 4 cubic */
    sv_pt p[4];
} pedge;

typedef struct clipper {
    sv_rect clip;
    bool cull_right;
    pedge e[CLIP_MAX];
    size_t n;
} clipper;

static void push_edge(clipper *c, int n, const sv_pt *p)
{
    int k;
    if (c->n >= CLIP_MAX) {
        return;
    }
    c->e[c->n].n = n;
    for (k = 0; k < n; k++) {
        c->e[c->n].p[k] = p[k];
    }
    c->n++;
}

static void push_vline(clipper *c, float x, float y0, float y1, bool reverse)
{
    sv_pt p[2];
    if (reverse) {
        float t = y0;
        y0 = y1;
        y1 = t;
    }
    p[0] = ts_pt(x, y0);
    p[1] = ts_pt(x, y1);
    push_edge(c, 2, p);
}

static void clip_line(clipper *c, sv_pt p0, sv_pt p1)
{
    sv_pt src[2];
    sv_pt pts[4];
    size_t n;
    size_t i;
    src[0] = p0;
    src[1] = p1;
    n = line_clip(src, c->clip, c->cull_right, pts);
    for (i = 0; i + 1 < n; i++) {
        push_edge(c, 2, pts + i);
    }
}

static bool sort_increasing_y(const sv_pt *src, size_t n, sv_pt *dst)
{
    size_t i;
    if (src[0].y > src[n - 1].y) {
        for (i = 0; i < n; i++) {
            dst[i] = src[n - 1 - i];
        }
        return true;
    }
    memcpy(dst, src, n * sizeof *dst);
    return false;
}

static bool chop_mono_quad_at(float c0, float c1, float c2, float target, float *t)
{
    float a = c0 - c1 - c1 + c2;
    float b = 2.0f * (c1 - c0);
    float c = c0 - target;
    float roots[3];
    if (ts_find_unit_quad_roots(a, b, c, roots) != 0) {
        *t = roots[0];
        return true;
    }
    return false;
}

static void push_quad(clipper *c, const sv_pt p[3], bool reverse)
{
    sv_pt q[3];
    if (reverse) {
        q[0] = p[2];
        q[1] = p[1];
        q[2] = p[0];
    } else {
        memcpy(q, p, sizeof q);
    }
    push_edge(c, 3, q);
}

static void chop_quad_in_y(sv_rect clip, sv_pt pts[3])
{
    float t = 0.5f;
    sv_pt tmp[5];
    int i;
    if (pts[0].y < clip.t) {
        if (chop_mono_quad_at(pts[0].y, pts[1].y, pts[2].y, clip.t, &t)) {
            ts_chop_quad_at(pts, t, tmp);
            tmp[2].y = clip.t;
            tmp[3].y = pc_rs_maxf(tmp[3].y, clip.t);
            pts[0] = tmp[2];
            pts[1] = tmp[3];
        } else {
            for (i = 0; i < 3; i++) {
                if (pts[i].y < clip.t) {
                    pts[i].y = clip.t;
                }
            }
        }
    }
    if (pts[2].y > clip.b) {
        if (chop_mono_quad_at(pts[0].y, pts[1].y, pts[2].y, clip.b, &t)) {
            ts_chop_quad_at(pts, t, tmp);
            tmp[1].y = pc_rs_minf(tmp[1].y, clip.b);
            tmp[2].y = clip.b;
            pts[1] = tmp[1];
            pts[2] = tmp[2];
        } else {
            for (i = 0; i < 3; i++) {
                if (pts[i].y > clip.b) {
                    pts[i].y = clip.b;
                }
            }
        }
    }
}

static void clip_mono_quad(clipper *c, const sv_pt src[3])
{
    sv_pt pts[3];
    bool reverse = sort_increasing_y(src, 3, pts);
    float t = 0.5f;
    sv_pt tmp[5];
    if (pts[2].y <= c->clip.t || pts[0].y >= c->clip.b) {
        return;
    }
    chop_quad_in_y(c->clip, pts);
    if (pts[0].x > pts[2].x) {
        sv_pt s = pts[0];
        pts[0] = pts[2];
        pts[2] = s;
        reverse = !reverse;
    }
    if (pts[2].x <= c->clip.l) {
        push_vline(c, c->clip.l, pts[0].y, pts[2].y, reverse);
        return;
    }
    if (pts[0].x >= c->clip.r) {
        if (!c->cull_right) {
            push_vline(c, c->clip.r, pts[0].y, pts[2].y, reverse);
        }
        return;
    }
    if (pts[0].x < c->clip.l) {
        if (chop_mono_quad_at(pts[0].x, pts[1].x, pts[2].x, c->clip.l, &t)) {
            ts_chop_quad_at(pts, t, tmp);
            push_vline(c, c->clip.l, tmp[0].y, tmp[2].y, reverse);
            tmp[2].x = c->clip.l;
            tmp[3].x = pc_rs_maxf(tmp[3].x, c->clip.l);
            pts[0] = tmp[2];
            pts[1] = tmp[3];
        } else {
            push_vline(c, c->clip.l, pts[0].y, pts[2].y, reverse);
            return;
        }
    }
    if (pts[2].x > c->clip.r) {
        if (chop_mono_quad_at(pts[0].x, pts[1].x, pts[2].x, c->clip.r, &t)) {
            ts_chop_quad_at(pts, t, tmp);
            tmp[1].x = pc_rs_minf(tmp[1].x, c->clip.r);
            tmp[2].x = c->clip.r;
            push_quad(c, tmp, reverse);
            push_vline(c, c->clip.r, tmp[2].y, tmp[4].y, reverse);
        } else {
            pts[1].x = pc_rs_minf(pts[1].x, c->clip.r);
            pts[2].x = pc_rs_minf(pts[2].x, c->clip.r);
            push_quad(c, pts, reverse);
        }
    } else {
        push_quad(c, pts, reverse);
    }
}

static void clip_quad(clipper *c, sv_pt p0, sv_pt p1, sv_pt p2)
{
    sv_pt pts[3];
    sv_rect b;
    sv_pt my[5];
    size_t ny;
    size_t y;
    pts[0] = p0;
    pts[1] = p1;
    pts[2] = p2;
    if (!ts_rect_from_points(pts, 3, &b)) {
        return;
    }
    if (b.t >= c->clip.b || b.b <= c->clip.t) {
        return;
    }
    ny = ts_chop_quad_at_y_extrema(pts, my);
    for (y = 0; y <= ny; y++) {
        sv_pt mx[5];
        size_t nx = ts_chop_quad_at_x_extrema(my + y * 2, mx);
        size_t x;
        for (x = 0; x <= nx; x++) {
            clip_mono_quad(c, mx + x * 2);
        }
    }
}

static float mono_cubic_closest_t(const float s[4], float x)
{
    float t = 0.5f;
    float last;
    float best = t;
    float step = 0.25f;
    float d = s[0];
    float a = s[3] + 3.0f * (s[1] - s[2]) - d;
    float b = 3.0f * (s[2] - s[1] - s[1] + d);
    float c = 3.0f * (s[1] - d);
    float closest = TS_SCALAR_MAX;
    int iter;
    x -= d;
    /* t moves by halving steps, so it settles within ~150 rounds; capped regardless */
    for (iter = 0; iter < 1024; iter++) {
        float loc = ((a * t + b) * t + c) * t;
        float dist = fabsf(loc - x);
        if (closest > dist) {
            closest = dist;
            best = t;
        }
        last = t;
        t += loc < x ? step : -step;
        step *= 0.5f;
        if (!(closest > 0.25f && last != t)) {
            break;
        }
    }
    /* NormalizedF32Exclusive::new(best).unwrap() */
    return best > 0.0f && best < 1.0f ? best : 0.5f;
}

static void chop_mono_cubic_at(const sv_pt src[4], float v, bool x, sv_pt dst[7])
{
    float s[4];
    int i;
    if (x ? ts_chop_mono_cubic_at_x(src, v, dst) : ts_chop_mono_cubic_at_y(src, v, dst)) {
        return;
    }
    for (i = 0; i < 4; i++) {
        s[i] = x ? src[i].x : src[i].y;
    }
    ts_chop_cubic_at2(src, mono_cubic_closest_t(s, v), dst);
}

static void chop_cubic_in_y(sv_rect clip, sv_pt pts[4])
{
    if (pts[0].y < clip.t) {
        sv_pt tmp[7];
        chop_mono_cubic_at(pts, clip.t, false, tmp);
        if (tmp[3].y < clip.t && tmp[4].y < clip.t && tmp[5].y < clip.t) {
            sv_pt t2[4];
            memcpy(t2, tmp + 3, sizeof t2);
            chop_mono_cubic_at(t2, clip.t, false, tmp);
        }
        tmp[3].y = clip.t;
        tmp[4].y = pc_rs_maxf(tmp[4].y, clip.t);
        pts[0] = tmp[3];
        pts[1] = tmp[4];
        pts[2] = tmp[5];
    }
    if (pts[3].y > clip.b) {
        sv_pt tmp[7];
        chop_mono_cubic_at(pts, clip.b, false, tmp);
        tmp[3].y = clip.b;
        tmp[2].y = pc_rs_minf(tmp[2].y, clip.b);
        pts[1] = tmp[1];
        pts[2] = tmp[2];
        pts[3] = tmp[3];
    }
}

static void push_cubic(clipper *c, const sv_pt p[4], bool reverse)
{
    sv_pt q[4];
    if (reverse) {
        q[0] = p[3];
        q[1] = p[2];
        q[2] = p[1];
        q[3] = p[0];
    } else {
        memcpy(q, p, sizeof q);
    }
    push_edge(c, 4, q);
}

static void clip_mono_cubic(clipper *c, const sv_pt src[4])
{
    sv_pt pts[4];
    bool reverse = sort_increasing_y(src, 4, pts);
    if (pts[3].y <= c->clip.t || pts[0].y >= c->clip.b) {
        return;
    }
    chop_cubic_in_y(c->clip, pts);
    if (pts[0].x > pts[3].x) {
        sv_pt s = pts[0];
        pts[0] = pts[3];
        pts[3] = s;
        s = pts[1];
        pts[1] = pts[2];
        pts[2] = s;
        reverse = !reverse;
    }
    if (pts[3].x <= c->clip.l) {
        push_vline(c, c->clip.l, pts[0].y, pts[3].y, reverse);
        return;
    }
    if (pts[0].x >= c->clip.r) {
        if (!c->cull_right) {
            push_vline(c, c->clip.r, pts[0].y, pts[3].y, reverse);
        }
        return;
    }
    if (pts[0].x < c->clip.l) {
        sv_pt tmp[7];
        chop_mono_cubic_at(pts, c->clip.l, true, tmp);
        push_vline(c, c->clip.l, tmp[0].y, tmp[3].y, reverse);
        tmp[3].x = c->clip.l;
        tmp[4].x = pc_rs_maxf(tmp[4].x, c->clip.l);
        pts[0] = tmp[3];
        pts[1] = tmp[4];
        pts[2] = tmp[5];
    }
    if (pts[3].x > c->clip.r) {
        sv_pt tmp[7];
        chop_mono_cubic_at(pts, c->clip.r, true, tmp);
        tmp[3].x = c->clip.r;
        tmp[2].x = pc_rs_minf(tmp[2].x, c->clip.r);
        push_cubic(c, tmp, reverse);
        push_vline(c, c->clip.r, tmp[3].y, tmp[6].y, reverse);
    } else {
        push_cubic(c, pts, reverse);
    }
}

static void clip_cubic(clipper *c, sv_pt p0, sv_pt p1, sv_pt p2, sv_pt p3)
{
    sv_pt pts[4];
    sv_rect b;
    const float lim = 4194304.0f;
    pts[0] = p0;
    pts[1] = p1;
    pts[2] = p2;
    pts[3] = p3;
    if (!ts_rect_from_points(pts, 4, &b)) {
        return;
    }
    if (b.b > c->clip.t && b.t < c->clip.b) {
        if (b.l < -lim || b.t < -lim || b.r > lim || b.b > lim) {
            clip_line(c, p0, p3);
        } else {
            sv_pt my[10];
            size_t ny = ts_chop_cubic_at_y_extrema(pts, my);
            size_t y;
            for (y = 0; y <= ny; y++) {
                sv_pt mx[10];
                size_t nx = ts_chop_cubic_at_x_extrema(my + y * 3, mx);
                size_t x;
                for (x = 0; x <= nx; x++) {
                    clip_mono_cubic(c, mx + x * 3);
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- edge_builder.rs */

typedef struct builder {
    ts_edge *e;
    size_t n, cap;
    int32_t shift;
    pc_status st;
} builder;

static ts_edge *new_edge(builder *b)
{
    if (b->st != PC_OK) {
        return NULL;
    }
    if (b->n >= TS_MAX_EDGES) {
        b->st = PC_ERR_LIMIT;
        return NULL;
    }
    if (b->n == b->cap) {
        size_t cap = b->cap;
        void *v = pc_grow_array(b->e, &cap, b->n + 1, sizeof *b->e);
        if (v == NULL) {
            b->st = PC_ERR_NOMEM;
            return NULL;
        }
        b->e = v;
        b->cap = cap;
    }
    return &b->e[b->n];
}

typedef enum { C_NO, C_PARTIAL, C_TOTAL } combine;

static combine combine_vertical(const ts_edge *edge, ts_edge *last)
{
    if (last->dx != 0 || edge->x != last->x) {
        return C_NO;
    }
    if (edge->winding == last->winding) {
        if (ts_wadd(edge->last_y, 1) == last->first_y) {
            last->first_y = edge->first_y;
            return C_PARTIAL;
        }
        if (edge->first_y == ts_wadd(last->last_y, 1)) {
            last->last_y = edge->last_y;
            return C_PARTIAL;
        }
        return C_NO;
    }
    if (edge->first_y == last->first_y) {
        if (edge->last_y == last->last_y) {
            return C_TOTAL;
        }
        if (edge->last_y < last->last_y) {
            last->first_y = ts_wadd(edge->last_y, 1);
            return C_PARTIAL;
        }
        last->first_y = ts_wadd(last->last_y, 1);
        last->last_y = edge->last_y;
        last->winding = edge->winding;
        return C_PARTIAL;
    }
    if (edge->last_y == last->last_y) {
        if (edge->first_y > last->first_y) {
            last->last_y = ts_wsub(edge->first_y, 1);
        } else {
            last->last_y = ts_wsub(last->first_y, 1);
            last->first_y = edge->first_y;
            last->winding = edge->winding;
        }
        return C_PARTIAL;
    }
    return C_NO;
}

static void push_line(builder *b, sv_pt p0, sv_pt p1)
{
    ts_edge e;
    ts_edge *slot;
    if (!line_new(&e, p0, p1, b->shift)) {
        return;
    }
    if (e.dx == 0 && b->n > 0 && b->e[b->n - 1].kind == TS_EDGE_LINE) {
        combine c = combine_vertical(&e, &b->e[b->n - 1]);
        if (c == C_TOTAL) {
            b->n--;
            return;
        }
        if (c == C_PARTIAL) {
            return;
        }
    }
    slot = new_edge(b);
    if (slot != NULL) {
        *slot = e;
        b->n++;
    }
}

static void push_quad_edge(builder *b, const sv_pt p[3])
{
    ts_edge e;
    ts_edge *slot;
    if (!quad_new(&e, p, b->shift)) {
        return;
    }
    slot = new_edge(b);
    if (slot != NULL) {
        *slot = e;
        b->n++;
    }
}

static void push_cubic_edge(builder *b, const sv_pt p[4])
{
    ts_edge e;
    ts_edge *slot;
    if (!cubic_new(&e, p, b->shift)) {
        return;
    }
    slot = new_edge(b);
    if (slot != NULL) {
        *slot = e;
        b->n++;
    }
}

/* PathEdgeIter: the path's line / quad / cubic edges with implicit closing lines. */
typedef struct eiter {
    const ts_path *p;
    size_t vi, pi;
    sv_pt move_to;
    bool needs_close;
} eiter;

/* 0 = end, else the point count (2..4) of the edge in out. */
static int eiter_next(eiter *it, sv_pt out[4])
{
    const ts_path *p = it->p;
    while (it->vi < p->nv) {
        uint8_t v = p->verbs[it->vi++];
        if (v == V_MOVE) {
            if (it->pi >= p->np) {
                return 0;
            }
            if (it->needs_close) {
                it->needs_close = false;
                out[0] = it->pi > 0 ? p->pts[it->pi - 1] : it->move_to;
                out[1] = it->move_to;
                it->move_to = p->pts[it->pi++];
                return 2;
            }
            it->move_to = p->pts[it->pi++];
        } else if (v == V_CLOSE) {
            if (it->needs_close) {
                it->needs_close = false;
                out[0] = it->pi > 0 ? p->pts[it->pi - 1] : it->move_to;
                out[1] = it->move_to;
                return 2;
            }
        } else {
            int n = v == V_LINE ? 1 : v == V_QUAD ? 2 : v == V_CUBIC ? 3 : 0;
            int k;
            if (n == 0 || it->pi == 0 || it->pi + (size_t)n > p->np) {
                return 0;
            }
            it->needs_close = true;
            out[0] = p->pts[it->pi - 1];
            for (k = 0; k < n; k++) {
                out[1 + k] = p->pts[it->pi + (size_t)k];
            }
            it->pi += (size_t)n;
            return n + 1;
        }
    }
    if (it->needs_close) {
        it->needs_close = false;
        out[0] = it->pi > 0 ? p->pts[it->pi - 1] : it->move_to;
        out[1] = it->move_to;
        return 2;
    }
    return 0;
}

static bool all_finite(const sv_pt *p, int n)
{
    int k;
    for (k = 0; k < n; k++) {
        if (!ts_pt_is_finite(p[k])) {
            return false;
        }
    }
    return true;
}

pc_status ts_build_edges(const ts_path *path, const sv_rect *clip, int32_t shift, ts_edge **out, size_t *nout)
{
    builder b;
    eiter it;
    sv_pt seg[4];
    int n;
    memset(&b, 0, sizeof b);
    b.shift = shift;
    memset(&it, 0, sizeof it);
    it.p = path;
    *out = NULL;
    *nout = 0;
    while ((n = eiter_next(&it, seg)) != 0 && b.st == PC_OK) {
        if (clip != NULL) {
            clipper c;
            size_t i;
            c.clip = *clip;
            c.cull_right = false;
            c.n = 0;
            if (n == 2) {
                clip_line(&c, seg[0], seg[1]);
            } else if (n == 3) {
                clip_quad(&c, seg[0], seg[1], seg[2]);
            } else {
                clip_cubic(&c, seg[0], seg[1], seg[2], seg[3]);
            }
            for (i = 0; i < c.n; i++) {
                if (!all_finite(c.e[i].p, c.e[i].n)) {
                    free(b.e);
                    return PC_OK; /* "infinite or NaN segments": nothing is drawn */
                }
                if (c.e[i].n == 2) {
                    push_line(&b, c.e[i].p[0], c.e[i].p[1]);
                } else if (c.e[i].n == 3) {
                    push_quad_edge(&b, c.e[i].p);
                } else {
                    push_cubic_edge(&b, c.e[i].p);
                }
            }
        } else if (n == 2) {
            push_line(&b, seg[0], seg[1]);
        } else if (n == 3) {
            sv_pt mono[5];
            size_t k = ts_chop_quad_at_y_extrema(seg, mono);
            size_t i;
            for (i = 0; i <= k; i++) {
                push_quad_edge(&b, mono + i * 2);
            }
        } else {
            sv_pt mono[10];
            size_t k = ts_chop_cubic_at_y_extrema(seg, mono);
            size_t i;
            for (i = 0; i <= k; i++) {
                push_cubic_edge(&b, mono + i * 3);
            }
        }
    }
    if (b.st != PC_OK || b.n < 2) {
        free(b.e);
        return b.st;
    }
    *out = b.e;
    *nout = b.n;
    return PC_OK;
}
