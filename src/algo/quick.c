/* Quick Selection brush (port of crates/algo/src/segment/quick.rs).
 *
 * Stroked pixels are hard foreground and seed a GMM; a geodesic distance over a barrier map
 * (raster-scan geodesic transform, Toivanen 1996; Criminisi et al., GeoS, ECCV 2008) separates
 * what is reachable without crossing an edge from what lies behind edges, which is sampled for the
 * background model; a contrast-sensitive min cut snaps the result to edges and only components
 * touching the stroke are kept (in the spirit of Liu, Sun & Shum, "Paint Selection", 2009). */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/segment.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Covariance ridge for the (often tiny) stroke sample: ~6 levels of standard deviation. */
#define QREG 6e-4f
/* Geodesic distance (colour units above noise) that counts as "behind an edge". */
#define EDGE 0.06f
/* Neighbour colour steps below this never count as edges. */
#define MIN_STEP 0.03f
/* Data-term weight of the geodesic distance (nats per EDGE). */
#define KAPPA 2.0f
#define GAMMA 50.0f
#define SQRT_2_F 1.41421356237309504880f

static float d2(const float a[3], const float b[3])
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

pc_status pc_algo_quick_broad_model(pc_algo_gmm *out, pc_error *err)
{
    float corners[8][3];
    bool some = false;
    int i;
    pc_status st;
    for (i = 0; i < 8; i++) {
        corners[i][0] = (float)(i & 1);
        corners[i][1] = (float)((i >> 1) & 1);
        corners[i][2] = (float)((i >> 2) & 1);
    }
    /* Eight fixed, distinct samples: the fit cannot fail short of memory. */
    st = pc_algo_gmm_fit((const float (*)[3])corners, 8, 1, 0.0f, out, &some, err);
    if (st == PC_OK && !some) {
        return pc_fail(err, PC_ERR_INVALID, "broad colour model failed");
    }
    return st;
}

void pc_algo_quick_stroke_mask(const float (*pts)[2], size_t n, float radius, size_t w, size_t h, bool *m)
{
    size_t nseg = n == 1 ? 1 : (n >= 2 ? n - 1 : 0), k, i;
    memset(m, 0, w * h * sizeof(bool));
    for (k = 0; k < nseg; k++) {
        const float *a = pts[k], *b = n == 1 ? pts[0] : pts[k + 1];
        size_t x0 = pc_f32_to_size(pc_rs_maxf_k(floorf(pc_rs_minf(a[0], b[0]) - radius), 0.0f));
        size_t y0 = pc_f32_to_size(pc_rs_maxf_k(floorf(pc_rs_minf(a[1], b[1]) - radius), 0.0f));
        size_t x1 = pc_f32_to_size(pc_rs_maxf_k(ceilf(pc_rs_maxf(a[0], b[0]) + radius), 0.0f));
        size_t y1 = pc_f32_to_size(pc_rs_maxf_k(ceilf(pc_rs_maxf(a[1], b[1]) + radius), 0.0f));
        float dx = b[0] - a[0], dy = b[1] - a[1], len2 = dx * dx + dy * dy;
        size_t x, y;
        x1 = x1 < w ? x1 : w;
        y1 = y1 < h ? y1 : h;
        for (y = y0; y < y1; y++) {
            for (x = x0; x < x1; x++) {
                float px = (float)x + 0.5f, py = (float)y + 0.5f, t, qx, qy;
                t = len2 > 0.0f ? pc_clampf(((px - a[0]) * dx + (py - a[1]) * dy) / len2, 0.0f, 1.0f) : 0.0f;
                qx = a[0] + t * dx - px;
                qy = a[1] + t * dy - py;
                if (qx * qx + qy * qy <= radius * radius) {
                    m[y * w + x] = true;
                }
            }
        }
    }
    for (i = 0; i < n; i++) {
        float x = pts[i][0], y = pts[i][1];
        if (x >= 0.0f && y >= 0.0f && pc_f32_to_size(x) < w && pc_f32_to_size(y) < h) {
            m[pc_f32_to_size(y) * w + pc_f32_to_size(x)] = true;
        }
    }
}

static pc_status blur3(const pc_algo_rgb_image *img, pc_algo_rgb_image *out, pc_error *err)
{
    size_t w = img->w, h = img->h, x, y;
    pc_algo_rgb_image tmp, o;
    int k;
    pc_status st = pc_algo_rgb_image_clone(img, &tmp, err);
    if (st != PC_OK) {
        return st;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const float *l = img->px[y * w + (x > 0 ? x - 1 : 0)],
                        *r = img->px[y * w + (x + 1 < w ? x + 1 : w - 1)];
            const float *c = img->px[y * w + x];
            for (k = 0; k < 3; k++) {
                tmp.px[y * w + x][k] = (l[k] + c[k] + r[k]) / 3.0f;
            }
        }
    }
    st = pc_algo_rgb_image_clone(&tmp, &o, err);
    if (st != PC_OK) {
        pc_algo_rgb_image_fini(&tmp);
        return st;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const float *u = tmp.px[(y > 0 ? y - 1 : 0) * w + x],
                        *d = tmp.px[(y + 1 < h ? y + 1 : h - 1) * w + x];
            const float *c = tmp.px[y * w + x];
            for (k = 0; k < 3; k++) {
                o.px[y * w + x][k] = (u[k] + c[k] + d[k]) / 3.0f;
            }
        }
    }
    pc_algo_rgb_image_fini(&tmp);
    *out = o;
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

pc_status pc_algo_quick_geodesic(const pc_algo_rgb_image *img, const bool *seeds, float **out, pc_error *err)
{
    size_t w = img->w, h = img->h, n = w * h, i, x, y, nd = 0, pass;
    pc_algo_rgb_image sm1, sm;
    bool *near = NULL;
    float *g = NULL, *diffs = NULL, *e = NULL, *d = NULL, tau = 0.0f, thr;
    pc_status st = blur3(img, &sm1, err);
    if (st != PC_OK) {
        return st;
    }
    st = blur3(&sm1, &sm, err);
    pc_algo_rgb_image_fini(&sm1);
    if (st != PC_OK) {
        return st;
    }
    near = pc_calloc_array(n ? n : 1, sizeof(bool));
    g = pc_malloc_array(n ? n : 1, sizeof(float));
    diffs = pc_malloc_array(n ? n : 1, sizeof(float));
    e = pc_malloc_array(n ? n : 1, sizeof(float));
    d = pc_malloc_array(n ? n : 1, sizeof(float));
    if (near == NULL || g == NULL || diffs == NULL || e == NULL || d == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a geodesic distance");
        goto done;
    }
    /* Texture level of what the user painted: median local gradient over the stroke grown by two
     * pixels. Gradual colour changes and texture at that level cost nothing. */
    for (i = 0; i < n; i++) {
        size_t yy, sx, ex;
        if (!seeds[i]) {
            continue;
        }
        x = i % w;
        y = i / w;
        sx = x > 2 ? x - 2 : 0;
        ex = x + 3 < w ? x + 3 : w;
        for (yy = y > 2 ? y - 2 : 0; yy < (y + 3 < h ? y + 3 : h); yy++) {
            memset(near + yy * w + sx, 1, (ex - sx) * sizeof(bool));
        }
    }
    /* Colour gradient magnitude (central differences) of the smoothed image. */
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const float *l = sm.px[y * w + (x > 0 ? x - 1 : 0)],
                        *r = sm.px[y * w + (x + 1 < w ? x + 1 : w - 1)];
            const float *u = sm.px[(y > 0 ? y - 1 : 0) * w + x],
                        *dn = sm.px[(y + 1 < h ? y + 1 : h - 1) * w + x];
            g[y * w + x] = sqrtf((d2(l, r) + d2(u, dn)) * 0.25f);
        }
    }
    for (i = 0; i < n; i++) {
        if (near[i]) {
            diffs[nd++] = g[i];
        }
    }
    if (nd > 0) {
        qsort(diffs, nd, sizeof(float), total_cmp);
        tau = diffs[nd / 2];
    }
    thr = pc_rs_maxf_k(2.0f * tau, MIN_STEP);
    /* Crossing pixels whose gradient exceeds the texture level costs the excess: a barrier map,
     * so an edge costs about its contrast whatever direction the path crosses it. */
    for (i = 0; i < n; i++) {
        e[i] = pc_rs_maxf_k(g[i] - thr, 0.0f);
        d[i] = seeds[i] ? 0.0f : INFINITY;
    }
#define COST(a, b, len) ((len) * (0.5f * (e[a] + e[b]) + 1e-6f))
    for (pass = 0; pass < 2; pass++) {
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                size_t ii = y * w + x;
                float v = d[ii];
                if (x > 0) {
                    v = pc_rs_minf(v, d[ii - 1] + COST(ii, ii - 1, 1.0f));
                }
                if (y > 0) {
                    v = pc_rs_minf(v, d[ii - w] + COST(ii, ii - w, 1.0f));
                    if (x > 0) {
                        v = pc_rs_minf(v, d[ii - w - 1] + COST(ii, ii - w - 1, SQRT_2_F));
                    }
                    if (x + 1 < w) {
                        v = pc_rs_minf(v, d[ii - w + 1] + COST(ii, ii - w + 1, SQRT_2_F));
                    }
                }
                d[ii] = v;
            }
        }
        for (y = h; y-- > 0;) {
            for (x = w; x-- > 0;) {
                size_t ii = y * w + x;
                float v = d[ii];
                if (x + 1 < w) {
                    v = pc_rs_minf(v, d[ii + 1] + COST(ii, ii + 1, 1.0f));
                }
                if (y + 1 < h) {
                    v = pc_rs_minf(v, d[ii + w] + COST(ii, ii + w, 1.0f));
                    if (x + 1 < w) {
                        v = pc_rs_minf(v, d[ii + w + 1] + COST(ii, ii + w + 1, SQRT_2_F));
                    }
                    if (x > 0) {
                        v = pc_rs_minf(v, d[ii + w - 1] + COST(ii, ii + w - 1, SQRT_2_F));
                    }
                }
                d[ii] = v;
            }
        }
    }
#undef COST
    *out = d;
    d = NULL;
done:
    pc_algo_rgb_image_fini(&sm);
    free(near);
    free(g);
    free(diffs);
    free(e);
    free(d);
    return st;
}

typedef struct qcost_ctx {
    const pc_algo_rgb_image *img;
    const float *geo;
    const pc_algo_gmm *fg, *bg;
    float *cf, *cb;
} qcost_ctx;

static pc_status qcost_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    qcost_ctx *c = ctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        const float *p = c->img->px[i];
        c->cf[i] = pc_algo_gmm_neg_log(c->fg, p) + KAPPA * pc_rs_minf_k(c->geo[i] / EDGE, 4.0f);
        c->cb[i] =
            c->bg != NULL ? -pc_f64_to_f32(log(0.85 * exp(pc_algo_gmm_log_prob(c->bg, p)) + 0.15)) : 0.0f;
    }
    return PC_OK;
}

pc_status pc_algo_quick_segment(const pc_algo_rgb_image *img, const bool *seeds, pc_algo_gmm *fg_out,
                                pc_algo_gmm *bg_out, bool *low, pc_error *err)
{
    size_t n = img->w * img->h, i, nf = 0, nb = 0, nsf = 0, nsb = 0, kf;
    float *geo = NULL, (*fs)[3] = NULL, (*bs)[3] = NULL, (*sf)[3] = NULL, (*sb)[3] = NULL, *cf = NULL,
          *cb = NULL;
    uint8_t *fixed = NULL;
    bool *cut = NULL, okf = false, okb = false;
    pc_algo_gmm fg = {NULL, 0, 0.0f}, bg = {NULL, 0, 0.0f};
    qcost_ctx qc;
    pc_status st = pc_algo_quick_geodesic(img, seeds, &geo, err);
    if (st != PC_OK) {
        return st;
    }
    fs = pc_malloc_array(n ? n : 1, sizeof *fs);
    bs = pc_malloc_array(n ? n : 1, sizeof *bs);
    cf = pc_malloc_array(n ? n : 1, sizeof(float));
    cb = pc_malloc_array(n ? n : 1, sizeof(float));
    fixed = pc_malloc_array(n ? n : 1, 1);
    cut = pc_malloc_array(n ? n : 1, sizeof(bool));
    if (fs == NULL || bs == NULL || cf == NULL || cb == NULL || fixed == NULL || cut == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Quick Selection");
        goto done;
    }
    for (i = 0; i < n; i++) {
        if (seeds[i]) {
            memcpy(fs[nf++], img->px[i], sizeof fs[0]);
        }
        if (geo[i] > EDGE) {
            memcpy(bs[nb++], img->px[i], sizeof bs[0]);
        }
    }
    if ((st = pc_algo_segment_subsample((const float (*)[3])fs, nf, 20000, &sf, &nsf, err)) != PC_OK ||
        (st = pc_algo_segment_subsample((const float (*)[3])bs, nb, 20000, &sb, &nsb, err)) != PC_OK) {
        goto done;
    }
    kf = nsf / 40;
    kf = kf < 1 ? 1 : (kf > 4 ? 4 : kf);
    st = pc_algo_gmm_fit((const float (*)[3])sf, nsf, kf, QREG, &fg, &okf, err);
    if (st == PC_OK && !okf) {
        st = pc_algo_quick_broad_model(&fg, err);
    }
    if (st == PC_OK && nsb >= 30) {
        st = pc_algo_gmm_fit((const float (*)[3])sb, nsb, 6, QREG, &bg, &okb, err);
    }
    if (st != PC_OK) {
        goto done;
    }
    qc.img = img;
    qc.geo = geo;
    qc.fg = &fg;
    qc.bg = okb ? &bg : NULL;
    qc.cf = cf;
    qc.cb = cb;
    (void)pc_algo_par_for(n, 4096, qcost_chunk, &qc);
    for (i = 0; i < n; i++) {
        fixed[i] = seeds[i] ? PC_ALGO_SEGMENT_HARD_FG : PC_ALGO_SEGMENT_FREE;
    }
    st = pc_algo_segment_grid_cut(img, cf, cb, fixed, GAMMA, pc_algo_segment_contrast_beta(img), cut, err);
    if (st == PC_OK) {
        st = pc_algo_segment_keep_seeded(cut, seeds, img->w, img->h, low, err);
    }
    if (st == PC_OK && !okb) {
        st = pc_algo_quick_broad_model(&bg, err);
    }
    if (st == PC_OK) {
        *fg_out = fg;
        *bg_out = bg;
        fg.comps = NULL;
        bg.comps = NULL;
    }
done:
    pc_algo_gmm_fini(&fg);
    pc_algo_gmm_fini(&bg);
    free(geo);
    free(fs);
    free(bs);
    free(sf);
    free(sb);
    free(cf);
    free(cb);
    free(fixed);
    free(cut);
    return st;
}

pc_status pc_algo_quick_select(const pc_algo_sampler *s, pc_rect canvas, const float (*points)[2],
                               size_t npoints, float size, size_t work_px, pc_algo_region *out, pc_error *err)
{
    float r, x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX, st_f;
    pc_rect bbox, window;
    int32_t margin;
    size_t step, lw, lh, i, n;
    pc_algo_rgb_image img = {0, 0, NULL};
    float (*pts)[2] = NULL;
    bool *seeds = NULL, *low = NULL, any = false;
    pc_algo_gmm fg = {NULL, 0, 0.0f}, bg = {NULL, 0, 0.0f};
    pc_status st;
    out->bbox = PC_RECT_EMPTY;
    out->mask = NULL;
    out->len = 0;
    if (npoints == 0) {
        return PC_OK;
    }
    size = pc_rs_maxf_k(size, 1.0f);
    r = size / 2.0f;
    for (i = 0; i < npoints; i++) {
        x0 = pc_rs_minf(x0, points[i][0] - r);
        y0 = pc_rs_minf(y0, points[i][1] - r);
        x1 = pc_rs_maxf(x1, points[i][0] + r);
        y1 = pc_rs_maxf(y1, points[i][1] + r);
    }
    bbox = pc_rect_intersect(pc_rect_make(pc_f32_to_i32(floorf(x0)), pc_f32_to_i32(floorf(y0)),
                                          pc_i32_sat_add(pc_f32_to_i32(ceilf(x1)), 1),
                                          pc_i32_sat_add(pc_f32_to_i32(ceilf(y1)), 1)),
                             canvas);
    if (pc_rect_is_empty(bbox)) {
        return PC_OK;
    }
    margin = pc_f32_to_i32(pc_rs_maxf_k(size * 6.0f, 512.0f));
    window = pc_rect_intersect(pc_rect_inflate(bbox, margin), canvas);
    step = pc_algo_segment_scale_for(window, work_px);
    st = pc_algo_sampler_rgb_scaled(s, window, step, &img, err);
    if (st != PC_OK) {
        return st;
    }
    lw = img.w;
    lh = img.h;
    n = lw * lh;
    st_f = (float)step;
    pts = pc_malloc_array(npoints, sizeof *pts);
    seeds = pc_malloc_array(n ? n : 1, sizeof(bool));
    low = pc_calloc_array(n ? n : 1, sizeof(bool));
    if (pts == NULL || seeds == NULL || low == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Quick Selection");
        goto done;
    }
    for (i = 0; i < npoints; i++) {
        pts[i][0] = (points[i][0] - (float)window.x0) / st_f;
        pts[i][1] = (points[i][1] - (float)window.y0) / st_f;
    }
    pc_algo_quick_stroke_mask((const float (*)[2])pts, npoints, pc_rs_maxf_k(r / st_f, 0.75f), lw, lh, seeds);
    for (i = 0; i < n && !any; i++) {
        any = seeds[i];
    }
    if (!any) {
        goto done;
    }
    st = pc_algo_quick_segment(&img, seeds, &fg, &bg, low, err);
    if (st != PC_OK) {
        goto done;
    }
    any = false;
    for (i = 0; i < n && !any; i++) {
        any = low[i];
    }
    if (any) {
        st = pc_algo_segment_finish_region(s, window, step, low, lw, lh, &fg, &bg, out, err);
    }
done:
    pc_algo_gmm_fini(&fg);
    pc_algo_gmm_fini(&bg);
    pc_algo_rgb_image_fini(&img);
    free(pts);
    free(seeds);
    free(low);
    return st;
}
