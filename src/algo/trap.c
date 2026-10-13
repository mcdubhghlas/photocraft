/* Image > Trap (port of crates/algo/src/trap.rs). */
#include "algo/trap.h"

#include <math.h>
#include <stdlib.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

typedef struct dil_ctx {
    const float *src;
    float *tmp, *out;
    size_t w, h, ch, c, r;
} dil_ctx;

/* The direct clipped-window maximum of len samples (base[i * stride]), floored at zero. */
static void max_direct(float *out, size_t len, size_t r, const float *base, size_t stride)
{
    size_t x, i;
    for (x = 0; x < len; x++) {
        size_t lo = x > r ? x - r : 0, hi = x + r < len - 1 ? x + r : len - 1;
        float m = 0.0f;
        for (i = lo; i <= hi; i++) {
            m = pc_rs_maxf(m, base[i * stride]);
        }
        out[x] = m;
    }
}

/* Prefix/suffix block maxima with clipped edge windows and the original zero floor: one comparison
 * per sample in each direction, independent of the trap width. `suffix` holds len floats (NULL:
 * the direct path). */
static void max_line(float *out, size_t len, size_t radius, const float *base, size_t stride, float *suffix)
{
    size_t r, block, i, x;
    float acc = 0.0f;
    bool negative_zero = false;
    if (len == 0) {
        return;
    }
    r = radius < len - 1 ? radius : len - 1;
    if (r <= 2 || suffix == NULL) {
        max_direct(out, len, r, base, stride);
        return;
    }
    block = 2 * r + 1;
    for (i = 0; i < len; i++) {
        float value = base[i * stride];
        if (i % block == 0) {
            acc = 0.0f;
        }
        negative_zero |= value == 0.0f && signbit(value);
        acc = pc_rs_maxf(acc, value);
        out[i] = acc;
    }
    /* f32::max may choose either sign of equal zeros depending on operand order: keep the original
     * left-to-right fold for these uncommon lines. */
    if (negative_zero) {
        max_direct(out, len, r, base, stride);
        return;
    }
    acc = 0.0f;
    for (i = len; i-- > 0;) {
        if (i == len - 1 || (i + 1) % block == 0) {
            acc = 0.0f;
        }
        acc = pc_rs_maxf(acc, base[i * stride]);
        suffix[i] = acc;
    }
    for (x = 0; x < len; x++) {
        size_t lo = x > r ? x - r : 0, hi = x + r < len - 1 ? x + r : len - 1;
        float value;
        if (lo == 0) {
            value = out[hi];
        } else if (hi == len - 1 && lo / block == hi / block) {
            value = suffix[lo];
        } else {
            value = pc_rs_maxf(suffix[lo], out[hi]);
        }
        out[x] = value;
    }
}

/* Horizontal pass: tmp row y = the clipped row maxima of channel c. */
static pc_status dil_rows(void *vc, size_t chunk, size_t begin, size_t end)
{
    dil_ctx *d = vc;
    float *suffix = d->r > 2 ? pc_malloc_array(d->w, sizeof *suffix) : NULL;
    size_t y;
    (void)chunk;
    for (y = begin; y < end; y++) {
        max_line(d->tmp + y * d->w, d->w, d->r, d->src + y * d->w * d->ch + d->c, d->ch, suffix);
    }
    free(suffix);
    return PC_OK;
}

/* Vertical pass, transposed: out row x = the clipped column maxima of tmp column x. */
static pc_status dil_cols(void *vc, size_t chunk, size_t begin, size_t end)
{
    dil_ctx *d = vc;
    float *suffix = d->r > 2 ? pc_malloc_array(d->h, sizeof *suffix) : NULL;
    size_t x;
    (void)chunk;
    for (x = begin; x < end; x++) {
        max_line(d->out + x * d->h, d->h, d->r, d->tmp + x, d->w, suffix);
    }
    free(suffix);
    return PC_OK;
}

pc_status pc_algo_trap(float *px, size_t len, size_t w, size_t h, size_t ch, size_t width, pc_error *err)
{
    size_t npx, need, c, i;
    float *dil[4] = {NULL, NULL, NULL, NULL}, *tmp;
    dil_ctx d;
    pc_status st = PC_OK;
    if (width == 0 || w == 0 || h == 0 || ch < 4) {
        return PC_OK;
    }
    if (!pc_size_mul3(w, h, ch, &need) || need != len || need > PC_MAX_ALLOC / sizeof(float)) {
        return pc_fail(err, PC_ERR_INVALID, "trap: buffer of %zu floats does not hold %zux%zux%zu", len, w, h,
                       ch);
    }
    npx = w * h;
    /* A window wider than the image covers all of it (the reference wraps only near SIZE_MAX). */
    if (width > w && width > h) {
        width = w > h ? w : h;
    }
    tmp = pc_malloc_array(npx, sizeof *tmp);
    for (c = 0; c < 4; c++) {
        dil[c] = pc_malloc_array(npx, sizeof(float));
    }
    if (tmp == NULL || dil[0] == NULL || dil[1] == NULL || dil[2] == NULL || dil[3] == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Trap");
        goto done;
    }
    for (c = 0; c < 4; c++) {
        d.src = px, d.tmp = tmp, d.out = dil[c], d.w = w, d.h = h, d.ch = ch, d.c = c, d.r = width;
        (void)pc_algo_par_for(h, 1, dil_rows, &d);
        (void)pc_algo_par_for(w, 1, dil_cols, &d);
    }
    for (i = 0; i < npx; i++) {
        float *p = px + i * ch;
        /* The dilations are transposed (x * h + y). */
        size_t t = (i % w) * h + i / w;
        /* Printed where any ink is present. */
        if (!(p[0] > 1e-4f || p[1] > 1e-4f || p[2] > 1e-4f || p[3] > 1e-4f)) {
            continue;
        }
        for (c = 0; c < 4; c++) {
            p[c] = pc_rs_maxf(p[c], dil[c][t]);
        }
    }
done:
    free(tmp);
    for (c = 0; c < 4; c++) {
        free(dil[c]);
    }
    return st;
}
