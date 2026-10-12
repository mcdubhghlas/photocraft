/* tiny-skia 0.11.4 scan/hairline.rs (hairline strokes: caps as end extensions, curves cut into
 * lines, aliased Bresenham-style lines) and scan/hairline_aa.rs (anti-aliased hairlines with
 * their cap / line blitters, the RectClipBlitter, fill_rect_aa), for the SVG rasteriser.
 * Coordinates are pre-clipped to +-32767 like the reference, so fixed-point maths stays in
 * range; the line subdivision recursion is bounded by that (and by an explicit depth cap). */
#include <string.h>

#include "io/svg_ts_edge.h"

#define FLOAT_PI 3.14159265f
#define MAX_CUBIC_LEVEL 9
#define MAX_QUAD_LEVEL 5
#define HALF16 (1 << 15)
#define ONE16 (1 << 16)

/* fdot16::from_f32 */
static int32_t fixed_f(float v)
{
    float x = v * 65536.0f;
    x = x < 2147483520.0f ? x : 2147483520.0f;
    x = x > -2147483520.0f ? x : -2147483520.0f;
    return (int32_t)x;
}

/* ---------------------------------------------------------------- blitters */

/* The RasterPipelineBlitter (clipped false) or a RectClipBlitter around it. */
typedef struct hb {
    const sv_spans *out;
    uint32_t ox, oy;
    bool clipped;
    ts_clip clip;
} hb;

static void out1(const hb *b, uint32_t x, uint32_t y, uint32_t w, uint8_t a)
{
    ts_emit(b->out, x + b->ox, y + b->oy, w, a);
}

/* blit_anti_h with one run of n pixels at alpha a */
static void hb_run(const hb *b, uint32_t x, uint32_t y, uint32_t n, uint8_t a)
{
    uint32_t x0 = x;
    uint32_t x1;
    if (!b->clipped) {
        out1(b, x, y, n, a);
        return;
    }
    if (!(y - b->clip.y < b->clip.h) || x >= b->clip.x + b->clip.w) {
        return;
    }
    x1 = x + n;
    if (x1 <= b->clip.x) {
        return;
    }
    if (x0 < b->clip.x) {
        x0 = b->clip.x;
    }
    if (x1 > b->clip.x + b->clip.w) {
        x1 = b->clip.x + b->clip.w;
    }
    if (x0 < x1) {
        out1(b, x0, y, x1 - x0, a);
    }
}

static void hb_v(const hb *b, uint32_t x, uint32_t y, uint32_t h, uint8_t a)
{
    uint32_t y0 = y;
    uint32_t y1 = y + h;
    uint32_t k;
    if (b->clipped) {
        if (!(x - b->clip.x < b->clip.w)) {
            return;
        }
        if (y0 < b->clip.y) {
            y0 = b->clip.y;
        }
        if (y1 > b->clip.y + b->clip.h) {
            y1 = b->clip.y + b->clip.h;
        }
    }
    for (k = y0; k < y1; k++) {
        out1(b, x, k, 1, a);
    }
}

static void hb_anti_h2(const hb *b, uint32_t x, uint32_t y, uint8_t a0, uint8_t a1)
{
    if (!b->clipped) {
        out1(b, x, y, 1, a0);
        out1(b, x + 1, y, 1, a1);
        return;
    }
    if (!(y - b->clip.y < b->clip.h) || x >= b->clip.x + b->clip.w || x + 2 <= b->clip.x) {
        return;
    }
    if (x >= b->clip.x) {
        out1(b, x, y, 1, a0);
    }
    if (x + 1 >= b->clip.x && x + 1 < b->clip.x + b->clip.w) {
        out1(b, x + 1, y, 1, a1);
    }
}

static void hb_anti_v2(const hb *b, uint32_t x, uint32_t y, uint8_t a0, uint8_t a1)
{
    if (!b->clipped) {
        out1(b, x, y, 1, a0);
        out1(b, x, y + 1, 1, a1);
        return;
    }
    hb_run(b, x, y, 1, a0);
    hb_run(b, x, y + 1, 1, a1);
}

static uint8_t small_scale(uint8_t v, int32_t dot6)
{
    return (uint8_t)(((int32_t)v * dot6) >> 6);
}

static uint8_t alpha_mul(uint8_t v, int32_t a256)
{
    return (uint8_t)(ts_wmul((int32_t)v, a256) >> 8);
}

static uint32_t sub1_floor0(uint32_t v)
{
    return (v > 1 ? v : 1) - 1;
}

/* ---------------------------------------------------------------- anti-hair blitters */

enum { K_HLINE, K_HORISH, K_VLINE, K_VERTISH };

static int32_t draw_cap(const hb *b, int kind, uint32_t x, int32_t f, int32_t slope, int32_t mod64)
{
    uint32_t i;
    uint8_t a;
    f = ts_wadd(f, HALF16);
    f = f > 0 ? f : 0;
    i = (uint32_t)(f >> 16);
    a = (uint8_t)((f >> 8) & 0xFF);
    switch (kind) {
    case K_HLINE: {
        uint8_t ma = small_scale(a, mod64);
        if (ma != 0) {
            hb_run(b, x, i, 1, ma);
        }
        ma = small_scale((uint8_t)(255 - a), mod64);
        if (ma != 0 && i >= 1) {
            hb_run(b, x, i - 1, 1, ma);
        }
        return ts_wsub(f, HALF16);
    }
    case K_HORISH:
        hb_anti_v2(b, x, sub1_floor0(i), small_scale((uint8_t)(255 - a), mod64), small_scale(a, mod64));
        return ts_wsub(ts_wadd(f, slope), HALF16);
    case K_VLINE: {
        uint8_t ma = small_scale(a, mod64);
        if (ma != 0) {
            hb_v(b, i, x, 1, ma);
        }
        ma = small_scale((uint8_t)(255 - a), mod64);
        if (ma != 0) {
            hb_v(b, sub1_floor0(i), x, 1, ma);
        }
        return ts_wsub(f, HALF16);
    }
    default:
        hb_anti_h2(b, sub1_floor0(i), x, small_scale((uint8_t)(255 - a), mod64), small_scale(a, mod64));
        return ts_wsub(ts_wadd(f, slope), HALF16);
    }
}

static int32_t draw_line(const hb *b, int kind, uint32_t x, uint32_t stop, int32_t f, int32_t slope)
{
    uint32_t i;
    uint8_t a;
    if (kind == K_HLINE || kind == K_VLINE) {
        uint32_t n = stop - x;
        if (n == 0) {
            return f;
        }
        f = ts_wadd(f, HALF16);
        f = f > 0 ? f : 0;
        i = (uint32_t)(f >> 16);
        a = (uint8_t)((f >> 8) & 0xFF);
        if (kind == K_HLINE) {
            if (a != 0) {
                hb_run(b, x, i, n, a);
            }
            a = (uint8_t)(255 - a);
            if (a != 0 && i >= 1) {
                hb_run(b, x, i - 1, n, a);
            }
        } else {
            if (a != 0) {
                hb_v(b, i, x, n, a);
            }
            a = (uint8_t)(255 - a);
            if (a != 0) {
                hb_v(b, sub1_floor0(i), x, n, a);
            }
        }
        return ts_wsub(f, HALF16);
    }
    f = ts_wadd(f, HALF16);
    for (;;) {
        f = f > 0 ? f : 0;
        i = (uint32_t)(f >> 16);
        a = (uint8_t)((f >> 8) & 0xFF);
        if (kind == K_HORISH) {
            hb_anti_v2(b, x, sub1_floor0(i), (uint8_t)(255 - a), a);
        } else {
            hb_anti_h2(b, sub1_floor0(i), x, (uint8_t)(255 - a), a);
        }
        f = ts_wadd(f, slope);
        x += 1;
        if (x >= stop) {
            break;
        }
    }
    return ts_wsub(f, HALF16);
}

static int32_t contribution_64(int32_t v)
{
    return (ts_wsub(v, 1) & 63) + 1;
}

static int32_t floor16(int32_t x)
{
    return x >> 16;
}

static int32_t ceil16(int32_t x)
{
    return ts_wadd(x, ONE16 - 1) >> 16;
}

static int32_t any_bad_ints(int32_t a, int32_t b, int32_t c, int32_t d)
{
    int32_t v = (a & ts_wsub(0, a)) | (b & ts_wsub(0, b)) | (c & ts_wsub(0, c)) | (d & ts_wsub(0, d));
    return v >> 31;
}

static void do_anti_hairline(int32_t x0, int32_t y0, int32_t x1, int32_t y1, const ts_irect *clip_in,
                             const sv_spans *out, uint32_t ox, uint32_t oy, int depth)
{
    int32_t scale_start;
    int32_t scale_stop;
    int32_t istart;
    int32_t istop;
    int32_t fstart;
    int32_t slope;
    int kind;
    bool clipping = clip_in != NULL;
    hb b;
    uint32_t us;
    uint32_t ue;
    int64_t full;
    if (any_bad_ints(x0, y0, x1, y1) != 0 || depth > 32) {
        return;
    }
    if (ts_abs(ts_wsub(x1, x0)) > 511 * 64 || ts_abs(ts_wsub(y1, y0)) > 511 * 64) {
        int32_t hx = ts_wadd(x0 >> 1, x1 >> 1);
        int32_t hy = ts_wadd(y0 >> 1, y1 >> 1);
        do_anti_hairline(x0, y0, hx, hy, clip_in, out, ox, oy, depth + 1);
        do_anti_hairline(hx, hy, x1, y1, clip_in, out, ox, oy, depth + 1);
        return;
    }
    if (ts_abs(ts_wsub(x1, x0)) > ts_abs(ts_wsub(y1, y0))) {
        if (x0 > x1) {
            int32_t t = x0;
            x0 = x1;
            x1 = t;
            t = y0;
            y0 = y1;
            y1 = t;
        }
        istart = x0 >> 6;
        istop = ts_wadd(x1, 63) >> 6;
        fstart = ts_shl(y0, 10);
        if (y0 == y1) {
            slope = 0;
            kind = K_HLINE;
        } else {
            slope = ts_fdot16_fast_div(ts_wsub(y1, y0), ts_wsub(x1, x0));
            fstart = ts_wadd(fstart, ts_wadd(ts_wmul(slope, 32 - (x0 & 63)), 32) >> 6);
            kind = K_HORISH;
        }
        if (ts_wsub(istop, istart) == 1) {
            scale_start = ts_wsub(x1, x0);
            scale_stop = 0;
        } else {
            scale_start = 64 - (x0 & 63);
            scale_stop = x1 & 63;
        }
        if (clipping) {
            ts_irect c = *clip_in;
            int32_t top;
            int32_t bottom;
            if (istart >= ts_irect_r(c) || istop <= c.x) {
                return;
            }
            if (istart < c.x) {
                fstart = ts_wadd(fstart, ts_wmul(slope, c.x - istart));
                istart = c.x;
                scale_start = 64;
                if (istop - istart == 1) {
                    scale_start = contribution_64(x1);
                    scale_stop = 0;
                }
            }
            if (istop > ts_irect_r(c)) {
                istop = ts_irect_r(c);
                scale_stop = 0;
            }
            if (istart == istop) {
                return;
            }
            if (slope >= 0) {
                top = floor16(ts_wsub(fstart, HALF16));
                bottom = ceil16(ts_wadd(ts_wadd(fstart, ts_wmul(istop - istart - 1, slope)), HALF16));
            } else {
                bottom = ceil16(ts_wadd(fstart, HALF16));
                top = floor16(ts_wsub(ts_wadd(fstart, ts_wmul(istop - istart - 1, slope)), HALF16));
            }
            top -= 1;
            bottom += 1;
            if (top >= ts_irect_b(c) || bottom <= c.y) {
                return;
            }
            if (c.y <= top && ts_irect_b(c) >= bottom) {
                clipping = false;
            }
        }
    } else {
        if (y0 > y1) {
            int32_t t = x0;
            x0 = x1;
            x1 = t;
            t = y0;
            y0 = y1;
            y1 = t;
        }
        istart = y0 >> 6;
        istop = ts_wadd(y1, 63) >> 6;
        fstart = ts_shl(x0, 10);
        if (x0 == x1) {
            if (y0 == y1) {
                return;
            }
            slope = 0;
            kind = K_VLINE;
        } else {
            slope = ts_fdot16_fast_div(ts_wsub(x1, x0), ts_wsub(y1, y0));
            fstart = ts_wadd(fstart, ts_wadd(ts_wmul(slope, 32 - (y0 & 63)), 32) >> 6);
            kind = K_VERTISH;
        }
        if (ts_wsub(istop, istart) == 1) {
            scale_start = ts_wsub(y1, y0);
            scale_stop = 0;
        } else {
            scale_start = 64 - (y0 & 63);
            scale_stop = y1 & 63;
        }
        if (clipping) {
            ts_irect c = *clip_in;
            int32_t left;
            int32_t right;
            if (istart >= ts_irect_b(c) || istop <= c.y) {
                return;
            }
            if (istart < c.y) {
                fstart = ts_wadd(fstart, ts_wmul(slope, c.y - istart));
                istart = c.y;
                scale_start = 64;
                if (istop - istart == 1) {
                    scale_start = contribution_64(y1);
                    scale_stop = 0;
                }
            }
            if (istop > ts_irect_b(c)) {
                istop = ts_irect_b(c);
                scale_stop = 0;
            }
            if (istart == istop) {
                return;
            }
            if (slope >= 0) {
                left = floor16(ts_wsub(fstart, HALF16));
                right = ceil16(ts_wadd(ts_wadd(fstart, ts_wmul(istop - istart - 1, slope)), HALF16));
            } else {
                right = ceil16(ts_wadd(fstart, HALF16));
                left = floor16(ts_wsub(ts_wadd(fstart, ts_wmul(istop - istart - 1, slope)), HALF16));
            }
            left -= 1;
            right += 1;
            if (left >= ts_irect_r(c) || right <= c.x) {
                return;
            }
            if (c.x <= left && ts_irect_r(c) >= right) {
                clipping = false;
            }
        }
    }
    if (istart < 0 || istop < 0) {
        return; /* debug_assert in the reference */
    }
    b.out = out;
    b.ox = ox;
    b.oy = oy;
    b.clipped = clipping;
    if (clipping) {
        b.clip.x = (uint32_t)clip_in->x;
        b.clip.y = (uint32_t)clip_in->y;
        b.clip.w = clip_in->w;
        b.clip.h = clip_in->h;
    }
    us = (uint32_t)istart;
    ue = (uint32_t)istop;
    fstart = draw_cap(&b, kind, us, fstart, slope, scale_start);
    us += 1;
    full = (int64_t)ue - us - (scale_stop > 0 ? 1 : 0);
    if (full > 0) {
        fstart = draw_line(&b, kind, us, us + (uint32_t)full, fstart, slope);
    }
    if (scale_stop > 0 && ue >= 1) {
        (void)draw_cap(&b, kind, ue - 1, fstart, slope, scale_stop);
    }
}

static sv_rect clip_rect_f(ts_clip c)
{
    sv_rect r;
    r.l = (float)c.x;
    r.t = (float)c.y;
    r.r = (float)c.x + (float)c.w;
    r.b = (float)c.y + (float)c.h;
    return r;
}

static int32_t fdot6_f(float v)
{
    return pc_f32_to_i32(v * 64.0f);
}

typedef struct hctx {
    bool aa;
    const sv_spans *out;
    uint32_t ox, oy;
} hctx;

/* anti_hair_line_rgn / hair_line_rgn over the polyline pts[0..n). */
static void line_proc(const hctx *h, const sv_pt *pts, size_t n, const ts_clip *clip)
{
    sv_rect fixed = {-32767.0f, -32767.0f, 32767.0f, 32767.0f};
    sv_rect cb;
    bool have_cb = false;
    size_t i;
    if (clip != NULL) {
        cb = clip_rect_f(*clip);
        if (h->aa) {
            have_cb = ts_rect_ltrb(cb.l - 1.0f, cb.t - 1.0f, cb.r + 1.0f, cb.b + 1.0f, &cb);
        } else {
            have_cb = true;
        }
    }
    for (i = 0; i + 1 < n; i++) {
        sv_pt seg[2];
        sv_pt p[2];
        int32_t x0;
        int32_t y0;
        int32_t x1;
        int32_t y1;
        seg[0] = pts[i];
        seg[1] = pts[i + 1];
        if (!ts_line_intersect(seg, fixed, p)) {
            continue;
        }
        if (have_cb) {
            sv_pt tmp[2];
            tmp[0] = p[0];
            tmp[1] = p[1];
            if (!ts_line_intersect(tmp, cb, p)) {
                continue;
            }
        }
        x0 = fdot6_f(p[0].x);
        y0 = fdot6_f(p[0].y);
        x1 = fdot6_f(p[1].x);
        y1 = fdot6_f(p[1].y);
        if (h->aa) {
            if (clip != NULL) {
                ts_irect ir;
                ts_irect ci;
                ts_irect sub;
                ci.x = (int32_t)clip->x;
                ci.y = (int32_t)clip->y;
                ci.w = clip->w;
                ci.h = clip->h;
                if (!ts_irect_ltrb(ts_wsub((x0 < x1 ? x0 : x1) >> 6, 1), ts_wsub((y0 < y1 ? y0 : y1) >> 6, 1),
                                   ts_wadd(ts_wadd(x0 > x1 ? x0 : x1, 63) >> 6, 1),
                                   ts_wadd(ts_wadd(y0 > y1 ? y0 : y1, 63) >> 6, 1), &ir)) {
                    return;
                }
                if (!ts_irect_intersect(ci, ir, &sub)) {
                    continue;
                }
                if (!ts_irect_contains(ci, ir)) {
                    if (ts_irect_intersect(ir, ci, &sub) && sub.x >= 0 && sub.y >= 0) {
                        do_anti_hairline(x0, y0, x1, y1, &sub, h->out, h->ox, h->oy, 0);
                    }
                    continue;
                }
            }
            do_anti_hairline(x0, y0, x1, y1, NULL, h->out, h->ox, h->oy, 0);
        } else {
            int32_t dx = ts_wsub(x1, x0);
            int32_t dy = ts_wsub(y1, y0);
            if (ts_abs(dx) > ts_abs(dy)) {
                int32_t ix0;
                int32_t ix1;
                int32_t slope;
                int32_t sy;
                int32_t max_y;
                if (x0 > x1) { /* swap the ends (y1 is not used again) */
                    int32_t t = x0;
                    x0 = x1;
                    x1 = t;
                    y0 = y1;
                }
                ix0 = ts_wadd(x0, 32) >> 6;
                ix1 = ts_wadd(x1, 32) >> 6;
                if (ix0 == ix1) {
                    continue;
                }
                slope = ts_fdot16_div(dy, dx);
                sy = ts_wadd(ts_shl(y0, 10), ts_wmul(slope, (32 - x0) & 63) >> 6);
                max_y = have_cb ? fixed_f(cb.b) : INT32_MAX;
                do {
                    if (ix0 >= 0 && sy >= 0 && sy < max_y) {
                        ts_emit(h->out, (uint32_t)ix0 + h->ox, (uint32_t)(sy >> 16) + h->oy, 1, 255);
                    }
                    sy = ts_wadd(sy, slope);
                    ix0++;
                } while (ix0 < ix1);
            } else {
                int32_t iy0;
                int32_t iy1;
                int32_t slope;
                int32_t sx;
                if (y0 > y1) { /* swap the ends (x1 is not used again) */
                    int32_t t = y0;
                    x0 = x1;
                    y0 = y1;
                    y1 = t;
                }
                iy0 = ts_wadd(y0, 32) >> 6;
                iy1 = ts_wadd(y1, 32) >> 6;
                if (iy0 == iy1) {
                    continue;
                }
                slope = ts_fdot16_div(dx, dy);
                sx = ts_wadd(ts_shl(x0, 10), ts_wmul(slope, (32 - y0) & 63) >> 6);
                do {
                    if (sx >= 0 && iy0 >= 0) {
                        ts_emit(h->out, (uint32_t)(sx >> 16) + h->ox, (uint32_t)iy0 + h->oy, 1, 255);
                    }
                    sx = ts_wadd(sx, slope);
                    iy0++;
                } while (iy0 < iy1);
            }
        }
    }
}

/* ---------------------------------------------------------------- curves (hairline.rs) */

static bool nocheck_bounds(const sv_pt *p, int n, sv_rect *out)
{
    float mnx = p[0].x;
    float mny = p[0].y;
    float mxx = p[0].x;
    float mxy = p[0].y;
    int i;
    for (i = 1; i < n; i++) {
        /* f32x2 pmin / pmax */
        mnx = p[i].x < mnx ? p[i].x : mnx;
        mny = p[i].y < mny ? p[i].y : mny;
        mxx = mxx < p[i].x ? p[i].x : mxx;
        mxy = mxy < p[i].y ? p[i].y : mxy;
    }
    return ts_rect_ltrb(mnx, mny, mxx, mxy, out);
}

static bool overlap(sv_rect a, sv_rect b)
{
    return a.l < b.r && b.l < a.r && a.t < b.b && b.t < a.b;
}

static bool contains(sv_rect o, sv_rect i)
{
    return i.r <= o.r && i.l >= o.l && i.b <= o.b && i.t >= o.t;
}

static sv_rect irect_f(ts_irect r)
{
    sv_rect o;
    o.l = (float)r.x;
    o.t = (float)r.y;
    o.r = (float)r.x + (float)r.w;
    o.b = (float)r.y + (float)r.h;
    return o;
}

typedef struct hstate {
    hctx h;
    const ts_clip *clip;
    bool have_inset;
    ts_irect inset, outset;
} hstate;

/* hair_quad / hair_cubic culling: false when the curve is outside; *clip set to NULL when it is
 * well inside. */
static bool cull(const hstate *s, const sv_pt *p, int n, const ts_clip **clip)
{
    sv_rect b;
    *clip = s->clip;
    if (!s->have_inset) {
        return true;
    }
    if (!nocheck_bounds(p, n, &b)) {
        return false;
    }
    if (!overlap(irect_f(s->outset), b)) {
        return false;
    }
    if (contains(irect_f(s->inset), b)) {
        *clip = NULL;
    }
    return true;
}

static uint32_t quad_dist(const sv_pt p[3])
{
    float dx = fabsf((p[0].x + p[2].x) * 0.5f - p[1].x);
    float dy = fabsf((p[0].y + p[2].y) * 0.5f - p[1].y);
    float cx = ceilf(dx);
    float cy = ceilf(dy);
    uint32_t idx;
    uint32_t idy;
    cx = cx < 2147483520.0f ? cx : 2147483520.0f;
    cx = cx > -2147483520.0f ? cx : -2147483520.0f;
    cy = cy < 2147483520.0f ? cy : 2147483520.0f;
    cy = cy > -2147483520.0f ? cy : -2147483520.0f;
    idx = (uint32_t)(int32_t)cx;
    idy = (uint32_t)(int32_t)cy;
    return idx > idy ? idx + (idy >> 1) : idy + (idx >> 1);
}

static void hair_quad(const hstate *s, const sv_pt p[3])
{
    const ts_clip *clip;
    uint32_t d;
    uint32_t level;
    uint32_t lines;
    sv_pt tmp[(1 << MAX_QUAD_LEVEL) + 1];
    sv_pt c;
    sv_pt b;
    sv_pt a;
    float t = 0.0f;
    float dt;
    uint32_t i;
    d = quad_dist(p);
    {
        uint32_t lz = 0;
        uint32_t v = d;
        if (v == 0) {
            lz = 32;
        } else {
            while ((v & 0x80000000u) == 0) {
                v <<= 1;
                lz++;
            }
        }
        level = (33 - lz) >> 1;
    }
    if (level > MAX_QUAD_LEVEL) {
        level = MAX_QUAD_LEVEL;
    }
    if (!cull(s, p, 3, &clip)) {
        return;
    }
    lines = 1u << level;
    c = p[0];
    b = ts_add(ts_sub(p[1], c), ts_sub(p[1], c));
    a = ts_add(ts_sub(p[2], ts_add(p[1], p[1])), c);
    tmp[0] = p[0];
    dt = 1.0f / (float)lines;
    for (i = 1; i < lines; i++) {
        t = t + dt;
        tmp[i] = ts_pt((a.x * t + b.x) * t + c.x, (a.y * t + b.y) * t + c.y);
    }
    tmp[lines] = p[2];
    line_proc(&s->h, tmp, lines + 1, clip);
}

static uint32_t cubic_segments(const sv_pt p[4])
{
    const float k1 = 1.0f / 3.0f;
    const float k2 = 2.0f / 3.0f;
    float p13x = k1 * p[3].x + k2 * p[0].x;
    float p13y = k1 * p[3].y + k2 * p[0].y;
    float p23x = k1 * p[0].x + k2 * p[3].x;
    float p23y = k1 * p[0].y + k2 * p[3].y;
    float ax = fabsf(p[1].x - p13x);
    float ay = fabsf(p[1].y - p13y);
    float bx = fabsf(p[2].x - p23x);
    float by = fabsf(p[2].y - p23y);
    float mx = ax < bx ? bx : ax; /* pmax */
    float my = ay < by ? by : ay;
    float diff = mx < my ? my : mx;
    float tol = 1.0f / 8.0f;
    uint32_t i;
    for (i = 0; i < MAX_CUBIC_LEVEL; i++) {
        if (diff < tol) {
            return 1u << i;
        }
        tol *= 4.0f;
    }
    return 1u << MAX_CUBIC_LEVEL;
}

static void hair_cubic2(const hstate *s, const sv_pt p[4], const ts_clip *clip)
{
    uint32_t lines = cubic_segments(p);
    sv_pt tmp[(1 << MAX_CUBIC_LEVEL) + 1];
    sv_pt c[4];
    sv_pt d12 = ts_sub(p[1], p[2]);
    sv_pt t2 = ts_add(ts_sub(p[2], ts_add(p[1], p[1])), p[0]);
    sv_pt d10 = ts_sub(p[1], p[0]);
    float t = 0.0f;
    float dt;
    uint32_t i;
    if (lines == 1) {
        sv_pt l[2];
        l[0] = p[0];
        l[1] = p[3];
        line_proc(&s->h, l, 2, clip);
        return;
    }
    c[0] = ts_sub(ts_add(p[3], ts_pt(3.0f * d12.x, 3.0f * d12.y)), p[0]);
    c[1] = ts_pt(3.0f * t2.x, 3.0f * t2.y);
    c[2] = ts_pt(3.0f * d10.x, 3.0f * d10.y);
    c[3] = p[0];
    memset(tmp, 0, sizeof tmp);
    tmp[0] = p[0];
    dt = 1.0f / (float)lines;
    for (i = 1; i < lines; i++) {
        t = t + dt;
        tmp[i] = ts_pt(((c[0].x * t + c[1].x) * t + c[2].x) * t + c[3].x,
                       ((c[0].y * t + c[1].y) * t + c[2].y) * t + c[3].y);
    }
    for (i = 0; i < (1u << MAX_CUBIC_LEVEL) + 1; i++) {
        if (!ts_pt_is_finite(tmp[i])) {
            return;
        }
    }
    tmp[lines] = p[3];
    line_proc(&s->h, tmp, lines + 1, clip);
}

static bool lt_90(sv_pt p0, sv_pt pivot, sv_pt p2)
{
    return ts_dot(ts_sub(p0, pivot), ts_sub(p2, pivot)) >= 0.0f;
}

static void hair_cubic(const hstate *s, const sv_pt p[4])
{
    const ts_clip *clip;
    if (!cull(s, p, 4, &clip)) {
        return;
    }
    if (lt_90(p[1], p[0], p[3]) && lt_90(p[2], p[0], p[3]) && lt_90(p[1], p[3], p[0]) &&
        lt_90(p[2], p[3], p[0])) {
        hair_cubic2(s, p, clip);
    } else {
        sv_pt tmp[13];
        size_t n = ts_chop_cubic_at_max_curvature(p, tmp);
        size_t i;
        for (i = 0; i < n && i * 3 + 3 < 13; i++) {
            hair_cubic2(s, tmp + i * 3, clip);
        }
    }
}

static void extend_pts(sv_cap cap, int prev_verb, int next_verb, sv_pt *pts, size_t n)
{
    float outset = cap == CAP_SQUARE ? 0.5f : FLOAT_PI / 8.0f;
    if (prev_verb == V_MOVE) {
        sv_pt first = pts[0];
        size_t off = 0;
        size_t controls = n - 1;
        sv_pt tan;
        for (;;) {
            off++;
            tan = ts_sub(first, pts[off]);
            if (tan.x != 0.0f || tan.y != 0.0f) {
                break;
            }
            controls--;
            if (controls == 0) {
                break;
            }
        }
        if (tan.x == 0.0f && tan.y == 0.0f) {
            tan = ts_pt(1.0f, 0.0f);
            controls = n - 1;
        } else {
            (void)ts_normalize(&tan);
        }
        off = 0;
        for (;;) {
            pts[off].x += tan.x * outset;
            pts[off].y += tan.y * outset;
            off++;
            controls++;
            if (controls >= n) {
                break;
            }
        }
    }
    if (next_verb == V_MOVE || next_verb == V_CLOSE || next_verb < 0) {
        sv_pt last = pts[n - 1];
        size_t off = n - 1;
        size_t controls = n - 1;
        sv_pt tan;
        for (;;) {
            off--;
            tan = ts_sub(last, pts[off]);
            if (tan.x != 0.0f || tan.y != 0.0f) {
                break;
            }
            controls--;
            if (controls == 0) {
                break;
            }
        }
        if (tan.x == 0.0f && tan.y == 0.0f) {
            tan = ts_pt(-1.0f, 0.0f);
            controls = n - 1;
        } else {
            (void)ts_normalize(&tan);
        }
        off = n - 1;
        for (;;) {
            pts[off].x += tan.x * outset;
            pts[off].y += tan.y * outset;
            if (off == 0) {
                break;
            }
            off--;
            controls++;
            if (controls >= n) {
                break;
            }
        }
    }
}

void ts_scan_hairline(const ts_path *path, sv_cap cap, bool aa, ts_clip clip, const sv_spans *out,
                      uint32_t ox, uint32_t oy)
{
    hstate s;
    sv_rect ob;
    ts_irect ib;
    ts_irect ci;
    ts_irect tmp;
    float co = cap == CAP_BUTT ? 1.0f : 2.0f;
    int prev_verb = V_MOVE;
    sv_pt first = {0.0f, 0.0f};
    sv_pt last = {0.0f, 0.0f};
    ts_iter it;
    ts_seg seg;
    memset(&s, 0, sizeof s);
    s.h.aa = aa;
    s.h.out = out;
    s.h.ox = ox;
    s.h.oy = oy;
    s.clip = &clip;
    if (!ts_rect_ltrb(path->bounds.l - co, path->bounds.t - co, path->bounds.r + co, path->bounds.b + co,
                      &ob) ||
        !ts_rect_round_out(ob, &ib)) {
        return;
    }
    ci.x = (int32_t)clip.x;
    ci.y = (int32_t)clip.y;
    ci.w = clip.w;
    ci.h = clip.h;
    if (!ts_irect_intersect(ci, ib, &tmp)) {
        return;
    }
    if (!ts_irect_contains(ci, ib)) {
        if (!ts_irect_ltrb(pc_i32_sat_sub(ci.x, 1), pc_i32_sat_sub(ci.y, 1),
                           pc_i32_sat_add(ts_irect_r(ci), 1), pc_i32_sat_add(ts_irect_b(ci), 1), &s.outset) ||
            !ts_irect_ltrb(ci.x + 1, ci.y + 1, ts_irect_r(ci) - 1, ts_irect_b(ci) - 1, &s.inset)) {
            return;
        }
        s.have_inset = true;
    }
    ts_iter_init(&it, path);
    while (ts_iter_next(&it, &seg)) {
        int verb = it.vi > 0 ? (int)path->verbs[it.vi - 1] : V_MOVE;
        int next = ts_iter_next_verb(&it);
        sv_pt last2;
        sv_pt p[4];
        switch (seg.verb) {
        case V_MOVE:
            first = seg.p[0];
            last = seg.p[0];
            last2 = seg.p[0];
            break;
        case V_LINE:
            p[0] = last;
            p[1] = seg.p[0];
            if (cap != CAP_BUTT) {
                extend_pts(cap, prev_verb, next, p, 2);
            }
            line_proc(&s.h, p, 2, &clip);
            last = seg.p[0];
            last2 = p[0];
            break;
        case V_QUAD:
            p[0] = last;
            p[1] = seg.p[0];
            p[2] = seg.p[1];
            if (cap != CAP_BUTT) {
                extend_pts(cap, prev_verb, next, p, 3);
            }
            hair_quad(&s, p);
            last = seg.p[1];
            last2 = p[0];
            break;
        case V_CUBIC:
            p[0] = last;
            p[1] = seg.p[0];
            p[2] = seg.p[1];
            p[3] = seg.p[2];
            if (cap != CAP_BUTT) {
                extend_pts(cap, prev_verb, next, p, 4);
            }
            hair_cubic(&s, p);
            last = seg.p[2];
            last2 = p[0];
            break;
        case V_CLOSE:
        default:
            p[0] = last;
            p[1] = first;
            if (cap != CAP_BUTT && prev_verb == V_MOVE) {
                extend_pts(cap, prev_verb, next, p, 2);
            }
            line_proc(&s.h, p, 2, &clip);
            last2 = p[0];
            break;
        }
        if (cap != CAP_BUTT) {
            if (prev_verb == V_MOVE && (verb == V_LINE || verb == V_QUAD || verb == V_CUBIC)) {
                first = last2;
            }
            prev_verb = verb;
        }
    }
}

/* ---------------------------------------------------------------- fill_rect_aa */

static void do_scanline(const hb *b, int32_t l, int32_t top, int32_t r, uint8_t alpha)
{
    int32_t left;
    int32_t right;
    int32_t w;
    if (top < 0) {
        return;
    }
    if ((l >> 8) == (ts_wsub(r, 1) >> 8)) {
        if ((l >> 8) >= 0) {
            hb_v(b, (uint32_t)(l >> 8), (uint32_t)top, 1, alpha_mul(alpha, ts_wsub(r, l)));
        }
        return;
    }
    left = l >> 8;
    if ((l & 0xFF) != 0) {
        if ((l >> 8) >= 0) {
            hb_v(b, (uint32_t)(l >> 8), (uint32_t)top, 1, alpha_mul(alpha, 256 - (l & 0xFF)));
        }
        left += 1;
    }
    right = r >> 8;
    w = ts_wsub(right, left);
    if (w > 0 && left >= 0) {
        hb_run(b, (uint32_t)left, (uint32_t)top, (uint32_t)w, alpha);
    }
    if ((r & 0xFF) != 0 && right >= 0) {
        hb_v(b, (uint32_t)right, (uint32_t)top, 1, alpha_mul(alpha, r & 0xFF));
    }
}

static void fill_dot8(const hb *b, int32_t l, int32_t t, int32_t r, int32_t bt)
{
    int32_t top;
    int32_t bottom;
    int32_t height;
    if (l >= r || t >= bt) {
        return;
    }
    top = t >> 8;
    if (top == (ts_wsub(bt, 1) >> 8)) {
        do_scanline(b, l, top, r, (uint8_t)ts_wsub(ts_wsub(bt, t), 1));
        return;
    }
    if ((t & 0xFF) != 0) {
        do_scanline(b, l, top, r, (uint8_t)(256 - (t & 0xFF)));
        top += 1;
    }
    bottom = bt >> 8;
    height = ts_wsub(bottom, top);
    if (height > 0) {
        int32_t left = l >> 8;
        if (left == (ts_wsub(r, 1) >> 8)) {
            if (left >= 0 && top >= 0) {
                hb_v(b, (uint32_t)left, (uint32_t)top, (uint32_t)height, (uint8_t)ts_wsub(ts_wsub(r, l), 1));
            }
        } else {
            int32_t right;
            int32_t width;
            if ((l & 0xFF) != 0) {
                if (left >= 0 && top >= 0) {
                    hb_v(b, (uint32_t)left, (uint32_t)top, (uint32_t)height, (uint8_t)(256 - (l & 0xFF)));
                }
                left += 1;
            }
            right = r >> 8;
            width = ts_wsub(right, left);
            if (width > 0 && left >= 0 && top >= 0) {
                int32_t y;
                for (y = 0; y < height; y++) {
                    out1(b, (uint32_t)left, (uint32_t)(top + y), (uint32_t)width, 255);
                }
            }
            if ((r & 0xFF) != 0 && right >= 0 && top >= 0) {
                hb_v(b, (uint32_t)right, (uint32_t)top, (uint32_t)height, (uint8_t)(r & 0xFF));
            }
        }
    }
    if ((bt & 0xFF) != 0) {
        do_scanline(b, l, bottom, r, (uint8_t)(bt & 0xFF));
    }
}

void ts_scan_fill_rect(sv_rect r, bool aa, ts_clip clip, const sv_spans *out)
{
    sv_rect c = clip_rect_f(clip);
    sv_rect i;
    hb b;
    if (!aa) {
        ts_scan_fill_rect_aliased(r, clip, out);
        return;
    }
    if (!ts_rect_ltrb(pc_rs_maxf(r.l, c.l), pc_rs_maxf(r.t, c.t), pc_rs_minf(r.r, c.r), pc_rs_minf(r.b, c.b),
                      &i)) {
        return;
    }
    memset(&b, 0, sizeof b);
    b.out = out;
    fill_dot8(&b, ts_wadd(fixed_f(i.l), 0x80) >> 8, ts_wadd(fixed_f(i.t), 0x80) >> 8,
              ts_wadd(fixed_f(i.r), 0x80) >> 8, ts_wadd(fixed_f(i.b), 0x80) >> 8);
}
