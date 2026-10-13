/* Brush tip outlines (outline.h). Float expressions keep the reference's f32 types and order. */
#include "paint/outline.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "paint/pmath.h"

/* Segments in the outline of a computed round tip. */
#define ELLIPSE_SEGMENTS 64
/* Budget for all traced ring vertices; the smallest rings are dropped first. */
#define MAX_POINTS 512
/* A traced ring smaller than this many bitmap pixels squared is noise. */
#define MIN_AREA 0.9f
/* "Full size" outlines everything above 1/255; the normal cursor outlines the 50 % contour. */
#define FULL_THRESHOLD (1.0f / 255.0f)
#define HALF_THRESHOLD 0.5f
/* Rust's f32::consts::TAU and to_radians factor. */
#define TAU_F 6.28318530717958647692f
#define RAD_PER_DEG_F (3.14159265358979323846f / 180.0f)

typedef float pt2[2];

/* A growable point list (one ring). */
typedef struct pvec {
    pt2 *v;
    size_t n, cap;
} pvec;

/* A growable list of rings. */
typedef struct rvec {
    pvec *v;
    size_t n, cap;
} rvec;

typedef struct seg {
    pt2 a, b;
} seg;

typedef struct segs {
    seg *v;
    size_t n, cap;
} segs;

#define NOMEM(err) pc_fail((err), PC_ERR_NOMEM, "out of memory for a brush outline")

static pc_status pv_push(pvec *p, float x, float y, pc_error *err)
{
    if (p->n == p->cap) {
        pt2 *nv = pc_grow_array(p->v, &p->cap, p->n + 1, sizeof *nv);
        if (nv == NULL) {
            return NOMEM(err);
        }
        p->v = nv;
    }
    p->v[p->n][0] = x;
    p->v[p->n][1] = y;
    p->n++;
    return PC_OK;
}

static void rv_fini(rvec *r)
{
    for (size_t i = 0; i < r->n; i++) {
        free(r->v[i].v);
    }
    free(r->v);
    memset(r, 0, sizeof *r);
}

/* Appends ring p (taking its points; freed on failure). */
static pc_status rv_push(rvec *r, pvec *p, pc_error *err)
{
    if (r->n == r->cap) {
        pvec *nv = pc_grow_array(r->v, &r->cap, r->n + 1, sizeof *nv);
        if (nv == NULL) {
            free(p->v);
            memset(p, 0, sizeof *p);
            return NOMEM(err);
        }
        r->v = nv;
    }
    r->v[r->n++] = *p;
    memset(p, 0, sizeof *p);
    return PC_OK;
}

void pc_paint_outline_fini(pc_paint_outline *o)
{
    if (o == NULL) {
        return;
    }
    for (size_t i = 0; i < o->count; i++) {
        free(o->rings[i].pts);
    }
    free(o->rings);
    o->rings = NULL;
    o->count = 0;
}

/* Hands the rings over as an outline (r is emptied either way). */
static pc_status to_outline(rvec *r, pc_paint_outline *out, pc_error *err)
{
    pc_paint_ring *rings = pc_malloc_array(r->n, sizeof *rings);
    if (rings == NULL) {
        rv_fini(r);
        return NOMEM(err);
    }
    for (size_t i = 0; i < r->n; i++) {
        rings[i].pts = r->v[i].v;
        rings[i].n = r->v[i].n;
    }
    out->rings = rings;
    out->count = r->n;
    free(r->v);
    memset(r, 0, sizeof *r);
    return PC_OK;
}

/* ---- tracing ---- */

/* Bilinear sample with zero outside (tips fade to nothing at their edges). */
static float tile_at(const pc_paint_gray_tile *t, int64_t x, int64_t y)
{
    if (x < 0 || y < 0 || x >= (int64_t)t->width || y >= (int64_t)t->height) {
        return 0.0f;
    }
    return pc_paint_gray_tile_get(t, (uint32_t)x, (uint32_t)y);
}

static float sample(const pc_paint_gray_tile *t, float x, float y)
{
    float fx0 = floorf(x), fy0 = floorf(y), tx = x - fx0, ty = y - fy0, a, b;
    int64_t x0 = pc_f32_to_i64(fx0), y0 = pc_f32_to_i64(fy0);
    a = tile_at(t, x0, y0) + (tile_at(t, x0 + 1, y0) - tile_at(t, x0, y0)) * tx;
    b = tile_at(t, x0, y0 + 1) + (tile_at(t, x0 + 1, y0 + 1) - tile_at(t, x0, y0 + 1)) * tx;
    return a + (b - a) * ty;
}

/* Shoelace area of a ring. */
static float ring_area(const pvec *r)
{
    float s = 0.0f;
    if (r->n < 3) {
        return 0.0f;
    }
    for (size_t i = 0; i < r->n; i++) {
        const float *a = r->v[i], *b = r->v[(i + 1) % r->n];
        s += a[0] * b[1] - b[0] * a[1];
    }
    return fabsf(s * 0.5f);
}

/* ---- stitching segments into rings ---- */

typedef struct end_ref {
    int64_t kx, ky;
    size_t seq; /* insertion order: 2 * segment + (0 for its start, 1 for its end) */
} end_ref;

static void end_key(const float *p, int64_t *kx, int64_t *ky)
{
    *kx = pc_f32_to_i64(roundf(p[0] * 4096.0f));
    *ky = pc_f32_to_i64(roundf(p[1] * 4096.0f));
}

static int cmp_end(const void *a, const void *b)
{
    const end_ref *p = a, *q = b;
    if (p->kx != q->kx) {
        return p->kx < q->kx ? -1 : 1;
    }
    if (p->ky != q->ky) {
        return p->ky < q->ky ? -1 : 1;
    }
    return p->seq < q->seq ? -1 : (p->seq > q->seq);
}

/* The first unused segment with an end at key (kx, ky), in insertion order; SIZE_MAX if none. */
static size_t find_unused(const end_ref *ends, size_t n, int64_t kx, int64_t ky, const bool *used)
{
    size_t lo = 0, hi = n;
    end_ref probe = {kx, ky, 0};
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cmp_end(&ends[mid], &probe) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (size_t i = lo; i < n && ends[i].kx == kx && ends[i].ky == ky; i++) {
        if (!used[ends[i].seq / 2]) {
            return ends[i].seq / 2;
        }
    }
    return SIZE_MAX;
}

/* Link segments end-to-end into closed rings (quantised endpoint keys absorb float noise). */
static pc_status stitch(const segs *sg, rvec *rings, pc_error *err)
{
    size_t n = sg->n;
    end_ref *ends;
    bool *used;
    pc_status st = PC_OK;
    if (n == 0) {
        return PC_OK;
    }
    ends = pc_malloc_array(2 * n, sizeof *ends);
    used = pc_calloc_array(n, sizeof *used);
    if (ends == NULL || used == NULL) {
        free(ends);
        free(used);
        return NOMEM(err);
    }
    for (size_t i = 0; i < n; i++) {
        end_key(sg->v[i].a, &ends[2 * i].kx, &ends[2 * i].ky);
        ends[2 * i].seq = 2 * i;
        end_key(sg->v[i].b, &ends[2 * i + 1].kx, &ends[2 * i + 1].ky);
        ends[2 * i + 1].seq = 2 * i + 1;
    }
    qsort(ends, 2 * n, sizeof *ends, cmp_end);
    for (size_t s = 0; s < n && st == PC_OK; s++) {
        pvec ring = {NULL, 0, 0};
        int64_t k0x, k0y;
        if (used[s]) {
            continue;
        }
        used[s] = true;
        st = pv_push(&ring, sg->v[s].a[0], sg->v[s].a[1], err);
        if (st == PC_OK) {
            st = pv_push(&ring, sg->v[s].b[0], sg->v[s].b[1], err);
        }
        end_key(ring.v != NULL ? ring.v[0] : sg->v[s].a, &k0x, &k0y);
        while (st == PC_OK) {
            const float *tail = ring.v[ring.n - 1], *a, *b, *p;
            int64_t tx, ty, ax, ay, px, py;
            size_t next;
            end_key(tail, &tx, &ty);
            next = find_unused(ends, 2 * n, tx, ty, used);
            if (next == SIZE_MAX) {
                break;
            }
            used[next] = true;
            a = sg->v[next].a;
            b = sg->v[next].b;
            end_key(a, &ax, &ay);
            p = ax == tx && ay == ty ? b : a;
            end_key(p, &px, &py);
            if (px == k0x && py == k0y) {
                break;
            }
            st = pv_push(&ring, p[0], p[1], err);
        }
        if (st == PC_OK && ring.n >= 3) {
            st = rv_push(rings, &ring, err);
        }
        free(ring.v);
    }
    free(ends);
    free(used);
    return st;
}

/* ---- marching squares ---- */

static pc_status seg_push(segs *s, const float *a, const float *b, pc_error *err)
{
    if (s->n == s->cap) {
        seg *nv = pc_grow_array(s->v, &s->cap, s->n + 1, sizeof *nv);
        if (nv == NULL) {
            return NOMEM(err);
        }
        s->v = nv;
    }
    memcpy(s->v[s->n].a, a, sizeof(pt2));
    memcpy(s->v[s->n].b, b, sizeof(pt2));
    s->n++;
    return PC_OK;
}

static float node(const float *v, size_t w, size_t h, int64_t x, int64_t y)
{
    if (x < 0 || y < 0 || x >= (int64_t)w || y >= (int64_t)h) {
        return 0.0f;
    }
    return v[(size_t)y * w + (size_t)x];
}

static void edge_lerp(float thr, float v0, float v1, float ax, float ay, float bx, float by, float out[2])
{
    float t = fabsf(v1 - v0) < 1e-9f ? 0.5f : pc_clampf((thr - v0) / (v1 - v0), 0.0f, 1.0f);
    out[0] = ax + (bx - ax) * t;
    out[1] = ay + (by - ay) * t;
}

/* Marching squares over v (w by h samples at lattice nodes), outside treated as 0 so every
 * contour closes; rings in node coordinates. */
static pc_status march(const float *v, size_t w, size_t h, float thr, rvec *rings, pc_error *err)
{
    segs sg = {NULL, 0, 0};
    pc_status st = PC_OK;
    for (int64_t y = -1; y < (int64_t)h && st == PC_OK; y++) {
        for (int64_t x = -1; x < (int64_t)w && st == PC_OK; x++) {
            float va = node(v, w, h, x, y), vb = node(v, w, h, x + 1, y), vc = node(v, w, h, x + 1, y + 1);
            float vd = node(v, w, h, x, y + 1), fx = (float)x, fy = (float)y;
            unsigned idx = (unsigned)(va >= thr) | (unsigned)(vb >= thr) << 1 | (unsigned)(vc >= thr) << 2 |
                           (unsigned)(vd >= thr) << 3;
            float top[2], right[2], bottom[2], left[2];
            bool mid;
            if (idx == 0 || idx == 15) {
                continue;
            }
            edge_lerp(thr, va, vb, fx, fy, fx + 1.0f, fy, top);
            edge_lerp(thr, vb, vc, fx + 1.0f, fy, fx + 1.0f, fy + 1.0f, right);
            edge_lerp(thr, vd, vc, fx, fy + 1.0f, fx + 1.0f, fy + 1.0f, bottom);
            edge_lerp(thr, va, vd, fx, fy, fx, fy + 1.0f, left);
            mid = (va + vb + vc + vd) * 0.25f >= thr;
            switch (idx) {
            case 1:
            case 14:
                st = seg_push(&sg, top, left, err);
                break;
            case 2:
            case 13:
                st = seg_push(&sg, top, right, err);
                break;
            case 3:
            case 12:
                st = seg_push(&sg, right, left, err);
                break;
            case 4:
            case 11:
                st = seg_push(&sg, right, bottom, err);
                break;
            case 6:
            case 9:
                st = seg_push(&sg, top, bottom, err);
                break;
            case 7:
            case 8:
                st = seg_push(&sg, left, bottom, err);
                break;
            default: /* 5, 10: saddles, resolved by the centre value */
                if ((idx == 5) != mid) {
                    st = seg_push(&sg, top, right, err);
                    if (st == PC_OK) {
                        st = seg_push(&sg, left, bottom, err);
                    }
                } else {
                    st = seg_push(&sg, top, left, err);
                    if (st == PC_OK) {
                        st = seg_push(&sg, right, bottom, err);
                    }
                }
                break;
            }
        }
    }
    if (st == PC_OK) {
        st = stitch(&sg, rings, err);
    }
    free(sg.v);
    return st;
}

/* ---- simplification ---- */

/* Squared distance from p to the segment a-b. */
static float seg_dist2(const float *p, const float *a, const float *b)
{
    float abx = b[0] - a[0], aby = b[1] - a[1], t, ex, ey;
    t = pc_clampf(((p[0] - a[0]) * abx + (p[1] - a[1]) * aby) /
                      pc_paint_maxf_k(abx * abx + aby * aby, 1e-12f),
                  0.0f, 1.0f);
    ex = p[0] - a[0] - abx * t;
    ey = p[1] - a[1] - aby * t;
    return ex * ex + ey * ey;
}

/* Iterative Douglas-Peucker: appends the points of pts farther than tol from the chord. */
static pc_status dp(pt2 *pts, size_t n, float tol, pvec *out, pc_error *err)
{
    bool *keep;
    size_t *stack;
    size_t top = 0;
    pc_status st = PC_OK;
    if (n == 0) {
        return PC_OK;
    }
    if (n < 3) {
        for (size_t i = 0; i < n && st == PC_OK; i++) {
            st = pv_push(out, pts[i][0], pts[i][1], err);
        }
        return st;
    }
    keep = pc_calloc_array(n, sizeof *keep);
    stack = pc_malloc_array(2 * n, sizeof *stack); /* each split adds at most two ranges */
    if (keep == NULL || stack == NULL) {
        free(keep);
        free(stack);
        return NOMEM(err);
    }
    keep[0] = true;
    keep[n - 1] = true;
    stack[top++] = 0;
    stack[top++] = n - 1;
    while (top > 0) {
        size_t j = stack[--top], i = stack[--top], k = i;
        float dmax = 0.0f;
        if (j <= i + 1) {
            continue;
        }
        for (size_t m = i + 1; m < j; m++) {
            float d = seg_dist2(pts[m], pts[i], pts[j]);
            if (d > dmax) {
                dmax = d;
                k = m;
            }
        }
        if (dmax > tol * tol) {
            keep[k] = true;
            stack[top++] = i;
            stack[top++] = k;
            stack[top++] = k;
            stack[top++] = j;
        }
    }
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        if (keep[i]) {
            st = pv_push(out, pts[i][0], pts[i][1], err);
        }
    }
    free(keep);
    free(stack);
    return st;
}

/* Douglas-Peucker on a closed ring (split at the point farthest from the first). */
static pc_status simplify(const pvec *ring, float tol, pvec *out, pc_error *err)
{
    size_t far = 0, n = ring->n;
    float best = 0.0f;
    pt2 *open;
    pc_status st;
    memset(out, 0, sizeof *out);
    if (n <= 4) {
        for (size_t i = 0; i < n; i++) {
            st = pv_push(out, ring->v[i][0], ring->v[i][1], err);
            if (st != PC_OK) {
                return st;
            }
        }
        return PC_OK;
    }
    for (size_t i = 1; i < n; i++) {
        float dx = ring->v[i][0] - ring->v[0][0], dy = ring->v[i][1] - ring->v[0][1], d = dx * dx + dy * dy;
        if (d > best) {
            best = d;
            far = i;
        }
    }
    open = pc_malloc_array(n - far + 1, sizeof *open);
    if (open == NULL) {
        return NOMEM(err);
    }
    memcpy(open, &ring->v[far], (n - far) * sizeof *open);
    memcpy(open[n - far], ring->v[0], sizeof *open);
    st = dp(ring->v, far + 1, tol, out, err);
    if (st == PC_OK) {
        out->n -= out->n > 0;
        st = dp(open, n - far + 1, tol, out, err);
    }
    if (st == PC_OK) {
        out->n -= out->n > 0;
    }
    free(open);
    return st;
}

/* Rings ordered by area, largest first (a stable sort, as the reference's sort_by). */
typedef struct by_area {
    float area;
    size_t index;
} by_area;

static int cmp_area(const void *a, const void *b)
{
    const by_area *p = a, *q = b;
    if (p->area != q->area) {
        return p->area > q->area ? -1 : 1;
    }
    return p->index < q->index ? -1 : (p->index > q->index);
}

/* Trace the contour(s) above thr and map them with the same tip transform as the rasteriser. */
static pc_status trace(const pc_paint_gray_tile *t, float thr, float sn, float cs, float fxs, float fys,
                       float ro, pc_paint_outline *out, pc_error *err)
{
    float tw = (float)t->width, th = (float)t->height, big = pc_rs_maxf(tw, th);
    float f = big <= 160.0f ? 2.0f : 160.0f / big, tol = 0.5f, s;
    size_t gw = pc_f32_to_size(ceilf(tw * f)), gh = pc_f32_to_size(ceilf(th * f)), total;
    float *vals = NULL;
    by_area *order = NULL;
    rvec raw = {NULL, 0, 0}, rings = {NULL, 0, 0}, thin = {NULL, 0, 0}, kept = {NULL, 0, 0};
    size_t budget = MAX_POINTS;
    bool finite = true;
    pc_status st;
    gw = gw > 0 ? gw : 1;
    gh = gh > 0 ? gh : 1;
    total = gw * gh; /* both <= 320 */
    vals = pc_malloc_array(total, sizeof *vals);
    if (vals == NULL) {
        return NOMEM(err);
    }
    for (size_t gy = 0; gy < gh; gy++) {
        for (size_t gx = 0; gx < gw; gx++) {
            vals[gy * gw + gx] = sample(t, ((float)gx + 0.5f) / f - 0.5f, ((float)gy + 0.5f) / f - 0.5f);
        }
    }
    st = march(vals, gw, gh, thr, &raw, err);
    free(vals);
    if (st == PC_OK && raw.n > 0) {
        order = pc_malloc_array(raw.n, sizeof *order);
        st = order == NULL ? NOMEM(err) : PC_OK;
    }
    for (size_t i = 0; st == PC_OK && i < raw.n; i++) {
        order[i] = (by_area){ring_area(&raw.v[i]), i};
    }
    if (st == PC_OK && raw.n > 0) {
        qsort(order, raw.n, sizeof *order, cmp_area);
    }
    for (size_t i = 0; st == PC_OK && i < raw.n; i++) {
        pvec r = raw.v[order[i].index];
        if (order[i].area >= MIN_AREA * f * f) {
            raw.v[order[i].index].v = NULL;
            st = rv_push(&rings, &r, err);
        }
    }
    free(order);
    rv_fini(&raw);
    if (st != PC_OK || rings.n == 0) {
        rv_fini(&rings);
        return st;
    }
    for (;;) {
        size_t sum = 0;
        rv_fini(&thin);
        for (size_t i = 0; i < rings.n && st == PC_OK; i++) {
            pvec r;
            st = simplify(&rings.v[i], tol, &r, err);
            if (st == PC_OK) {
                sum += r.n;
                st = rv_push(&thin, &r, err);
            }
        }
        if (st != PC_OK || sum <= MAX_POINTS || tol >= 8.0f) {
            break;
        }
        tol *= 1.5f;
    }
    for (size_t i = 0; st == PC_OK && i < thin.n; i++) {
        pvec *r = &thin.v[i];
        if (r->n >= 3 && r->n <= budget) {
            budget -= r->n;
            st = rv_push(&kept, r, err);
        }
    }
    rv_fini(&thin);
    rv_fini(&rings);
    if (st != PC_OK || kept.n == 0) {
        rv_fini(&kept);
        return st;
    }
    s = 2.0f / big;
    for (size_t i = 0; i < kept.n; i++) {
        for (size_t j = 0; j < kept.v[i].n; j++) {
            float *p = kept.v[i].v[j];
            float px = (p[0] + 0.5f) / f - 0.5f, py = (p[1] + 0.5f) / f - 0.5f;
            float u = (px - tw / 2.0f) * s * fxs, v = -(py - th / 2.0f) * s * ro * fys;
            float ux = u * cs - v * sn, uy = u * sn + v * cs;
            p[0] = ux;
            p[1] = -uy;
            finite = finite && isfinite(p[0]) && isfinite(p[1]);
        }
    }
    if (!finite) {
        rv_fini(&kept);
        return PC_OK;
    }
    return to_outline(&kept, out, err);
}

pc_status pc_paint_tip_outline(const pc_paint_brush *b, bool full, pc_paint_outline *out, pc_error *err)
{
    float ro, sn, cs, a;
    const pc_paint_gray_tile *t;
    memset(out, 0, sizeof *out);
    if (!isfinite(b->size) || b->size <= 0.0f || !isfinite(b->angle) || !isfinite(b->roundness)) {
        return PC_OK;
    }
    ro = pc_paint_minf_k(pc_rs_maxf(pc_clampf(b->roundness, 0.0f, 1.0f), 0.5f / (b->size / 2.0f)), 1.0f);
    a = b->angle * RAD_PER_DEG_F;
    sn = sinf(a);
    cs = cosf(a);
    if (b->tip.kind == PC_PAINT_TIP_ROUND) {
        /* The normal cursor is the 50 % contour of the (1 - t^2)^4 falloff. */
        float f = full ? 1.0f : pc_paint_half_coverage_radius(1.0f, b->hardness);
        rvec r = {NULL, 0, 0};
        pvec ring = {NULL, 0, 0};
        pc_status st = PC_OK;
        for (int i = 0; i < ELLIPSE_SEGMENTS && st == PC_OK; i++) {
            float tt = TAU_F * (float)i / (float)ELLIPSE_SEGMENTS;
            /* Tip-local (y up), rotated CCW by the angle, then flipped to screen y down. */
            float lx = cosf(tt) * f, ly = sinf(tt) * f * ro;
            st = pv_push(&ring, lx * cs - ly * sn, -(lx * sn + ly * cs), err);
        }
        if (st == PC_OK) {
            st = rv_push(&r, &ring, err);
        }
        free(ring.v);
        if (st != PC_OK) {
            rv_fini(&r);
            return st;
        }
        return to_outline(&r, out, err);
    }
    t = pc_paint_tip_bitmap(&b->tip);
    if (t == NULL || !pc_paint_gray_tile_is_valid(t)) {
        return PC_OK;
    }
    return trace(t, full ? FULL_THRESHOLD : HALF_THRESHOLD, sn, cs, b->flip_x ? -1.0f : 1.0f,
                 b->flip_y ? -1.0f : 1.0f, ro, out, err);
}
