/* Click-local red-eye correction (port of crates/algo/src/redeye.rs); see redeye.h. */
#include "algo/redeye.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

int32_t pc_algo_redeye_halo_radius(void)
{
    float blur = ceilf(PC_ALGO_REDEYE_SIGMA * 3.0f);
    int32_t b = isfinite(blur) ? pc_f32_to_i32(pc_clampf(blur, 0.0f, 64.0f)) : 3;
    return 1 + b + 1;
}

float pc_algo_redeye_search_radius(float pupil_size)
{
    float p = isfinite(pupil_size) ? pc_clampf(pupil_size, 1.0f, 100.0f) : 50.0f;
    return pc_clampf(4.0f + 1.20f * p, 8.0f, 128.0f);
}

bool pc_algo_redeye_unclamped_window_side(float pupil_size, uint32_t *side)
{
    float r, half, s;
    if (!isfinite(pupil_size)) {
        return false;
    }
    r = 4.0f + 1.20f * pupil_size;
    if (!isfinite(r)) {
        return false;
    }
    half = fabsf(r) + (float)pc_algo_redeye_halo_radius();
    if (!isfinite(half) || half > (float)PC_ALGO_REDEYE_MAX_WINDOW) {
        return false;
    }
    s = ceilf(2.0f * half + 1.0f);
    if (!isfinite(s) || s > (float)PC_ALGO_REDEYE_MAX_WINDOW) {
        return false;
    }
    *side = pc_f32_to_u32(s);
    return true;
}

static float luma(const float p[4])
{
    return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
}

static bool is_red_eye(const float p[4])
{
    float r = p[0], g = p[1], b = p[2], mx, mn;
    if (!(isfinite(r) && isfinite(g) && isfinite(b))) {
        return false;
    }
    if (r <= g + 0.15f || r <= b + 0.15f) {
        return false;
    }
    if (luma(p) >= 0.92f) {
        return false;
    }
    mx = pc_rs_maxf(pc_rs_maxf(r, g), b);
    mn = pc_rs_minf(pc_rs_minf(r, g), b);
    return mx - mn >= 0.08f;
}

/* `x as i32` of a column index (wraps like the reference past i32::MAX). */
static int32_t as_i32(size_t x)
{
    return pc_i32_from_bits((uint32_t)x);
}

static bool within_search(int32_t dx, int32_t dy, float radius)
{
    float d = hypotf((float)dx, (float)dy);
    return isfinite(d) && d <= radius;
}

/* Offset of pixel (x, y) from the click, saturating like the reference. */
static void offset(size_t x, size_t y, const int32_t origin[2], const int32_t click[2], int32_t *dx,
                   int32_t *dy)
{
    *dx = pc_i32_sat_sub(pc_i32_sat_add(origin[0], as_i32(x)), click[0]);
    *dy = pc_i32_sat_sub(pc_i32_sat_add(origin[1], as_i32(y)), click[1]);
}

static bool nearest_seed(const bool *mask, size_t w, size_t h, const int32_t origin[2],
                         const int32_t click[2], float radius, size_t *seed)
{
    size_t x, y, best = 0;
    int64_t bd = 0;
    bool have = false;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int32_t dx, dy;
            int64_t d2;
            if (!mask[y * w + x]) {
                continue;
            }
            offset(x, y, origin, click, &dx, &dy);
            if (!within_search(dx, dy, radius)) {
                continue;
            }
            d2 = (int64_t)dx * dx + (int64_t)dy * dy; /* cannot overflow i64 */
            if (!have || d2 < bd) {
                have = true, best = y * w + x, bd = d2;
            }
        }
    }
    *seed = best;
    return have;
}

/* Keep only the 4-connected component of `mask` that contains seed. */
static pc_status flood_keep(bool *mask, size_t w, size_t h, size_t seed, pc_error *err)
{
    size_t n = w * h, len = 0, cap = 0, i;
    size_t *stack = NULL;
    bool *keep;
    if (seed >= n || w == 0) {
        return PC_OK;
    }
    keep = pc_calloc_array(n, sizeof *keep);
    stack = pc_grow_array(NULL, &cap, 64, sizeof *stack);
    if (keep == NULL || stack == NULL) {
        free(keep);
        free(stack);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    stack[len++] = seed;
    while (len > 0) {
        size_t x, y, *ns;
        i = stack[--len];
        if (keep[i] || !mask[i]) {
            continue;
        }
        keep[i] = true;
        x = i % w, y = i / w;
        ns = pc_grow_array(stack, &cap, len + 4, sizeof *stack);
        if (ns == NULL) {
            free(keep);
            free(stack);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        stack = ns;
        if (x > 0) {
            stack[len++] = i - 1;
        }
        if (x + 1 < w) {
            stack[len++] = i + 1;
        }
        if (y > 0) {
            stack[len++] = i - w;
        }
        if (y + 1 < h) {
            stack[len++] = i + w;
        }
        if (len > n * 4) {
            break;
        }
    }
    memcpy(mask, keep, n * sizeof *mask);
    free(keep);
    free(stack);
    return PC_OK;
}

static void erode(const bool *mask, size_t w, size_t h, bool *out)
{
    size_t x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            bool on = true;
            int dx, dy;
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int64_t xx = (int64_t)x + dx, yy = (int64_t)y + dy;
                    if (xx < 0 || yy < 0 || xx >= (int64_t)w || yy >= (int64_t)h ||
                        !mask[(size_t)yy * w + (size_t)xx]) {
                        on = false;
                    }
                }
            }
            out[y * w + x] = on;
        }
    }
}

/* Separable Gaussian of the mask (edge-clamped), sigma PC_ALGO_REDEYE_SIGMA. */
static pc_status blur(const float *src, size_t w, size_t h, float sigma, float *out, pc_error *err)
{
    size_t n = w * h, x, y, nk = 0, ki;
    float k[65], sum = 0.0f, rc, *tmp;
    int32_t r, i;
    if (!isfinite(sigma) || sigma < 0.1f) {
        memcpy(out, src, n * sizeof *out);
        return PC_OK;
    }
    rc = ceilf(sigma * 3.0f);
    r = isfinite(rc) ? pc_f32_to_i32(pc_clampf(rc, 1.0f, 32.0f)) : 3;
    for (i = -r; i <= r; i++) {
        float wgt = expf((float)(-(i * i)) / (2.0f * sigma * sigma));
        if (!isfinite(wgt)) {
            continue;
        }
        k[nk++] = wgt;
        sum += wgt;
    }
    if (nk == 0 || !isfinite(sum) || sum <= 0.0f) {
        memcpy(out, src, n * sizeof *out);
        return PC_OK;
    }
    for (ki = 0; ki < nk; ki++) {
        k[ki] /= sum;
    }
    tmp = pc_malloc_array(n, sizeof *tmp);
    if (tmp == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float a = 0.0f;
            for (ki = 0; ki < nk; ki++) {
                size_t xx = (size_t)pc_clamp_i64((int64_t)x + (int64_t)ki - r, 0, (int64_t)w - 1);
                a += src[y * w + xx] * k[ki];
            }
            tmp[y * w + x] = a;
        }
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float a = 0.0f;
            for (ki = 0; ki < nk; ki++) {
                size_t yy = (size_t)pc_clamp_i64((int64_t)y + (int64_t)ki - r, 0, (int64_t)h - 1);
                a += tmp[yy * w + x] * k[ki];
            }
            out[y * w + x] = a;
        }
    }
    free(tmp);
    return PC_OK;
}

pc_status pc_algo_redeye_apply(float (*px)[4], size_t n, size_t w, size_t h, int32_t click_x, int32_t click_y,
                               int32_t origin_x, int32_t origin_y, float pupil_size, float darken,
                               uint32_t *changed, pc_error *err)
{
    const int32_t click[2] = {click_x, click_y}, origin[2] = {origin_x, origin_y};
    size_t np, x, y, seed, i;
    float radius, t_amt, *soft = NULL;
    bool *hard = NULL, *eroded = NULL;
    uint32_t count = 0;
    pc_status st = PC_OK;
    if (w == 0 || h == 0 || !pc_size_mul(w, h, &np) || n < np || px == NULL) {
        *changed = 0;
        return PC_OK;
    }
    radius = pc_algo_redeye_search_radius(pupil_size);
    t_amt = isfinite(darken) ? pc_clampf(pc_clampf(darken, 0.0f, 100.0f) / 100.0f, 0.0f, 1.0f) : 0.5f;
    hard = pc_calloc_array(np, sizeof *hard);
    eroded = pc_calloc_array(np, sizeof *eroded);
    soft = pc_malloc_array(np, 2 * sizeof *soft);
    if (hard == NULL || eroded == NULL || soft == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int32_t dx, dy;
            offset(x, y, origin, click, &dx, &dy);
            if (within_search(dx, dy, radius) && is_red_eye(px[y * w + x])) {
                hard[y * w + x] = true;
            }
        }
    }
    if (!nearest_seed(hard, w, h, origin, click, radius, &seed)) {
        goto cleanup;
    }
    if ((st = flood_keep(hard, w, h, seed, err)) != PC_OK) {
        goto cleanup;
    }
    erode(hard, w, h, eroded);
    for (i = 0; i < np; i++) {
        soft[i] = eroded[i] ? 1.0f : 0.0f;
    }
    if ((st = blur(soft, w, h, PC_ALGO_REDEYE_SIGMA, soft + np, err)) != PC_OK) {
        goto cleanup;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float *p = px[y * w + x], m, t, r0, g0, b0, neut, r1, scale, r2, g2, b2;
            int32_t dx, dy;
            offset(x, y, origin, click, &dx, &dy);
            if (!within_search(dx, dy, radius) || luma(p) >= 0.92f) {
                continue;
            }
            m = pc_clampf(soft[np + y * w + x], 0.0f, 1.0f);
            t = m * t_amt;
            if (t <= 1.0e-4f) {
                continue;
            }
            r0 = p[0], g0 = p[1], b0 = p[2];
            neut = (g0 + b0) * 0.5f;
            r1 = r0 + (neut - r0) * t;
            scale = 1.0f - 0.45f * t;
            r2 = r1 * scale, g2 = g0 * scale, b2 = b0 * scale;
            if (!isfinite(r2) || !isfinite(g2) || !isfinite(b2)) {
                continue;
            }
            if (fabsf(r2 - r0) < 1.0e-6f && fabsf(g2 - g0) < 1.0e-6f && fabsf(b2 - b0) < 1.0e-6f) {
                continue;
            }
            p[0] = r2, p[1] = g2, p[2] = b2;
            count = count < UINT32_MAX ? count + 1 : count;
        }
    }
cleanup:
    free(hard);
    free(eroded);
    free(soft);
    if (st == PC_OK) {
        *changed = count;
    }
    return st;
}
