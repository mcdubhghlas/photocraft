/* Distance fields for chiselled bevels (port of the sharp-edge half of crates/compose/src/effects.rs,
 * #1926): the plain distance to a pixel centre follows the staircase of an oblique edge and the
 * facets came out striped. A sharp, anti-aliased edge pixel puts the edge where its coverage says,
 * along the normal the coverage gradient gives (refined by the line through its neighbours' edge
 * points), and the pixels near it measure to that edge.
 *
 * The reference runs these passes on scoped OS threads; every value is computed from its own
 * inputs alone, so this sequential port gives the same bits. Expression order follows the
 * reference exactly (f32 throughout). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "compose/fx_internal.h"

/* Half the spacing of the edge pixels along an edge (at most sqrt 2 / 2, along a diagonal): a pixel
 * within it of the foot of the perpendicular sees the edge as a line, one further out as its end. */
#define EDGE_REACH 0.75f
/* How far (px) around an edge pixel the normal refinement looks for its neighbours. */
#define NORMAL_RADIUS 3
/* The edge points around a pixel must spread at least this far (px, standard deviation) along their
 * line to give it a direction, and stay within EDGE_LINE_MAX_OFF of it. */
#define EDGE_LINE_MIN_SPREAD 0.7f
#define EDGE_LINE_MAX_OFF 0.1f
/* How far from the foot of the perpendicular (px) edge_distance looks for edge pixels. */
#define FOOT_RADIUS 2
/* Pixels past the reach that are still refined, so the value is right where the ramp ends. */
#define EDGE_MARGIN 2.0f

static pc_status nomem(pc_error *err)
{
    return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
}

/* Rust's release-mode i64 `+` / `-`: wrapping. */
static int64_t wadd(int64_t a, int64_t b)
{
    return (int64_t)((uint64_t)a + (uint64_t)b);
}

static int64_t wsub(int64_t a, int64_t b)
{
    return (int64_t)((uint64_t)a - (uint64_t)b);
}

float pc_fx_edge_offset(const float n[2], float a)
{
    /* A unit pixel cut by a straight edge at distance r from its centre (on the side holding at
     * most half the pixel) is covered 1/2 - r/u while the edge crosses two opposite sides, and
     * ((u + v)/2 - r)^2 / (2uv) once it only cuts off a corner (u >= v the normal's components). */
    float u;
    float v;
    float c;
    bool inside;
    float corner;
    float r;
    if (!isfinite(a)) {
        return 0.0f;
    }
    u = pc_rs_maxf(fabsf(n[0]), fabsf(n[1]));
    v = pc_rs_minf(fabsf(n[0]), fabsf(n[1]));
    if (a > 0.5f) {
        c = 1.0f - a;
        inside = true;
    } else {
        c = a;
        inside = false;
    }
    c = pc_clampf(c, 0.0f, 0.5f);
    corner = u > 0.0f ? v / (2.0f * u) : 0.0f;
    r = c >= corner ? (0.5f - c) * u : 0.5f * (u + v) - sqrtf(2.0f * u * v * c);
    return inside ? r : -r;
}

/* SharpEdge::distance: to the edge from the pixel (dx, dy) away, on the shape's side (inside) or
 * the empty side; false when the edge has no direction or (dx, dy) is on the wrong side. */
static bool edge_dist(const pc_fx_sharp_edge *e, float dx, float dy, bool inside, float *out)
{
    float nx = e->normal[0];
    float ny = e->normal[1];
    float signed_d;
    float across;
    float along;
    if (nx == 0.0f && ny == 0.0f) {
        return false;
    }
    signed_d = e->offset + dx * nx + dy * ny;
    across = inside ? signed_d : -signed_d;
    along = fabsf(dx * -ny + dy * nx);
    if (!(across > 0.0f)) {
        return false;
    }
    *out = hypotf(across, pc_rs_maxf_k(along - EDGE_REACH, 0.0f));
    return true;
}

static const pc_fx_sharp_edge *edge_get(const pc_fx_sharp_edges *s, size_t i)
{
    uint32_t k;
    if (s->slot == NULL || i >= s->w * s->h) {
        return NULL;
    }
    k = s->slot[i];
    if (k == 0 || k - 1 >= s->npixels) {
        return NULL;
    }
    return &s->pixels[k - 1].edge;
}

static const pc_fx_sharp_edge *edge_at(const pc_fx_sharp_edges *s, size_t x, size_t y)
{
    if (x >= s->w || y >= s->h) {
        return NULL;
    }
    return edge_get(s, y * s->w + x);
}

void pc_fx_sharp_edges_fini(pc_fx_sharp_edges *e)
{
    free(e->slot);
    free(e->pixels);
    memset(e, 0, sizeof *e);
}

/* local_coverage of the pixel (x, y) of s, on the map. */
static float cov_at(const pc_fxm *s, int64_t x, int64_t y)
{
    float a = s->v[(size_t)y * s->w + (size_t)x];
    float mx;
    int dy;
    int dx;
    if (a <= PC_FX_INSIDE_EPS) {
        return a;
    }
    mx = a;
    for (dy = -1; dy <= 1; dy++) {
        for (dx = -1; dx <= 1; dx++) {
            mx = pc_rs_maxf(mx, pc_fxm_get(s, x + dx, y + dy));
        }
    }
    return pc_rs_minf_k(a / mx, 1.0f);
}

/* The coverage with the map's border repeated, for the gradient. */
static float cov_clamped(const pc_fxm *s, int64_t x, int64_t y)
{
    int64_t wi = (int64_t)s->w;
    int64_t hi = (int64_t)s->h;
    int64_t cx = x < 0 ? 0 : x;
    int64_t cy = y < 0 ? 0 : y;
    return cov_at(s, cx < wi - 1 ? cx : wi - 1, cy < hi - 1 ? cy : hi - 1);
}

/* Not empty; off the map counts as covered (only a neighbour on the map can be empty). */
static bool solid(const pc_fxm *s, int64_t x, int64_t y)
{
    float a;
    if (x < 0 || y < 0 || x >= (int64_t)s->w || y >= (int64_t)s->h) {
        return true;
    }
    a = s->v[(size_t)y * s->w + (size_t)x];
    return isnan(a) || a > PC_FX_INSIDE_EPS;
}

/* The 3x3 gradient of the coverage gives an edge's direction to 1-2 degrees with a systematic
 * error; the line through the edge points (pixel centres moved to their edge) of the pixels around
 * one gives it to a fraction of a degree. Where those points aren't on a line (corners, tight
 * curves) the gradient's normal stays. Each pixel reads the unrefined edges. */
static pc_status refine_edge_normals(pc_fx_edge_pixel *pixels, size_t n, const uint32_t *slot, size_t w,
                                     size_t h, pc_error *err)
{
    int64_t wi = (int64_t)w;
    int64_t hi = (int64_t)h;
    float (*points)[2] = NULL;
    pc_fx_sharp_edge *before = NULL;
    float (*near)[2] = NULL;
    size_t k;
    if (n == 0) {
        return PC_OK;
    }
    points = pc_malloc_array(n, sizeof *points);
    before = pc_malloc_array(n, sizeof *before);
    near = pc_malloc_array((2 * NORMAL_RADIUS + 1) * (2 * NORMAL_RADIUS + 1), sizeof *near);
    if (points == NULL || before == NULL || near == NULL) {
        free(points);
        free(before);
        free(near);
        return nomem(err);
    }
    for (k = 0; k < n; k++) {
        const pc_fx_edge_pixel *p = &pixels[k];
        points[k][0] = (float)((int64_t)p->at % wi) - p->edge.offset * p->edge.normal[0];
        points[k][1] = (float)((int64_t)p->at / wi) - p->edge.offset * p->edge.normal[1];
        before[k] = p->edge;
    }
    for (k = 0; k < n; k++) {
        pc_fx_edge_pixel *pixel = &pixels[k];
        const pc_fx_sharp_edge e = before[k];
        int64_t x = (int64_t)pixel->at % wi;
        int64_t y = (int64_t)pixel->at / wi;
        float cnt = 0.0f;
        float sum[2] = {0.0f, 0.0f};
        size_t nn = 0;
        float mean[2];
        float sxx = 0.0f;
        float sxy = 0.0f;
        float syy = 0.0f;
        float half;
        float root;
        float along;
        float across;
        float angle;
        float normal[2];
        int64_t dy;
        int64_t dx;
        size_t i;
        for (dy = -NORMAL_RADIUS; dy <= NORMAL_RADIUS; dy++) {
            for (dx = -NORMAL_RADIUS; dx <= NORMAL_RADIUS; dx++) {
                int64_t nx = x + dx;
                int64_t ny = y + dy;
                uint32_t sl;
                size_t j;
                if (dx * dx + dy * dy > NORMAL_RADIUS * NORMAL_RADIUS || nx < 0 || ny < 0 || nx >= wi ||
                    ny >= hi) {
                    continue;
                }
                sl = slot[(size_t)(ny * wi + nx)];
                if (sl == 0 || (size_t)sl - 1 >= n) {
                    continue;
                }
                j = (size_t)sl - 1;
                if (before[j].normal[0] * e.normal[0] + before[j].normal[1] * e.normal[1] > 0.9f) {
                    near[nn][0] = points[j][0];
                    near[nn][1] = points[j][1];
                    nn++;
                    sum[0] = sum[0] + points[j][0];
                    sum[1] = sum[1] + points[j][1];
                    cnt += 1.0f;
                }
            }
        }
        if (nn < 3) {
            continue;
        }
        mean[0] = sum[0] / cnt;
        mean[1] = sum[1] / cnt;
        for (i = 0; i < nn; i++) {
            float ddx = near[i][0] - mean[0];
            float ddy = near[i][1] - mean[1];
            sxx += ddx * ddx;
            sxy += ddx * ddy;
            syy += ddy * ddy;
        }
        /* Spread along and across the best line through the points. */
        half = 0.5f * (sxx + syy);
        root = sqrtf(0.25f * (sxx - syy) * (sxx - syy) + sxy * sxy);
        along = sqrtf(pc_rs_maxf_k((half + root) / cnt, 0.0f));
        across = sqrtf(pc_rs_maxf_k((half - root) / cnt, 0.0f));
        if (!(along > EDGE_LINE_MIN_SPREAD && across < EDGE_LINE_MAX_OFF)) {
            continue;
        }
        angle = 0.5f * atan2f(2.0f * sxy, sxx - syy);
        normal[0] = -sinf(angle);
        normal[1] = cosf(angle);
        if (normal[0] * e.normal[0] + normal[1] * e.normal[1] < 0.0f) {
            normal[0] = -normal[0];
            normal[1] = -normal[1];
        }
        pixel->edge.normal[0] = normal[0];
        pixel->edge.normal[1] = normal[1];
        pixel->edge.offset = pc_fx_edge_offset(normal, pixel->cov);
    }
    free(points);
    free(before);
    free(near);
    return PC_OK;
}

pc_status pc_fx_sharp_edges_find(const pc_fxm *s, pc_fx_sharp_edges *out, pc_error *err)
{
    size_t w = s->w;
    size_t h = s->h;
    int64_t wi = (int64_t)w;
    int64_t hi = (int64_t)h;
    pc_fx_sharp_edges e;
    size_t cap = 0;
    int64_t x;
    int64_t y;
    size_t k;
    pc_status st;
    memset(&e, 0, sizeof e);
    e.w = w;
    e.h = h;
    if (w == 0 || h == 0 || s->v == NULL || w * h >= UINT32_MAX) {
        *out = e;
        return PC_OK;
    }
    for (y = 0; y < hi; y++) {
        for (x = 0; x < wi; x++) {
            size_t i = (size_t)(y * wi + x);
            bool all = true;
            bool empty = false;
            bool full = false;
            float c;
            float gx;
            float gy;
            float len;
            float normal[2];
            float nrm[2];
            int dy;
            int dx;
            if (!solid(s, x, y)) {
                continue;
            }
            for (dy = -1; dy <= 1 && all; dy++) {
                for (dx = -1; dx <= 1 && all; dx++) {
                    all = solid(s, x + dx, y + dy);
                }
            }
            if (all) {
                continue;
            }
            c = cov_at(s, x, y);
            if (s->v[i] <= PC_FX_INSIDE_EPS || c >= 1.0f - PC_FX_INSIDE_EPS) {
                continue;
            }
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int64_t nx = x + dx;
                    int64_t ny = y + dy;
                    if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= wi || ny >= hi) {
                        continue;
                    }
                    empty = empty || pc_fxm_get(s, nx, ny) <= PC_FX_INSIDE_EPS;
                    full = full || cov_at(s, nx, ny) >= 1.0f - PC_FX_INSIDE_EPS;
                }
            }
            if (!(empty && full)) {
                continue;
            }
            gx = (cov_clamped(s, x + 1, y - 1) + 2.0f * cov_clamped(s, x + 1, y) +
                  cov_clamped(s, x + 1, y + 1)) -
                 (cov_clamped(s, x - 1, y - 1) + 2.0f * cov_clamped(s, x - 1, y) +
                  cov_clamped(s, x - 1, y + 1));
            gy = (cov_clamped(s, x - 1, y + 1) + 2.0f * cov_clamped(s, x, y + 1) +
                  cov_clamped(s, x + 1, y + 1)) -
                 (cov_clamped(s, x - 1, y - 1) + 2.0f * cov_clamped(s, x, y - 1) +
                  cov_clamped(s, x + 1, y - 1));
            len = hypotf(gx, gy);
            if (isfinite(len) && len > 1e-6f) {
                normal[0] = nrm[0] = gx / len;
                normal[1] = nrm[1] = gy / len;
            } else {
                normal[0] = normal[1] = 0.0f;
                nrm[0] = 1.0f;
                nrm[1] = 0.0f;
            }
            if (e.npixels == cap) {
                pc_fx_edge_pixel *grown = pc_grow_array(e.pixels, &cap, e.npixels + 1, sizeof *e.pixels);
                if (grown == NULL) {
                    pc_fx_sharp_edges_fini(&e);
                    return nomem(err);
                }
                e.pixels = grown;
            }
            e.pixels[e.npixels].at = (uint32_t)i;
            e.pixels[e.npixels].cov = c;
            e.pixels[e.npixels].edge.offset = pc_fx_edge_offset(nrm, c);
            e.pixels[e.npixels].edge.normal[0] = normal[0];
            e.pixels[e.npixels].edge.normal[1] = normal[1];
            e.npixels++;
        }
    }
    e.slot = pc_calloc_array(w * h, sizeof *e.slot);
    if (e.slot == NULL) {
        pc_fx_sharp_edges_fini(&e);
        return nomem(err);
    }
    for (k = 0; k < e.npixels; k++) {
        e.slot[e.pixels[k].at] = (uint32_t)k + 1;
    }
    st = refine_edge_normals(e.pixels, e.npixels, e.slot, w, h, err);
    if (st != PC_OK) {
        pc_fx_sharp_edges_fini(&e);
        return st;
    }
    *out = e;
    return PC_OK;
}

/* edge_distance: from pixel p to the sharp edge whose pixel q is nearest to it: the smallest
 * distance of the edge pixels around the foot of p's perpendicular on q's line (exact for a
 * straight edge). False when q gives no usable distance. */
static bool edge_distance(const pc_fx_sharp_edges *sharp, size_t p0, size_t p1, size_t q0, size_t q1,
                          bool inside, float *out)
{
    float px = (float)p0;
    float py = (float)p1;
    const pc_fx_sharp_edge *e = edge_at(sharp, q0, q1);
    float best;
    float signed_d;
    int64_t fx;
    int64_t fy;
    int64_t sx;
    int64_t sy;
    int64_t sy1;
    int64_t sx1;
    if (e == NULL || !edge_dist(e, px - (float)q0, py - (float)q1, inside, &best)) {
        return false;
    }
    signed_d = e->offset + (px - (float)q0) * e->normal[0] + (py - (float)q1) * e->normal[1];
    fx = pc_f32_to_i64(roundf(px - signed_d * e->normal[0]));
    fy = pc_f32_to_i64(roundf(py - signed_d * e->normal[1]));
    sy = wsub(fy, FOOT_RADIUS);
    sy = sy > 0 ? sy : 0;
    sy1 = wadd(fy, FOOT_RADIUS);
    sy1 = sy1 < (int64_t)sharp->h - 1 ? sy1 : (int64_t)sharp->h - 1;
    for (; sy <= sy1; sy++) {
        sx = wsub(fx, FOOT_RADIUS);
        sx = sx > 0 ? sx : 0;
        sx1 = wadd(fx, FOOT_RADIUS);
        sx1 = sx1 < (int64_t)sharp->w - 1 ? sx1 : (int64_t)sharp->w - 1;
        for (; sx <= sx1; sx++) {
            const pc_fx_sharp_edge *nb = edge_at(sharp, (size_t)sx, (size_t)sy);
            float d;
            if (nb != NULL && edge_dist(nb, px - (float)sx, py - (float)sy, inside, &d)) {
                best = pc_rs_minf(best, d);
            }
        }
    }
    *out = best;
    return true;
}

static pc_status covered_and_edt(const pc_fxm *s, bool want_empty, bool **mask, float **d, size_t **near,
                                 pc_error *err)
{
    size_t n = pc_fxm_len(s);
    size_t i;
    bool *m = pc_malloc_array(n > 0 ? n : 1, sizeof *m);
    pc_status st;
    if (m == NULL) {
        return nomem(err);
    }
    for (i = 0; i < n; i++) {
        m[i] = want_empty ? s->v[i] <= PC_FX_INSIDE_EPS : s->v[i] > PC_FX_INSIDE_EPS;
    }
    st = pc_fx_edt_nearest(m, s->w, s->h, d, near, err);
    if (st != PC_OK) {
        free(m);
        return st;
    }
    *mask = m;
    return PC_OK;
}

pc_status pc_fx_bevel_dist_inside(const pc_fxm *s, const pc_fx_sharp_edges *sharp, float reach, float **out,
                                  pc_error *err)
{
    /* The plain distance (to the nearest empty pixel, less half a pixel) never falls short of the
     * true one, so it caps the refined value where an edge lies exactly on a pixel boundary. */
    size_t w = s->w;
    size_t n = pc_fxm_len(s);
    bool *empty = NULL;
    float *d = NULL;
    size_t *near = NULL;
    float *o;
    size_t i;
    pc_status st = covered_and_edt(s, true, &empty, &d, &near, err);
    if (st != PC_OK) {
        return st;
    }
    o = pc_malloc_array(n > 0 ? n : 1, sizeof *o);
    if (o == NULL) {
        free(empty);
        free(d);
        free(near);
        return nomem(err);
    }
    for (i = 0; i < n; i++) {
        const pc_fx_sharp_edge *e;
        float plain;
        int64_t px;
        int64_t py;
        int64_t ox;
        int64_t oy;
        int64_t qx = 0;
        int64_t qy = 0;
        int64_t best = 0;
        bool have = false;
        int dy;
        int dx;
        float refined;
        if (empty[i]) {
            o[i] = -0.5f;
            continue;
        }
        e = edge_get(sharp, i);
        if (e != NULL) {
            o[i] = e->offset;
            continue;
        }
        plain = d[i] - 0.5f;
        if (d[i] > reach + EDGE_MARGIN || sharp->npixels == 0) {
            o[i] = plain;
            continue;
        }
        /* The sharp pixel next to the nearest empty one that is nearest to this pixel (the first
         * of equals, row by row). */
        px = (int64_t)(i % w);
        py = (int64_t)(i / w);
        ox = (int64_t)(near[i] % w);
        oy = (int64_t)(near[i] / w);
        for (dy = -1; dy <= 1; dy++) {
            for (dx = -1; dx <= 1; dx++) {
                int64_t x = ox + dx;
                int64_t y = oy + dy;
                int64_t key;
                if (x < 0 || y < 0 || edge_at(sharp, (size_t)x, (size_t)y) == NULL) {
                    continue;
                }
                key = (x - px) * (x - px) + (y - py) * (y - py);
                if (!have || key < best) {
                    have = true;
                    best = key;
                    qx = x;
                    qy = y;
                }
            }
        }
        if (have && edge_distance(sharp, (size_t)px, (size_t)py, (size_t)qx, (size_t)qy, true, &refined)) {
            o[i] = pc_rs_minf(refined, plain);
        } else {
            o[i] = plain;
        }
    }
    free(empty);
    free(d);
    free(near);
    *out = o;
    return PC_OK;
}

pc_status pc_fx_bevel_dist_outside(const pc_fxm *s, const pc_fx_sharp_edges *sharp, float reach, float **out,
                                   pc_error *err)
{
    size_t w = s->w;
    size_t n = pc_fxm_len(s);
    bool *inside = NULL;
    float *d = NULL;
    size_t *near = NULL;
    float *o;
    size_t i;
    pc_status st = covered_and_edt(s, false, &inside, &d, &near, err);
    if (st != PC_OK) {
        return st;
    }
    o = pc_malloc_array(n > 0 ? n : 1, sizeof *o);
    if (o == NULL) {
        free(inside);
        free(d);
        free(near);
        return nomem(err);
    }
    for (i = 0; i < n; i++) {
        const pc_fx_sharp_edge *e = edge_get(sharp, i);
        size_t q;
        float measured;
        bool ok;
        if (e != NULL) {
            o[i] = -e->offset;
            continue;
        }
        if (inside[i]) {
            o[i] = -0.5f;
            continue;
        }
        q = near[i] < n ? near[i] : 0;
        e = edge_get(sharp, q);
        if (e == NULL) {
            o[i] = d[i] - (pc_rs_minf_k(s->v[q], 1.0f) - 0.5f);
            continue;
        }
        if (d[i] <= reach + EDGE_MARGIN) {
            ok = edge_distance(sharp, i % w, i / w, q % w, q / w, false, &measured);
        } else {
            ok = edge_dist(e, (float)(i % w) - (float)(q % w), (float)(i / w) - (float)(q / w), false,
                           &measured);
        }
        o[i] = ok ? measured : d[i] - e->offset;
    }
    free(inside);
    free(d);
    free(near);
    *out = o;
    return PC_OK;
}
