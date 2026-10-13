/* Proximity match (inpaint::best_offset, with the hole-bounded summed-area table of
 * inpaint/proximity_integral.rs) and Create Texture (inpaint::synthesize with min-cut seams);
 * see inpaint.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/inpaint.h"
#include "algo/inpaint_int.h"
#include "algo/poisson.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* ---- the hole-bounded summed-area table ------------------------------------------------------------ */

pc_status pc_ip_hole_integral_new(size_t stride, const bool *hole, size_t hole_len, const size_t b[4],
                                  pc_ip_hole_integral *out, pc_error *err)
{
    size_t w, h, rs, len, x, y;
    uint32_t *data;
    memset(out, 0, sizeof *out);
    if (b[2] < b[0] || b[3] < b[1]) {
        return PC_OK;
    }
    w = b[2] - b[0], h = b[3] - b[1];
    if (w == SIZE_MAX || h == SIZE_MAX || !pc_size_mul(w + 1, h + 1, &len) || b[0] > INT32_MAX ||
        b[1] > INT32_MAX) {
        return PC_OK;
    }
    for (y = b[1]; y < b[3]; y++) {
        size_t base, lo, hi;
        if (!pc_size_mul(y, stride, &base) || !pc_size_add(base, b[0], &lo) || !pc_size_add(base, b[2], &hi) ||
            hi > hole_len) {
            return PC_OK;
        }
    }
    if (h > 0 && w > 0 && hole == NULL) {
        return PC_OK;
    }
    rs = w + 1;
    data = pc_calloc_array(len, sizeof *data);
    if (data == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (y = 0; y < h; y++) {
        const bool *row = &hole[(b[1] + y) * stride + b[0]];
        uint32_t count = 0;
        for (x = 0; x < w; x++) {
            count += row[x] ? 1u : 0u;
            data[(y + 1) * rs + x + 1] = data[y * rs + x + 1] + count;
        }
    }
    *out = (pc_ip_hole_integral){(int64_t)b[0], (int64_t)b[1], w, h, len, data};
    return PC_OK;
}

void pc_ip_hole_integral_free(pc_ip_hole_integral *t)
{
    free(t->data);
    t->data = NULL;
}

uint32_t pc_ip_hole_integral_count(const pc_ip_hole_integral *t, int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    return pc_ip_window_count(t->data, t->w, t->h, x0 - t->x0, y0 - t->y0, x1 - t->x0, y1 - t->y0);
}

/* ---- proximity match ------------------------------------------------------------------------------- */

typedef struct offset_ctx {
    size_t w, h, ch;
    const float *img;
    const pc_ip_hole_integral *hs;
    const pc_ip_pt *pts;
    size_t npts;
    int64_t x0, y0, x1, y1;
    int32_t max_radius;
    bool have;
    int32_t bx, by;
    float be;
} offset_ctx;

/* The error of displacement (dx, dy), or false when it doesn't fit or can't beat cutoff. */
static bool eval_offset(const offset_ctx *o, int32_t dx, int32_t dy, float cutoff, float *out)
{
    int64_t hx0 = o->x0 + dx, hy0 = o->y0 + dy, hx1 = o->x1 + dx, hy1 = o->y1 + dy;
    bool inside = hx0 >= 0 && hy0 >= 0 && hx1 <= (int64_t)o->w && hy1 <= (int64_t)o->h;
    float e = 0.0f, n, penalty;
    size_t i, c, ch = o->ch;
    if ((dx == 0 && dy == 0) || !inside || pc_ip_hole_integral_count(o->hs, hx0, hy0, hx1, hy1) != 0) {
        return false;
    }
    n = (float)(o->npts > 1 ? o->npts : 1);
    penalty = 1.0f + 1e-3f * (hypotf((float)dx, (float)dy) / (float)o->max_radius);
    for (i = 0; i < o->npts; i++) {
        size_t a = ((size_t)o->pts[i].y * o->w + (size_t)o->pts[i].x) * ch;
        size_t b = ((size_t)(o->pts[i].y + dy) * o->w + (size_t)(o->pts[i].x + dx)) * ch;
        for (c = 0; c < ch; c++) {
            float d = o->img[a + c] - o->img[b + c];
            e += d * d;
        }
        /* The partial score is a lower bound on the final one (same division and penalty). */
        if (i % 32 == 31 && isfinite(cutoff) && e / n * penalty >= cutoff) {
            return false;
        }
    }
    /* Mild preference for nearer sources on ties. */
    *out = e / n * penalty;
    return true;
}

static void consider_offset(offset_ctx *o, int32_t dx, int32_t dy)
{
    float e;
    if (eval_offset(o, dx, dy, o->have ? o->be : INFINITY, &e) && (!o->have || e < o->be)) {
        o->have = true, o->bx = dx, o->by = dy, o->be = e;
    }
}

pc_status pc_algo_inpaint_best_offset(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                      const bool *hole, size_t hole_len, size_t ring, int32_t max_radius,
                                      bool *found, int32_t *dx, int32_t *dy, pc_error *err)
{
    size_t n, b[4], npts = 0, stride, k, maxwh;
    int32_t extent, ringi, step, ddx, ddy;
    int64_t x, y;
    pc_ip_hole_integral hs;
    pc_ip_pt *pts;
    offset_ctx o;
    pc_status st = pc_algo_heal_dims(w, h, ch, len, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (hole_len != n || (n > 0 && (img == NULL || hole == NULL))) {
        return pc_fail(err, PC_ERR_INVALID, "hole mask holds %zu entries, expected %zu", hole_len, n);
    }
    *found = false;
    if (!pc_ip_hole_bbox(w, h, hole, b)) {
        return PC_OK;
    }
    /* No valid translation can move farther than the image's extent. */
    maxwh = w > h ? w : h;
    extent = (int32_t)(maxwh < (size_t)(INT32_MAX / 4) ? maxwh : (size_t)(INT32_MAX / 4));
    max_radius = max_radius > 0 ? max_radius : 0;
    max_radius = max_radius < extent ? max_radius : extent;
    if (max_radius == 0) {
        return PC_OK;
    }
    ring = ring > 1 ? ring : 1;
    ringi = (int32_t)(ring < (size_t)extent ? ring : (size_t)extent);
    if ((st = pc_ip_hole_integral_new(w, hole, hole_len, b, &hs, err)) != PC_OK) {
        return st;
    }
    if (hs.data == NULL) {
        return PC_OK;
    }
    pts = pc_malloc_array(n, sizeof *pts);
    if (pts == NULL) {
        pc_ip_hole_integral_free(&hs);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    o.x0 = (int64_t)b[0] - ringi, o.y0 = (int64_t)b[1] - ringi;
    o.x1 = (int64_t)b[2] + ringi, o.y1 = (int64_t)b[3] + ringi;
    /* Sample points: the hole's ring (dilated minus hole), subsampled for speed. */
    for (y = o.y0 > 0 ? o.y0 : 0; y < (o.y1 < (int64_t)h ? o.y1 : (int64_t)h); y++) {
        for (x = o.x0 > 0 ? o.x0 : 0; x < (o.x1 < (int64_t)w ? o.x1 : (int64_t)w); x++) {
            if (hole[(size_t)y * w + (size_t)x]) {
                continue;
            }
            if (pc_ip_hole_integral_count(&hs, x - ringi, y - ringi, x + ringi + 1, y + ringi + 1) > 0) {
                pts[npts++] = (pc_ip_pt){(int32_t)x, (int32_t)y};
            }
        }
    }
    stride = npts / 2000 > 1 ? npts / 2000 : 1;
    {
        size_t m = 0;
        for (k = 0; k < npts; k += stride) {
            pts[m++] = pts[k];
        }
        npts = m;
    }
    o.w = w, o.h = h, o.ch = ch, o.img = img, o.hs = &hs, o.pts = pts, o.npts = npts;
    o.max_radius = max_radius, o.have = false, o.bx = 0, o.by = 0, o.be = INFINITY;
    /* Coarse grid over the search window, then a full-resolution refinement around the winner. */
    step = max_radius / 24 > 1 ? max_radius / 24 : 1;
    for (ddy = -max_radius; ddy <= max_radius; ddy += step) {
        for (ddx = -max_radius; ddx <= max_radius; ddx += step) {
            consider_offset(&o, ddx, ddy);
        }
    }
    if (step > 1 && o.have) {
        int32_t bx = o.bx, by = o.by;
        for (ddy = by - step; ddy <= by + step; ddy++) {
            for (ddx = bx - step; ddx <= bx + step; ddx++) {
                consider_offset(&o, ddx, ddy);
            }
        }
    }
    free(pts);
    pc_ip_hole_integral_free(&hs);
    *found = o.have;
    if (o.have) {
        *dx = o.bx;
        *dy = o.by;
    }
    return PC_OK;
}

/* ---- texture synthesis ------------------------------------------------------------------------------ */

/* The minimum-error cut through a band `across` wide and `along` long (Efros & Freeman's boundary
 * cut): acc holds err(u, v) at [v * across + u] on entry (it is overwritten); *cut gets `along`
 * offsets, or *ncut 0 when there is no cost anywhere. */
static void min_cut(size_t across, size_t along, float *acc, size_t *cut, size_t *ncut)
{
    size_t u, v, k, n = across * along;
    bool any = false;
    *ncut = 0;
    if (across == 0 || along == 0) {
        return;
    }
    for (k = 0; k < n; k++) {
        any = any || acc[k] != 0.0f;
    }
    if (!any) {
        return;
    }
    for (v = 1; v < along; v++) {
        for (u = 0; u < across; u++) {
            size_t lo = u > 0 ? u - 1 : 0, hi = u + 1 < across - 1 ? u + 1 : across - 1;
            float m = INFINITY;
            for (k = lo; k <= hi; k++) {
                m = pc_rs_minf(m, acc[(v - 1) * across + k]);
            }
            acc[v * across + u] += m;
        }
    }
    for (v = along; v-- > 0;) {
        size_t lo, hi, best;
        if (v + 1 < along) {
            u = cut[v + 1];
            lo = u > 0 ? u - 1 : 0, hi = u + 1 < across - 1 ? u + 1 : across - 1;
        } else {
            lo = 0, hi = across - 1;
        }
        /* The first minimum in total order. */
        best = lo;
        for (k = lo + 1; k <= hi; k++) {
            float a = acc[v * across + k], b = acc[v * across + best];
            int32_t ia, ib;
            memcpy(&ia, &a, 4);
            memcpy(&ib, &b, 4);
            ia ^= (int32_t)((uint32_t)(ia >> 31) >> 1);
            ib ^= (int32_t)((uint32_t)(ib >> 31) >> 1);
            if (ia < ib) {
                best = k;
            }
        }
        cut[v] = best;
    }
    *ncut = along;
}

/* Squared difference between what is there and the block's pixel, where both exist. */
static float seam_err(size_t w, size_t ch, const float *o, const float *img, const bool *hole, const bool *filled,
                      int64_t x, int64_t y, int64_t bx, int64_t by, pc_ip_pt c)
{
    size_t i = (size_t)y * w + (size_t)x, s, q;
    float sum = -0.0f;
    if (!(hole[i] && filled[i])) {
        return 0.0f;
    }
    s = ((size_t)(y - by + c.y) * w + (size_t)(x - bx + c.x)) * ch;
    for (q = 0; q < ch; q++) {
        float d = o[i * ch + q] - img[s + q];
        sum += d * d;
    }
    return sum;
}

pc_status pc_algo_inpaint_synthesize(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                     const bool *hole, size_t hole_len, size_t block, uint64_t seed,
                                     float **out, pc_error *err)
{
    size_t n, bb[4], ncand = 0, q, side;
    int64_t b, ov, x, y, bx, by;
    uint32_t *hs = NULL;
    pc_ip_pt *cands = NULL;
    bool *filled = NULL;
    float *o, *acc = NULL;
    size_t *left = NULL, *top = NULL;
    pc_ip_rng g;
    pc_status st = pc_algo_heal_dims(w, h, ch, len, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (hole_len != n || (n > 0 && (img == NULL || hole == NULL))) {
        return pc_fail(err, PC_ERR_INVALID, "hole mask holds %zu entries, expected %zu", hole_len, n);
    }
    if (block > PC_ALGO_HEAL_MAX_SIDE) {
        return pc_fail(err, PC_ERR_LIMIT, "block %zu too large", block);
    }
    o = pc_malloc_array(n * ch > 0 ? n * ch : 1, sizeof *o);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (n > 0) {
        memcpy(o, img, n * ch * sizeof *o);
    }
    if (!pc_ip_hole_bbox(w, h, hole, bb)) {
        *out = o;
        return PC_OK;
    }
    b = block > 4 ? (int64_t)block : 4;
    ov = b / 3 > 1 ? b / 3 : 1;
    side = (size_t)(b + 2 * ov);
    hs = pc_ip_integral(w, h, hole);
    cands = pc_malloc_array(n, sizeof *cands);
    if (hs == NULL || cands == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    /* Candidate block origins: the whole block (with overlap) inside the grid and outside the hole. */
    for (y = ov; y < (int64_t)h - b - ov; y++) {
        for (x = ov; x < (int64_t)w - b - ov; x++) {
            if (pc_ip_window_count(hs, w, h, x - ov, y - ov, x + b + ov, y + b + ov) == 0) {
                cands[ncand++] = (pc_ip_pt){(int32_t)x, (int32_t)y};
            }
        }
    }
    if (ncand == 0) {
        float *m = NULL;
        st = pc_algo_poisson_membrane_fill(w, h, ch, img, len, hole, hole_len, NULL, &m, err);
        if (st == PC_OK) {
            free(o);
            o = m;
        }
        goto cleanup;
    }
    filled = pc_malloc_array(n, sizeof *filled);
    acc = pc_malloc_array(side, side * sizeof *acc);
    left = pc_malloc_array(side, sizeof *left);
    top = pc_malloc_array(side, sizeof *top);
    if (filled == NULL || acc == NULL || left == NULL || top == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    for (q = 0; q < n; q++) {
        filled[q] = !hole[q];
    }
    g = pc_ip_rng_new(seed);
    for (by = (int64_t)bb[1]; by < (int64_t)bb[3]; by += b) {
        for (bx = (int64_t)bb[0]; bx < (int64_t)bb[2]; bx += b) {
            pc_ip_pt best;
            float best_e = INFINITY;
            int k;
            int64_t x0, y0, x1, y1, lx1, ty1;
            size_t nleft, ntop, across, along, u, v;
            if (pc_ip_window_count(hs, w, h, bx, by, bx + b, by + b) == 0) {
                continue;
            }
            /* Pick the candidate whose block best agrees with already-known pixels around/inside. */
            best = cands[0];
            for (k = 0; k < 48; k++) {
                pc_ip_pt c = cands[pc_ip_rng_next(&g) % (uint64_t)ncand];
                float e = 0.0f;
                uint32_t cnt = 0;
                for (y = by - ov; y < by + b + ov; y++) {
                    for (x = bx - ov; x < bx + b + ov; x++) {
                        size_t a, s;
                        if (x < 0 || y < 0 || x >= (int64_t)w || y >= (int64_t)h ||
                            !filled[(size_t)y * w + (size_t)x]) {
                            continue;
                        }
                        a = ((size_t)y * w + (size_t)x) * ch;
                        s = ((size_t)(y - by + c.y) * w + (size_t)(x - bx + c.x)) * ch;
                        for (q = 0; q < ch; q++) {
                            float d = o[a + q] - img[s + q];
                            e += d * d;
                        }
                        cnt++;
                    }
                }
                e = cnt == 0 ? 0.0f : e / (float)cnt;
                if (e < best_e) {
                    best = c;
                    best_e = e;
                }
            }
            x0 = bx - ov > 0 ? bx - ov : 0, y0 = by - ov > 0 ? by - ov : 0;
            x1 = bx + b + ov < (int64_t)w ? bx + b + ov : (int64_t)w;
            y1 = by + b + ov < (int64_t)h ? by + b + ov : (int64_t)h;
            /* Minimum-error cuts through the overlaps with the blocks to the left and above. */
            lx1 = bx + ov < x1 ? bx + ov : x1;
            across = lx1 > x0 ? (size_t)(lx1 - x0) : 0, along = y1 > y0 ? (size_t)(y1 - y0) : 0;
            for (v = 0; v < along; v++) {
                for (u = 0; u < across; u++) {
                    acc[v * across + u] = seam_err(w, ch, o, img, hole, filled, x0 + (int64_t)u, y0 + (int64_t)v,
                                                   bx, by, best);
                }
            }
            min_cut(across, along, acc, left, &nleft);
            ty1 = by + ov < y1 ? by + ov : y1;
            across = ty1 > y0 ? (size_t)(ty1 - y0) : 0, along = x1 > x0 ? (size_t)(x1 - x0) : 0;
            for (v = 0; v < along; v++) {
                for (u = 0; u < across; u++) {
                    acc[v * across + u] = seam_err(w, ch, o, img, hole, filled, x0 + (int64_t)v, y0 + (int64_t)u,
                                                   bx, by, best);
                }
            }
            min_cut(across, along, acc, top, &ntop);
            for (y = y0; y < y1; y++) {
                for (x = x0; x < x1; x++) {
                    size_t i = (size_t)y * w + (size_t)x;
                    bool keep;
                    u = (size_t)(x - x0), v = (size_t)(y - y0);
                    if (!hole[i]) {
                        continue;
                    }
                    keep = filled[i] && ((v < nleft && u < left[v]) || (u < ntop && v < top[u]));
                    if (!keep) {
                        size_t s = ((size_t)(y - by + best.y) * w + (size_t)(x - bx + best.x)) * ch;
                        memcpy(&o[i * ch], &img[s], ch * sizeof(float));
                    }
                    filled[i] = true;
                }
            }
        }
    }
cleanup:
    free(hs);
    free(cands);
    free(filled);
    free(acc);
    free(left);
    free(top);
    if (st != PC_OK) {
        free(o);
        return st;
    }
    *out = o;
    return PC_OK;
}
