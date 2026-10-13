/* Non-local patch-based completion (port of crates/algo/src/nonlocal.rs); see nonlocal.h. The seam
 * blending (blend_seams, fuse_seams) is in nonlocal_seams.c. */
#include "algo/nonlocal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/inpaint.h"
#include "algo/inpaint_int.h"
#include "algo/nonlocal_int.h"
#include "algo/poisson.h"
#include "algo/remove.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Below this many grid cells a level runs single-threaded. */
#define NL_PAR_MIN ((size_t)128 * 128)
#define NL_BAND ((size_t)16)
#define NL_MAX_LEVELS 64

pc_algo_nonlocal_params pc_algo_nonlocal_params_default(void)
{
    pc_algo_nonlocal_params p = {3, 50.0f, 10, 2, 0.1f / 255.0f, 1};
    return p;
}

/* Fully-known patch centres: mask (*valid) and list (*list, *nlist). */
static pc_status nl_valid_sources(size_t w, size_t h, const bool *hole, size_t r, bool **valid, pc_ip_pt **list,
                                  size_t *nlist, pc_error *err)
{
    size_t w1 = w + 1, x, y, n = 0;
    bool *v = pc_calloc_array(w * h > 0 ? w * h : 1, sizeof *v);
    pc_ip_pt *l = pc_malloc_array(w * h > 0 ? w * h : 1, sizeof *l);
    uint32_t *sat;
    if (v == NULL || l == NULL) {
        free(v);
        free(l);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (w >= 2 * r + 1 && h >= 2 * r + 1) {
        sat = pc_calloc_array(w1 * (h + 1), sizeof *sat);
        if (sat == NULL) {
            free(v);
            free(l);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (y = 0; y < h; y++) {
            uint32_t row = 0;
            for (x = 0; x < w; x++) {
                row += hole[y * w + x] ? 1u : 0u;
                sat[(y + 1) * w1 + x + 1] = sat[y * w1 + x + 1] + row;
            }
        }
        for (y = r; y < h - r; y++) {
            for (x = r; x < w - r; x++) {
                size_t x0 = x - r, y0 = y - r, x1 = x + r + 1, y1 = y + r + 1;
                if (sat[y1 * w1 + x1] + sat[y0 * w1 + x0] == sat[y0 * w1 + x1] + sat[y1 * w1 + x0]) {
                    v[y * w + x] = true;
                    l[n++] = (pc_ip_pt){(int32_t)x, (int32_t)y};
                }
            }
        }
        free(sat);
    }
    *valid = v, *list = l, *nlist = n;
    return PC_OK;
}

/* One texture feature table: the mean |g(n) - g(i)| over a side-wide window, n = i + (dx, dy). */
static pc_status feature_table(size_t w, size_t h, const float *g, const bool *hole, size_t side, size_t dx,
                               size_t dy, float *out, pc_error *err)
{
    size_t w1 = w + 1, x, y, half = side / 2;
    double *sum = pc_calloc_array(w1 * (h + 1), sizeof *sum);
    uint32_t *cnt = pc_calloc_array(w1 * (h + 1), sizeof *cnt);
    if (sum == NULL || cnt == NULL) {
        free(sum);
        free(cnt);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (y = 0; y < h; y++) {
        double rs = 0.0;
        uint32_t rc = 0;
        for (x = 0; x < w; x++) {
            size_t i = y * w + x, nx = x + dx, ny = y + dy;
            if (nx < w && ny < h && !hole[i] && !hole[ny * w + nx]) {
                rs += (double)fabsf(g[ny * w + nx] - g[i]);
                rc++;
            }
            sum[(y + 1) * w1 + x + 1] = sum[y * w1 + x + 1] + rs;
            cnt[(y + 1) * w1 + x + 1] = cnt[y * w1 + x + 1] + rc;
        }
    }
    for (y = 0; y < h; y++) {
        size_t y0 = y > half ? y - half : 0, y1 = y + half + 1 < h ? y + half + 1 : h;
        for (x = 0; x < w; x++) {
            size_t i = y * w + x, x0 = x > half ? x - half : 0, x1 = x + half + 1 < w ? x + half + 1 : w;
            double s;
            uint32_t n;
            if (hole[i]) {
                out[i] = 0.0f;
                continue;
            }
            s = sum[y1 * w1 + x1] + sum[y0 * w1 + x0] - sum[y0 * w1 + x1] - sum[y1 * w1 + x0];
            n = cnt[y1 * w1 + x1] + cnt[y0 * w1 + x0] - cnt[y0 * w1 + x1] - cnt[y1 * w1 + x0];
            out[i] = n == 0 ? 0.0f : (float)(s / (double)n);
        }
    }
    free(sum);
    free(cnt);
    return PC_OK;
}

/* The next coarser level: colour from a 3x3 Gaussian over the known cells, features by
 * nearest-neighbour subsampling, a cell in the hole when any of its four is. */
static pc_status nl_downsample(const nl_level *l, size_t cha, size_t ch, nl_level *out, pc_error *err)
{
    static const float K[3] = {0.6412f, 0.8007f, 0.6412f};
    size_t w = (l->w + 1) / 2, h = (l->h + 1) / 2, x, y, c;
    nl_level o = {w, h, pc_calloc_array(w * h * cha, sizeof(float)), pc_calloc_array(w * h, sizeof(bool))};
    if (o.img == NULL || o.hole == NULL) {
        free(o.img);
        free(o.hole);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t fx = x * 2, fy = y * 2, o_ = (y * w + x) * cha, si;
            float wsum = 0.0f;
            int ky, kx;
            bool hl = false;
            for (ky = 0; ky < 2 && !hl; ky++) {
                for (kx = 0; kx < 2; kx++) {
                    if (fx + (size_t)kx < l->w && fy + (size_t)ky < l->h &&
                        l->hole[(fy + (size_t)ky) * l->w + fx + (size_t)kx]) {
                        hl = true;
                        break;
                    }
                }
            }
            o.hole[y * w + x] = hl;
            for (ky = 0; ky < 3; ky++) {
                for (kx = 0; kx < 3; kx++) {
                    int64_t sx = (int64_t)fx + kx - 1, sy = (int64_t)fy + ky - 1;
                    float k;
                    if (sx < 0 || sy < 0 || (size_t)sx >= l->w || (size_t)sy >= l->h) {
                        continue;
                    }
                    si = (size_t)sy * l->w + (size_t)sx;
                    if (l->hole[si]) {
                        continue;
                    }
                    k = K[kx] * K[ky];
                    for (c = 0; c < ch; c++) {
                        o.img[o_ + c] += l->img[si * cha + c] * k;
                    }
                    wsum += k;
                }
            }
            if (wsum > 0.0f) {
                for (c = 0; c < ch; c++) {
                    o.img[o_ + c] /= wsum;
                }
            }
            si = (fy * l->w + fx) * cha;
            memcpy(&o.img[o_ + ch], &l->img[si + ch], (cha - ch) * sizeof(float));
        }
    }
    *out = o;
    return PC_OK;
}

pc_status pc_algo_nonlocal_level_count(size_t w, size_t h, const bool *hole, size_t r, size_t *out, pc_error *err)
{
    size_t n = 2 * r + 1, lw = w, lh = h, levels = 1, want;
    uint32_t no;
    double v;
    pc_status st = pc_algo_remove_hole_radius(w, h, hole, w * h, &no, err);
    if (st != PC_OK) {
        return st;
    }
    no = no < (1u << 20) ? no : (1u << 20);
    v = pc_rs_max_k(ceil(log2(2.0 * (double)no / (double)n)), 1.0);
    want = (size_t)v;
    while (levels < want && lw / 2 >= 3 * n && lh / 2 >= 3 * n) {
        lw = (lw + 1) / 2, lh = (lh + 1) / 2, levels++;
    }
    *out = levels;
    return PC_OK;
}

/* ---- the solver ---------------------------------------------------------------------------------- */

bool pc_nl_is_valid(const nl_solver *s, int32_t x, int32_t y)
{
    return x >= 0 && y >= 0 && (size_t)x < s->w && (size_t)y < s->h && s->valid[(size_t)y * s->w + (size_t)x];
}

static pc_ip_pt random_source(const nl_solver *s, pc_ip_rng *rng)
{
    uint64_t n = s->nvalid > 1 ? (uint64_t)s->nvalid : 1;
    size_t k = (size_t)(pc_ip_rng_next(rng) % n);
    return k < s->nvalid ? s->valid_list[k] : (pc_ip_pt){0, 0};
}

/* Mean squared difference between the target patch at t (clipped; with `known`, only its known
 * cells) and the source patch at src; infinity once it can't beat cutoff (without `known`). */
static float nl_dist(const nl_solver *s, pc_ip_pt t, pc_ip_pt src, float cutoff, const bool *known)
{
    int32_t w = (int32_t)s->w, h = (int32_t)s->h, r = s->r, dy, dx;
    size_t cha = s->cha, n = 0, c;
    float full = (float)((2 * r + 1) * (2 * r + 1)) * (float)cha, sum = 0.0f;
    for (dy = -r; dy <= r; dy++) {
        int32_t ty = t.y + dy;
        if (ty < 0 || ty >= h) {
            continue;
        }
        for (dx = -r; dx <= r; dx++) {
            int32_t tx = t.x + dx;
            size_t tc, ti, si;
            if (tx < 0 || tx >= w) {
                continue;
            }
            tc = (size_t)ty * s->w + (size_t)tx;
            if (known != NULL && !known[tc]) {
                continue;
            }
            ti = tc * cha;
            si = ((size_t)(src.y + dy) * s->w + (size_t)(src.x + dx)) * cha;
            for (c = 0; c < cha; c++) {
                float d = s->img[ti + c] - s->img[si + c];
                sum += d * d;
            }
            n += cha;
        }
        if (known == NULL && sum > cutoff * full) {
            return INFINITY;
        }
    }
    return n == 0 ? INFINITY : sum / (float)n;
}

typedef struct nl_pm_job {
    const nl_solver *s;
    pc_ip_pt *nnf;
    float *cost;
    size_t pass;
    uint64_t seed;
    int32_t radius;
} nl_pm_job;

static void nl_try(const nl_solver *s, pc_ip_pt t, pc_ip_pt cand, pc_ip_pt *best, float *bc)
{
    if ((cand.x != best->x || cand.y != best->y) && pc_nl_is_valid(s, cand.x, cand.y)) {
        float d = nl_dist(s, t, cand, *bc, NULL);
        if (d < *bc) {
            *best = cand, *bc = d;
        }
    }
}

static pc_status nl_pm_bands(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const nl_pm_job *j = ctx;
    const nl_solver *s = j->s;
    size_t w = s->w, bi;
    bool reverse = j->pass % 2 == 1;
    int32_t step = reverse ? -1 : 1;
    (void)chunk;
    for (bi = begin; bi < end; bi++) {
        size_t y0 = bi * NL_BAND, rows = y0 + NL_BAND < s->h ? NL_BAND : s->h - y0, ry, rx;
        pc_ip_pt *nn = &j->nnf[y0 * w];
        float *co = &j->cost[y0 * w];
        pc_ip_rng rng = pc_ip_rng_new(j->seed ^ ((uint64_t)bi * 0x2545F4914F6CDD1Dull) ^ ((uint64_t)j->pass << 40));
        if (pc_interrupt_cancelled(s->ctl)) {
            continue;
        }
        for (ry = 0; ry < rows; ry++) {
            size_t ly = reverse ? rows - 1 - ry : ry, y = y0 + ly;
            for (rx = 0; rx < w; rx++) {
                size_t x = reverse ? w - 1 - rx : rx, li = ly * w + x;
                int64_t nx = (int64_t)x - step, nly = (int64_t)ly - step;
                pc_ip_pt t = {(int32_t)x, (int32_t)y}, best = nn[li];
                float bc = co[li];
                int32_t rad;
                if (!s->targets[y * w + x]) {
                    continue;
                }
                if (nx >= 0 && (size_t)nx < w) {
                    pc_ip_pt c = nn[ly * w + (size_t)nx];
                    nl_try(s, t, (pc_ip_pt){c.x + step, c.y}, &best, &bc);
                }
                if (nly >= 0 && (size_t)nly < rows) {
                    pc_ip_pt c = nn[(size_t)nly * w + x];
                    nl_try(s, t, (pc_ip_pt){c.x, c.y + step}, &best, &bc);
                }
                for (rad = j->radius; rad >= 1; rad /= 2) {
                    int32_t cx = best.x + pc_ip_rng_range(&rng, -rad, rad);
                    int32_t cy = best.y + pc_ip_rng_range(&rng, -rad, rad);
                    nl_try(s, t, (pc_ip_pt){cx, cy}, &best, &bc);
                }
                nn[li] = best;
                co[li] = bc;
            }
        }
    }
    return PC_OK;
}

static void nl_patchmatch(const nl_solver *s, pc_ip_pt *nnf, float *cost, size_t pass, uint64_t seed, int32_t radius)
{
    nl_pm_job j = {s, nnf, cost, pass, seed, radius};
    size_t nb = (s->h + NL_BAND - 1) / NL_BAND;
    if (s->w * s->h >= NL_PAR_MIN) {
        (void)pc_algo_par_for(nb, 1, nl_pm_bands, &j);
    } else {
        (void)nl_pm_bands(&j, 0, 0, nb);
    }
}

typedef struct nl_cost_job {
    const nl_solver *s;
    const pc_ip_pt *nnf;
    float *cost;
} nl_cost_job;

static pc_status nl_cost_run(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const nl_cost_job *j = ctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        pc_ip_pt t = {(int32_t)(i % j->s->w), (int32_t)(i / j->s->w)};
        j->cost[i] = j->s->targets[i] ? nl_dist(j->s, t, j->nnf[i], INFINITY, NULL) : INFINITY;
    }
    return PC_OK;
}

static void nl_refresh_costs(const nl_solver *s, const pc_ip_pt *nnf, float *cost)
{
    nl_cost_job j = {s, nnf, cost};
    size_t n = s->w * s->h;
    if (n >= NL_PAR_MIN) {
        (void)pc_algo_par_for(n, 1024, nl_cost_run, &j);
    } else {
        (void)nl_cost_run(&j, 0, 0, n);
    }
}

/* ---- votes over the hole cells ------------------------------------------------------------------- */

typedef struct nl_vote_job {
    const nl_solver *s;
    const pc_ip_pt *nnf;
    const float *cost;
    float sigma2;
    bool uniform, best_only;
    float *vals;   /* nhole * cha (vote) */
    size_t *pick;  /* nhole (best_only) */
    float *pcost;  /* nhole (best_only) */
    bool *got;     /* nhole */
} nl_vote_job;

static void nl_cell(const nl_vote_job *j, size_t hi)
{
    const nl_solver *s = j->s;
    size_t i = s->hole_list[hi], x = i % s->w, y = i / s->w, cha = s->cha, c;
    int32_t w = (int32_t)s->w, h = (int32_t)s->h, r = s->r, dy, dx;
    float wsum = 0.0f, *v = j->best_only ? NULL : &j->vals[hi * cha], bc = 0.0f;
    bool have = false;
    if (v != NULL) {
        memset(v, 0, cha * sizeof *v);
    }
    for (dy = -r; dy <= r; dy++) {
        int32_t ty = (int32_t)y + dy;
        if (ty < 0 || ty >= h) {
            continue;
        }
        for (dx = -r; dx <= r; dx++) {
            int32_t tx = (int32_t)x + dx, sx, sy;
            size_t ti, si;
            if (tx < 0 || tx >= w) {
                continue;
            }
            ti = (size_t)ty * s->w + (size_t)tx;
            if (!s->targets[ti] || !isfinite(j->cost[ti])) {
                continue;
            }
            sx = j->nnf[ti].x - dx, sy = j->nnf[ti].y - dy;
            if (sx < 0 || sy < 0 || sx >= w || sy >= h) {
                continue;
            }
            si = (size_t)sy * s->w + (size_t)sx;
            if (j->best_only) {
                if (!have || j->cost[ti] < bc) {
                    have = true, bc = j->cost[ti];
                    j->pick[hi] = si;
                }
            } else {
                float wt = j->uniform ? 1.0f : pc_rs_maxf_k(expf(-j->cost[ti] / (2.0f * j->sigma2)), 1e-8f);
                for (c = 0; c < cha; c++) {
                    v[c] += s->img[si * cha + c] * wt;
                }
                wsum += wt;
            }
        }
    }
    if (j->best_only) {
        j->got[hi] = have;
        j->pcost[hi] = bc;
    } else {
        j->got[hi] = wsum > 0.0f;
        if (wsum > 0.0f) {
            for (c = 0; c < cha; c++) {
                v[c] /= wsum;
            }
        }
    }
}

static pc_status nl_cells_run(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const nl_vote_job *j = ctx;
    size_t hi;
    (void)chunk;
    if (pc_interrupt_cancelled(j->s->ctl)) {
        return PC_OK;
    }
    for (hi = begin; hi < end; hi++) {
        nl_cell(j, hi);
    }
    return PC_OK;
}

static void nl_cells(const nl_solver *s, nl_vote_job *j)
{
    if (s->w * s->h >= NL_PAR_MIN) {
        (void)pc_algo_par_for(s->nhole, 256, nl_cells_run, j);
    } else {
        (void)nl_cells_run(j, 0, 0, s->nhole);
    }
}

/* The similarity-weighted vote (uniform: all weights equal). *change: the mean absolute change of
 * the colour channels. */
static pc_status nl_vote(nl_solver *s, const pc_ip_pt *nnf, const float *cost, bool uniform, float *change,
                         pc_error *err)
{
    size_t n = s->w * s->h, i, nc = 0, cha = s->cha, c, k;
    float *cs = pc_malloc_array(n, sizeof *cs);
    nl_vote_job j;
    double ch_sum = 0.0;
    pc_status st;
    memset(&j, 0, sizeof j);
    j.vals = pc_calloc_array(s->nhole > 0 ? s->nhole * cha : 1, sizeof *j.vals);
    j.got = pc_calloc_array(s->nhole > 0 ? s->nhole : 1, sizeof *j.got);
    if (cs == NULL || j.vals == NULL || j.got == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    for (i = 0; i < n; i++) {
        if (s->targets[i] && isfinite(cost[i])) {
            cs[nc++] = cost[i];
        }
    }
    j.s = s, j.nnf = nnf, j.cost = cost, j.uniform = uniform;
    if ((st = pc_ip_sigma2(cs, nc, &j.sigma2, err)) != PC_OK) {
        goto cleanup;
    }
    nl_cells(s, &j);
    nc = 0;
    for (k = 0; k < s->nhole; k++) {
        if (!j.got[k]) {
            continue;
        }
        i = s->hole_list[k];
        for (c = 0; c < s->ch; c++) {
            ch_sum += (double)fabsf(j.vals[k * cha + c] - s->img[i * cha + c]);
        }
        nc += s->ch;
        memcpy(&s->img[i * cha], &j.vals[k * cha], cha * sizeof(float));
    }
    *change = nc == 0 ? 0.0f : (float)(ch_sum / (double)nc);
cleanup:
    free(cs);
    free(j.vals);
    free(j.got);
    return st;
}

/* The final reconstruction: each hole cell copies the source cell of the lowest-cost patch covering
 * it, in raster order. *copied: (hole cell, source cell, cost) triples. */
static pc_status nl_copy_best(nl_solver *s, const pc_ip_pt *nnf, const float *cost, nl_copied **copied,
                              size_t *ncopied, pc_error *err)
{
    nl_vote_job j;
    size_t k, m = 0, cha = s->cha;
    nl_copied *out;
    memset(&j, 0, sizeof j);
    j.s = s, j.nnf = nnf, j.cost = cost, j.best_only = true;
    j.pick = pc_malloc_array(s->nhole > 0 ? s->nhole : 1, sizeof *j.pick);
    j.pcost = pc_malloc_array(s->nhole > 0 ? s->nhole : 1, sizeof *j.pcost);
    j.got = pc_calloc_array(s->nhole > 0 ? s->nhole : 1, sizeof *j.got);
    out = pc_malloc_array(s->nhole > 0 ? s->nhole : 1, sizeof *out);
    if (j.pick == NULL || j.pcost == NULL || j.got == NULL || out == NULL) {
        free(j.pick), free(j.pcost), free(j.got), free(out);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    nl_cells(s, &j);
    for (k = 0; k < s->nhole; k++) {
        if (j.got[k]) {
            size_t i = s->hole_list[k];
            memmove(&s->img[i * cha], &s->img[j.pick[k] * cha], cha * sizeof(float));
            out[m++] = (nl_copied){i, j.pick[k], j.pcost[k]};
        }
    }
    free(j.pick), free(j.pcost), free(j.got);
    *copied = out, *ncopied = m;
    return PC_OK;
}

/* ---- onion peel --------------------------------------------------------------------------------- */

typedef struct peel_job {
    const nl_solver *s;
    const pc_ip_pt *nnf;
    const bool *has, *known;
    const size_t *centres;
    pc_ip_pt *found;
    float *fcost;
    uint64_t seed, layer_no;
} peel_job;

static void peel_try(const nl_solver *s, pc_ip_pt t, pc_ip_pt cand, const bool *known, pc_ip_pt *best, float *bc)
{
    if (pc_nl_is_valid(s, cand.x, cand.y)) {
        float d = nl_dist(s, t, cand, *bc, known);
        if (d < *bc) {
            *best = cand, *bc = d;
        }
    }
}

static pc_status peel_search(void *ctx, size_t chunk, size_t begin, size_t end)
{
    static const int32_t NB[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    const peel_job *j = ctx;
    const nl_solver *s = j->s;
    size_t q, w = s->w, h = s->h;
    (void)chunk;
    for (q = begin; q < end; q++) {
        size_t ci = j->centres[q];
        pc_ip_pt t = {(int32_t)(ci % w), (int32_t)(ci / w)}, best = {0, 0};
        pc_ip_rng rng = pc_ip_rng_new(j->seed ^ ((uint64_t)ci * 0x9E3779B9ull) ^ (j->layer_no << 48));
        float bc = INFINITY;
        int k, pass;
        if (j->has[ci]) {
            peel_try(s, t, j->nnf[ci], j->known, &best, &bc);
        }
        for (k = 0; k < 4; k++) {
            int32_t nx = t.x + NB[k][0], ny = t.y + NB[k][1];
            if (nx >= 0 && ny >= 0 && (size_t)nx < w && (size_t)ny < h && j->has[(size_t)ny * w + (size_t)nx]) {
                pc_ip_pt sp = j->nnf[(size_t)ny * w + (size_t)nx];
                peel_try(s, t, (pc_ip_pt){sp.x - NB[k][0], sp.y - NB[k][1]}, j->known, &best, &bc);
            }
        }
        for (k = 0; k < 8; k++) {
            peel_try(s, t, random_source(s, &rng), j->known, &best, &bc);
        }
        for (pass = 0; pass < 2; pass++) {
            int32_t rad = (int32_t)(w > h ? w : h);
            for (; rad >= 1; rad /= 2) {
                int32_t cx = best.x + pc_ip_rng_range(&rng, -rad, rad);
                int32_t cy = best.y + pc_ip_rng_range(&rng, -rad, rad);
                peel_try(s, t, (pc_ip_pt){cx, cy}, j->known, &best, &bc);
            }
        }
        j->found[q] = best;
        j->fcost[q] = bc;
    }
    return PC_OK;
}

/* Fill the hole one boundary layer at a time; leaves the matches found in nnf (has marks them). */
static pc_status nl_onion_peel(nl_solver *s, pc_ip_pt *nnf, bool *has, uint64_t seed, pc_error *err)
{
    size_t w = s->w, h = s->h, n = w * h, cha = s->cha, nunk = 0, i, k, nl, nc, nv;
    int32_t r = s->r;
    bool *known = pc_malloc_array(n, sizeof *known), *centre = pc_calloc_array(n, sizeof *centre);
    size_t *unknown = pc_malloc_array(n, sizeof *unknown), *layer = pc_malloc_array(n, sizeof *layer);
    size_t *centres = pc_malloc_array(n, sizeof *centres), *vidx = pc_malloc_array(n, sizeof *vidx);
    float *layer_cost = pc_malloc_array(n, sizeof *layer_cost), *fcost = pc_malloc_array(n, sizeof *fcost);
    float *cs = pc_malloc_array(n, sizeof *cs), *vals = NULL;
    pc_ip_pt *found = pc_malloc_array(n, sizeof *found);
    uint64_t layer_no = 0;
    pc_status st = PC_OK;
    if (known == NULL || centre == NULL || unknown == NULL || layer == NULL || centres == NULL || vidx == NULL ||
        layer_cost == NULL || fcost == NULL || cs == NULL || found == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    vals = pc_malloc_array(n, cha * sizeof *vals);
    if (vals == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    for (i = 0; i < n; i++) {
        known[i] = !s->hole[i];
        layer_cost[i] = INFINITY;
        if (s->hole[i]) {
            unknown[nunk++] = i;
        }
    }
    for (;;) {
        float sigma2;
        peel_job pj;
        if ((st = pc_interrupt_check(s->ctl, err)) != PC_OK) {
            goto cleanup;
        }
        nl = 0;
        for (k = 0; k < nunk; k++) {
            size_t u = unknown[k], x = u % w, y = u / w;
            if ((x > 0 && known[u - 1]) || (x + 1 < w && known[u + 1]) || (y > 0 && known[u - w]) ||
                (y + 1 < h && known[u + w])) {
                layer[nl++] = u;
            }
        }
        if (nl == 0) {
            break;
        }
        /* Known centres whose patch reaches into the layer. */
        nc = 0;
        for (k = 0; k < nl; k++) {
            int32_t x = (int32_t)(layer[k] % w), y = (int32_t)(layer[k] / w), cx, cy;
            int32_t ylo = y - r > 0 ? y - r : 0, yhi = y + r < (int32_t)h - 1 ? y + r : (int32_t)h - 1;
            int32_t xlo = x - r > 0 ? x - r : 0, xhi = x + r < (int32_t)w - 1 ? x + r : (int32_t)w - 1;
            for (cy = ylo; cy <= yhi; cy++) {
                for (cx = xlo; cx <= xhi; cx++) {
                    size_t ci = (size_t)cy * w + (size_t)cx;
                    if (known[ci] && !centre[ci]) {
                        centre[ci] = true;
                        centres[nc++] = ci;
                    }
                }
            }
        }
        for (k = 0; k < nc; k++) {
            centre[centres[k]] = false;
        }
        pj = (peel_job){s, nnf, has, known, centres, found, fcost, seed, layer_no};
        (void)pc_algo_par_for(nc, 16, peel_search, &pj);
        {
            size_t ncs = 0;
            for (k = 0; k < nc; k++) {
                if (isfinite(fcost[k])) {
                    nnf[centres[k]] = found[k];
                    has[centres[k]] = true;
                    layer_cost[centres[k]] = fcost[k];
                    cs[ncs++] = fcost[k];
                }
            }
            if ((st = pc_ip_sigma2(cs, ncs, &sigma2, err)) != PC_OK) {
                goto cleanup;
            }
        }
        nv = 0;
        for (k = 0; k < nl; k++) {
            int32_t x = (int32_t)(layer[k] % w), y = (int32_t)(layer[k] / w), cx, cy;
            int32_t ylo = y - r > 0 ? y - r : 0, yhi = y + r < (int32_t)h - 1 ? y + r : (int32_t)h - 1;
            int32_t xlo = x - r > 0 ? x - r : 0, xhi = x + r < (int32_t)w - 1 ? x + r : (int32_t)w - 1;
            float *v = &vals[nv * cha], wsum = 0.0f;
            size_t c;
            memset(v, 0, cha * sizeof *v);
            for (cy = ylo; cy <= yhi; cy++) {
                for (cx = xlo; cx <= xhi; cx++) {
                    size_t ci = (size_t)cy * w + (size_t)cx, si;
                    int32_t sx, sy;
                    float wt;
                    if (!isfinite(layer_cost[ci])) {
                        continue;
                    }
                    sx = nnf[ci].x + x - cx, sy = nnf[ci].y + y - cy;
                    if (sx < 0 || sy < 0 || (size_t)sx >= w || (size_t)sy >= h) {
                        continue;
                    }
                    si = (size_t)sy * w + (size_t)sx;
                    wt = pc_rs_maxf_k(expf(-layer_cost[ci] / (2.0f * sigma2)), 1e-8f);
                    for (c = 0; c < cha; c++) {
                        v[c] += s->img[si * cha + c] * wt;
                    }
                    wsum += wt;
                }
            }
            if (wsum > 0.0f) {
                for (c = 0; c < cha; c++) {
                    v[c] /= wsum;
                }
                vidx[nv++] = layer[k];
            }
        }
        for (k = 0; k < nc; k++) {
            layer_cost[centres[k]] = INFINITY;
        }
        if (nv == 0) {
            break; /* no known patch reaches the layer: left to the EM iterations */
        }
        for (k = 0; k < nv; k++) {
            memcpy(&s->img[vidx[k] * cha], &vals[k * cha], cha * sizeof(float));
            known[vidx[k]] = true;
        }
        {
            size_t m = 0;
            for (k = 0; k < nunk; k++) {
                if (!known[unknown[k]]) {
                    unknown[m++] = unknown[k];
                }
            }
            nunk = m;
        }
        layer_no++;
    }
cleanup:
    free(known), free(centre), free(unknown), free(layer), free(centres), free(vidx);
    free(layer_cost), free(fcost), free(cs), free(vals), free(found);
    return st;
}

/* ---- the driver --------------------------------------------------------------------------------- */

static pc_status copy_out(const float *img, size_t len, float **out, pc_error *err)
{
    float *o = pc_malloc_array(len > 0 ? len : 1, sizeof *o);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (len > 0) {
        memcpy(o, img, len * sizeof *o);
    }
    *out = o;
    return PC_OK;
}

/* Solve level li of the pyramid; on success *prev_nnf holds its match field (w * h, owned) and at
 * level 0 *result its pixels. */
static pc_status nl_solve_level(const nl_level *lv, size_t li, size_t nlev, size_t ch, size_t cha, float k,
                          const pc_algo_nonlocal_params *p, size_t r, const pc_interrupt *ctl, size_t pw,
                          pc_ip_pt **prev_nnf, float **result, bool *none, pc_error *err)
{
    nl_solver s;
    size_t lw = lv->w, lh = lv->h, n = lw * lh, i, it, pass;
    int32_t ri = (int32_t)r, full;
    pc_ip_pt *nnf = NULL, *pnnf = *prev_nnf;
    float *cost = NULL;
    bool *has = NULL, coarsest = li + 1 == nlev;
    nl_copied *copied = NULL;
    size_t ncopied = 0;
    pc_ip_rng rng;
    pc_status st;
    memset(&s, 0, sizeof s);
    *prev_nnf = NULL;
    if ((st = nl_valid_sources(lw, lh, lv->hole, r, &s.valid, &s.valid_list, &s.nvalid, err)) != PC_OK) {
        goto cleanup;
    }
    if (s.nvalid == 0) {
        *none = true;
        goto cleanup;
    }
    s.w = lw, s.h = lh, s.cha = cha, s.ch = ch, s.r = ri, s.hole = lv->hole, s.ctl = ctl;
    s.targets = pc_calloc_array(n, sizeof *s.targets);
    s.hole_list = pc_malloc_array(n, sizeof *s.hole_list);
    s.img = pc_malloc_array(n, cha * sizeof *s.img);
    nnf = pc_calloc_array(n, sizeof *nnf);
    cost = pc_malloc_array(n, sizeof *cost);
    if (s.targets == NULL || s.hole_list == NULL || s.img == NULL || nnf == NULL || cost == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    memcpy(s.img, lv->img, n * cha * sizeof *s.img);
    for (i = 0; i < n; i++) {
        cost[i] = INFINITY;
        if (lv->hole[i]) {
            int32_t x = (int32_t)(i % lw), y = (int32_t)(i / lw), tx, ty;
            int32_t ylo = y - ri > 0 ? y - ri : 0, yhi = y + ri < (int32_t)lh - 1 ? y + ri : (int32_t)lh - 1;
            int32_t xlo = x - ri > 0 ? x - ri : 0, xhi = x + ri < (int32_t)lw - 1 ? x + ri : (int32_t)lw - 1;
            s.hole_list[s.nhole++] = i;
            for (ty = ylo; ty <= yhi; ty++) {
                for (tx = xlo; tx <= xhi; tx++) {
                    s.targets[(size_t)ty * lw + (size_t)tx] = true;
                }
            }
        }
    }
    rng = pc_ip_rng_new(p->seed ^ ((uint64_t)li << 20));
    if (pnnf == NULL) {
        /* Coarsest level: onion peel, then random matches for the targets it didn't reach. */
        has = pc_calloc_array(n, sizeof *has);
        if (has == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            goto cleanup;
        }
        if ((st = nl_onion_peel(&s, nnf, has, p->seed ^ 0x0c104e00ull, err)) != PC_OK) {
            goto cleanup;
        }
        for (i = 0; i < n; i++) {
            if (s.targets[i] && !has[i]) {
                nnf[i] = random_source(&s, &rng);
            }
        }
    } else {
        float change;
        size_t x, y;
        size_t plen = pw * ((lh + 1) / 2);
        for (y = 0; y < lh; y++) {
            for (x = 0; x < lw; x++) {
                size_t pi = (y / 2) * pw + x / 2;
                pc_ip_pt c = pi < plen ? pnnf[pi] : (pc_ip_pt){-1, -1};
                pc_ip_pt cand = {c.x * 2 + (int32_t)(x % 2), c.y * 2 + (int32_t)(y % 2)};
                i = y * lw + x;
                if (!s.targets[i]) {
                    continue;
                }
                if (pc_nl_is_valid(&s, cand.x, cand.y)) {
                    nnf[i] = cand;
                } else if (pc_nl_is_valid(&s, c.x * 2, c.y * 2)) {
                    nnf[i] = (pc_ip_pt){c.x * 2, c.y * 2};
                } else {
                    nnf[i] = random_source(&s, &rng);
                }
            }
        }
        /* Uniform weights: the costs only need to be finite. */
        for (i = 0; i < n; i++) {
            if (s.targets[i]) {
                cost[i] = 0.0f;
            }
        }
        if ((st = nl_vote(&s, nnf, cost, true, &change, err)) != PC_OK) {
            goto cleanup;
        }
    }
    full = (int32_t)(lw > lh ? lw : lh);
    for (it = 0; it < (p->max_iters > 1 ? p->max_iters : 1); it++) {
        int32_t radius;
        float change;
        if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
            goto cleanup;
        }
        nl_refresh_costs(&s, nnf, cost);
        radius = coarsest || it == 0 ? full : (8 * (2 * ri + 1) < full ? 8 * (2 * ri + 1) : full);
        for (pass = 0; pass < (p->pm_passes > 1 ? p->pm_passes : 1); pass++) {
            if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
                goto cleanup;
            }
            nl_patchmatch(&s, nnf, cost, pass + it * p->pm_passes, p->seed + (uint64_t)(li * 1000 + it), radius);
        }
        if ((st = pc_interrupt_check(ctl, err)) != PC_OK || (st = nl_vote(&s, nnf, cost, false, &change, err)) != PC_OK ||
            (st = pc_interrupt_check(ctl, err)) != PC_OK) {
            goto cleanup;
        }
        if (change < p->tolerance) {
            break;
        }
    }
    if (li == 0) {
        nl_refresh_costs(&s, nnf, cost);
        if ((st = nl_copy_best(&s, nnf, cost, &copied, &ncopied, err)) != PC_OK ||
            (st = pc_interrupt_check(ctl, err)) != PC_OK ||
            (st = pc_nl_blend_seams(&s, copied, ncopied, k, err)) != PC_OK ||
            (st = pc_interrupt_check(ctl, err)) != PC_OK ||
            (st = pc_nl_fuse_seams(&s, copied, ncopied, k, 100, err)) != PC_OK ||
            (st = pc_interrupt_check(ctl, err)) != PC_OK) {
            goto cleanup;
        }
        *result = s.img;
        s.img = NULL;
    }
    *prev_nnf = nnf;
    nnf = NULL;
cleanup:
    free(pnnf);
    free(nnf);
    free(cost);
    free(has);
    free(copied);
    free(s.img);
    free(s.valid);
    free(s.valid_list);
    free(s.targets);
    free(s.hole_list);
    return st;
}

pc_status pc_algo_nonlocal_complete(size_t w, size_t h, size_t ch, const float *img, size_t len, const bool *hole,
                                    size_t hole_len, const pc_algo_nonlocal_params *p, const pc_interrupt *ctl,
                                    float **out, pc_error *err)
{
    nl_level pyr[NL_MAX_LEVELS];
    size_t n, nch, i, r, levels, npyr = 0, cha = ch + 2, total_px = 0, done_px = 0, li, pw = 0;
    float *tx = NULL, *ty = NULL, *g = NULL, k, *result = NULL, *o;
    pc_ip_pt *prev = NULL;
    bool any = false, none = false;
    pc_status st;
    if (p == NULL || (len > 0 && img == NULL)) {
        return pc_fail(err, PC_ERR_INVALID, "missing parameters");
    }
    if (ch == 0 || !pc_size_mul(w, h, &n) || hole_len != n || !pc_size_mul(n, ch, &nch) || len != nch ||
        (n > 0 && hole == NULL)) {
        return copy_out(img, len, out, err);
    }
    for (i = 0; i < n && !any; i++) {
        any = hole[i];
    }
    if (!any) {
        return copy_out(img, len, out, err);
    }
    if ((st = pc_algo_heal_dims(w, h, ch, len, NULL, err)) != PC_OK) {
        return st;
    }
    if (p->patch_radius > PC_ALGO_INPAINT_MAX_RADIUS) {
        return pc_fail(err, PC_ERR_LIMIT, "patch radius %zu too large", p->patch_radius);
    }
    if (p->max_iters > PC_ALGO_INPAINT_MAX_ITERS || p->pm_passes > PC_ALGO_INPAINT_MAX_ITERS) {
        return pc_fail(err, PC_ERR_LIMIT, "too many iterations");
    }
    r = p->patch_radius > 1 ? p->patch_radius : 1;
    if ((st = pc_algo_nonlocal_level_count(w, h, hole, r, &levels, err)) != PC_OK) {
        return st;
    }
    levels = levels < NL_MAX_LEVELS ? levels : NL_MAX_LEVELS;
    /* Texture features at full resolution, averaged over about a coarsest-level cell. */
    g = pc_malloc_array(n, sizeof *g);
    tx = pc_malloc_array(n, sizeof *tx);
    ty = pc_malloc_array(n, sizeof *ty);
    memset(pyr, 0, sizeof pyr);
    if (g == NULL || tx == NULL || ty == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    for (i = 0; i < n; i++) {
        float s = -0.0f;
        size_t c;
        for (c = 0; c < ch; c++) {
            s += img[i * ch + c];
        }
        g[i] = s / (float)ch;
    }
    {
        size_t side = (size_t)1 << (levels < 8 ? levels : 8);
        if ((st = feature_table(w, h, g, hole, side, 1, 0, tx, err)) != PC_OK ||
            (st = feature_table(w, h, g, hole, side, 0, 1, ty, err)) != PC_OK) {
            goto cleanup;
        }
    }
    k = sqrtf(pc_rs_maxf_k(p->lambda, 0.0f));
    pyr[0].w = w, pyr[0].h = h;
    pyr[0].img = pc_malloc_array(n, cha * sizeof(float));
    pyr[0].hole = pc_malloc_array(n, sizeof(bool));
    npyr = 1;
    if (pyr[0].img == NULL || pyr[0].hole == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    memcpy(pyr[0].hole, hole, n * sizeof(bool));
    for (i = 0; i < n; i++) {
        memcpy(&pyr[0].img[i * cha], &img[i * ch], ch * sizeof(float));
        pyr[0].img[i * cha + ch] = tx[i] * k;
        pyr[0].img[i * cha + ch + 1] = ty[i] * k;
    }
    free(g), free(tx), free(ty);
    g = tx = ty = NULL;
    while (npyr < levels) {
        nl_level next;
        bool *vv = NULL;
        pc_ip_pt *vl = NULL;
        size_t nvl = 0;
        if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
            goto cleanup;
        }
        if ((st = nl_downsample(&pyr[npyr - 1], cha, ch, &next, err)) != PC_OK) {
            goto cleanup;
        }
        /* A level with nothing to copy from can't seed the finer ones: stop above it. */
        st = nl_valid_sources(next.w, next.h, next.hole, r, &vv, &vl, &nvl, err);
        free(vv), free(vl);
        if (st != PC_OK || nvl == 0) {
            free(next.img), free(next.hole);
            if (st != PC_OK) {
                goto cleanup;
            }
            break;
        }
        pyr[npyr++] = next;
    }
    for (li = 0; li < npyr; li++) {
        total_px += pyr[li].w * pyr[li].h;
    }
    for (li = npyr; li-- > 0;) {
        if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
            goto cleanup;
        }
        st = nl_solve_level(&pyr[li], li, npyr, ch, cha, k, p, r, ctl, pw, &prev, &result, &none, err);
        if (st != PC_OK || none) {
            goto cleanup;
        }
        pw = pyr[li].w;
        done_px += pyr[li].w * pyr[li].h;
        pc_interrupt_progress(ctl, (float)done_px / (float)(total_px > 1 ? total_px : 1));
    }
    if (result == NULL) {
        none = true;
        goto cleanup;
    }
    o = pc_malloc_array(len, sizeof *o);
    if (o == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    memcpy(o, img, len * sizeof *o);
    for (i = 0; i < n; i++) {
        if (hole[i]) {
            memcpy(&o[i * ch], &result[i * cha], ch * sizeof *o);
        }
    }
    *out = o;
cleanup:
    for (li = 0; li < npyr; li++) {
        free(pyr[li].img);
        free(pyr[li].hole);
    }
    free(g), free(tx), free(ty);
    free(prev);
    free(result);
    if (st == PC_OK && none) {
        *out = NULL;
    }
    return st;
}
