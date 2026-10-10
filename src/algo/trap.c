/* Image > Trap (port of crates/algo/src/trap.rs). */
#include "algo/trap.h"

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

static pc_status dil_rows(void *vc, size_t chunk, size_t begin, size_t end)
{
    dil_ctx *d = vc;
    size_t y, x, xx;
    (void)chunk;
    for (y = begin; y < end; y++) {
        for (x = 0; x < d->w; x++) {
            size_t lo = x > d->r ? x - d->r : 0, hi = x + d->r < d->w - 1 ? x + d->r : d->w - 1;
            float m = 0.0f;
            for (xx = lo; xx <= hi; xx++) {
                m = pc_rs_maxf(m, d->src[(y * d->w + xx) * d->ch + d->c]);
            }
            d->tmp[y * d->w + x] = m;
        }
    }
    return PC_OK;
}

static pc_status dil_cols(void *vc, size_t chunk, size_t begin, size_t end)
{
    dil_ctx *d = vc;
    size_t y, x, yy;
    (void)chunk;
    for (y = begin; y < end; y++) {
        size_t lo = y > d->r ? y - d->r : 0, hi = y + d->r < d->h - 1 ? y + d->r : d->h - 1;
        for (x = 0; x < d->w; x++) {
            float m = 0.0f;
            for (yy = lo; yy <= hi; yy++) {
                m = pc_rs_maxf(m, d->tmp[yy * d->w + x]);
            }
            d->out[y * d->w + x] = m;
        }
    }
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
        (void)pc_algo_par_for(h, 1, dil_cols, &d);
    }
    for (i = 0; i < npx; i++) {
        float *p = px + i * ch;
        /* Printed where any ink is present. */
        if (!(p[0] > 1e-4f || p[1] > 1e-4f || p[2] > 1e-4f || p[3] > 1e-4f)) {
            continue;
        }
        for (c = 0; c < 4; c++) {
            p[c] = pc_rs_maxf(p[c], dil[c][i]);
        }
    }
done:
    free(tmp);
    for (c = 0; c < 4; c++) {
        free(dil[c]);
    }
    return st;
}
