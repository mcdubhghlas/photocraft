/* Smart selection core (port of crates/algo/src/segment/mod.rs): RGB images, samplers, the
 * contrast-sensitive grid cut, connected components, mask cleaning, and the full-resolution
 * finish (band re-cut, anti-aliasing, trimming). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/segment.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define SQRT_2_F 1.41421356237309504880f
#define TAU_F 6.28318530717958647692f

static float d2(const float a[3], const float b[3])
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

static pc_status count2(size_t w, size_t h, size_t elem, size_t *n, pc_error *err)
{
    size_t c;
    if (!pc_size_mul(w, h, &c) || (elem > 0 && c > PC_MAX_ALLOC / elem)) {
        return pc_fail(err, PC_ERR_LIMIT, "image too large");
    }
    *n = c;
    return PC_OK;
}

/* ---- RGB images ---- */

pc_status pc_algo_rgb_image_init(pc_algo_rgb_image *img, size_t w, size_t h, pc_error *err)
{
    size_t n;
    pc_status st = count2(w, h, sizeof(float[3]), &n, err);
    if (st != PC_OK) {
        return st;
    }
    img->w = w;
    img->h = h;
    img->px = NULL;
    if (n > 0) {
        img->px = pc_calloc_array(n, sizeof(float[3]));
        if (img->px == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for an image");
        }
    }
    return PC_OK;
}

void pc_algo_rgb_image_fini(pc_algo_rgb_image *img)
{
    if (img == NULL) {
        return;
    }
    free(img->px);
    img->px = NULL;
    img->w = 0;
    img->h = 0;
}

pc_status pc_algo_rgb_image_clone(const pc_algo_rgb_image *img, pc_algo_rgb_image *out, pc_error *err)
{
    pc_algo_rgb_image o;
    pc_status st = pc_algo_rgb_image_init(&o, img->w, img->h, err);
    if (st != PC_OK) {
        return st;
    }
    if (o.px != NULL) {
        memcpy(o.px, img->px, img->w * img->h * sizeof(float[3]));
    }
    *out = o;
    return PC_OK;
}

pc_status pc_algo_rgb_image_from_rgba(const float (*px)[4], size_t n, size_t w, size_t h,
                                      pc_algo_rgb_image *out, pc_error *err)
{
    pc_algo_rgb_image o;
    size_t i;
    pc_status st;
    if (n != w * h) {
        return pc_fail(err, PC_ERR_INVALID, "%zu pixels for a %zux%zu image", n, w, h);
    }
    st = pc_algo_rgb_image_init(&o, w, h, err);
    if (st != PC_OK) {
        return st;
    }
    /* Transparent pixels composite over mid grey, so transparency is a colour models separate. */
    for (i = 0; i < n && o.px != NULL; i++) {
        float a = pc_clampf(px[i][3], 0.0f, 1.0f);
        int c;
        for (c = 0; c < 3; c++) {
            o.px[i][c] = pc_clampf(px[i][c], 0.0f, 1.0f) * a + 0.5f * (1.0f - a);
        }
    }
    *out = o;
    return PC_OK;
}

pc_status pc_algo_rgb_image_downsample(const pc_algo_rgb_image *img, size_t step, pc_algo_rgb_image *out,
                                       pc_error *err)
{
    size_t ow, oh, ox, oy, x, y;
    pc_algo_rgb_image o;
    pc_status st;
    if (step <= 1) {
        return pc_algo_rgb_image_clone(img, out, err);
    }
    ow = img->w / step + (img->w % step != 0);
    oh = img->h / step + (img->h % step != 0);
    st = pc_algo_rgb_image_init(&o, ow, oh, err);
    if (st != PC_OK) {
        return st;
    }
    for (oy = 0; oy < oh && o.px != NULL && img->px != NULL; oy++) {
        for (ox = 0; ox < ow; ox++) {
            float acc[3] = {0.0f, 0.0f, 0.0f}, n = 0.0f;
            size_t ye = (oy + 1) * step < img->h ? (oy + 1) * step : img->h;
            size_t xe = (ox + 1) * step < img->w ? (ox + 1) * step : img->w;
            int c;
            for (y = oy * step; y < ye; y++) {
                for (x = ox * step; x < xe; x++) {
                    const float *p = img->px[y * img->w + x];
                    for (c = 0; c < 3; c++) {
                        acc[c] += p[c];
                    }
                    n += 1.0f;
                }
            }
            for (c = 0; c < 3; c++) {
                o.px[oy * ow + ox][c] = acc[c] / n;
            }
        }
    }
    *out = o;
    return PC_OK;
}

/* ---- samplers ---- */

pc_status pc_algo_sampler_rgb(const pc_algo_sampler *s, pc_rect r, pc_algo_rgb_image *out, pc_error *err)
{
    size_t w = pc_rect_width(r), h = pc_rect_height(r), n;
    float (*px)[4] = NULL;
    pc_status st = count2(w, h, sizeof(float[4]), &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n > 0) {
        px = pc_calloc_array(n, sizeof(float[4]));
        if (px == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory sampling an image");
        }
        st = s->rgba(s->ctx, r, px, n, err);
        if (st != PC_OK) {
            free(px);
            return st;
        }
    }
    st = pc_algo_rgb_image_from_rgba((const float (*)[4])px, n, w, h, out, err);
    free(px);
    return st;
}

typedef struct scaled_ctx {
    const pc_algo_sampler *s;
    pc_rect r;
    size_t step, rows_per, oh, ow;
    pc_algo_rgb_image *out;
} scaled_ctx;

static pc_status scaled_strips(void *ctx, size_t chunk, size_t begin, size_t end)
{
    scaled_ctx *c = ctx;
    size_t si;
    (void)chunk;
    for (si = begin; si < end; si++) {
        size_t oy0 = si * c->rows_per, oy1 = oy0 + c->rows_per < c->oh ? oy0 + c->rows_per : c->oh;
        int32_t y0 = c->r.y0 + (int32_t)(oy0 * c->step);
        int32_t y1 = c->r.y0 + (int32_t)(oy1 * c->step);
        pc_algo_rgb_image strip, ds;
        pc_status st;
        if (y1 > c->r.y1) {
            y1 = c->r.y1;
        }
        st = pc_algo_sampler_rgb(c->s, pc_rect_make(c->r.x0, y0, c->r.x1, y1), &strip, NULL);
        if (st != PC_OK) {
            return st;
        }
        st = pc_algo_rgb_image_downsample(&strip, c->step, &ds, NULL);
        pc_algo_rgb_image_fini(&strip);
        if (st != PC_OK) {
            return st;
        }
        if (ds.px != NULL && ds.w * ds.h <= (oy1 - oy0) * c->ow) {
            memcpy(c->out->px[oy0 * c->ow], ds.px, ds.w * ds.h * sizeof(float[3]));
        }
        pc_algo_rgb_image_fini(&ds);
    }
    return PC_OK;
}

pc_status pc_algo_sampler_rgb_scaled(const pc_algo_sampler *s, pc_rect r, size_t step, pc_algo_rgb_image *out,
                                     pc_error *err)
{
    size_t w = pc_rect_width(r), h = pc_rect_height(r);
    scaled_ctx c;
    pc_algo_rgb_image o;
    pc_status st;
    if (step <= 1) {
        return pc_algo_sampler_rgb(s, r, out, err);
    }
    c.s = s;
    c.r = r;
    c.step = step;
    c.ow = w / step + (w % step != 0);
    c.oh = h / step + (h % step != 0);
    c.rows_per = 64 / step + (64 % step != 0);
    if (c.rows_per < 1) {
        c.rows_per = 1;
    }
    st = pc_algo_rgb_image_init(&o, c.ow, c.oh, err);
    if (st != PC_OK) {
        return st;
    }
    c.out = &o;
    if (o.px != NULL) {
        st = pc_algo_par_for(c.oh / c.rows_per + (c.oh % c.rows_per != 0), 1, scaled_strips, &c);
        if (st != PC_OK) {
            pc_algo_rgb_image_fini(&o);
            return pc_fail(err, st, "sampling the image failed");
        }
    }
    *out = o;
    return PC_OK;
}

static pc_status surface_rgba(const void *ctx, pc_rect r, float (*out)[4], size_t n, pc_error *err)
{
    if (pc_rect_is_empty(r)) {
        return PC_OK;
    }
    return pc_surface_read_rgba_into(ctx, r, out, n, err);
}

pc_algo_sampler pc_algo_surface_sampler(const pc_surface *s)
{
    pc_algo_sampler smp;
    smp.rgba = surface_rgba;
    smp.ctx = s;
    return smp;
}

static pc_status image_rgba(const void *ctx, pc_rect r, float (*out)[4], size_t n, pc_error *err)
{
    const pc_algo_image_sampler *is = ctx;
    size_t i = 0;
    int32_t x, y;
    (void)err;
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1 && i < n; x++, i++) {
            int64_t ix = (int64_t)x - is->ox, iy = (int64_t)y - is->oy;
            if (ix < 0 || iy < 0 || (uint64_t)ix >= is->img->w || (uint64_t)iy >= is->img->h) {
                out[i][0] = out[i][1] = out[i][2] = out[i][3] = 0.0f;
            } else {
                const float *p = is->img->px[(size_t)iy * is->img->w + (size_t)ix];
                out[i][0] = p[0];
                out[i][1] = p[1];
                out[i][2] = p[2];
                out[i][3] = 1.0f;
            }
        }
    }
    return PC_OK;
}

pc_algo_sampler pc_algo_image_sampler_make(const pc_algo_image_sampler *holder)
{
    pc_algo_sampler smp;
    smp.rgba = image_rgba;
    smp.ctx = holder;
    return smp;
}

size_t pc_algo_segment_scale_for(pc_rect area, size_t max_px)
{
    double n = (double)pc_rect_width(area) * (double)pc_rect_height(area);
    size_t s = pc_f64_to_size(ceil(sqrt(n / (double)(max_px > 0 ? max_px : 1))));
    return s > 1 ? s : 1;
}

/* The 4 forward neighbour offsets of an 8-connected grid and their lengths. */
static const int32_t NB[4][2] = {{1, 0}, {0, 1}, {1, 1}, {-1, 1}};
static const float NB_LEN[4] = {1.0f, 1.0f, SQRT_2_F, SQRT_2_F};

float pc_algo_segment_contrast_beta(const pc_algo_rgb_image *img)
{
    int64_t w = (int64_t)img->w, h = (int64_t)img->h, x, y;
    double sum = 0.0;
    uint64_t n = 0;
    int k;
    if (img->px == NULL) {
        return 0.0f;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const float *a = img->px[y * w + x];
            for (k = 0; k < 4; k++) {
                int64_t nx = x + NB[k][0], ny = y + NB[k][1];
                if (nx >= 0 && nx < w && ny < h) {
                    sum += (double)d2(a, img->px[ny * w + nx]);
                    n++;
                }
            }
        }
    }
    if (n == 0 || sum <= 1e-12) {
        return 0.0f;
    }
    return pc_f64_to_f32((double)n / (2.0 * sum));
}

static pc_status fixed_link(pc_algo_maxflow *g, size_t node, uint8_t fixed, float wt)
{
    if (fixed == PC_ALGO_SEGMENT_HARD_FG) {
        /* Labelling node background cuts the link. */
        pc_algo_maxflow_add_tweights(g, node, wt, 0.0f);
    } else {
        pc_algo_maxflow_add_tweights(g, node, 0.0f, wt);
    }
    return PC_OK;
}

pc_status pc_algo_segment_grid_cut(const pc_algo_rgb_image *img, const float *cost_fg, const float *cost_bg,
                                   const uint8_t *fixed, float gamma, float beta, bool *out, pc_error *err)
{
    int64_t w = (int64_t)img->w, h = (int64_t)img->h, x, y;
    size_t n = img->w * img->h, i, count = 0, edges;
    uint32_t *idx;
    pc_algo_maxflow *g = NULL;
    double flow;
    pc_status st = PC_OK;
    for (i = 0; i < n; i++) {
        out[i] = fixed[i] == PC_ALGO_SEGMENT_HARD_FG;
    }
    idx = pc_malloc_array(n ? n : 1, sizeof(uint32_t));
    if (idx == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a graph cut");
    }
    for (i = 0; i < n; i++) {
        if (fixed[i] == PC_ALGO_SEGMENT_FREE) {
            if (count >= (size_t)UINT32_MAX - 3) {
                free(idx);
                return pc_fail(err, PC_ERR_LIMIT, "graph cut too large");
            }
            idx[i] = (uint32_t)count++;
        } else {
            idx[i] = UINT32_MAX;
        }
    }
    if (count == 0 || img->px == NULL) {
        free(idx);
        return PC_OK;
    }
    if (!pc_size_mul(count, 4, &edges)) {
        free(idx);
        return pc_fail(err, PC_ERR_LIMIT, "graph cut too large");
    }
    st = pc_algo_maxflow_new(count, edges, &g, err);
    if (st != PC_OK) {
        free(idx);
        return st;
    }
    for (i = 0; i < n; i++) {
        if (idx[i] != UINT32_MAX) {
            pc_algo_maxflow_add_tweights(g, idx[i], cost_bg[i], cost_fg[i]);
        }
    }
    for (y = 0; y < h && st == PC_OK; y++) {
        for (x = 0; x < w && st == PC_OK; x++) {
            size_t ii = (size_t)(y * w + x);
            const float *a = img->px[ii];
            int k;
            for (k = 0; k < 4; k++) {
                int64_t nx = x + NB[k][0], ny = y + NB[k][1];
                size_t j;
                float wt;
                bool fi, fj;
                if (nx < 0 || nx >= w || ny >= h) {
                    continue;
                }
                j = (size_t)(ny * w + nx);
                wt = gamma * expf(-beta * d2(a, img->px[j])) / NB_LEN[k];
                fi = idx[ii] != UINT32_MAX;
                fj = idx[j] != UINT32_MAX;
                if (fi && fj) {
                    st = pc_algo_maxflow_add_edge(g, idx[ii], idx[j], wt, wt, err);
                    if (st != PC_OK) {
                        break;
                    }
                } else if (fi) {
                    /* Neighbour fixed: disagreeing with it costs wt. */
                    (void)fixed_link(g, idx[ii], fixed[j], wt);
                } else if (fj) {
                    (void)fixed_link(g, idx[j], fixed[ii], wt);
                }
            }
        }
    }
    if (st == PC_OK) {
        st = pc_algo_maxflow_run(g, &flow, err);
    }
    if (st == PC_OK) {
        for (i = 0; i < n; i++) {
            if (idx[i] != UINT32_MAX) {
                out[i] = pc_algo_maxflow_in_source(g, idx[i]);
            }
        }
    }
    pc_algo_maxflow_free(g);
    free(idx);
    return st;
}

typedef struct costs_ctx {
    const pc_algo_rgb_image *img;
    const pc_algo_gmm *fg, *bg;
    float *cf, *cb;
} costs_ctx;

static pc_status costs_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    costs_ctx *c = ctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        c->cf[i] = pc_algo_gmm_neg_log(c->fg, c->img->px[i]);
        c->cb[i] = pc_algo_gmm_neg_log(c->bg, c->img->px[i]);
    }
    return PC_OK;
}

pc_status pc_algo_segment_data_costs(const pc_algo_rgb_image *img, const pc_algo_gmm *fg,
                                     const pc_algo_gmm *bg, float *cost_fg, float *cost_bg, pc_error *err)
{
    costs_ctx c;
    (void)err;
    c.img = img;
    c.fg = fg;
    c.bg = bg;
    c.cf = cost_fg;
    c.cb = cost_bg;
    return pc_algo_par_for(img->w * img->h, 4096, costs_chunk, &c);
}

/* Shared by the 8- and 4-connected labellings. */
static pc_status label_components(const bool *mask, size_t w, size_t h, bool eight, uint32_t **labels_out,
                                  size_t **sizes_out, size_t *nsizes, pc_error *err)
{
    size_t n = w * h, s, *stack, top, *sizes = NULL, scap = 0, ns = 1;
    uint32_t *lab = pc_calloc_array(n ? n : 1, sizeof(uint32_t));
    stack = pc_malloc_array(n ? n : 1, sizeof(size_t));
    sizes = pc_grow_array(NULL, &scap, 16, sizeof(size_t));
    if (lab == NULL || stack == NULL || sizes == NULL) {
        free(lab);
        free(stack);
        free(sizes);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory labelling components");
    }
    sizes[0] = 0;
    for (s = 0; s < n; s++) {
        uint32_t l;
        if (!mask[s] || lab[s] != 0) {
            continue;
        }
        if (ns >= (size_t)UINT32_MAX) {
            free(lab);
            free(stack);
            free(sizes);
            return pc_fail(err, PC_ERR_LIMIT, "too many components");
        }
        l = (uint32_t)ns;
        if (ns == scap) {
            void *g = pc_grow_array(sizes, &scap, ns + 1, sizeof(size_t));
            if (g == NULL) {
                free(lab);
                free(stack);
                free(sizes);
                return pc_fail(err, PC_ERR_NOMEM, "out of memory labelling components");
            }
            sizes = g;
        }
        sizes[ns++] = 0;
        lab[s] = l;
        top = 0;
        stack[top++] = s; /* each pixel is pushed once (labelled before the push) */
        while (top > 0) {
            size_t i = stack[--top];
            sizes[l]++;
            if (eight) {
                int64_t x = (int64_t)(i % w), y = (int64_t)(i / w), dx, dy;
                for (dy = -1; dy <= 1; dy++) {
                    for (dx = -1; dx <= 1; dx++) {
                        int64_t nx = x + dx, ny = y + dy;
                        size_t j;
                        if (nx < 0 || ny < 0 || nx >= (int64_t)w || ny >= (int64_t)h) {
                            continue;
                        }
                        j = (size_t)ny * w + (size_t)nx;
                        if (mask[j] && lab[j] == 0) {
                            lab[j] = l;
                            stack[top++] = j;
                        }
                    }
                }
            } else {
                size_t x = i % w, y = i / w, nb[4], cnt = 0, k;
                if (x > 0) {
                    nb[cnt++] = i - 1;
                }
                if (x + 1 < w) {
                    nb[cnt++] = i + 1;
                }
                if (y > 0) {
                    nb[cnt++] = i - w;
                }
                if (y + 1 < h) {
                    nb[cnt++] = i + w;
                }
                for (k = 0; k < cnt; k++) {
                    size_t j = nb[k];
                    if (mask[j] && lab[j] == 0) {
                        lab[j] = l;
                        stack[top++] = j;
                    }
                }
            }
        }
    }
    free(stack);
    *labels_out = lab;
    *sizes_out = sizes;
    *nsizes = ns;
    return PC_OK;
}

pc_status pc_algo_segment_components(const bool *mask, size_t w, size_t h, uint32_t **labels, size_t **sizes,
                                     size_t *nsizes, pc_error *err)
{
    return label_components(mask, w, h, true, labels, sizes, nsizes, err);
}

pc_status pc_algo_segment_keep_seeded(const bool *mask, const bool *seeds, size_t w, size_t h, bool *out,
                                      pc_error *err)
{
    uint32_t *lab;
    size_t *sizes, ns, i, n = w * h;
    bool *keep;
    pc_status st = label_components(mask, w, h, true, &lab, &sizes, &ns, err);
    if (st != PC_OK) {
        return st;
    }
    keep = pc_calloc_array(ns, sizeof(bool));
    if (keep == NULL) {
        free(lab);
        free(sizes);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory labelling components");
    }
    for (i = 0; i < n; i++) {
        if (seeds[i] && lab[i] != 0) {
            keep[lab[i]] = true;
        }
    }
    for (i = 0; i < n; i++) {
        out[i] = keep[lab[i]] && lab[i] != 0;
    }
    free(keep);
    free(lab);
    free(sizes);
    return PC_OK;
}

pc_status pc_algo_segment_clean_mask(const bool *mask, size_t w, size_t h, float frac, float hole_frac,
                                     bool *out, pc_error *err)
{
    uint32_t *lab, *hl;
    size_t *sizes, *hs, ns, nhs, i, n = w * h, largest = 0, min, area = 0, max_hole, x, y;
    bool *inv, *touches;
    pc_status st = label_components(mask, w, h, true, &lab, &sizes, &ns, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 1; i < ns; i++) {
        largest = sizes[i] > largest ? sizes[i] : largest;
    }
    if (largest == 0) {
        memset(out, 0, n * sizeof(bool));
        free(lab);
        free(sizes);
        return PC_OK;
    }
    min = pc_f32_to_size(ceilf((float)largest * frac));
    for (i = 0; i < n; i++) {
        out[i] = lab[i] != 0 && sizes[lab[i]] >= min;
        area += out[i];
    }
    free(lab);
    free(sizes);
    max_hole = pc_f32_to_size((float)area * hole_frac);
    inv = pc_malloc_array(n, sizeof(bool));
    if (inv == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory cleaning a mask");
    }
    for (i = 0; i < n; i++) {
        inv[i] = !out[i];
    }
    st = label_components(inv, w, h, false, &hl, &hs, &nhs, err);
    free(inv);
    if (st != PC_OK) {
        return st;
    }
    touches = pc_calloc_array(nhs, sizeof(bool));
    if (touches == NULL) {
        free(hl);
        free(hs);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory cleaning a mask");
    }
    for (x = 0; x < w; x++) {
        touches[hl[x]] = true;
        touches[hl[(h - 1) * w + x]] = true;
    }
    for (y = 0; y < h; y++) {
        touches[hl[y * w]] = true;
        touches[hl[y * w + w - 1]] = true;
    }
    for (i = 0; i < n; i++) {
        if (hl[i] != 0 && !touches[hl[i]] && hs[hl[i]] <= max_hole) {
            out[i] = true;
        }
    }
    free(touches);
    free(hl);
    free(hs);
    return PC_OK;
}

typedef struct aa8_ctx {
    const uint8_t *src;
    uint8_t *mask;
    size_t w, h;
} aa8_ctx;

static pc_status aa8_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    aa8_ctx *c = ctx;
    size_t y, x;
    (void)chunk;
    for (y = begin; y < end; y++) {
        for (x = 0; x < c->w; x++) {
            uint8_t v = c->src[y * c->w + x];
            uint32_t sum = 0;
            bool edge = false;
            int dy, dx;
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int64_t nx = (int64_t)x + dx, ny = (int64_t)y + dy;
                    uint8_t nv = (nx < 0 || ny < 0 || nx >= (int64_t)c->w || ny >= (int64_t)c->h)
                                     ? v
                                     : c->src[(size_t)ny * c->w + (size_t)nx];
                    edge |= nv != v;
                    sum += nv;
                }
            }
            if (edge) {
                float avg = (float)sum / (9.0f * 255.0f);
                float f = v > 127 ? 0.5f + 0.5f * avg : 0.5f * avg;
                c->mask[y * c->w + x] = pc_f32_to_u8(roundf(f * 255.0f));
            }
        }
    }
    return PC_OK;
}

pc_status pc_algo_segment_antialias_u8(uint8_t *mask, size_t w, size_t h, pc_error *err)
{
    aa8_ctx c;
    size_t n = w * h;
    uint8_t *src;
    pc_status st;
    if (n == 0) {
        return PC_OK;
    }
    src = pc_malloc(n);
    if (src == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory anti-aliasing a mask");
    }
    memcpy(src, mask, n);
    c.src = src;
    c.mask = mask;
    c.w = w;
    c.h = h;
    st = pc_algo_par_for(h, 1, aa8_rows, &c);
    free(src);
    return st;
}

pc_status pc_algo_segment_trim_region(pc_algo_region *r, pc_algo_region *out, pc_error *err)
{
    size_t w = pc_rect_width(r->bbox), h, x0 = SIZE_MAX, y0 = SIZE_MAX, x1 = 0, y1 = 0, y, nw;
    uint8_t *m;
    pc_rect b = r->bbox;
    if (w == 0) {
        pc_algo_region_fini(r);
        out->bbox = PC_RECT_EMPTY;
        out->mask = NULL;
        out->len = 0;
        return PC_OK;
    }
    h = r->len / w;
    for (y = 0; y < h; y++) {
        const uint8_t *row = r->mask + y * w;
        size_t a, bb;
        for (a = 0; a < w && row[a] == 0; a++) {
        }
        if (a == w) {
            continue;
        }
        for (bb = w; bb > 0 && row[bb - 1] == 0; bb--) {
        }
        x0 = x0 < a ? x0 : a;
        x1 = x1 > bb ? x1 : bb;
        y0 = y0 < y ? y0 : y;
        y1 = y + 1;
    }
    if (x0 == SIZE_MAX) {
        pc_algo_region_fini(r);
        out->bbox = PC_RECT_EMPTY;
        out->mask = NULL;
        out->len = 0;
        return PC_OK;
    }
    if (x0 == 0 && y0 == 0 && x1 == w && y1 == h) {
        *out = *r;
        r->mask = NULL;
        r->len = 0;
        r->bbox = PC_RECT_EMPTY;
        return PC_OK;
    }
    nw = x1 - x0;
    m = pc_malloc(nw * (y1 - y0));
    if (m == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory trimming a region");
    }
    for (y = y0; y < y1; y++) {
        memcpy(m + (y - y0) * nw, r->mask + y * w + x0, nw);
    }
    pc_algo_region_fini(r);
    out->bbox = pc_rect_make(b.x0 + (int32_t)x0, b.y0 + (int32_t)y0, b.x0 + (int32_t)x1, b.y0 + (int32_t)y1);
    out->mask = m;
    out->len = nw * (y1 - y0);
    return PC_OK;
}

/* ---- finish_region / refine_band ---- */

#define BAND_T 192
#define BAND_HALO 12

typedef struct band_ctx {
    const pc_algo_sampler *s;
    pc_rect window;
    size_t step, lw, lh, w;
    const bool *low, *band;
    const pc_algo_gmm *fg, *bg;
    const pc_rect *tiles;
    uint8_t *mask;
} band_ctx;

static size_t block_of(const band_ctx *c, int32_t x, int32_t y)
{
    size_t by = (size_t)(y - c->window.y0) / c->step, bx = (size_t)(x - c->window.x0) / c->step;
    by = by < c->lh - 1 ? by : c->lh - 1;
    bx = bx < c->lw - 1 ? bx : c->lw - 1;
    return by * c->lw + bx;
}

static pc_status band_tile(band_ctx *c, pc_rect t)
{
    pc_rect r = pc_rect_intersect(pc_rect_inflate(t, BAND_HALO), c->window);
    pc_algo_rgb_image img;
    size_t rw = pc_rect_width(r), rh = pc_rect_height(r), n = rw * rh, xx, yy;
    size_t cw = pc_rect_width(t), ch = pc_rect_height(t);
    uint8_t *fixed = NULL;
    float *cfg = NULL, *cbg = NULL;
    bool *cut = NULL;
    pc_status st;
    if (n == 0) {
        return PC_OK;
    }
    st = pc_algo_sampler_rgb(c->s, r, &img, NULL);
    if (st != PC_OK) {
        return st;
    }
    fixed = pc_calloc_array(n ? n : 1, 1);
    cfg = pc_calloc_array(n ? n : 1, sizeof(float));
    cbg = pc_calloc_array(n ? n : 1, sizeof(float));
    cut = pc_calloc_array(n ? n : 1, sizeof(bool));
    if (fixed == NULL || cfg == NULL || cbg == NULL || cut == NULL) {
        st = PC_ERR_NOMEM;
        goto done;
    }
    for (yy = 0; yy < rh; yy++) {
        for (xx = 0; xx < rw; xx++) {
            size_t i = yy * rw + xx, b = block_of(c, r.x0 + (int32_t)xx, r.y0 + (int32_t)yy);
            bool lab = c->low[b];
            const float *p;
            if (!c->band[b]) {
                fixed[i] = lab ? PC_ALGO_SEGMENT_HARD_FG : PC_ALGO_SEGMENT_HARD_BG;
                continue;
            }
            p = img.px[i];
            /* Colour likelihoods plus a weak prior towards the working-resolution label. */
            cfg[i] = pc_algo_gmm_neg_log(c->fg, p) + (lab ? 0.0f : 0.4f);
            cbg[i] = pc_algo_gmm_neg_log(c->bg, p) + (lab ? 0.4f : 0.0f);
        }
    }
    st = pc_algo_segment_grid_cut(&img, cfg, cbg, fixed, 50.0f, pc_algo_segment_contrast_beta(&img), cut,
                                  NULL);
    if (st != PC_OK) {
        goto done;
    }
    for (yy = 0; yy < ch; yy++) {
        size_t sy = (size_t)(t.y0 - r.y0) + yy, sx = (size_t)(t.x0 - r.x0);
        size_t o = ((size_t)(t.y0 - c->window.y0) + yy) * c->w + (size_t)(t.x0 - c->window.x0);
        for (xx = 0; xx < cw; xx++) {
            c->mask[o + xx] = cut[sy * rw + sx + xx] ? 255 : 0;
        }
    }
done:
    pc_algo_rgb_image_fini(&img);
    free(fixed);
    free(cfg);
    free(cbg);
    free(cut);
    return st;
}

static pc_status band_tiles(void *ctx, size_t chunk, size_t begin, size_t end)
{
    band_ctx *c = ctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        pc_status st = band_tile(c, c->tiles[i]);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

/* Full-resolution graph cut restricted to the band blocks. */
static pc_status refine_band(band_ctx *c, pc_error *err)
{
    pc_rect *tiles = NULL;
    size_t nt = 0, tcap = 0;
    int32_t tx, ty;
    pc_status st;
    for (ty = c->window.y0; ty < c->window.y1; ty = pc_i32_sat_add(ty, BAND_T)) {
        for (tx = c->window.x0; tx < c->window.x1; tx = pc_i32_sat_add(tx, BAND_T)) {
            pc_rect t =
                pc_rect_make(tx, ty, pc_i32_sat_add(tx, BAND_T) < c->window.x1 ? tx + BAND_T : c->window.x1,
                             pc_i32_sat_add(ty, BAND_T) < c->window.y1 ? ty + BAND_T : c->window.y1);
            size_t bx0 = (size_t)(t.x0 - c->window.x0) / c->step,
                   by0 = (size_t)(t.y0 - c->window.y0) / c->step;
            size_t bx1 = (size_t)(t.x1 - 1 - c->window.x0) / c->step,
                   by1 = (size_t)(t.y1 - 1 - c->window.y0) / c->step;
            size_t bx, by;
            bool any = false;
            bx1 = bx1 < c->lw - 1 ? bx1 : c->lw - 1;
            by1 = by1 < c->lh - 1 ? by1 : c->lh - 1;
            /* Any band block overlapping the tile? */
            for (by = by0; by <= by1 && !any; by++) {
                for (bx = bx0; bx <= bx1; bx++) {
                    if (c->band[by * c->lw + bx]) {
                        any = true;
                        break;
                    }
                }
            }
            if (any) {
                if (nt == tcap) {
                    void *g = pc_grow_array(tiles, &tcap, nt + 1, sizeof *tiles);
                    if (g == NULL) {
                        free(tiles);
                        return pc_fail(err, PC_ERR_NOMEM, "out of memory refining a selection edge");
                    }
                    tiles = g;
                }
                tiles[nt++] = t;
            }
            if (tx > INT32_MAX - BAND_T) {
                break;
            }
        }
        if (ty > INT32_MAX - BAND_T) {
            break;
        }
    }
    c->tiles = tiles;
    st = pc_algo_par_for(nt, 1, band_tiles, c);
    free(tiles);
    if (st != PC_OK) {
        return pc_fail(err, st, "refining the selection edge failed");
    }
    return PC_OK;
}

pc_status pc_algo_segment_finish_region(const pc_algo_sampler *s, pc_rect window, size_t step,
                                        const bool *low, size_t lw, size_t lh, const pc_algo_gmm *fg,
                                        const pc_algo_gmm *bg, pc_algo_region *out, pc_error *err)
{
    size_t w = pc_rect_width(window), h = pc_rect_height(window), n, i, x, y;
    uint8_t *mask;
    pc_algo_region reg;
    pc_status st = count2(w, h, 1, &n, err);
    if (st != PC_OK) {
        return st;
    }
    mask = pc_calloc_array(n ? n : 1, 1);
    if (mask == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a selection");
    }
    if (step <= 1) {
        size_t m = n < lw * lh ? n : lw * lh;
        for (i = 0; i < m; i++) {
            mask[i] = low[i] ? 255 : 0;
        }
    } else if (lw > 0 && lh > 0) {
        /* Band blocks: working pixels with a differently labelled pixel within one block. */
        bool *band = pc_calloc_array(lw * lh, sizeof(bool));
        if (band == NULL) {
            free(mask);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for a selection");
        }
        for (y = 0; y < lh; y++) {
            for (x = 0; x < lw; x++) {
                bool v = low[y * lw + x], found = false;
                int dy, dx;
                for (dy = -1; dy <= 1 && !found; dy++) {
                    for (dx = -1; dx <= 1; dx++) {
                        int64_t nx = (int64_t)x + dx, ny = (int64_t)y + dy;
                        if (nx >= 0 && ny >= 0 && (size_t)nx < lw && (size_t)ny < lh &&
                            low[(size_t)ny * lw + (size_t)nx] != v) {
                            found = true;
                            break;
                        }
                    }
                }
                band[y * lw + x] = found;
            }
        }
        for (y = 0; y < h; y++) {
            size_t ly = y / step < lh - 1 ? y / step : lh - 1;
            for (x = 0; x < w; x++) {
                size_t lx = x / step < lw - 1 ? x / step : lw - 1;
                mask[y * w + x] = low[ly * lw + lx] ? 255 : 0;
            }
        }
        if (fg != NULL && bg != NULL) {
            band_ctx c;
            c.s = s;
            c.window = window;
            c.step = step;
            c.lw = lw;
            c.lh = lh;
            c.w = w;
            c.low = low;
            c.band = band;
            c.fg = fg;
            c.bg = bg;
            c.tiles = NULL;
            c.mask = mask;
            st = refine_band(&c, err);
        }
        free(band);
        if (st != PC_OK) {
            free(mask);
            return st;
        }
    }
    st = pc_algo_segment_antialias_u8(mask, w, h, err);
    if (st != PC_OK) {
        free(mask);
        return st;
    }
    reg.bbox = window;
    reg.mask = mask;
    reg.len = n;
    return pc_algo_segment_trim_region(&reg, out, err);
}

/* ---- RNG, subsampling ---- */

pc_algo_segment_rng pc_algo_segment_rng_new(uint64_t seed)
{
    pc_algo_segment_rng r;
    r.s = (seed * 0x9E3779B97F4A7C15ull) | 1u;
    return r;
}

uint64_t pc_algo_segment_rng_next(pc_algo_segment_rng *r)
{
    uint64_t x = r->s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    r->s = x;
    return x;
}

float pc_algo_segment_rng_f32(pc_algo_segment_rng *r)
{
    return (float)(pc_algo_segment_rng_next(r) >> 40) / 16777216.0f;
}

float pc_algo_segment_rng_normal(pc_algo_segment_rng *r)
{
    float u = pc_rs_maxf_k(pc_algo_segment_rng_f32(r), 1e-7f);
    float v = pc_algo_segment_rng_f32(r);
    return sqrtf(-2.0f * logf(u)) * cosf(TAU_F * v);
}

pc_status pc_algo_segment_subsample(const float (*v)[3], size_t n, size_t max, float (**out)[3], size_t *nout,
                                    pc_error *err)
{
    size_t k, m, i, j = 0;
    float (*o)[3];
    if (n <= max) {
        k = 1;
        m = n;
    } else if (max == 0) {
        k = 1;
        m = 0;
        n = 0;
    } else {
        k = n / max + (n % max != 0);
        m = (n + k - 1) / k;
    }
    o = pc_malloc_array(m ? m : 1, sizeof *o);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory subsampling");
    }
    for (i = 0; i < n; i += k) {
        o[j][0] = v[i][0];
        o[j][1] = v[i][1];
        o[j][2] = v[i][2];
        j++;
    }
    *out = o;
    *nout = j;
    return PC_OK;
}
