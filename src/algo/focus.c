/* Focus Area, heuristic (port of crates/algo/src/segment/focus.rs): local Laplacian energy (Nayar &
 * Nakagawa, "Shape from Focus", PAMI 1994) normalised by its 99th percentile, thresholded by the
 * In-Focus Range, smoothed with a guided filter so it follows edges, cleaned, and upsampled with
 * a soft edge. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/matting.h"
#include "algo/segment.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* f32::total_cmp as a qsort comparator. */
static int total_cmp(const void *a, const void *b)
{
    int32_t x, y;
    memcpy(&x, a, 4);
    memcpy(&y, b, 4);
    x ^= (int32_t)(((uint32_t)(x >> 31)) >> 1);
    y ^= (int32_t)(((uint32_t)(y >> 31)) >> 1);
    return x < y ? -1 : (x > y ? 1 : 0);
}

pc_status pc_algo_focus_sharpness(const pc_algo_rgb_image *img, float noise, float **out, pc_error *err)
{
    size_t w = img->w, h = img->h, n = w * h, x, yy, r, k;
    float *y, *lap, *e = NULL, *sorted, p99, floor_v;
    pc_status st;
    if (n == 0) {
        *out = NULL;
        return PC_OK;
    }
    y = pc_malloc_array(n, sizeof(float));
    lap = pc_malloc_array(n, sizeof(float));
    if (y == NULL || lap == NULL) {
        free(y);
        free(lap);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory measuring focus");
    }
    for (x = 0; x < n; x++) {
        y[x] = 0.299f * img->px[x][0] + 0.587f * img->px[x][1] + 0.114f * img->px[x][2];
    }
    for (yy = 0; yy < h; yy++) {
        for (x = 0; x < w; x++) {
            float c = y[yy * w + x];
            float l =
                fabsf(2.0f * c - y[yy * w + (x > 0 ? x - 1 : 0)] - y[yy * w + (x + 1 < w ? x + 1 : w - 1)]) +
                fabsf(2.0f * c - y[(yy > 0 ? yy - 1 : 0) * w + x] - y[(yy + 1 < h ? yy + 1 : h - 1) * w + x]);
            lap[yy * w + x] = l;
        }
    }
    r = (w < h ? w : h) / 60;
    r = r > 3 ? r : 3;
    st = pc_algo_matting_box_mean(lap, w, h, r, &e, err);
    free(y);
    free(lap);
    if (st != PC_OK) {
        return st;
    }
    sorted = pc_malloc_array(n, sizeof(float));
    if (sorted == NULL) {
        free(e);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory measuring focus");
    }
    memcpy(sorted, e, n * sizeof(float));
    k = pc_f32_to_size((float)n * 0.99f);
    k = k < n - 1 ? k : n - 1;
    qsort(sorted, n, sizeof(float), total_cmp);
    p99 = sorted[k];
    free(sorted);
    floor_v = pc_clampf(noise, 0.0f, 1.0f) * 0.5f;
    for (x = 0; x < n; x++) {
        e[x] = pc_rs_maxf_k(pc_rs_minf_k(e[x] / pc_rs_maxf_k(p99, 1e-6f), 1.0f) - floor_v, 0.0f) /
               pc_rs_maxf_k(1.0f - floor_v, 1e-3f);
    }
    *out = e;
    return PC_OK;
}

typedef struct up_ctx {
    const float *field;
    size_t w, h, fw;
    float st;
    uint8_t *out;
} up_ctx;

static pc_status up_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    up_ctx *c = ctx;
    size_t yy, xx;
    (void)chunk;
    for (yy = begin; yy < end; yy++) {
        float fy = pc_clampf(((float)yy + 0.5f) / c->st - 0.5f, 0.0f, (float)(c->h - 1));
        size_t y0 = pc_f32_to_size(floorf(fy)), y1;
        float ty = fy - truncf(fy);
        y1 = y0 + 1 < c->h - 1 ? y0 + 1 : c->h - 1;
        for (xx = 0; xx < c->fw; xx++) {
            float fx = pc_clampf(((float)xx + 0.5f) / c->st - 0.5f, 0.0f, (float)(c->w - 1));
            size_t x0 = pc_f32_to_size(floorf(fx)), x1;
            float tx = fx - truncf(fx), top, bot, v;
            x1 = x0 + 1 < c->w - 1 ? x0 + 1 : c->w - 1;
            top = c->field[y0 * c->w + x0] * (1.0f - tx) + c->field[y0 * c->w + x1] * tx;
            bot = c->field[y1 * c->w + x0] * (1.0f - tx) + c->field[y1 * c->w + x1] * tx;
            v = top * (1.0f - ty) + bot * ty;
            c->out[yy * c->fw + xx] =
                pc_f32_to_u8(roundf(pc_clampf((v - 0.5f) * 3.0f + 0.5f, 0.0f, 1.0f) * 255.0f));
        }
    }
    return PC_OK;
}

pc_status pc_algo_focus_area(const pc_algo_sampler *s, pc_rect canvas, float range, float noise,
                             pc_algo_region *out, pc_error *err)
{
    size_t step, w, h, n, i, r, fn;
    pc_algo_rgb_image img = {0, 0, NULL};
    float *sh = NULL, *bin = NULL, *smooth = NULL, *field = NULL, t, rr;
    bool *mask = NULL, *clean = NULL, any = false;
    up_ctx uc;
    pc_algo_region reg;
    pc_status st;
    out->bbox = PC_RECT_EMPTY;
    out->mask = NULL;
    out->len = 0;
    if (pc_rect_width(canvas) < 4 || pc_rect_height(canvas) < 4) {
        return PC_OK;
    }
    step = pc_algo_segment_scale_for(canvas, PC_ALGO_FOCUS_WORK_PX);
    st = pc_algo_sampler_rgb_scaled(s, canvas, step, &img, err);
    if (st != PC_OK) {
        return st;
    }
    w = img.w;
    h = img.h;
    n = w * h;
    st = pc_algo_focus_sharpness(&img, noise, &sh, err);
    if (st != PC_OK) {
        goto done;
    }
    rr = 1.0f - pc_clampf(range, 0.0f, 1.0f);
    t = 0.05f + 0.9f * (rr * rr);
    bin = pc_malloc_array(n ? n : 1, sizeof(float));
    mask = pc_malloc_array(n ? n : 1, sizeof(bool));
    clean = pc_malloc_array(n ? n : 1, sizeof(bool));
    if (bin == NULL || mask == NULL || clean == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Focus Area");
        goto done;
    }
    for (i = 0; i < n; i++) {
        bin[i] = sh[i] >= t ? 1.0f : 0.0f;
    }
    r = (w < h ? w : h) / 50;
    r = r > 2 ? r : 2;
    st = pc_algo_matting_guided_filter_color(&img, bin, r, 1e-2f, &smooth, err);
    if (st != PC_OK) {
        goto done;
    }
    for (i = 0; i < n; i++) {
        mask[i] = smooth[i] >= 0.5f;
    }
    st = pc_algo_segment_clean_mask(mask, w, h, 0.05f, 0.02f, clean, err);
    if (st != PC_OK) {
        goto done;
    }
    for (i = 0; i < n && !any; i++) {
        any = clean[i];
    }
    if (!any) {
        goto done;
    }
    /* Soft upsampling: bilinear field, steepened to about a working pixel of transition. */
    field = bin; /* reuse */
    for (i = 0; i < n; i++) {
        field[i] = clean[i] ? pc_clampf(smooth[i], 0.5f, 1.0f) : pc_clampf(smooth[i], 0.0f, 0.5f);
    }
    uc.field = field;
    uc.w = w;
    uc.h = h;
    uc.fw = pc_rect_width(canvas);
    uc.st = (float)step;
    if (!pc_size_mul(uc.fw, pc_rect_height(canvas), &fn) || fn > PC_MAX_ALLOC) {
        st = pc_fail(err, PC_ERR_LIMIT, "canvas too large for Focus Area");
        goto done;
    }
    uc.out = pc_malloc(fn);
    if (uc.out == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Focus Area");
        goto done;
    }
    (void)pc_algo_par_for(pc_rect_height(canvas), 1, up_rows, &uc);
    reg.bbox = canvas;
    reg.mask = uc.out;
    reg.len = fn;
    st = pc_algo_segment_trim_region(&reg, out, err);
done:
    pc_algo_rgb_image_fini(&img);
    free(sh);
    free(bin);
    free(smooth);
    free(mask);
    free(clean);
    return st;
}
