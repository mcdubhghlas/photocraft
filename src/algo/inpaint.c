/* Patch-based hole filling (port of crates/algo/src/inpaint.rs); see inpaint.h. */
#include "algo/inpaint.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/poisson.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Below this many grid cells a level runs single-threaded (the reference's PAR_MIN; results are
 * the same either way). */
#define PAR_MIN ((size_t)128 * 128)
/* PatchMatch row band height: each band propagates within itself with its own RNG stream. */
#define BAND ((size_t)16)

/* ---- SplitMix64 ------------------------------------------------------------------------------- */

typedef struct rng {
    uint64_t s;
} rng;

static rng rng_new(uint64_t seed)
{
    return (rng){seed ^ 0x9E3779B97F4A7C15ull};
}

static uint64_t rng_next(rng *r)
{
    uint64_t z;
    r->s += 0x9E3779B97F4A7C15ull;
    z = r->s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Uniform integer in lo..=hi. */
static int32_t rng_range(rng *r, int32_t lo, int32_t hi)
{
    uint64_t span;
    if (hi <= lo) {
        return lo;
    }
    span = (uint64_t)((int64_t)hi - (int64_t)lo + 1);
    return (int32_t)((int64_t)lo + (int64_t)(rng_next(r) % span));
}

/* ---- grids ------------------------------------------------------------------------------------- */

typedef struct level {
    size_t w, h;
    float *img;
    bool *hole;
} level;

typedef struct ipt {
    int32_t x, y;
} ipt;

static pc_status downsample(const level *l, size_t ch, level *out, pc_error *err)
{
    size_t w = (l->w + 1) / 2, h = (l->h + 1) / 2, x, y, c;
    float *img = pc_calloc_array(w * h * ch, sizeof *img);
    bool *hole = pc_calloc_array(w * h, sizeof *hole);
    static const size_t DX[4] = {0, 1, 0, 1}, DY[4] = {0, 0, 1, 1};
    if (img == NULL || hole == NULL) {
        free(img);
        free(hole);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float n = 0.0f;
            size_t o = (y * w + x) * ch;
            int k;
            for (k = 0; k < 4; k++) {
                size_t sx = x * 2 + DX[k], sy = y * 2 + DY[k], si;
                if (sx >= l->w || sy >= l->h) {
                    continue;
                }
                si = sy * l->w + sx;
                if (l->hole[si]) {
                    hole[y * w + x] = true;
                } else {
                    for (c = 0; c < ch; c++) {
                        img[o + c] += l->img[si * ch + c];
                    }
                    n += 1.0f;
                }
            }
            if (n > 0.0f) {
                for (c = 0; c < ch; c++) {
                    img[o + c] /= n;
                }
            }
        }
    }
    *out = (level){w, h, img, hole};
    return PC_OK;
}

/* Summed-area table of a boolean grid ((w+1) x (h+1)); u32 arithmetic wraps like the reference's
 * release build, and window differences come out exact. */
static uint32_t *integral(size_t w, size_t h, const bool *m)
{
    size_t x, y;
    uint32_t *s = pc_calloc_array((w + 1) * (h + 1), sizeof *s);
    if (s == NULL) {
        return NULL;
    }
    for (y = 0; y < h; y++) {
        uint32_t row = 0;
        for (x = 0; x < w; x++) {
            row += m[y * w + x] ? 1u : 0u;
            s[(y + 1) * (w + 1) + x + 1] = s[y * (w + 1) + x + 1] + row;
        }
    }
    return s;
}

/* Count of set cells in the clipped window [x0, x1) x [y0, y1). */
static uint32_t window_count(const uint32_t *s, size_t gw, size_t gh, int64_t x0, int64_t y0, int64_t x1,
                             int64_t y1)
{
    size_t ux0 = x0 > 0 ? (size_t)x0 : 0, uy0 = y0 > 0 ? (size_t)y0 : 0;
    size_t ux1 = x1 > 0 ? (size_t)x1 : 0, uy1 = y1 > 0 ? (size_t)y1 : 0;
    size_t w1 = gw + 1;
    if (ux1 > gw) {
        ux1 = gw;
    }
    if (uy1 > gh) {
        uy1 = gh;
    }
    if (ux1 <= ux0 || uy1 <= uy0) {
        return 0;
    }
    return s[uy1 * w1 + ux1] + s[uy0 * w1 + ux0] - s[uy0 * w1 + ux1] - s[uy1 * w1 + ux0];
}

static bool hole_bbox(size_t w, size_t h, const bool *hole, size_t b[4])
{
    size_t x, y;
    bool any = false;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            if (hole[y * w + x]) {
                if (!any) {
                    b[0] = x, b[1] = y, b[2] = x + 1, b[3] = y + 1;
                    any = true;
                } else {
                    b[0] = x < b[0] ? x : b[0];
                    b[1] = y < b[1] ? y : b[1];
                    b[2] = x + 1 > b[2] ? x + 1 : b[2];
                    b[3] = y + 1 > b[3] ? y + 1 : b[3];
                }
            }
        }
    }
    return any;
}

/* f32::total_cmp as an unsigned key. */
static uint32_t total_key(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

static int cmp_total(const void *a, const void *b)
{
    uint32_t ka = total_key(*(const float *)a), kb = total_key(*(const float *)b);
    return ka < kb ? -1 : (ka > kb ? 1 : 0);
}

/* ---- per-level PatchMatch / EM ------------------------------------------------------------------ */

typedef struct solver {
    size_t w, h, ch;
    int32_t r;
    float *img;
    const bool *hole;
    bool *valid; /* the whole patch is inside the grid and contains no hole pixel */
    ipt *valid_list;
    size_t valid_n;
    bool *targets; /* the patch overlaps the hole */
    const pc_interrupt *ctl;
} solver;

static bool is_valid(const solver *s, int32_t x, int32_t y)
{
    return x >= 0 && y >= 0 && (size_t)x < s->w && (size_t)y < s->h && s->valid[(size_t)y * s->w + (size_t)x];
}

/* SSD between the (clipped) target patch at t and the source patch at s, per sample. */
static float dist(const solver *sv, ipt t, ipt s, float cutoff)
{
    int32_t w = (int32_t)sv->w, h = (int32_t)sv->h, r = sv->r, dx, dy;
    size_t ch = sv->ch, n = 0, c;
    float full = (float)((2 * r + 1) * (2 * r + 1)) * (float)ch;
    float sum = 0.0f;
    for (dy = -r; dy <= r; dy++) {
        int32_t ty = t.y + dy, sy = s.y + dy;
        if (ty < 0 || ty >= h) {
            continue;
        }
        for (dx = -r; dx <= r; dx++) {
            int32_t tx = t.x + dx;
            size_t ti, si;
            if (tx < 0 || tx >= w) {
                continue;
            }
            ti = ((size_t)ty * sv->w + (size_t)tx) * ch;
            si = ((size_t)sy * sv->w + (size_t)(s.x + dx)) * ch;
            for (c = 0; c < ch; c++) {
                float d = sv->img[ti + c] - sv->img[si + c];
                sum += d * d;
            }
            n += ch;
        }
        /* Early out: the mean can only be >= sum / (full patch size). */
        if (sum > cutoff * full) {
            return INFINITY;
        }
    }
    return n == 0 ? INFINITY : sum / (float)n;
}

typedef struct pm_job {
    const solver *sv;
    ipt *nnf;
    float *cost;
    size_t pass;
    uint64_t seed;
    int32_t search_radius;
} pm_job;

static void pm_try(const solver *sv, ipt t, ipt cand, ipt *best, float *bc)
{
    if (is_valid(sv, cand.x, cand.y) && (cand.x != best->x || cand.y != best->y)) {
        float d = dist(sv, t, cand, *bc);
        if (d < *bc) {
            *best = cand;
            *bc = d;
        }
    }
}

static void pm_band(const pm_job *j, size_t bi)
{
    const solver *sv = j->sv;
    size_t w = sv->w, y0 = bi * BAND, rows = sv->h - y0 < BAND ? sv->h - y0 : BAND, ry, rx;
    ipt *nn = j->nnf + y0 * w;
    float *co = j->cost + y0 * w;
    bool reverse = j->pass % 2 == 1;
    int32_t step = reverse ? -1 : 1;
    rng g;
    if (pc_interrupt_cancelled(sv->ctl)) {
        return;
    }
    g = rng_new(j->seed ^ ((uint64_t)bi * 0x2545F4914F6CDD1Dull) ^ ((uint64_t)j->pass << 40));
    for (ry = 0; ry < rows; ry++) {
        size_t ly = reverse ? rows - 1 - ry : ry, y = y0 + ly;
        for (rx = 0; rx < w; rx++) {
            size_t x = reverse ? w - 1 - rx : rx, li = ly * w + x;
            ipt t = {(int32_t)x, (int32_t)y}, best;
            float bc;
            int64_t nx, nly;
            int32_t rad;
            if (!sv->targets[y * w + x]) {
                continue;
            }
            best = nn[li];
            bc = co[li];
            /* Propagation from the already-visited neighbours (within this band). */
            nx = (int64_t)x - step;
            if (nx >= 0 && (size_t)nx < w) {
                ipt c = nn[ly * w + (size_t)nx];
                pm_try(sv, t, (ipt){c.x + step, c.y}, &best, &bc);
            }
            nly = (int64_t)ly - step;
            if (nly >= 0 && (size_t)nly < rows) {
                ipt c = nn[(size_t)nly * w + x];
                pm_try(sv, t, (ipt){c.x, c.y + step}, &best, &bc);
            }
            /* Random search in exponentially shrinking windows. */
            for (rad = j->search_radius; rad >= 1; rad /= 2) {
                int32_t cx = best.x + rng_range(&g, -rad, rad);
                int32_t cy = best.y + rng_range(&g, -rad, rad);
                pm_try(sv, t, (ipt){cx, cy}, &best, &bc);
            }
            nn[li] = best;
            co[li] = bc;
        }
    }
}

static pc_status pm_run(void *ctx, size_t chunk, size_t begin, size_t end)
{
    size_t bi;
    (void)chunk;
    for (bi = begin; bi < end; bi++) {
        pm_band(ctx, bi);
    }
    return PC_OK;
}

static void patchmatch(const solver *sv, ipt *nnf, float *cost, size_t pass, uint64_t seed,
                       int32_t search_radius)
{
    pm_job j = {sv, nnf, cost, pass, seed, search_radius};
    size_t bands = (sv->h + BAND - 1) / BAND, bi;
    if (sv->w * sv->h >= PAR_MIN) {
        (void)pc_algo_par_for(bands, 1, pm_run, &j);
        return;
    }
    for (bi = 0; bi < bands; bi++) {
        pm_band(&j, bi);
    }
}

typedef struct vote_job {
    const solver *sv;
    const ipt *nnf;
    const float *cost;
    float sigma2;
    float *out; /* the new image (a copy of img; voted hole pixels overwritten) */
} vote_job;

static void vote_row(const vote_job *j, size_t y)
{
    const solver *sv = j->sv;
    size_t w = sv->w, h = sv->h, ch = sv->ch, x, c;
    int32_t r = sv->r, dx, dy;
    if (pc_interrupt_cancelled(sv->ctl)) {
        return;
    }
    for (x = 0; x < w; x++) {
        float acc[PC_ALGO_HEAL_MAX_CH], wsum = 0.0f;
        if (!sv->hole[y * w + x]) {
            continue;
        }
        for (c = 0; c < ch; c++) {
            acc[c] = 0.0f;
        }
        for (dy = -r; dy <= r; dy++) {
            int64_t ty = (int64_t)y + dy;
            if (ty < 0 || (size_t)ty >= h) {
                continue;
            }
            for (dx = -r; dx <= r; dx++) {
                int64_t tx = (int64_t)x + dx, sx, sy;
                size_t ti, si;
                float wt;
                ipt s;
                if (tx < 0 || (size_t)tx >= w) {
                    continue;
                }
                ti = (size_t)ty * w + (size_t)tx;
                if (!sv->targets[ti] || !isfinite(j->cost[ti])) {
                    continue;
                }
                s = j->nnf[ti];
                sx = (int64_t)s.x - dx;
                sy = (int64_t)s.y - dy;
                if (sx < 0 || sy < 0 || (size_t)sx >= w || (size_t)sy >= h) {
                    continue; /* unreachable: matches are valid centres */
                }
                wt = pc_rs_maxf_k(expf(-j->cost[ti] / (2.0f * j->sigma2)), 1e-8f);
                si = ((size_t)sy * w + (size_t)sx) * ch;
                for (c = 0; c < ch; c++) {
                    acc[c] += sv->img[si + c] * wt;
                }
                wsum += wt;
            }
        }
        if (wsum > 0.0f) {
            for (c = 0; c < ch; c++) {
                j->out[(y * w + x) * ch + c] = acc[c] / wsum;
            }
        }
    }
}

static pc_status vote_run(void *ctx, size_t chunk, size_t begin, size_t end)
{
    size_t y;
    (void)chunk;
    for (y = begin; y < end; y++) {
        vote_row(ctx, y);
    }
    return PC_OK;
}

/* Re-estimate hole pixels by a similarity-weighted vote of all overlapping patch matches. */
static pc_status vote(solver *sv, const ipt *nnf, const float *cost, pc_error *err)
{
    size_t n = sv->w * sv->h, ncs = 0, i;
    float *cs = pc_malloc_array(n, sizeof *cs), *nimg, sigma2;
    vote_job j;
    if (cs == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < n; i++) {
        if (sv->targets[i] && isfinite(cost[i])) {
            cs[ncs++] = cost[i];
        }
    }
    /* sigma^2 from the 75th percentile of match costs (a robust scale, as Wexler et al. use). */
    if (ncs == 0) {
        sigma2 = 1.0f;
    } else {
        size_t k = ncs * 3 / 4 < ncs - 1 ? ncs * 3 / 4 : ncs - 1;
        qsort(cs, ncs, sizeof *cs, cmp_total);
        sigma2 = pc_rs_maxf_k(cs[k], 1e-6f);
    }
    free(cs);
    nimg = pc_malloc_array(n * sv->ch, sizeof *nimg);
    if (nimg == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    memcpy(nimg, sv->img, n * sv->ch * sizeof *nimg);
    j = (vote_job){sv, nnf, cost, sigma2, nimg};
    if (n >= PAR_MIN) {
        (void)pc_algo_par_for(sv->h, 1, vote_run, &j);
    } else {
        for (i = 0; i < sv->h; i++) {
            vote_row(&j, i);
        }
    }
    memcpy(sv->img, nimg, n * sv->ch * sizeof *nimg);
    free(nimg);
    return PC_OK;
}

pc_algo_complete_params pc_algo_complete_params_default(void)
{
    return (pc_algo_complete_params){3, 8, 2, 2, 96, 1};
}

/* Everything one completion owns, freed in one place. */
/* Each level halves the previous one and stops above 3 patches across, so a side of at most
 * 2^24 gives at most 25 levels. */
#define MAX_LEVELS 32

typedef struct complete_state {
    level levels[MAX_LEVELS];
    size_t nlev;
    size_t exts[MAX_LEVELS];
    float *prev; /* the previous (coarser) level's result */
    size_t pw, ph;
    ipt *nnf;
    bool *valid, *targets;
    ipt *valid_list;
    uint32_t *hs;
    float *cost;
} complete_state;

static void state_free(complete_state *s)
{
    size_t i;
    for (i = 0; i < s->nlev; i++) {
        free(s->levels[i].img);
        free(s->levels[i].hole);
    }
    free(s->prev);
    free(s->nnf);
    free(s->valid);
    free(s->targets);
    free(s->valid_list);
    free(s->hs);
    free(s->cost);
}

/* One pyramid level: initialise the hole, build the patch sets, run EM. On PC_OK the level's
 * image is in s->prev (ownership moved). */
static pc_status complete_level(complete_state *s, size_t li, size_t ch, const pc_algo_complete_params *p,
                                size_t r, const pc_interrupt *ctl, bool *none, bool *ran, pc_error *err)
{
    level *L = &s->levels[li];
    size_t lw = L->w, lh = L->h, n = lw * lh, psz = 2 * r + 1, x, y, i, em, pm_iters, it, pass,
           nlev = s->nlev;
    int32_t ri = (int32_t)r, full_search;
    float *limg = L->img;
    bool refine_only;
    pc_status st;
    solver sv;
    rng g;
    ipt *nnf;
    if (s->prev == NULL) {
        float *m = NULL;
        st = pc_algo_poisson_membrane_fill(lw, lh, ch, limg, n * ch, L->hole, n, NULL, &m, err);
        if (st != PC_OK) {
            return st;
        }
        free(limg);
        L->img = limg = m;
    } else {
        for (y = 0; y < lh; y++) {
            for (x = 0; x < lw; x++) {
                if (L->hole[y * lw + x]) {
                    size_t sx = x / 2 < s->pw - 1 ? x / 2 : s->pw - 1,
                           sy = y / 2 < s->ph - 1 ? y / 2 : s->ph - 1;
                    memcpy(&limg[(y * lw + x) * ch], &s->prev[(sy * s->pw + sx) * ch], ch * sizeof(float));
                }
            }
        }
    }
    free(s->hs);
    free(s->valid);
    free(s->targets);
    free(s->valid_list);
    s->hs = integral(lw, lh, L->hole);
    s->valid = pc_calloc_array(n, sizeof(bool));
    s->targets = pc_calloc_array(n, sizeof(bool));
    s->valid_list = pc_malloc_array(n, sizeof(ipt));
    if (s->hs == NULL || s->valid == NULL || s->targets == NULL || s->valid_list == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    sv = (solver){lw, lh, ch, ri, limg, L->hole, s->valid, s->valid_list, 0, s->targets, ctl};
    for (y = 0; y < lh; y++) {
        if (y % 32 == 0 && (st = pc_interrupt_check(ctl, err)) != PC_OK) {
            return st;
        }
        for (x = 0; x < lw; x++) {
            int64_t xi = (int64_t)x, yi = (int64_t)y;
            uint32_t cnt = window_count(s->hs, lw, lh, xi - ri, yi - ri, xi + ri + 1, yi + ri + 1);
            bool inside = xi >= ri && yi >= ri && xi + ri < (int64_t)lw && yi + ri < (int64_t)lh;
            i = y * lw + x;
            if (inside && cnt == 0) {
                sv.valid[i] = true;
                sv.valid_list[sv.valid_n++] = (ipt){(int32_t)x, (int32_t)y};
            }
            sv.targets[i] = cnt > 0;
        }
    }
    if (sv.valid_n == 0) {
        if (li == 0) {
            *none = true;
            return PC_OK;
        }
        free(s->prev);
        s->prev = limg;
        L->img = NULL;
        s->pw = lw, s->ph = lh;
        free(s->nnf);
        s->nnf = NULL; /* the next level starts from random matches */
        return PC_OK;
    }
    /* NNF init: upsampled from the coarser level where possible, random otherwise. */
    g = rng_new(p->seed ^ ((uint64_t)li << 20));
    nnf = pc_calloc_array(n, sizeof *nnf);
    if (nnf == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (y = 0; y < lh; y++) {
        if (y % 32 == 0 && (st = pc_interrupt_check(ctl, err)) != PC_OK) {
            free(nnf);
            return st;
        }
        for (x = 0; x < lw; x++) {
            bool have = false;
            ipt cand = {0, 0};
            if (!sv.targets[y * lw + x]) {
                continue;
            }
            if (s->nnf != NULL && s->prev != NULL && s->pw > 0) {
                size_t pi = (y / 2) * s->pw + x / 2;
                if (pi < s->pw * s->ph) {
                    ipt o = s->nnf[pi];
                    ipt c = {pc_i32_wrapping_add(pc_i32_wrapping_mul(o.x, 2), (int32_t)(x % 2)),
                             pc_i32_wrapping_add(pc_i32_wrapping_mul(o.y, 2), (int32_t)(y % 2))};
                    if (is_valid(&sv, c.x, c.y)) {
                        cand = c;
                        have = true;
                    }
                }
            }
            nnf[y * lw + x] = have ? cand : sv.valid_list[rng_next(&g) % (uint64_t)sv.valid_n];
        }
    }
    free(s->nnf);
    s->nnf = nnf;
    refine_only = li + 1 < nlev && s->exts[li] > p->refine_extent;
    if (li + 1 == nlev) {
        em = p->em_iters;
    } else if (refine_only) {
        em = 1;
    } else {
        size_t sh = nlev - 1 - li;
        em = sh < 64 ? p->em_iters >> sh : 0;
        em = em > p->min_em_iters ? em : p->min_em_iters;
    }
    pm_iters = refine_only ? 1 : p->pm_iters;
    full_search = (int32_t)(lw > lh ? lw : lh);
    free(s->cost);
    s->cost = pc_malloc_array(n, sizeof(float));
    if (s->cost == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < n; i++) {
        s->cost[i] = INFINITY;
    }
    for (it = 0; it < em; it++) {
        int32_t radius;
        /* Costs change as hole pixels are re-estimated: refresh before each PatchMatch. */
        for (i = 0; i < n; i++) {
            if (i % (lw * 32 > 0 ? lw * 32 : 1) == 0 && (st = pc_interrupt_check(ctl, err)) != PC_OK) {
                return st;
            }
            if (sv.targets[i]) {
                s->cost[i] = dist(&sv, (ipt){(int32_t)(i % lw), (int32_t)(i / lw)}, nnf[i], INFINITY);
            }
        }
        /* Wide random search on coarse levels / the first iteration, local refinement after. */
        if (refine_only) {
            radius = 2;
        } else if (li + 1 == nlev || it == 0) {
            radius = full_search;
        } else {
            radius = 4 * (int32_t)psz < full_search ? 4 * (int32_t)psz : full_search;
        }
        for (pass = 0; pass < pm_iters; pass++) {
            if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
                return st;
            }
            patchmatch(&sv, nnf, s->cost, pass + it * pm_iters, p->seed + (uint64_t)(li * 1000 + it), radius);
        }
        if ((st = pc_interrupt_check(ctl, err)) != PC_OK || (st = vote(&sv, nnf, s->cost, err)) != PC_OK ||
            (st = pc_interrupt_check(ctl, err)) != PC_OK) {
            return st;
        }
    }
    free(s->prev);
    s->prev = limg;
    L->img = NULL;
    s->pw = lw, s->ph = lh;
    *ran = true;
    return PC_OK;
}

pc_status pc_algo_inpaint_complete(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                   const bool *hole, size_t hole_len, const pc_algo_complete_params *p,
                                   const pc_interrupt *ctl, float **out, pc_error *err)
{
    complete_state s;
    size_t n, b[4], r, psz, ext, total_px = 0, done_px = 0, li;
    pc_status st = pc_algo_heal_dims(w, h, ch, len, &n, err);
    bool none = false;
    if (st != PC_OK) {
        return st;
    }
    if (p == NULL || hole_len != n || (n > 0 && (img == NULL || hole == NULL))) {
        return pc_fail(err, PC_ERR_INVALID, "hole mask holds %zu entries, expected %zu", hole_len, n);
    }
    if (p->patch_radius > PC_ALGO_INPAINT_MAX_RADIUS || p->em_iters > PC_ALGO_INPAINT_MAX_ITERS ||
        p->min_em_iters > PC_ALGO_INPAINT_MAX_ITERS || p->pm_iters > PC_ALGO_INPAINT_MAX_ITERS) {
        return pc_fail(err, PC_ERR_LIMIT, "completion parameters too large");
    }
    memset(&s, 0, sizeof s);
    if (!hole_bbox(w, h, hole, b)) {
        float *o = pc_malloc_array(n * ch, sizeof *o);
        if (o == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        if (n > 0) {
            memcpy(o, img, n * ch * sizeof *o);
        }
        *out = o;
        return PC_OK;
    }
    r = p->patch_radius > 1 ? p->patch_radius : 1;
    psz = 2 * r + 1;
    /* Pyramid: shrink until the hole is a few patches across. */
    s.levels[0].img = pc_malloc_array(n * ch, sizeof(float));
    s.levels[0].hole = pc_malloc_array(n, sizeof(bool));
    s.nlev = 1;
    if (s.levels[0].img == NULL || s.levels[0].hole == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    s.levels[0].w = w, s.levels[0].h = h;
    memcpy(s.levels[0].img, img, n * ch * sizeof(float));
    memcpy(s.levels[0].hole, hole, n * sizeof(bool));
    ext = b[2] - b[0] > b[3] - b[1] ? b[2] - b[0] : b[3] - b[1];
    s.exts[0] = ext;
    for (;;) {
        const level *l = &s.levels[s.nlev - 1];
        if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
            goto cleanup;
        }
        if (ext <= 2 * psz || l->w / 2 < 3 * psz || l->h / 2 < 3 * psz || s.nlev == MAX_LEVELS) {
            break;
        }
        if ((st = downsample(l, ch, &s.levels[s.nlev], err)) != PC_OK) {
            goto cleanup;
        }
        ext = (ext + 1) / 2;
        s.exts[s.nlev] = ext;
        s.nlev++;
    }
    for (li = 0; li < s.nlev; li++) {
        total_px += s.levels[li].w * s.levels[li].h;
    }
    for (li = s.nlev; li-- > 0;) {
        size_t lw = s.levels[li].w, lh = s.levels[li].h;
        bool ran = false;
        if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
            goto cleanup;
        }
        if ((st = complete_level(&s, li, ch, p, r, ctl, &none, &ran, err)) != PC_OK) {
            goto cleanup;
        }
        if (none) {
            *out = NULL;
            goto cleanup;
        }
        if (ran) {
            /* Finer levels cost ~4x the previous one: weight progress by pixel count. */
            done_px += lw * lh;
            pc_interrupt_progress(ctl, (float)done_px / (float)(total_px > 0 ? total_px : 1));
        }
    }
    *out = s.prev;
    s.prev = NULL;
cleanup:
    state_free(&s);
    return st;
}

/* ---- proximity match ---------------------------------------------------------------------------- */

pc_status pc_algo_inpaint_best_offset(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                      const bool *hole, size_t hole_len, size_t ring, int32_t max_radius,
                                      bool *found, int32_t *dx, int32_t *dy, pc_error *err)
{
    size_t n, b[4], npts = 0, stride, k, c;
    int64_t ringi, x0, y0, x1, y1, x, y, step, ddx, ddy, bx = 0, by = 0;
    uint32_t *hs;
    ipt *pts;
    float best_e = 0.0f;
    bool have = false;
    pc_status st = pc_algo_heal_dims(w, h, ch, len, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (hole_len != n || (n > 0 && (img == NULL || hole == NULL))) {
        return pc_fail(err, PC_ERR_INVALID, "hole mask holds %zu entries, expected %zu", hole_len, n);
    }
    if (ring > PC_ALGO_HEAL_MAX_SIDE) {
        return pc_fail(err, PC_ERR_LIMIT, "ring %zu too wide", ring);
    }
    if (!hole_bbox(w, h, hole, b)) {
        *found = false;
        return PC_OK;
    }
    ringi = ring > 1 ? (int64_t)ring : 1;
    hs = integral(w, h, hole);
    pts = pc_malloc_array(n, sizeof *pts);
    if (hs == NULL || pts == NULL) {
        free(hs);
        free(pts);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    /* Sample points: the hole's ring (dilated minus hole), subsampled for speed. */
    x0 = (int64_t)b[0] - ringi, y0 = (int64_t)b[1] - ringi;
    x1 = (int64_t)b[2] + ringi, y1 = (int64_t)b[3] + ringi;
    for (y = y0 > 0 ? y0 : 0; y < (y1 < (int64_t)h ? y1 : (int64_t)h); y++) {
        for (x = x0 > 0 ? x0 : 0; x < (x1 < (int64_t)w ? x1 : (int64_t)w); x++) {
            if (hole[(size_t)y * w + (size_t)x]) {
                continue;
            }
            if (window_count(hs, w, h, x - ringi, y - ringi, x + ringi + 1, y + ringi + 1) > 0) {
                pts[npts++] = (ipt){(int32_t)x, (int32_t)y};
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
    step = max_radius / 24 > 1 ? max_radius / 24 : 1;
    /* Coarse grid over the search window, then a full-resolution refinement around the winner.
     * The loops run in i64: the reference's i32 counters overflow (and never end) for radii near
     * i32::MAX. */
    for (int pass = 0; pass < 2; pass++) {
        int64_t lo_y, hi_y, lo_x, hi_x, inc;
        if (pass == 0) {
            lo_y = lo_x = -(int64_t)max_radius, hi_y = hi_x = max_radius, inc = step;
        } else {
            if (step <= 1 || !have) {
                break;
            }
            lo_y = by - step, hi_y = by + step, lo_x = bx - step, hi_x = bx + step, inc = 1;
        }
        for (ddy = lo_y; ddy <= hi_y; ddy += inc) {
            for (ddx = lo_x; ddx <= hi_x; ddx += inc) {
                int64_t hx0 = x0 + ddx, hy0 = y0 + ddy, hx1 = x1 + ddx, hy1 = y1 + ddy;
                bool inside = hx0 >= 0 && hy0 >= 0 && hx1 <= (int64_t)w && hy1 <= (int64_t)h;
                float e = 0.0f, d2, score;
                int32_t sdx = (int32_t)ddx, sdy = (int32_t)ddy;
                if ((ddx == 0 && ddy == 0) || !inside || window_count(hs, w, h, hx0, hy0, hx1, hy1) != 0) {
                    continue;
                }
                for (k = 0; k < npts; k++) {
                    size_t a = ((size_t)pts[k].y * w + (size_t)pts[k].x) * ch;
                    size_t bb = ((size_t)(pts[k].y + sdy) * w + (size_t)(pts[k].x + sdx)) * ch;
                    for (c = 0; c < ch; c++) {
                        float d = img[a + c] - img[bb + c];
                        e += d * d;
                    }
                }
                /* Mild preference for nearer sources on ties. */
                d2 = (float)pc_i32_wrapping_add(pc_i32_wrapping_mul(sdx, sdx), pc_i32_wrapping_mul(sdy, sdy));
                score = e / (float)(npts > 1 ? npts : 1) *
                        (1.0f + 1e-3f * sqrtf(d2) / (float)(max_radius > 1 ? max_radius : 1));
                if (!have || score < best_e) {
                    have = true;
                    best_e = score;
                    bx = ddx, by = ddy;
                }
            }
        }
    }
    free(hs);
    free(pts);
    *found = have;
    if (have) {
        *dx = (int32_t)bx;
        *dy = (int32_t)by;
    }
    return PC_OK;
}

/* ---- texture synthesis -------------------------------------------------------------------------- */

pc_status pc_algo_inpaint_synthesize(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                     const bool *hole, size_t hole_len, size_t block, uint64_t seed,
                                     float **out, pc_error *err)
{
    size_t n, bb[4], ncand = 0, q;
    int64_t b, ov, x, y, bx, by;
    uint32_t *hs = NULL;
    ipt *cands = NULL;
    bool *filled = NULL;
    float *o;
    rng g;
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
    o = pc_malloc_array(n * ch, sizeof *o);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (n > 0) {
        memcpy(o, img, n * ch * sizeof *o);
    }
    if (!hole_bbox(w, h, hole, bb)) {
        *out = o;
        return PC_OK;
    }
    b = block > 4 ? (int64_t)block : 4;
    ov = b / 3 > 1 ? b / 3 : 1;
    hs = integral(w, h, hole);
    cands = pc_malloc_array(n, sizeof *cands);
    if (hs == NULL || cands == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    /* Candidate block origins: the whole block (with overlap) inside the grid and outside the hole. */
    for (y = ov; y < (int64_t)h - b - ov; y++) {
        for (x = ov; x < (int64_t)w - b - ov; x++) {
            if (window_count(hs, w, h, x - ov, y - ov, x + b + ov, y + b + ov) == 0) {
                cands[ncand++] = (ipt){(int32_t)x, (int32_t)y};
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
    if (filled == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    for (q = 0; q < n; q++) {
        filled[q] = !hole[q];
    }
    g = rng_new(seed);
    for (by = (int64_t)bb[1]; by < (int64_t)bb[3]; by += b) {
        for (bx = (int64_t)bb[0]; bx < (int64_t)bb[2]; bx += b) {
            ipt best;
            float best_e = INFINITY;
            int k;
            if (window_count(hs, w, h, bx, by, bx + b, by + b) == 0) {
                continue;
            }
            /* Pick the candidate whose block best agrees with already-known pixels around/inside. */
            best = cands[0];
            for (k = 0; k < 48; k++) {
                ipt c = cands[rng_next(&g) % (uint64_t)ncand];
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
            for (y = by - ov; y < by + b + ov; y++) {
                for (x = bx - ov; x < bx + b + ov; x++) {
                    size_t i, a, s;
                    float kf;
                    if (x < 0 || y < 0 || x >= (int64_t)w || y >= (int64_t)h) {
                        continue;
                    }
                    i = (size_t)y * w + (size_t)x;
                    if (!hole[i]) {
                        continue;
                    }
                    a = i * ch;
                    s = ((size_t)(y - by + best.y) * w + (size_t)(x - bx + best.x)) * ch;
                    /* Feather into already-synthesised overlap. */
                    kf = filled[i] ? 0.5f : 1.0f;
                    for (q = 0; q < ch; q++) {
                        o[a + q] += (img[s + q] - o[a + q]) * kf;
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
    if (st != PC_OK) {
        free(o);
        return st;
    }
    *out = o;
    return PC_OK;
}
