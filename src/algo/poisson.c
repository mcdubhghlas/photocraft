/* Poisson / membrane solver (port of crates/algo/src/poisson.rs); see poisson.h. */
#include "algo/poisson.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Convergence threshold (max per-sweep update) for the SOR sweeps. */
#define TOL 2e-5f
/* SOR sweeps (cap) and relaxation factor on every level above the coarsest. */
#define FINE_ITERS 30u
#define FINE_OMEGA 1.7f
/* The coarsest level runs up to 2000 sweeps; past this many unknowns that is minutes of work for
 * a degenerate strip the cascade cannot shrink (plan section 7). */
#define MAX_DIRECT_UNKNOWN ((size_t)1 << 24)

pc_status pc_algo_heal_dims(size_t w, size_t h, size_t ch, size_t len, size_t *npx, pc_error *err)
{
    size_t n, nf;
    if (ch == 0 || ch > PC_ALGO_HEAL_MAX_CH) {
        return pc_fail(err, PC_ERR_INVALID, "channel count %zu outside 1..=%zu", ch, PC_ALGO_HEAL_MAX_CH);
    }
    if (w > PC_ALGO_HEAL_MAX_SIDE || h > PC_ALGO_HEAL_MAX_SIDE || !pc_size_mul(w, h, &n) || n > UINT32_MAX ||
        !pc_size_mul3(n, ch, sizeof(float), &nf) || nf > PC_MAX_ALLOC) {
        return pc_fail(err, PC_ERR_LIMIT, "image %zu x %zu x %zu is too large", w, h, ch);
    }
    if (len != SIZE_MAX && len != n * ch) {
        return pc_fail(err, PC_ERR_INVALID, "buffer holds %zu samples, expected %zu", len, n * ch);
    }
    if (npx != NULL) {
        *npx = n;
    }
    return PC_OK;
}

static bool stopped(const pc_interrupt *ctl)
{
    return pc_interrupt_cancelled(ctl);
}

/* SOR sweeps over the unknown pixels until the largest update drops below TOL. */
static pc_status sor(size_t w, size_t h, const bool *unknown, float *v, size_t max_iter, float omega,
                     const pc_interrupt *ctl, pc_error *err)
{
    size_t n = w * h, count = 0, i, it;
    uint32_t *cell;
    uint8_t *mask;
    for (i = 0; i < n; i++) {
        count += unknown[i] ? 1u : 0u;
    }
    cell = pc_malloc_array(count, sizeof *cell);
    mask = pc_malloc_array(count, 1);
    if (cell == NULL || mask == NULL) {
        free(cell);
        free(mask);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    count = 0;
    for (i = 0; i < n; i++) {
        if (unknown[i]) {
            size_t x = i % w, y = i / w;
            cell[count] = (uint32_t)i;
            mask[count] = (uint8_t)((x > 0 ? 1u : 0u) | (x + 1 < w ? 2u : 0u) | (y > 0 ? 4u : 0u) |
                                    (y + 1 < h ? 8u : 0u));
            count++;
        }
    }
    for (it = 0; it < max_iter; it++) {
        float max_d = 0.0f;
        size_t k;
        if (stopped(ctl)) {
            break;
        }
        for (k = 0; k < count; k++) {
            size_t j = cell[k];
            uint8_t m = mask[k];
            float s, cnt, d;
            if (m == 15u) {
                s = v[j - 1] + v[j + 1] + v[j - w] + v[j + w];
                cnt = 4.0f;
            } else {
                s = 0.0f;
                cnt = 0.0f;
                if (m & 1u) {
                    s += v[j - 1];
                    cnt += 1.0f;
                }
                if (m & 2u) {
                    s += v[j + 1];
                    cnt += 1.0f;
                }
                if (m & 4u) {
                    s += v[j - w];
                    cnt += 1.0f;
                }
                if (m & 8u) {
                    s += v[j + w];
                    cnt += 1.0f;
                }
                if (cnt == 0.0f) {
                    continue;
                }
            }
            d = omega * (s / cnt - v[j]);
            v[j] += d;
            max_d = pc_rs_maxf(max_d, fabsf(d));
        }
        if (max_d < TOL) {
            break;
        }
    }
    free(cell);
    free(mask);
    return PC_OK;
}

static pc_status solve_rec(size_t w, size_t h, const bool *unknown, float *v, size_t depth,
                           const pc_interrupt *ctl, pc_error *err)
{
    size_t n = w * h, n_unknown = 0, n_known = 0, i;
    float known_sum = -0.0f;
    if (stopped(ctl)) {
        return PC_OK;
    }
    for (i = 0; i < n; i++) {
        if (unknown[i]) {
            n_unknown++;
        } else {
            n_known++;
            known_sum += v[i];
        }
    }
    if (n_unknown == 0 || n_known == 0) {
        return PC_OK; /* nothing to solve, or nothing to interpolate from */
    }
    if (w >= 8 && h >= 8 && n_unknown > 64 && depth < 16) {
        /* Restrict: a coarse pixel is known if any of its children is (value = mean of those). */
        size_t cw = (w + 1) / 2, chh = (h + 1) / 2, cx, cy, x, y;
        bool *cu = pc_malloc_array(cw * chh, sizeof *cu);
        float *cv = pc_calloc_array(cw * chh, sizeof *cv);
        pc_status st;
        static const size_t DX[4] = {0, 1, 0, 1}, DY[4] = {0, 0, 1, 1};
        if (cu == NULL || cv == NULL) {
            free(cu);
            free(cv);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (cy = 0; cy < chh; cy++) {
            for (cx = 0; cx < cw; cx++) {
                float s = 0.0f;
                uint32_t cnt = 0;
                int k;
                for (k = 0; k < 4; k++) {
                    x = cx * 2 + DX[k];
                    y = cy * 2 + DY[k];
                    if (x < w && y < h && !unknown[y * w + x]) {
                        s += v[y * w + x];
                        cnt++;
                    }
                }
                cu[cy * cw + cx] = cnt == 0;
                if (cnt > 0) {
                    cv[cy * cw + cx] = s / (float)cnt;
                }
            }
        }
        st = solve_rec(cw, chh, cu, cv, depth + 1, ctl, err);
        if (st != PC_OK) {
            free(cu);
            free(cv);
            return st;
        }
        /* Prolong: bilinear sample of the coarse solution as the initial guess. */
        for (y = 0; y < h; y++) {
            float fy = pc_clampf(((float)y + 0.5f) / 2.0f - 0.5f, 0.0f, (float)(chh - 1));
            size_t y0 = pc_f32_to_size(floorf(fy));
            size_t y1 = y0 + 1 < chh - 1 ? y0 + 1 : chh - 1;
            float ty = fy - (float)y0;
            for (x = 0; x < w; x++) {
                float fx, tx, a, b;
                size_t x0, x1;
                if (!unknown[y * w + x]) {
                    continue;
                }
                fx = pc_clampf(((float)x + 0.5f) / 2.0f - 0.5f, 0.0f, (float)(cw - 1));
                x0 = pc_f32_to_size(floorf(fx));
                x1 = x0 + 1 < cw - 1 ? x0 + 1 : cw - 1;
                tx = fx - (float)x0;
                a = cv[y0 * cw + x0] + (cv[y0 * cw + x1] - cv[y0 * cw + x0]) * tx;
                b = cv[y1 * cw + x0] + (cv[y1 * cw + x1] - cv[y1 * cw + x0]) * tx;
                v[y * w + x] = a + (b - a) * ty;
            }
        }
        free(cu);
        free(cv);
        return sor(w, h, unknown, v, FINE_ITERS, FINE_OMEGA, ctl, err);
    }
    if (n_unknown > MAX_DIRECT_UNKNOWN) {
        return pc_fail(err, PC_ERR_LIMIT, "membrane solve over %zu unknown pixels in a %zu x %zu strip",
                       n_unknown, w, h);
    }
    {
        float mean = known_sum / (float)n_known;
        for (i = 0; i < n; i++) {
            if (unknown[i]) {
                v[i] = mean;
            }
        }
    }
    return sor(w, h, unknown, v, 2000, 1.85f, ctl, err);
}

pc_status pc_algo_poisson_solve_membrane(size_t w, size_t h, const bool *unknown, float *v,
                                         const pc_interrupt *ctl, pc_error *err)
{
    pc_status st = pc_algo_heal_dims(w, h, 1, SIZE_MAX, NULL, err);
    if (st != PC_OK) {
        return st;
    }
    if (w * h == 0) {
        return PC_OK;
    }
    if (unknown == NULL || v == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing buffer");
    }
    return solve_rec(w, h, unknown, v, 0, ctl, err);
}

pc_status pc_algo_poisson_solve_membrane_reference(size_t w, size_t h, const bool *unknown, float *v,
                                                   float tol, size_t max_iter, pc_error *err)
{
    size_t n, i, it;
    pc_status st = pc_algo_heal_dims(w, h, 1, SIZE_MAX, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n == 0) {
        return PC_OK;
    }
    if (unknown == NULL || v == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing buffer");
    }
    for (i = 0; i < n; i++) {
        if (unknown[i]) {
            v[i] = 0.0f;
        }
    }
    for (it = 0; it < max_iter; it++) {
        float max_d = 0.0f;
        size_t x, y;
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                size_t j = y * w + x;
                float s = 0.0f, cnt = 0.0f;
                if (!unknown[j]) {
                    continue;
                }
                if (x > 0) {
                    s += v[j - 1];
                    cnt += 1.0f;
                }
                if (x + 1 < w) {
                    s += v[j + 1];
                    cnt += 1.0f;
                }
                if (y > 0) {
                    s += v[j - w];
                    cnt += 1.0f;
                }
                if (y + 1 < h) {
                    s += v[j + w];
                    cnt += 1.0f;
                }
                if (cnt > 0.0f) {
                    float nv = s / cnt;
                    max_d = pc_rs_maxf(max_d, fabsf(nv - v[j]));
                    v[j] = nv;
                }
            }
        }
        if (max_d < tol) {
            break;
        }
    }
    return PC_OK;
}

/* ---- per-channel driver ---------------------------------------------------------------------- */

typedef struct chan_job {
    size_t w, h, ch;
    const float *src; /* seamless clone: source; membrane fill: the image */
    const float *dst; /* seamless clone: destination; NULL for a membrane fill */
    const bool *mask;
    const pc_interrupt *ctl;
    float *out;
    pc_error *errs; /* one per channel */
} chan_job;

static pc_status chan_run(void *ctx, size_t chunk, size_t begin, size_t end)
{
    chan_job *j = ctx;
    size_t c, i, n = j->w * j->h, ch = j->ch;
    (void)chunk;
    for (c = begin; c < end; c++) {
        float *plane = pc_malloc_array(n, sizeof *plane);
        pc_status st;
        if (plane == NULL) {
            return pc_fail(&j->errs[c], PC_ERR_NOMEM, "out of memory");
        }
        for (i = 0; i < n; i++) {
            if (j->dst != NULL) {
                plane[i] = j->mask[i] ? 0.0f : j->dst[i * ch + c] - j->src[i * ch + c];
            } else {
                plane[i] = j->mask[i] ? 0.0f : j->src[i * ch + c];
            }
        }
        st = solve_rec(j->w, j->h, j->mask, plane, 0, j->ctl, &j->errs[c]);
        if (st != PC_OK) {
            free(plane);
            return st;
        }
        for (i = 0; i < n; i++) {
            if (j->dst != NULL) {
                j->out[i * ch + c] = j->mask[i] ? j->src[i * ch + c] + plane[i] : j->dst[i * ch + c];
            } else {
                j->out[i * ch + c] = plane[i];
            }
        }
        free(plane);
    }
    return PC_OK;
}

static pc_status per_channel(chan_job *j, pc_error *err)
{
    pc_error errs[PC_ALGO_HEAL_MAX_CH];
    pc_status st;
    size_t c;
    memset(errs, 0, sizeof errs);
    j->errs = errs;
    st = pc_algo_par_for(j->ch, 1, chan_run, j);
    if (st != PC_OK && err != NULL) {
        for (c = 0; c < j->ch; c++) {
            if (errs[c].code != PC_OK) {
                *err = errs[c];
                break;
            }
        }
    }
    return st;
}

pc_status pc_algo_poisson_seamless_clone(size_t w, size_t h, size_t ch, const float *src, const float *dst,
                                         size_t len, const bool *mask, size_t mask_len, float **out,
                                         pc_error *err)
{
    size_t n;
    float *o;
    chan_job j;
    pc_status st = pc_algo_heal_dims(w, h, ch, len, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (mask_len != n || (n > 0 && (src == NULL || dst == NULL || mask == NULL))) {
        return pc_fail(err, PC_ERR_INVALID, "mask holds %zu entries, expected %zu", mask_len, n);
    }
    o = pc_calloc_array(n * ch, sizeof *o);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    j = (chan_job){w, h, ch, src, dst, mask, NULL, o, NULL};
    if (n > 0 && (st = per_channel(&j, err)) != PC_OK) {
        free(o);
        return st;
    }
    *out = o;
    return PC_OK;
}

pc_status pc_algo_poisson_membrane_fill(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                        const bool *hole, size_t hole_len, const pc_interrupt *ctl,
                                        float **out, pc_error *err)
{
    size_t n;
    float *o;
    chan_job j;
    pc_status st = pc_algo_heal_dims(w, h, ch, len, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (hole_len != n || (n > 0 && (img == NULL || hole == NULL))) {
        return pc_fail(err, PC_ERR_INVALID, "hole mask holds %zu entries, expected %zu", hole_len, n);
    }
    if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
        return st;
    }
    o = pc_calloc_array(n * ch, sizeof *o);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    j = (chan_job){w, h, ch, img, NULL, hole, ctl, o, NULL};
    if ((n > 0 && (st = per_channel(&j, err)) != PC_OK) || (st = pc_interrupt_check(ctl, err)) != PC_OK) {
        free(o);
        return st;
    }
    *out = o;
    return PC_OK;
}
