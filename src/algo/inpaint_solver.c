/* Per-level PatchMatch / EM state of the completion (inpaint::Solver); see inpaint_int.h. Every
 * pass works per row or per row band and gives the same result whatever the thread count. The
 * reference also sweeps PatchMatch bands in wavefronts when there are fewer bands than threads;
 * that visits every cell with the same neighbours and random numbers as the raster order, so only
 * the raster order is implemented here. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/inpaint_int.h"
#include "algo/poisson.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define PM_BAND ((size_t)16)

static size_t c8_of(size_t ch)
{
    return ch < PC_IP_MOM_CH ? ch : PC_IP_MOM_CH;
}

void pc_ip_solver_fini(pc_ip_solver *s)
{
    free(s->valid);
    free(s->targets);
    free(s->full);
    free(s->tmom);
    free(s->smom);
    s->valid = s->targets = s->full = NULL;
    s->tmom = s->smom = NULL;
}

bool pc_ip_solver_is_valid(const pc_ip_solver *s, int32_t x, int32_t y)
{
    return x >= 0 && y >= 0 && (size_t)x < s->w && (size_t)y < s->h && s->valid[(size_t)y * s->w + (size_t)x];
}

/* sum v, sum v^2 per channel (the first c8) of the side x side patch with top-left (x0, y0). */
static void patch_moments(const float *img, size_t w, size_t ch, size_t c8, size_t side, size_t x0, size_t y0,
                          float *out)
{
    size_t y, x, c;
    memset(out, 0, c8 * 2 * sizeof(float));
    for (y = y0; y < y0 + side; y++) {
        const float *row = &img[(y * w + x0) * ch];
        for (x = 0; x < side; x++) {
            for (c = 0; c < c8; c++) {
                float a = row[x * ch + c];
                out[c * 2] += a;
                out[c * 2 + 1] += a * a;
            }
        }
    }
}

typedef struct prep_job {
    pc_ip_solver *s;
    size_t side, c8;
} prep_job;

static pc_status prep_full(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const prep_job *j = ctx;
    const pc_ip_solver *s = j->s;
    size_t r = (size_t)s->r, w = s->w, h = s->h, tw = pc_ip_bw(s->tbox), ty, tx, yy, xx;
    (void)chunk;
    for (ty = begin; ty < end; ty++) {
        size_t y = s->tbox.y0 + ty;
        if (y < r || y + r >= h) {
            continue;
        }
        for (tx = 0; tx < tw; tx++) {
            size_t x = s->tbox.x0 + tx;
            bool f = true;
            if (x < r || x + r >= w) {
                continue;
            }
            for (yy = y - r; yy <= y + r && f; yy++) {
                for (xx = x - r; xx <= x + r; xx++) {
                    if (s->px[yy * w + xx] == PC_ALGO_PX_VOID) {
                        f = false;
                        break;
                    }
                }
            }
            s->full[ty * tw + tx] = f;
        }
    }
    return PC_OK;
}

static pc_status prep_smom(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const prep_job *j = ctx;
    const pc_ip_solver *s = j->s;
    size_t r = (size_t)s->r, w = s->w, y, x;
    (void)chunk;
    for (y = begin; y < end; y++) {
        if (pc_interrupt_cancelled(s->ctl)) {
            continue;
        }
        for (x = 0; x < w; x++) {
            if (s->valid[y * w + x]) {
                patch_moments(s->img, w, s->ch, j->c8, j->side, x - r, y - r, &s->smom[(y * w + x) * j->c8 * 2]);
            }
        }
    }
    return PC_OK;
}

pc_status pc_ip_solver_prepare(pc_ip_solver *s, bool keep_source_moments, pc_error *err)
{
    size_t area = pc_ip_barea(s->tbox), th = pc_ip_bh(s->tbox), c8 = c8_of(s->ch);
    prep_job j = {s, 2 * (size_t)s->r + 1, c8};
    s->full = pc_calloc_array(area > 0 ? area : 1, sizeof *s->full);
    if (s->full == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (area >= PC_IP_PAR_MIN_PATCHES) {
        (void)pc_algo_par_for(th, 1, prep_full, &j);
    } else {
        (void)prep_full(&j, 0, 0, th);
    }
    if (pc_algo_adapt_is_none(s->adapt)) {
        return PC_OK;
    }
    s->tmom = pc_calloc_array(area > 0 ? area * c8 : 1, 2 * sizeof(float));
    if (s->tmom == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (!keep_source_moments) {
        return PC_OK;
    }
    s->smom = pc_calloc_array(s->w * s->h * c8, 2 * sizeof(float));
    if (s->smom == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (s->w * s->h >= PC_IP_PAR_MIN_PATCHES) {
        (void)pc_algo_par_for(s->h, 1, prep_smom, &j);
    } else {
        (void)prep_smom(&j, 0, 0, s->h);
    }
    return PC_OK;
}

float pc_ip_dist_clipped(const pc_ip_solver *s, pc_ip_pt t, pc_ip_pt src, float cutoff)
{
    int32_t w = (int32_t)s->w, h = (int32_t)s->h, r = s->r, dy, dx;
    size_t ch = s->ch, n = 0, c;
    float full = (float)((2 * r + 1) * (2 * r + 1)) * (float)ch, sum = 0.0f;
    for (dy = -r; dy <= r; dy++) {
        int32_t ty = t.y + dy, sy = src.y + dy;
        if (ty < 0 || ty >= h) {
            continue;
        }
        for (dx = -r; dx <= r; dx++) {
            int32_t tx = t.x + dx;
            size_t ti, si;
            if (tx < 0 || tx >= w || s->px[(size_t)ty * s->w + (size_t)tx] == PC_ALGO_PX_VOID) {
                continue;
            }
            ti = ((size_t)ty * s->w + (size_t)tx) * ch;
            si = ((size_t)sy * s->w + (size_t)(src.x + dx)) * ch;
            for (c = 0; c < ch; c++) {
                float d = s->img[ti + c] - s->img[si + c];
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

float pc_ip_dist_full(const pc_ip_solver *s, pc_ip_pt t, pc_ip_pt src, float cutoff)
{
    size_t w = s->w, ch = s->ch, side = (size_t)(2 * s->r + 1), span = side * ch, i;
    float full = (float)((2 * s->r + 1) * (2 * s->r + 1)) * (float)ch, limit = cutoff * full, sum = 0.0f;
    int32_t dy;
    for (dy = -s->r; dy <= s->r; dy++) {
        const float *tp = &s->img[((size_t)(t.y + dy) * w + (size_t)(t.x - s->r)) * ch];
        const float *sp = &s->img[((size_t)(src.y + dy) * w + (size_t)(src.x - s->r)) * ch];
        for (i = 0; i < span; i += ch) {
            size_t c;
            for (c = 0; c < ch; c++) {
                float d = tp[i + c] - sp[i + c];
                sum += d * d;
            }
            if (sum > limit) {
                return INFINITY;
            }
        }
    }
    return sum / (float)(side * side * ch);
}

size_t pc_ip_moments(const pc_ip_solver *s, pc_ip_pt t, pc_ip_pt src, float (*m)[5], size_t c8)
{
    int32_t w = (int32_t)s->w, h = (int32_t)s->h, r = s->r, dy, dx;
    size_t n = 0, c;
    memset(m, 0, c8 * sizeof *m);
    for (dy = -r; dy <= r; dy++) {
        int32_t ty = t.y + dy, sy = src.y + dy;
        if (ty < 0 || ty >= h) {
            continue;
        }
        for (dx = -r; dx <= r; dx++) {
            int32_t tx = t.x + dx;
            size_t ti, si;
            if (tx < 0 || tx >= w || s->px[(size_t)ty * s->w + (size_t)tx] == PC_ALGO_PX_VOID) {
                continue;
            }
            ti = ((size_t)ty * s->w + (size_t)tx) * s->ch;
            si = ((size_t)sy * s->w + (size_t)(src.x + dx)) * s->ch;
            for (c = 0; c < c8; c++) {
                float a = s->img[ti + c], b = s->img[si + c];
                m[c][0] += a;
                m[c][1] += a * a;
                m[c][2] += b;
                m[c][3] += b * b;
                m[c][4] += a * b;
            }
            n++;
        }
    }
    return n;
}

void pc_ip_moments_full(const pc_ip_solver *s, size_t k, pc_ip_pt t, pc_ip_pt src, float (*m)[5], size_t c8)
{
    size_t w = s->w, ch = s->ch, side = (size_t)(2 * s->r + 1), c, x;
    const float *tm = &s->tmom[k * c8 * 2];
    const float *sm = s->smom != NULL ? &s->smom[((size_t)src.y * w + (size_t)src.x) * c8 * 2] : NULL;
    float acc[PC_IP_MOM_CH][3];
    int32_t dy;
    memset(acc, 0, sizeof acc);
    for (dy = -s->r; dy <= s->r; dy++) {
        const float *trow = &s->img[((size_t)(t.y + dy) * w + (size_t)(t.x - s->r)) * ch];
        const float *srow = &s->img[((size_t)(src.y + dy) * w + (size_t)(src.x - s->r)) * ch];
        for (x = 0; x < side; x++) {
            const float *tp = &trow[x * ch], *sp = &srow[x * ch];
            if (sm != NULL) {
                for (c = 0; c < c8; c++) {
                    acc[c][2] += tp[c] * sp[c];
                }
            } else {
                for (c = 0; c < c8; c++) {
                    float a = tp[c], b = sp[c];
                    acc[c][0] += b;
                    acc[c][1] += b * b;
                    acc[c][2] += a * b;
                }
            }
        }
    }
    for (c = 0; c < c8; c++) {
        m[c][0] = tm[c * 2];
        m[c][1] = tm[c * 2 + 1];
        m[c][2] = sm != NULL ? sm[c * 2] : acc[c][0];
        m[c][3] = sm != NULL ? sm[c * 2 + 1] : acc[c][1];
        m[c][4] = acc[c][2];
    }
}

/* What is left of the target after the source is fitted to it, per sample. */
static float adapted_cost(pc_algo_adapt a, float nf, const float (*m)[5], size_t c8)
{
    float sum = 0.0f;
    size_t c;
    for (c = 0; c < c8; c++) {
        float g, b;
        pc_algo_adapt_fit(a, nf, m[c][0], m[c][1], m[c][2], m[c][3], &g, &b);
        sum += m[c][1] + g * g * m[c][3] + nf * b * b - 2.0f * g * m[c][4] - 2.0f * b * m[c][0] +
               2.0f * g * b * m[c][2];
    }
    return pc_rs_maxf_k(sum, 0.0f) / (nf * (float)c8);
}

float pc_ip_dist_adapted(const pc_ip_solver *s, pc_ip_pt t, pc_ip_pt src)
{
    float m[PC_IP_MOM_CH][5];
    size_t c8 = c8_of(s->ch), n = pc_ip_moments(s, t, src, m, c8);
    if (n == 0) {
        return INFINITY;
    }
    return adapted_cost(s->adapt, (float)n, (const float (*)[5])m, c8);
}

float pc_ip_dist_adapted_full(const pc_ip_solver *s, size_t k, pc_ip_pt t, pc_ip_pt src)
{
    float m[PC_IP_MOM_CH][5];
    size_t c8 = c8_of(s->ch), side = (size_t)(2 * s->r + 1);
    pc_ip_moments_full(s, k, t, src, m, c8);
    return adapted_cost(s->adapt, (float)(side * side), (const float (*)[5])m, c8);
}

float pc_ip_dist(const pc_ip_solver *s, pc_ip_pt t, size_t k, pc_ip_pt src, float cutoff)
{
    bool full = s->full[k];
    if (!pc_algo_adapt_is_none(s->adapt)) {
        return full ? pc_ip_dist_adapted_full(s, k, t, src) : pc_ip_dist_adapted(s, t, src);
    }
    return full ? pc_ip_dist_full(s, t, src, cutoff) : pc_ip_dist_clipped(s, t, src, cutoff);
}

/* Gain and bias per channel (out: ch pairs) for the match t -> src. */
static void transform(const pc_ip_solver *s, pc_ip_pt t, size_t k, pc_ip_pt src, float (*out)[2])
{
    float m[PC_IP_MOM_CH][5];
    size_t c8 = c8_of(s->ch), n, c;
    if (pc_algo_adapt_is_none(s->adapt)) {
        for (c = 0; c < s->ch; c++) {
            out[c][0] = 1.0f, out[c][1] = 0.0f;
        }
        return;
    }
    if (s->full[k]) {
        size_t side = (size_t)(2 * s->r + 1);
        pc_ip_moments_full(s, k, t, src, m, c8);
        n = side * side;
    } else {
        n = pc_ip_moments(s, t, src, m, c8);
    }
    for (c = 0; c < s->ch; c++) {
        if (n == 0 || c >= c8) {
            out[c][0] = 1.0f, out[c][1] = 0.0f;
        } else {
            pc_algo_adapt_fit(s->adapt, (float)n, m[c][0], m[c][1], m[c][2], m[c][3], &out[c][0], &out[c][1]);
        }
    }
}

static void try_cand(const pc_ip_solver *s, pc_ip_pt t, size_t k, pc_ip_pt cand, pc_ip_pt *best, float *bc)
{
    if (pc_ip_solver_is_valid(s, cand.x, cand.y) && (cand.x != best->x || cand.y != best->y)) {
        float d = pc_ip_dist(s, t, k, cand, *bc);
        if (d < *bc) {
            *best = cand;
            *bc = d;
        }
    }
}

/* ---- PatchMatch ---------------------------------------------------------------------------------- */

typedef struct pm_job {
    const pc_ip_solver *s;
    pc_ip_pt *nnf;
    float *cost;
    size_t pass;
    uint64_t seed;
    int32_t radius;
    size_t first_band; /* band index of the target box's first row */
} pm_job;

static void pm_band(const pm_job *j, size_t bi)
{
    const pc_ip_solver *s = j->s;
    pc_ip_bounds tb = s->tbox;
    size_t tw = pc_ip_bw(tb), ys = bi * PM_BAND > tb.y0 ? bi * PM_BAND : tb.y0;
    size_t ye = (bi + 1) * PM_BAND < tb.y1 ? (bi + 1) * PM_BAND : tb.y1, rows = ye - ys, ry, rx;
    bool reverse = j->pass % 2 == 1;
    int32_t step = reverse ? -1 : 1;
    pc_ip_pt *nn = &j->nnf[(ys - tb.y0) * tw];
    float *co = &j->cost[(ys - tb.y0) * tw];
    pc_ip_rng rng = pc_ip_rng_new(j->seed ^ ((uint64_t)bi * 0x2545F4914F6CDD1Dull) ^ ((uint64_t)j->pass << 40));
    if (pc_interrupt_cancelled(s->ctl)) {
        return;
    }
    for (ry = 0; ry < rows; ry++) {
        size_t ly = reverse ? rows - 1 - ry : ry, y = ys + ly;
        if (pc_interrupt_cancelled(s->ctl)) {
            return;
        }
        for (rx = 0; rx < tw; rx++) {
            size_t lx = reverse ? tw - 1 - rx : rx, li = ly * tw + lx, k = (y - tb.y0) * tw + lx;
            int64_t nx = (int64_t)lx - step, nly = (int64_t)ly - step;
            pc_ip_pt t, best = nn[li];
            float bc = co[li];
            int32_t rad;
            if (!s->targets[k]) {
                continue;
            }
            t = (pc_ip_pt){(int32_t)(tb.x0 + lx), (int32_t)y};
            if (nx >= 0 && (size_t)nx < tw) {
                pc_ip_pt c = nn[ly * tw + (size_t)nx];
                try_cand(s, t, k, (pc_ip_pt){c.x + step, c.y}, &best, &bc);
            }
            if (nly >= 0 && (size_t)nly < rows) {
                pc_ip_pt c = nn[(size_t)nly * tw + lx];
                try_cand(s, t, k, (pc_ip_pt){c.x, c.y + step}, &best, &bc);
            }
            for (rad = j->radius; rad >= 1; rad /= 2) {
                int32_t cx = best.x + pc_ip_rng_range(&rng, -rad, rad);
                int32_t cy = best.y + pc_ip_rng_range(&rng, -rad, rad);
                try_cand(s, t, k, (pc_ip_pt){cx, cy}, &best, &bc);
            }
            nn[li] = best;
            co[li] = bc;
        }
    }
}

static pc_status pm_run(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const pm_job *j = ctx;
    size_t b;
    (void)chunk;
    for (b = begin; b < end; b++) {
        pm_band(j, j->first_band + b);
    }
    return PC_OK;
}

void pc_ip_patchmatch(const pc_ip_solver *s, pc_ip_pt *nnf, float *cost, size_t pass, uint64_t seed,
                      int32_t search_radius)
{
    pc_ip_bounds tb = s->tbox;
    size_t first, last, nb;
    pm_job j;
    if (pc_ip_bw(tb) == 0 || pc_ip_bh(tb) == 0) {
        return;
    }
    first = tb.y0 / PM_BAND, last = (tb.y1 - 1) / PM_BAND, nb = last - first + 1;
    j = (pm_job){s, nnf, cost, pass, seed, search_radius, first};
    if (pc_ip_barea(tb) >= PC_IP_PAR_MIN_PATCHES && nb > 1) {
        (void)pc_algo_par_for(nb, 1, pm_run, &j);
    } else {
        (void)pm_run(&j, 0, 0, nb);
    }
}

/* ---- costs ---------------------------------------------------------------------------------------- */

typedef struct cost_job {
    pc_ip_solver *s;
    const pc_ip_pt *nnf;
    float *cost;
    size_t c8, side;
} cost_job;

static pc_status cost_tmom(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const cost_job *j = ctx;
    const pc_ip_solver *s = j->s;
    size_t tw = pc_ip_bw(s->tbox), r = (size_t)s->r, ty, tx;
    (void)chunk;
    for (ty = begin; ty < end; ty++) {
        if (pc_interrupt_cancelled(s->ctl)) {
            continue;
        }
        for (tx = 0; tx < tw; tx++) {
            size_t k = ty * tw + tx;
            if (s->full[k]) {
                patch_moments(s->img, s->w, s->ch, j->c8, j->side, s->tbox.x0 + tx - r, s->tbox.y0 + ty - r,
                              &s->tmom[k * j->c8 * 2]);
            }
        }
    }
    return PC_OK;
}

static pc_status cost_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const cost_job *j = ctx;
    const pc_ip_solver *s = j->s;
    size_t tw = pc_ip_bw(s->tbox), ty, tx;
    (void)chunk;
    for (ty = begin; ty < end; ty++) {
        if (pc_interrupt_cancelled(s->ctl)) {
            continue;
        }
        for (tx = 0; tx < tw; tx++) {
            size_t k = ty * tw + tx;
            if (s->targets[k]) {
                pc_ip_pt t = {(int32_t)(s->tbox.x0 + tx), (int32_t)(s->tbox.y0 + ty)};
                j->cost[k] = pc_ip_dist(s, t, k, j->nnf[k], INFINITY);
            }
        }
    }
    return PC_OK;
}

pc_status pc_ip_costs(pc_ip_solver *s, const pc_ip_pt *nnf, float *cost, pc_error *err)
{
    size_t th = pc_ip_bh(s->tbox);
    bool par = pc_ip_barea(s->tbox) >= PC_IP_PAR_MIN_PATCHES;
    cost_job j = {s, nnf, cost, c8_of(s->ch), 2 * (size_t)s->r + 1};
    pc_status st = pc_interrupt_check(s->ctl, err);
    if (st != PC_OK) {
        return st;
    }
    if (!pc_algo_adapt_is_none(s->adapt)) {
        (void)(par ? pc_algo_par_for(th, 1, cost_tmom, &j) : cost_tmom(&j, 0, 0, th));
    }
    (void)(par ? pc_algo_par_for(th, 1, cost_rows, &j) : cost_rows(&j, 0, 0, th));
    return pc_interrupt_check(s->ctl, err);
}

/* ---- vote ----------------------------------------------------------------------------------------- */

typedef struct vote_job {
    const pc_ip_solver *s;
    const pc_ip_pt *nnf;
    const float *cost;
    float sigma2;
    float *wt;
    int64_t *src;
    float (*gb)[2]; /* tbox area * ch, with adaptation */
    float *out;     /* hole box area * ch */
    bool *got;      /* hole box area */
} vote_job;

static bool used(const vote_job *j, size_t k)
{
    return j->s->targets[k] && isfinite(j->cost[k]);
}

static pc_status vote_weights(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const vote_job *j = ctx;
    size_t tw = pc_ip_bw(j->s->tbox), w = j->s->w, ty, tx;
    (void)chunk;
    for (ty = begin; ty < end; ty++) {
        for (tx = 0; tx < tw; tx++) {
            size_t k = ty * tw + tx;
            if (used(j, k)) {
                j->wt[k] = pc_rs_maxf_k(expf(-j->cost[k] / (2.0f * j->sigma2)), 1e-8f);
                j->src[k] = (int64_t)j->nnf[k].y * (int64_t)w + (int64_t)j->nnf[k].x;
            }
        }
    }
    return PC_OK;
}

static pc_status vote_transforms(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const vote_job *j = ctx;
    const pc_ip_solver *s = j->s;
    size_t tw = pc_ip_bw(s->tbox), ty, tx;
    (void)chunk;
    for (ty = begin; ty < end; ty++) {
        if (pc_interrupt_cancelled(s->ctl)) {
            continue;
        }
        for (tx = 0; tx < tw; tx++) {
            size_t k = ty * tw + tx;
            if (used(j, k)) {
                pc_ip_pt t = {(int32_t)(s->tbox.x0 + tx), (int32_t)(s->tbox.y0 + ty)};
                transform(s, t, k, j->nnf[k], &j->gb[k * s->ch]);
            }
        }
    }
    return PC_OK;
}

/* Hole pixel (x, y)'s vote into out: the weighted mean of what the matches of the patches covering
 * it put there, summed patch by patch in raster order of their centres. False when none votes. */
static bool vote_pixel(const vote_job *j, size_t x, size_t y, float *out, float *acc)
{
    const pc_ip_solver *s = j->s;
    size_t w = s->w, r = (size_t)s->r, ch = s->ch, tw = pc_ip_bw(s->tbox), ty, tx, c;
    size_t ylo = y > r ? y - r : 0, yhi = y + r < s->h - 1 ? y + r : s->h - 1;
    size_t xlo = x > r ? x - r : 0, xhi = x + r < w - 1 ? x + r : w - 1;
    float wsum = 0.0f;
    memset(acc, 0, ch * sizeof *acc);
    for (ty = ylo; ty <= yhi; ty++) {
        int64_t dy = (int64_t)ty - (int64_t)y;
        size_t krow = (ty - s->tbox.y0) * tw;
        for (tx = xlo; tx <= xhi; tx++) {
            size_t k = krow + tx - s->tbox.x0, si;
            float wt = j->wt[k];
            if (wt == 0.0f) {
                continue;
            }
            si = (size_t)(j->src[k] - dy * (int64_t)w - ((int64_t)tx - (int64_t)x)) * ch;
            for (c = 0; c < ch; c++) {
                float v = j->gb != NULL ? j->gb[k * ch + c][0] * s->img[si + c] + j->gb[k * ch + c][1]
                                        : s->img[si + c];
                acc[c] += v * wt;
            }
            wsum += wt;
        }
    }
    if (wsum > 0.0f) {
        for (c = 0; c < ch; c++) {
            out[c] = acc[c] / wsum;
        }
        return true;
    }
    return false;
}

static pc_status vote_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const vote_job *j = ctx;
    const pc_ip_solver *s = j->s;
    pc_ip_bounds hb = s->hole_box;
    size_t hw = pc_ip_bw(hb), ch = s->ch, yy, x;
    float acc[PC_ALGO_HEAL_MAX_CH];
    (void)chunk;
    for (yy = begin; yy < end; yy++) {
        size_t y = hb.y0 + yy;
        if (pc_interrupt_cancelled(s->ctl)) {
            continue;
        }
        for (x = hb.x0; x < hb.x1; x++) {
            size_t o = yy * hw + (x - hb.x0);
            if (s->px[y * s->w + x] != PC_ALGO_PX_HOLE) {
                continue;
            }
            j->got[o] = vote_pixel(j, x, y, &j->out[o * ch], acc);
        }
    }
    return PC_OK;
}

pc_status pc_ip_vote(pc_ip_solver *s, const pc_ip_pt *nnf, const float *cost, pc_error *err)
{
    pc_ip_bounds tb = s->tbox, hb = s->hole_box;
    size_t area = pc_ip_barea(tb), harea = pc_ip_barea(hb), ch = s->ch, k, n = 0, yy, xx;
    size_t th = pc_ip_bh(tb), hh = pc_ip_bh(hb), hw = pc_ip_bw(hb);
    bool par = area >= PC_IP_PAR_MIN_PATCHES;
    float *cs = NULL;
    vote_job j;
    pc_status st = PC_OK;
    /* A cancelled solve is thrown away. */
    if (pc_interrupt_cancelled(s->ctl)) {
        return PC_OK;
    }
    memset(&j, 0, sizeof j);
    j.s = s, j.nnf = nnf, j.cost = cost;
    cs = pc_malloc_array(area > 0 ? area : 1, sizeof *cs);
    j.wt = pc_calloc_array(area > 0 ? area : 1, sizeof *j.wt);
    j.src = pc_calloc_array(area > 0 ? area : 1, sizeof *j.src);
    j.out = pc_calloc_array(harea > 0 ? harea * ch : 1, sizeof *j.out);
    j.got = pc_calloc_array(harea > 0 ? harea : 1, sizeof *j.got);
    if (cs == NULL || j.wt == NULL || j.src == NULL || j.out == NULL || j.got == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    /* sigma^2 from the 75th percentile of match costs. */
    for (k = 0; k < area; k++) {
        if (used(&j, k)) {
            cs[n++] = cost[k];
        }
    }
    if ((st = pc_ip_sigma2(cs, n, &j.sigma2, err)) != PC_OK) {
        goto cleanup;
    }
    (void)(par ? pc_algo_par_for(th, 1, vote_weights, &j) : vote_weights(&j, 0, 0, th));
    if (!pc_algo_adapt_is_none(s->adapt)) {
        j.gb = pc_malloc_array(area > 0 ? area * ch : 1, sizeof *j.gb);
        if (j.gb == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            goto cleanup;
        }
        for (k = 0; k < area * ch; k++) {
            j.gb[k][0] = 1.0f, j.gb[k][1] = 0.0f;
        }
        (void)(par ? pc_algo_par_for(th, 1, vote_transforms, &j) : vote_transforms(&j, 0, 0, th));
    }
    (void)(harea >= PC_IP_PAR_MIN_PATCHES ? pc_algo_par_for(hh, 1, vote_rows, &j) : vote_rows(&j, 0, 0, hh));
    for (yy = 0; yy < hh; yy++) {
        for (xx = 0; xx < hw; xx++) {
            if (j.got[yy * hw + xx]) {
                memcpy(&s->img[((hb.y0 + yy) * s->w + hb.x0 + xx) * ch], &j.out[(yy * hw + xx) * ch],
                       ch * sizeof(float));
            }
        }
    }
cleanup:
    free(cs);
    free(j.wt);
    free(j.src);
    free(j.gb);
    free(j.out);
    free(j.got);
    return st;
}
