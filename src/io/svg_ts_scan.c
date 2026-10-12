/* tiny-skia 0.11.4 scan/path.rs (sorted edge list walked row by row, non-zero / even-odd spans),
 * scan/path_aa.rs (SuperBlitter: 4x4 supersampling accumulated per pixel exactly as AlphaRuns
 * does, including its u8 wrap-arounds) and scan/mod.rs fill_rect, for the SVG rasteriser.
 *
 * AlphaRuns stores a row as runs of equal alpha; it is only an encoding of one u8 per pixel, so
 * the port keeps the per-pixel array (and the reference's extra slot past the right edge) and
 * emits equal-alpha stretches as spans when a row is flushed.
 *
 * Bounds: at most TS_MAX_EDGES edges per fill, and the edge walk stops after WALK_BUDGET edge
 * steps (rows x active edges; about a minute of work), returning PC_ERR_LIMIT with the rows
 * done so far drawn. Linked-list indices are checked; a broken list ends the walk. */
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "io/svg_ts_edge.h"

#define SHIFT 2
#define WALK_BUDGET ((uint64_t)1 << 32)

/* ---------------------------------------------------------------- edge list */

typedef struct ekey {
    int32_t first_y, x;
    uint32_t idx;
} ekey;

static int ekey_cmp(const void *a, const void *b)
{
    const ekey *p = a;
    const ekey *q = b;
    int32_t va = p->first_y;
    int32_t vb = q->first_y;
    if (va == vb) {
        va = p->x;
        vb = q->x;
    }
    if (va != vb) {
        return va < vb ? -1 : 1;
    }
    /* sort_by is stable */
    return p->idx < q->idx ? -1 : p->idx > q->idx ? 1 : 0;
}

/* The sorted edges between a head (x = i32::MIN) and a tail (first_y = i32::MAX). */
static pc_status sorted_list(ts_edge *e, size_t n, ts_edge **out)
{
    ekey *k = pc_malloc_array(n, sizeof *k);
    ts_edge *l;
    size_t i;
    if (k == NULL) {
        return PC_ERR_NOMEM;
    }
    for (i = 0; i < n; i++) {
        k[i].first_y = e[i].first_y;
        k[i].x = e[i].x;
        k[i].idx = (uint32_t)i;
    }
    qsort(k, n, sizeof *k, ekey_cmp);
    l = pc_calloc_array(n + 2, sizeof *l);
    if (l == NULL) {
        free(k);
        return PC_ERR_NOMEM;
    }
    for (i = 0; i < n; i++) {
        l[i + 1] = e[k[i].idx];
        l[i + 1].prev = (int32_t)i;
        l[i + 1].next = (int32_t)i + 2;
    }
    free(k);
    l[0].prev = -1;
    l[0].next = 1;
    l[0].x = INT32_MIN;
    l[0].first_y = INT32_MIN;
    l[n + 1].prev = (int32_t)n;
    l[n + 1].next = -1;
    l[n + 1].first_y = INT32_MAX;
    *out = l;
    return PC_OK;
}

typedef struct walker {
    ts_edge *e;
    int32_t n; /* list length, head and tail included */
    bool broken;
} walker;

static bool ok_idx(walker *w, int32_t i)
{
    if (i < 0 || i >= w->n) {
        w->broken = true;
        return false;
    }
    return true;
}

static void remove_edge(walker *w, int32_t i)
{
    int32_t p = w->e[i].prev;
    int32_t nx = w->e[i].next;
    if (!ok_idx(w, p) || !ok_idx(w, nx)) {
        return;
    }
    w->e[p].next = nx;
    w->e[nx].prev = p;
}

static void insert_after(walker *w, int32_t i, int32_t after)
{
    int32_t an = w->e[after].next;
    if (!ok_idx(w, an)) {
        return;
    }
    w->e[i].prev = after;
    w->e[i].next = an;
    w->e[an].prev = i;
    w->e[after].next = i;
}

static void backward_insert(walker *w, int32_t i)
{
    int32_t x = w->e[i].x;
    int32_t p = w->e[i].prev;
    int32_t nx;
    int32_t guard = w->n;
    if (!ok_idx(w, p)) {
        return;
    }
    while (p != 0 && guard-- > 0) {
        if (w->e[p].x > x) {
            p = w->e[p].prev;
            if (!ok_idx(w, p)) {
                return;
            }
        } else {
            break;
        }
    }
    nx = w->e[p].next;
    if (nx != i) {
        remove_edge(w, i);
        insert_after(w, i, p);
    }
}

static int32_t backward_insert_start(walker *w, int32_t p, int32_t x)
{
    int32_t guard = w->n;
    while (w->e[p].prev >= 0 && guard-- > 0) {
        p = w->e[p].prev;
        if (!ok_idx(w, p)) {
            return 0;
        }
        if (w->e[p].x <= x) {
            break;
        }
    }
    return p;
}

static void insert_new_edges(walker *w, int32_t ni, int32_t curr_y)
{
    int32_t p;
    int32_t start;
    int32_t guard = w->n;
    if (w->e[ni].first_y != curr_y) {
        return;
    }
    p = w->e[ni].prev;
    if (!ok_idx(w, p) || w->e[p].x <= w->e[ni].x) {
        return;
    }
    start = backward_insert_start(w, p, w->e[ni].x);
    while (!w->broken && guard-- > 0) {
        int32_t next = w->e[ni].next;
        bool keep = false;
        int32_t g2 = w->n;
        if (!ok_idx(w, next)) {
            return;
        }
        while (g2-- > 0) {
            int32_t after = w->e[start].next;
            if (!ok_idx(w, after)) {
                return;
            }
            if (after == ni) {
                keep = true;
                break;
            }
            if (w->e[after].x >= w->e[ni].x) {
                break;
            }
            start = after;
        }
        if (!keep) {
            remove_edge(w, ni);
            insert_after(w, ni, start);
        }
        start = ni;
        ni = next;
        if (w->e[ni].first_y != curr_y) {
            break;
        }
    }
}

static pc_status walk_edges(walker *w, bool evenodd, uint32_t start_y, uint32_t stop_y, uint32_t right_clip,
                            ts_blit_h_fn h, void *ctx)
{
    uint32_t curr_y = start_y;
    int32_t mask = evenodd ? 1 : -1;
    uint64_t budget = WALK_BUDGET;
    ts_edge *e = w->e;
    for (;;) {
        int32_t wind = 0;
        uint32_t left = 0;
        int32_t prev_x = e[0].x;
        int32_t ci = e[0].next;
        if (!ok_idx(w, ci)) {
            return PC_OK;
        }
        while (e[ci].first_y <= (int32_t)curr_y) {
            uint32_t x = (uint32_t)(ts_wadd(e[ci].x, 1 << 15) >> 16);
            int32_t ni;
            int32_t nx;
            if (budget-- == 0) {
                return PC_ERR_LIMIT;
            }
            if ((wind & mask) == 0) {
                left = x;
            }
            wind = ts_wadd(wind, e[ci].winding);
            if ((wind & mask) == 0 && x - left != 0) {
                h(ctx, left, curr_y, x - left);
            }
            ni = e[ci].next;
            if (!ok_idx(w, ni)) {
                return PC_OK;
            }
            if (e[ci].last_y == (int32_t)curr_y) {
                if (e[ci].kind == TS_EDGE_LINE) {
                    remove_edge(w, ci);
                } else {
                    bool more = e[ci].kind == TS_EDGE_QUAD ? e[ci].curve_count > 0 && ts_quad_update(&e[ci])
                                                           : e[ci].curve_count < 0 && ts_cubic_update(&e[ci]);
                    if (more) {
                        nx = e[ci].x;
                        if (nx < prev_x) {
                            backward_insert(w, ci);
                        } else {
                            prev_x = nx;
                        }
                    } else {
                        remove_edge(w, ci);
                    }
                }
            } else {
                nx = ts_wadd(e[ci].x, e[ci].dx);
                e[ci].x = nx;
                if (nx < prev_x) {
                    backward_insert(w, ci);
                } else {
                    prev_x = nx;
                }
            }
            if (w->broken) {
                return PC_OK;
            }
            ci = ni;
        }
        if ((wind & mask) != 0 && right_clip - left != 0) {
            h(ctx, left, curr_y, right_clip - left);
        }
        curr_y += 1;
        if (curr_y >= stop_y) {
            break;
        }
        insert_new_edges(w, ci, (int32_t)curr_y);
        if (w->broken) {
            return PC_OK;
        }
    }
    return PC_OK;
}

/* ScreenIntRect::from_xywh */
static bool screen_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, ts_clip *out)
{
    if (x > INT32_MAX || y > INT32_MAX || w > INT32_MAX || h > INT32_MAX || w == 0 || h == 0 ||
        (uint64_t)x + w > UINT32_MAX || (uint64_t)y + h > UINT32_MAX) {
        return false;
    }
    out->x = x;
    out->y = y;
    out->w = w;
    out->h = h;
    return true;
}

pc_status ts_fill_path_impl(const ts_path *path, bool evenodd, ts_clip clip, int32_t start_y, int32_t stop_y,
                            int32_t shift, bool contained, ts_blit_h_fn h, void *ctx)
{
    ts_clip sh;
    sv_rect rc;
    ts_edge *edges = NULL;
    size_t n = 0;
    walker w;
    pc_status st;
    int32_t top;
    int32_t bottom;
    if (!screen_rect(clip.x << shift, clip.y << shift, clip.w << shift, clip.h << shift, &sh)) {
        return PC_OK;
    }
    /* ShiftedIntRect::recover().to_rect() */
    rc.l = (float)(sh.x >> shift);
    rc.t = (float)(sh.y >> shift);
    rc.r = (float)(sh.x >> shift) + (float)(sh.w >> shift);
    rc.b = (float)(sh.y >> shift) + (float)(sh.h >> shift);
    st = ts_build_edges(path, contained ? NULL : &rc, shift, &edges, &n);
    if (st != PC_OK || edges == NULL) {
        return st;
    }
    memset(&w, 0, sizeof w);
    st = sorted_list(edges, n, &w.e);
    free(edges);
    if (st != PC_OK) {
        return st;
    }
    w.n = (int32_t)n + 2;
    start_y = ts_shl(start_y, shift);
    stop_y = ts_shl(stop_y, shift);
    top = (int32_t)sh.y;
    if (!contained && start_y < top) {
        start_y = top;
    }
    bottom = (int32_t)(sh.y + sh.h);
    if (!contained && stop_y > bottom) {
        stop_y = bottom;
    }
    if (start_y < 0 || stop_y < 0) {
        free(w.e);
        return PC_OK;
    }
    st = walk_edges(&w, evenodd, (uint32_t)start_y, (uint32_t)stop_y, sh.x + sh.w, h, ctx);
    free(w.e);
    return st;
}

/* ---------------------------------------------------------------- non-AA (scan/path.rs) */

typedef struct direct {
    const sv_spans *out;
    uint32_t ox, oy;
} direct;

static void direct_h(void *ctx, uint32_t x, uint32_t y, uint32_t w)
{
    direct *d = ctx;
    ts_emit(d->out, x + d->ox, y + d->oy, w, 255);
}

/* i32::saturate_from(f64) */
static int32_t sat_i32_d(double x)
{
    x = x < (double)INT32_MAX ? x : (double)INT32_MAX;
    x = x > (double)INT32_MIN ? x : (double)INT32_MIN;
    return (int32_t)x;
}

static bool to_screen(ts_irect r, ts_clip *out)
{
    if (r.x < 0 || r.y < 0) {
        return false;
    }
    return screen_rect((uint32_t)r.x, (uint32_t)r.y, r.w, r.h, out);
}

static bool clip_contains(ts_clip a, ts_clip b)
{
    return a.x <= b.x && a.y <= b.y && (uint64_t)a.x + a.w >= (uint64_t)b.x + b.w &&
           (uint64_t)a.y + a.h >= (uint64_t)b.y + b.h;
}

static ts_irect clip_irect(ts_clip c)
{
    ts_irect r;
    r.x = (int32_t)c.x;
    r.y = (int32_t)c.y;
    r.w = c.w;
    r.h = c.h;
    return r;
}

static pc_status fill_aliased(const ts_path *path, bool evenodd, ts_clip clip, const sv_spans *out,
                              uint32_t ox, uint32_t oy)
{
    const double bias = 0.5 + 1.5 / 64.0;
    ts_irect ir;
    ts_clip sb;
    bool contained;
    direct d;
    if (!ts_irect_ltrb(sat_i32_d(ceil((double)path->bounds.l - bias)),
                       sat_i32_d(ceil((double)path->bounds.t - bias)),
                       sat_i32_d(floor((double)path->bounds.r + bias)),
                       sat_i32_d(floor((double)path->bounds.b + bias)), &ir)) {
        return PC_OK;
    }
    contained = to_screen(ir, &sb) && clip_contains(clip, sb);
    d.out = out;
    d.ox = ox;
    d.oy = oy;
    return ts_fill_path_impl(path, evenodd, clip, ir.y, ts_irect_b(ir), 0, contained, direct_h, &d);
}

/* ---------------------------------------------------------------- AA (scan/path_aa.rs) */

typedef struct super_blit {
    const sv_spans *out;
    uint32_t ox, oy;
    int32_t curr_iy;
    uint32_t width, left, super_left;
    int32_t curr_y, top;
    uint8_t *alpha;  /* width + 1 */
    uint32_t lo, hi; /* touched range [lo, hi) */
} super_blit;

static void sb_flush(super_blit *s)
{
    if (s->curr_iy >= s->top) {
        if (s->hi > s->lo) {
            uint32_t x = s->lo;
            uint32_t end = s->hi < s->width ? s->hi : s->width;
            while (x < end) {
                uint8_t a = s->alpha[x];
                uint32_t k = x + 1;
                while (k < end && s->alpha[k] == a) {
                    k++;
                }
                ts_emit(s->out, s->left + x + s->ox, (uint32_t)s->curr_iy + s->oy, k - x, a);
                x = k;
            }
            memset(s->alpha + s->lo, 0, s->hi - s->lo);
            s->lo = UINT32_MAX;
            s->hi = 0;
        }
        s->curr_iy = s->top - 1;
    }
}

static void sb_touch(super_blit *s, uint32_t i)
{
    s->lo = i < s->lo ? i : s->lo;
    s->hi = i + 1 > s->hi ? i + 1 : s->hi;
}

/* AlphaRuns::add, per pixel; indices past the extra slot are dropped (the reference panics). */
static void sb_add(super_blit *s, uint32_t x, uint8_t start_a, uint32_t middle, uint8_t stop_a, uint8_t maxv)
{
    uint32_t i = x;
    uint32_t lim = s->width; /* last valid index (the extra slot) */
    if (start_a != 0) {
        if (i <= lim) {
            uint32_t t = (uint32_t)s->alpha[i] + start_a;
            s->alpha[i] = (uint8_t)(t - (t >> 8));
            sb_touch(s, i);
        }
        i++;
    }
    if (middle != 0) {
        uint32_t k;
        for (k = 0; k < middle && i <= lim; k++, i++) {
            uint32_t t = (uint32_t)s->alpha[i] + maxv;
            s->alpha[i] = (uint8_t)(t - (t >> 8));
            sb_touch(s, i);
        }
        i += middle - k;
    }
    if (stop_a != 0 && i <= lim) {
        s->alpha[i] = (uint8_t)(s->alpha[i] + stop_a);
        sb_touch(s, i);
    }
}

static void sb_blit_h(void *ctx, uint32_t x, uint32_t y, uint32_t width)
{
    super_blit *s = ctx;
    int32_t iy = (int32_t)(y >> SHIFT);
    uint32_t start;
    uint32_t stop;
    uint32_t fb;
    uint32_t fe;
    int32_t n;
    uint8_t maxv;
    if (x >= s->super_left) {
        x -= s->super_left;
    } else {
        width = x + width;
        x = 0;
        if (width == 0) {
            return;
        }
    }
    if (s->curr_y != (int32_t)y) {
        s->curr_y = (int32_t)y;
    }
    if (iy != s->curr_iy) {
        sb_flush(s);
        s->curr_iy = iy;
    }
    start = x;
    stop = x + width;
    fb = start & ((1u << SHIFT) - 1);
    fe = stop & ((1u << SHIFT) - 1);
    n = ts_wsub(ts_wsub((int32_t)stop >> SHIFT, (int32_t)start >> SHIFT), 1);
    if (n < 0) {
        fb = fe - fb;
        n = 0;
        fe = 0;
    } else if (fb == 0) {
        n += 1;
    } else {
        fb = (1u << SHIFT) - fb;
    }
    maxv = (uint8_t)((1u << (8 - SHIFT)) - (((y & ((1u << SHIFT) - 1)) + 1) >> SHIFT));
    sb_add(s, x >> SHIFT, (uint8_t)(fb << (8 - 2 * SHIFT)), (uint32_t)n, (uint8_t)(fe << (8 - 2 * SHIFT)),
           maxv);
}

static int32_t overflows_short_shift(int32_t v, int32_t shift)
{
    int32_t s = 16 + shift;
    return ts_wsub(ts_shl(v, s) >> s, v);
}

pc_status ts_scan_fill(const ts_path *path, bool evenodd, bool aa, ts_clip clip, const sv_spans *out,
                       uint32_t ox, uint32_t oy)
{
    sv_rect rf;
    ts_irect ir;
    ts_irect clipped;
    ts_irect sect;
    ts_clip sc;
    ts_clip irs;
    super_blit s;
    bool contained;
    pc_status st;
    if (!aa) {
        return fill_aliased(path, evenodd, clip, out, ox, oy);
    }
    if (!ts_rect_ltrb(floorf(path->bounds.l), floorf(path->bounds.t), ceilf(path->bounds.r),
                      ceilf(path->bounds.b), &rf) ||
        !ts_rect_round_out(rf, &ir)) {
        return PC_OK;
    }
    if (!ts_irect_intersect(ir, clip_irect(clip), &clipped)) {
        return PC_OK;
    }
    if ((overflows_short_shift(clipped.x, SHIFT) | overflows_short_shift(clipped.y, SHIFT) |
         overflows_short_shift(ts_irect_r(clipped), SHIFT) |
         overflows_short_shift(ts_irect_b(clipped), SHIFT)) != 0) {
        return fill_aliased(path, evenodd, clip, out, ox, oy);
    }
    if (clip.x + clip.w > 32767 || clip.y + clip.h > 32767) {
        return PC_OK;
    }
    /* SuperBlitter::new */
    if (!ts_irect_intersect(ir, clip_irect(clip), &sect) || !to_screen(sect, &sc)) {
        return PC_OK;
    }
    memset(&s, 0, sizeof s);
    s.out = out;
    s.ox = ox;
    s.oy = oy;
    s.curr_iy = (int32_t)sc.y - 1;
    s.width = sc.w;
    s.left = sc.x;
    s.super_left = sc.x << SHIFT;
    s.curr_y = (int32_t)(sc.y << SHIFT) - 1;
    s.top = (int32_t)sc.y;
    s.lo = UINT32_MAX;
    s.hi = 0;
    s.alpha = calloc((size_t)sc.w + 1, 1);
    if (s.alpha == NULL) {
        return PC_ERR_NOMEM;
    }
    contained = to_screen(ir, &irs) && clip_contains(clip, irs);
    st = ts_fill_path_impl(path, evenodd, clip, ir.y, ts_irect_b(ir), SHIFT, contained, sb_blit_h, &s);
    sb_flush(&s);
    free(s.alpha);
    return st;
}

/* ---------------------------------------------------------------- scan::fill_rect */

/* i32::saturate_round(f32): saturate_from(floor(x) + 0.5) */
static int32_t sat_round(float x)
{
    float v = floorf(x) + 0.5f;
    v = v < 2147483520.0f ? v : 2147483520.0f;
    v = v > -2147483520.0f ? v : -2147483520.0f;
    return (int32_t)v;
}

void ts_scan_fill_rect_aliased(sv_rect r, ts_clip clip, const sv_spans *out)
{
    ts_irect ir;
    ts_irect is;
    ts_clip sc;
    int32_t w = sat_round(r.r - r.l);
    int32_t h = sat_round(r.b - r.t);
    uint32_t y;
    if (!ts_irect_xywh(sat_round(r.l), sat_round(r.t), (uint32_t)w > 1 ? (uint32_t)w : 1,
                       (uint32_t)h > 1 ? (uint32_t)h : 1, &ir)) {
        return;
    }
    if (!ts_irect_intersect(ir, clip_irect(clip), &is) || !to_screen(is, &sc)) {
        return;
    }
    for (y = 0; y < sc.h; y++) {
        ts_emit(out, sc.x, sc.y + y, sc.w, 255);
    }
}
