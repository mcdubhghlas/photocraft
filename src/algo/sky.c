/* Select > Sky (port of crates/algo/src/segment/sky.rs): a colour prior times a smoothness term at
 * working resolution, region growing from the top rows, sky seen through gaps added by a colour
 * model of the grown sky, then a guided-filter refinement at full resolution where the mask is
 * mixed (He, Sun & Tang, ECCV 2010). */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/matting.h"
#include "algo/segment.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define SKY_T 256

pc_algo_sky_params pc_algo_sky_params_default(void)
{
    pc_algo_sky_params p = {50.0f, 50.0f};
    return p;
}

static float smooth(float e0, float e1, float x)
{
    float t = pc_clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float luma(const float p[3])
{
    return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
}

float pc_algo_sky_colour_prior(const float p[3])
{
    float r = p[0], g = p[1], b = p[2], y = luma(p);
    float mx = pc_rs_maxf(pc_rs_maxf(r, g), b), mn = pc_rs_minf(pc_rs_minf(r, g), b);
    float sat = mx > 1e-4f ? (mx - mn) / mx : 0.0f;
    /* Blue sky: blue dominates red clearly and isn't far below green (excludes foliage). */
    float blue = smooth(0.02f, 0.12f, b - r) * smooth(-0.12f, 0.0f, b - g) * smooth(0.18f, 0.4f, y);
    /* Overcast / clouds / haze: bright and nearly neutral. */
    float white = smooth(0.5f, 0.75f, y) * (1.0f - smooth(0.12f, 0.3f, sat));
    /* Sunset glow: bright and warm, kept weaker (warm walls are common too). */
    float warm = 0.6f * smooth(0.55f, 0.8f, y) * smooth(0.0f, 0.1f, r - b) *
                 (1.0f - smooth(0.05f, 0.25f, g - b - 0.2f));
    return pc_rs_maxf(pc_rs_maxf(blue, white), warm);
}

pc_status pc_algo_sky_likelihood(const pc_algo_rgb_image *img, float **out, pc_error *err)
{
    size_t w = img->w, h = img->h, n = w * h, i;
    float *y = pc_malloc_array(n ? n : 1, sizeof(float)), *grad = pc_malloc_array(n ? n : 1, sizeof(float));
    float *tex = NULL;
    int64_t xx, yy;
    pc_status st;
    if (y == NULL || grad == NULL) {
        free(y);
        free(grad);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for sky detection");
    }
    for (i = 0; i < n; i++) {
        y[i] = luma(img->px[i]);
    }
#define AT(ax, ay)                                                                                           \
    y[(size_t)pc_clamp_i64((ay), 0, (int64_t)h - 1) * w + (size_t)pc_clamp_i64((ax), 0, (int64_t)w - 1)]
    for (yy = 0; yy < (int64_t)h; yy++) {
        for (xx = 0; xx < (int64_t)w; xx++) {
            float gx = (AT(xx + 1, yy - 1) + 2.0f * AT(xx + 1, yy) + AT(xx + 1, yy + 1)) -
                       (AT(xx - 1, yy - 1) + 2.0f * AT(xx - 1, yy) + AT(xx - 1, yy + 1));
            float gy = (AT(xx - 1, yy + 1) + 2.0f * AT(xx, yy + 1) + AT(xx + 1, yy + 1)) -
                       (AT(xx - 1, yy - 1) + 2.0f * AT(xx, yy - 1) + AT(xx + 1, yy - 1));
            grad[(size_t)yy * w + (size_t)xx] = sqrtf(gx * gx + gy * gy) / 8.0f;
        }
    }
#undef AT
    st = pc_algo_matting_box_mean(grad, w, h, 2, &tex, err);
    free(y);
    free(grad);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < n; i++) {
        tex[i] = pc_algo_sky_colour_prior(img->px[i]) * (1.0f - smooth(0.015f, 0.07f, tex[i]));
    }
    *out = tex;
    return PC_OK;
}

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

pc_status pc_algo_sky_mask_low(const pc_algo_rgb_image *img, float threshold, bool **out, bool *some,
                               pc_error *err)
{
    size_t w = img->w, h = img->h, n = w * h, i, top, sp = 0, ns = 0;
    float *like = NULL, t;
    bool *mask = NULL, *clean = NULL, any = false;
    size_t *stack = NULL;
    float (*samples)[3] = NULL;
    pc_status st;
    *some = false;
    if (w < 2 || h < 2) {
        return PC_OK;
    }
    st = pc_algo_sky_likelihood(img, &like, err);
    if (st != PC_OK) {
        return st;
    }
    t = 0.12f + 0.6f * (pc_clampf(threshold, 0.0f, 100.0f) / 100.0f);
    mask = pc_calloc_array(n, sizeof(bool));
    stack = pc_malloc_array(n, sizeof(size_t));
    if (mask == NULL || stack == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for sky detection");
        goto done;
    }
    /* Seeds: likely sky in the top rows. */
    top = h / 40 > 1 ? h / 40 : 1;
    for (i = 0; i < top * w; i++) {
        if (like[i] > t) {
            mask[i] = true;
            stack[sp++] = i;
        }
    }
    if (sp * 20 < top * w) {
        goto done; /* less than 5 % of the top edge looks like sky */
    }
    /* Region growing with a gradual-change constraint (each pixel is pushed once). */
    while (sp > 0) {
        size_t c = stack[--sp], x = c % w, y = c / w, nb[4], cnt = 0, k;
        if (x > 0) {
            nb[cnt++] = c - 1;
        }
        if (x + 1 < w) {
            nb[cnt++] = c + 1;
        }
        if (y > 0) {
            nb[cnt++] = c - w;
        }
        if (y + 1 < h) {
            nb[cnt++] = c + w;
        }
        for (k = 0; k < cnt; k++) {
            size_t j = nb[k];
            const float *a = img->px[c], *b = img->px[j];
            float d0, d1, dd2, d;
            if (mask[j] || like[j] <= t) {
                continue;
            }
            d0 = a[0] - b[0];
            d1 = a[1] - b[1];
            dd2 = a[2] - b[2];
            d = sqrtf(d0 * d0 + d1 * d1 + dd2 * dd2);
            if (d < 0.12f) {
                mask[j] = true;
                stack[sp++] = j;
            }
        }
    }
    /* Sky seen through gaps: likely-sky pixels that fit the grown sky's colours. */
    samples = pc_malloc_array(n, sizeof *samples);
    if (samples == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for sky detection");
        goto done;
    }
    for (i = 0; i < n; i++) {
        if (mask[i]) {
            memcpy(samples[ns++], img->px[i], sizeof samples[0]);
        }
    }
    if (ns >= 16) {
        float (*sub)[3] = NULL;
        size_t nsub = 0;
        pc_algo_gmm g;
        bool ok = false;
        if ((st = pc_algo_segment_subsample((const float (*)[3])samples, ns, 4000, &sub, &nsub, err)) !=
            PC_OK) {
            goto done;
        }
        st = pc_algo_gmm_fit((const float (*)[3])sub, nsub, 3, 1e-4f, &g, &ok, err);
        free(sub);
        if (st != PC_OK) {
            goto done;
        }
        if (ok) {
            float *nl;
            float cut;
            if ((st = pc_algo_segment_subsample((const float (*)[3])samples, ns, 2000, &sub, &nsub, err)) !=
                PC_OK) {
                pc_algo_gmm_fini(&g);
                goto done;
            }
            nl = pc_malloc_array(nsub ? nsub : 1, sizeof(float));
            if (nl == NULL) {
                free(sub);
                pc_algo_gmm_fini(&g);
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory for sky detection");
                goto done;
            }
            for (i = 0; i < nsub; i++) {
                nl[i] = pc_algo_gmm_neg_log(&g, sub[i]);
            }
            free(sub);
            qsort(nl, nsub, sizeof(float), total_cmp);
            cut = nl[nsub * 95 / 100 < nsub - 1 ? nsub * 95 / 100 : nsub - 1];
            free(nl);
            for (i = 0; i < n; i++) {
                if (!mask[i] && like[i] > t + 0.1f && pc_algo_gmm_neg_log(&g, img->px[i]) <= cut) {
                    mask[i] = true;
                }
            }
            pc_algo_gmm_fini(&g);
        }
    }
    clean = pc_malloc_array(n, sizeof(bool));
    if (clean == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for sky detection");
        goto done;
    }
    st = pc_algo_segment_clean_mask(mask, w, h, 0.002f, 0.01f, clean, err);
    if (st != PC_OK) {
        goto done;
    }
    for (i = 0; i < n && !any; i++) {
        any = clean[i];
    }
    if (any) {
        *out = clean;
        clean = NULL;
        *some = true;
    }
done:
    free(like);
    free(mask);
    free(stack);
    free(samples);
    free(clean);
    return st;
}

typedef struct sky_ctx {
    const pc_algo_sampler *s;
    pc_rect canvas;
    size_t step, lw, lh, w, r;
    const float *lowf;
    float eps, k;
    const pc_rect *tiles;
    uint8_t *mask;
} sky_ctx;

/* Bilinear upsample of the working mask (pixel centres). */
static float coarse(const sky_ctx *c, int32_t x, int32_t y)
{
    float st = (float)c->step;
    float fx = ((float)(x - c->canvas.x0) + 0.5f) / st - 0.5f,
          fy = ((float)(y - c->canvas.y0) + 0.5f) / st - 0.5f;
    float x0 = floorf(fx), y0 = floorf(fy), ax = fx - x0, ay = fy - y0, top, bot;
#define G(xx, yy)                                                                                            \
    c->lowf[pc_f32_to_size(pc_clampf((yy), 0.0f, (float)c->lh - 1.0f)) * c->lw +                             \
            pc_f32_to_size(pc_clampf((xx), 0.0f, (float)c->lw - 1.0f))]
    top = G(x0, y0) * (1.0f - ax) + G(x0 + 1.0f, y0) * ax;
    bot = G(x0, y0 + 1.0f) * (1.0f - ax) + G(x0 + 1.0f, y0 + 1.0f) * ax;
#undef G
    return top * (1.0f - ay) + bot * ay;
}

static pc_status sky_tile(sky_ctx *c, pc_rect t)
{
    pc_rect win = pc_rect_intersect(pc_rect_inflate(t, (int32_t)c->r), c->canvas);
    size_t ww = pc_rect_width(win), wh = pc_rect_height(win), n = ww * wh, i;
    size_t tw = pc_rect_width(t);
    float *pm = pc_malloc_array(n ? n : 1, sizeof(float)), lo = FLT_MAX, hi = -FLT_MAX, *guide = NULL,
          *q = NULL;
    pc_algo_rgb_image img;
    int32_t x, y;
    pc_status st;
    if (pm == NULL) {
        return PC_ERR_NOMEM;
    }
    for (i = 0; i < n; i++) {
        pm[i] = coarse(c, win.x0 + (int32_t)(i % ww), win.y0 + (int32_t)(i / ww));
        lo = pc_rs_minf(lo, pm[i]);
        hi = pc_rs_maxf(hi, pm[i]);
    }
    if (hi <= 0.0f || lo >= 1.0f) {
        uint8_t v = lo >= 1.0f ? 255 : 0;
        for (y = t.y0; y < t.y1; y++) {
            memset(c->mask + (size_t)(y - c->canvas.y0) * c->w + (size_t)(t.x0 - c->canvas.x0), v, tw);
        }
        free(pm);
        return PC_OK;
    }
    st = pc_algo_sampler_rgb(c->s, win, &img, NULL);
    if (st != PC_OK) {
        free(pm);
        return st;
    }
    guide = pc_malloc_array(n ? n : 1, sizeof(float));
    if (guide == NULL) {
        pc_algo_rgb_image_fini(&img);
        free(pm);
        return PC_ERR_NOMEM;
    }
    for (i = 0; i < n; i++) {
        guide[i] = luma(img.px[i]);
    }
    pc_algo_rgb_image_fini(&img);
    st = pc_algo_matting_guided_filter_gray(guide, pm, ww, wh, c->r, c->eps, &q, NULL);
    free(guide);
    free(pm);
    if (st != PC_OK) {
        return st;
    }
    for (y = t.y0; y < t.y1; y++) {
        uint8_t *o = c->mask + (size_t)(y - c->canvas.y0) * c->w + (size_t)(t.x0 - c->canvas.x0);
        for (x = t.x0; x < t.x1; x++) {
            float v = q[(size_t)(y - win.y0) * ww + (size_t)(x - win.x0)];
            o[x - t.x0] = pc_f32_to_u8(roundf(smooth(0.5f - c->k, 0.5f + c->k, v) * 255.0f));
        }
    }
    free(q);
    return PC_OK;
}

static pc_status sky_tiles(void *ctx, size_t chunk, size_t begin, size_t end)
{
    sky_ctx *c = ctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        pc_status st = sky_tile(c, c->tiles[i]);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

pc_status pc_algo_sky_select(const pc_algo_sampler *s, pc_rect canvas, pc_algo_sky_params p,
                             pc_algo_region *out, pc_error *err)
{
    sky_ctx c;
    pc_algo_rgb_image img = {0, 0, NULL};
    bool *low = NULL, some = false;
    float *lowf = NULL, soft;
    pc_rect *tiles = NULL;
    size_t nt = 0, tcap = 0, i, n;
    int64_t tx, ty;
    pc_algo_region reg;
    pc_status st;
    out->bbox = PC_RECT_EMPTY;
    out->mask = NULL;
    out->len = 0;
    if (pc_rect_width(canvas) < 4 || pc_rect_height(canvas) < 4) {
        return PC_OK;
    }
    c.step = pc_algo_segment_scale_for(canvas, PC_ALGO_SKY_WORK_PX);
    st = pc_algo_sampler_rgb_scaled(s, canvas, c.step, &img, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_algo_sky_mask_low(&img, p.threshold, &low, &some, err);
    if (st != PC_OK || !some) {
        pc_algo_rgb_image_fini(&img);
        return st;
    }
    c.lw = img.w;
    c.lh = img.h;
    pc_algo_rgb_image_fini(&img);
    lowf = pc_malloc_array(c.lw * c.lh, sizeof(float));
    if (lowf == NULL) {
        free(low);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for sky selection");
    }
    for (i = 0; i < c.lw * c.lh; i++) {
        lowf[i] = low[i] ? 1.0f : 0.0f;
    }
    free(low);
    soft = pc_clampf(p.softness, 0.0f, 100.0f) / 100.0f;
    c.r = pc_f32_to_size(ceilf((float)c.step * (1.0f + 2.0f * soft)));
    c.r = c.r > 3 ? c.r : 3;
    c.eps = 1e-4f + 4e-3f * soft;
    /* Contrast after filtering: soft 0 -> narrow ramp, 1 -> the filter output as is. */
    c.k = 0.08f + 0.42f * soft;
    c.s = s;
    c.canvas = canvas;
    c.lowf = lowf;
    c.w = pc_rect_width(canvas);
    if (!pc_size_mul(c.w, pc_rect_height(canvas), &n) || n > PC_MAX_ALLOC || c.r > (size_t)INT32_MAX / 2) {
        free(lowf);
        return pc_fail(err, PC_ERR_LIMIT, "canvas too large for sky selection");
    }
    for (ty = canvas.y0; ty < canvas.y1; ty += SKY_T) {
        for (tx = canvas.x0; tx < canvas.x1; tx += SKY_T) {
            if (nt == tcap) {
                void *g = pc_grow_array(tiles, &tcap, nt + 1, sizeof *tiles);
                if (g == NULL) {
                    free(tiles);
                    free(lowf);
                    return pc_fail(err, PC_ERR_NOMEM, "out of memory for sky selection");
                }
                tiles = g;
            }
            tiles[nt++] = pc_rect_make((int32_t)tx, (int32_t)ty,
                                       (int32_t)(tx + SKY_T < canvas.x1 ? tx + SKY_T : canvas.x1),
                                       (int32_t)(ty + SKY_T < canvas.y1 ? ty + SKY_T : canvas.y1));
        }
    }
    c.mask = pc_calloc_array(n, 1);
    if (c.mask == NULL) {
        free(tiles);
        free(lowf);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for sky selection");
    }
    c.tiles = tiles;
    st = pc_algo_par_for(nt, 1, sky_tiles, &c);
    free(tiles);
    free(lowf);
    if (st != PC_OK) {
        free(c.mask);
        return pc_fail(err, st, "sky selection failed");
    }
    reg.bbox = canvas;
    reg.mask = c.mask;
    reg.len = n;
    return pc_algo_segment_trim_region(&reg, out, err);
}
