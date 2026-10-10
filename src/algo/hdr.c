/* Merge to HDR Pro (port of crates/algo/src/hdr.rs). See hdr.h. */
#include "algo/hdr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/l9util.h"
#include "algo/photo_util.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

static float luma(const float c[4])
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

/* Hat weight on a 0..=255 code value. */
static double hat(double z)
{
    return pc_rs_max_k(z <= 127.5 ? z : 255.0 - z, 0.0);
}

typedef struct key_ent {
    double v;
    size_t i;
} key_ent;

/* Ascending by total_cmp, stable (index tiebreak). */
static int cmp_key(const void *pa, const void *pb)
{
    const key_ent *a = pa, *b = pb;
    int c = pc_algo_l9u_total_cmp(a->v, b->v);
    return c != 0 ? c : (a->i < b->i ? -1 : (a->i > b->i ? 1 : 0));
}

/* Indices 0..n sorted by key (stable). */
static pc_status sorted_order(const double *key, size_t n, size_t *order, pc_error *err)
{
    key_ent *e = pc_malloc_array(n == 0 ? 1 : n, sizeof *e);
    size_t i;
    if (e == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < n; i++) {
        e[i].v = key[i];
        e[i].i = i;
    }
    if (n > 1) {
        qsort(e, n, sizeof *e, cmp_key);
    }
    for (i = 0; i < n; i++) {
        order[i] = e[i].i;
    }
    free(e);
    return PC_OK;
}

pc_status pc_algo_hdr_estimate_exposures(const pc_algo_hdr_img *imgs, size_t n_imgs, double *out,
                                         pc_error *err)
{
    double *mean = NULL, *ratios = NULL, *ex = NULL;
    size_t *order = NULL, j, k, i;
    pc_status st;
    if (n_imgs == 0) {
        return PC_OK;
    }
    if ((st = pc_algo_l9u_alloc_d(n_imgs, false, &mean, err)) != PC_OK ||
        (st = pc_algo_l9u_alloc_d(n_imgs, false, &ex, err)) != PC_OK) {
        goto done;
    }
    order = pc_malloc_array(n_imgs, sizeof *order);
    if (order == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto done;
    }
    for (j = 0; j < n_imgs; j++) {
        double s = -0.0;
        for (i = 0; i < imgs[j].n; i++) {
            s += (double)luma(imgs[j].px[i]);
        }
        mean[j] = s / (double)(imgs[j].n > 1 ? imgs[j].n : 1);
        ex[j] = 1.0;
    }
    if ((st = sorted_order(mean, n_imgs, order, err)) != PC_OK) {
        goto done;
    }
    for (k = 1; k < n_imgs; k++) {
        size_t a = order[k - 1], b = order[k], len = imgs[a].n < imgs[b].n ? imgs[a].n : imgs[b].n, nr = 0;
        double r;
        free(ratios);
        ratios = NULL;
        if ((st = pc_algo_l9u_alloc_d(len / 7 + 1, false, &ratios, err)) != PC_OK) {
            goto done;
        }
        for (i = 0; i < len; i += 7) {
            float la = luma(imgs[a].px[i]), lb = luma(imgs[b].px[i]);
            if (la >= 0.08f && la < 0.92f && lb >= 0.08f && lb < 0.92f) {
                ratios[nr++] = (double)pc_algo_photo_srgb_to_linear(lb) /
                               (double)pc_rs_maxf_k(pc_algo_photo_srgb_to_linear(la), 1e-6f);
            }
        }
        pc_algo_l9u_sort(ratios, nr);
        r = nr == 0 ? 2.0 : pc_rs_max_k(ratios[nr / 2], 1.0);
        ex[b] = ex[a] * r;
    }
    memcpy(out, ex, n_imgs * sizeof *out);
    st = PC_OK;
done:
    free(mean), free(ratios), free(ex), free(order);
    return st;
}

/* ---------- alignment ---------- */

static pc_status shrink(size_t w, size_t h, const float *g, size_t *nw_out, size_t *nh_out, float **out,
                        pc_error *err)
{
    size_t nw = w / 2 < 1 ? 1 : w / 2, nh = h / 2 < 1 ? 1 : h / 2, x, y;
    float *o = NULL;
    pc_status st = pc_algo_l9u_alloc_f(nw * nh, false, &o, err);
    if (st != PC_OK) {
        return st;
    }
#define P(xx, yy) g[((yy) < h - 1 ? (yy) : h - 1) * w + ((xx) < w - 1 ? (xx) : w - 1)]
    for (y = 0; y < nh; y++) {
        for (x = 0; x < nw; x++) {
            o[y * nw + x] = 0.25f * (P(2 * x, 2 * y) + P(2 * x + 1, 2 * y) + P(2 * x, 2 * y + 1) +
                                     P(2 * x + 1, 2 * y + 1));
        }
    }
#undef P
    *nw_out = nw;
    *nh_out = nh;
    *out = o;
    return PC_OK;
}

static pc_status bitmaps(const float *g, size_t n, uint8_t *t, uint8_t *e, pc_error *err)
{
    float *s = NULL, med;
    size_t i;
    pc_status st = pc_algo_l9u_alloc_f(n, false, &s, err);
    if (st != PC_OK) {
        return st;
    }
    memcpy(s, g, n * sizeof *s);
    pc_algo_l9u_sortf(s, n);
    med = s[n / 2];
    free(s);
    for (i = 0; i < n; i++) {
        t[i] = g[i] > med;
        e[i] = fabsf(g[i] - med) > 4.0f / 255.0f;
    }
    return PC_OK;
}

pc_status pc_algo_hdr_mtb_offset(size_t w, size_t h, const float *reference, const float *img, size_t levels,
                                 int32_t *dx_out, int32_t *dy_out, pc_error *err)
{
    int32_t cur_x, cur_y, dx, dy, best_x = 0, best_y = 0;
    uint64_t best = UINT64_MAX;
    uint8_t *tr = NULL, *er = NULL, *ti = NULL, *ei = NULL;
    size_t sw, sh, n;
    float *sr = NULL, *si = NULL;
    pc_status st;
    if (levels == 0 || w < 16 || h < 16) {
        *dx_out = 0;
        *dy_out = 0;
        return PC_OK;
    }
    if (w > INT32_MAX || h > INT32_MAX) {
        return pc_fail(err, PC_ERR_LIMIT, "image too large to align");
    }
    n = w * h;
    if ((st = shrink(w, h, reference, &sw, &sh, &sr, err)) != PC_OK ||
        (st = shrink(w, h, img, &sw, &sh, &si, err)) != PC_OK ||
        (st = pc_algo_hdr_mtb_offset(sw, sh, sr, si, levels - 1, &cur_x, &cur_y, err)) != PC_OK) {
        goto done;
    }
    cur_x *= 2;
    cur_y *= 2;
    tr = pc_malloc_array(n, 1);
    er = pc_malloc_array(n, 1);
    ti = pc_malloc_array(n, 1);
    ei = pc_malloc_array(n, 1);
    if (tr == NULL || er == NULL || ti == NULL || ei == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto done;
    }
    if ((st = bitmaps(reference, n, tr, er, err)) != PC_OK || (st = bitmaps(img, n, ti, ei, err)) != PC_OK) {
        goto done;
    }
    for (dy = -1; dy <= 1; dy++) {
        for (dx = -1; dx <= 1; dx++) {
            int32_t ox = cur_x + dx, oy = cur_y + dy, x, y;
            uint64_t e = 0;
            for (y = 0; y < (int32_t)h; y++) {
                int64_t sy = (int64_t)y - oy;
                if (sy < 0 || sy >= (int64_t)h) {
                    continue;
                }
                for (x = 0; x < (int32_t)w; x++) {
                    int64_t sx = (int64_t)x - ox;
                    size_t a, b;
                    if (sx < 0 || sx >= (int64_t)w) {
                        continue;
                    }
                    a = (size_t)y * w + (size_t)x;
                    b = (size_t)sy * w + (size_t)sx;
                    if (tr[a] != ti[b] && er[a] && ei[b]) {
                        e++;
                    }
                }
            }
            if (e < best) {
                best = e;
                best_x = ox;
                best_y = oy;
            }
        }
    }
    *dx_out = best_x;
    *dy_out = best_y;
    st = PC_OK;
done:
    free(sr), free(si), free(tr), free(er), free(ti), free(ei);
    return st;
}

/* ---------- response curve ---------- */

typedef struct bin {
    size_t *v;
    size_t n, cap;
} bin;

pc_status pc_algo_hdr_sample_pixels(size_t w, size_t h, const float (*middle)[4], size_t count, size_t **out,
                                    size_t *n_out, pc_error *err)
{
    bin bins[32];
    size_t step, x, y, k = 0, nout = 0, cap = 0, i, total = 0, denom;
    size_t *o = NULL;
    pc_status st = PC_OK;
    memset(bins, 0, sizeof bins);
    denom = pc_size_mul(count, 40, &denom) ? denom : SIZE_MAX;
    denom = denom < 1 ? 1 : denom;
    step = pc_f64_to_size(pc_rs_max_k(sqrt((double)(w * h / denom)), 1.0));
    step = step < 1 ? 1 : step;
    for (y = 2; y + 2 < h; y += step) {
        for (x = 2; x + 2 < w; x += step) {
            size_t ii = y * w + x, j[4] = {ii - 2, ii + 2, ii - 2 * w, ii + 2 * w}, b;
            float l = luma(middle[ii]);
            bool flat = true;
            int q;
            for (q = 0; q < 4 && flat; q++) {
                flat = fabsf(luma(middle[j[q]]) - l) < 0.04f;
            }
            if (!flat) {
                continue;
            }
            b = pc_f32_to_size(l * 31.99f);
            b = b > 31 ? 31 : b;
            if (bins[b].n == bins[b].cap) {
                size_t c = bins[b].cap;
                size_t *p = pc_grow_array(bins[b].v, &c, bins[b].n + 1, sizeof *p);
                if (p == NULL) {
                    st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                    goto done;
                }
                bins[b].v = p;
                bins[b].cap = c;
            }
            bins[b].v[bins[b].n++] = ii;
            total++;
        }
    }
    /* Round-robin over the bins so every intensity is represented. */
    cap = count < total ? count : total;
    o = pc_malloc_array(cap == 0 ? 1 : cap, sizeof *o);
    if (o == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto done;
    }
    for (;;) {
        bool any = false;
        if (nout >= count) {
            break;
        }
        for (i = 0; i < 32; i++) {
            any = any || k < bins[i].n;
        }
        if (!any) {
            break;
        }
        for (i = 0; i < 32; i++) {
            if (k < bins[i].n) {
                o[nout++] = bins[i].v[(k * 7) % bins[i].n];
                if (nout >= count) {
                    break;
                }
            }
        }
        k++;
    }
    /* sort_unstable + dedup */
    for (i = 1; i < nout; i++) {
        size_t v = o[i], j2 = i;
        while (j2 > 0 && o[j2 - 1] > v) {
            o[j2] = o[j2 - 1];
            j2--;
        }
        o[j2] = v;
    }
    if (nout > 0) {
        size_t m = 1;
        for (i = 1; i < nout; i++) {
            if (o[i] != o[m - 1]) {
                o[m++] = o[i];
            }
        }
        nout = m;
    }
    *out = o;
    *n_out = nout;
    o = NULL;
done:
    free(o);
    for (i = 0; i < 32; i++) {
        free(bins[i].v);
    }
    return st;
}

pc_status pc_algo_hdr_response_curve(const uint8_t *z, size_t p, size_t n, const double *log_dt,
                                     double lambda, double g[256], pc_error *err)
{
    pc_algo_photo_normal ne;
    double *x = NULL;
    size_t j, i, k, nu;
    pc_status st;
    if (!pc_size_add(256, n, &nu)) {
        return pc_fail(err, PC_ERR_LIMIT, "too many samples");
    }
    if ((st = pc_algo_photo_normal_init(&ne, nu, err)) != PC_OK) {
        return st;
    }
    for (j = 0; j < p; j++) {
        for (i = 0; i < n; i++) {
            size_t zz = z[j * n + i];
            pc_algo_photo_term row[2] = {{zz, 1.0}, {256 + i, -1.0}};
            pc_algo_photo_normal_add(&ne, row, 2, log_dt[j], hat((double)zz) + 1.0);
        }
    }
    {
        pc_algo_photo_term row[1] = {{128, 1.0}};
        pc_algo_photo_normal_add(&ne, row, 1, 0.0, 100.0);
    }
    for (k = 1; k < 255; k++) {
        pc_algo_photo_term row[3] = {{k - 1, 1.0}, {k, -2.0}, {k + 1, 1.0}};
        pc_algo_photo_normal_add(&ne, row, 3, 0.0, sqrt(lambda) * (hat((double)k) + 1.0));
    }
    if ((st = pc_algo_l9u_alloc_d(nu, false, &x, err)) != PC_OK) {
        pc_algo_photo_normal_fini(&ne);
        return st;
    }
    if (!pc_algo_photo_normal_solve(&ne, x)) {
        /* Fall back to the sRGB curve. */
        double mid = log((double)pc_algo_photo_srgb_to_linear(128.0f / 255.0f));
        for (k = 0; k < 256; k++) {
            g[k] = log((double)pc_rs_maxf_k(pc_algo_photo_srgb_to_linear((float)k / 255.0f), 1e-5f)) - mid;
        }
    } else {
        memcpy(g, x, 256 * sizeof *g);
        /* Enforce monotonicity (noise at the extremes can fold the curve). */
        for (k = 128; k-- > 0;) {
            g[k] = pc_rs_min(g[k], g[k + 1] - 1e-4);
        }
        for (k = 129; k < 256; k++) {
            g[k] = pc_rs_max(g[k], g[k - 1] + 1e-4);
        }
    }
    free(x);
    pc_algo_photo_normal_fini(&ne);
    return PC_OK;
}

static double g_at(const double *g, float v)
{
    double x = (double)(pc_clampf(v, 0.0f, 1.0f) * 255.0f), t;
    size_t i = pc_f64_to_size(floor(x));
    i = i > 254 ? 254 : i;
    t = x - (double)i;
    return g[i] * (1.0 - t) + g[i + 1] * t;
}

/* ---------- merge ---------- */

typedef struct merge_ctx {
    const pc_algo_hdr_img *imgs;
    size_t p, w;
    const double *log_dt;
    const double (*resp)[256];
    float (*px)[4];
    float *score;
} merge_ctx;

static pc_status merge_rows(void *vc, size_t chunk, size_t y0, size_t y1)
{
    merge_ctx *m = vc;
    size_t i, j;
    int c;
    (void)chunk;
    for (i = y0 * m->w; i < y1 * m->w; i++) {
        float *out = m->px[i], alpha = 0.0f;
        for (c = 0; c < 3; c++) {
            double num = 0.0, den = 0.0, le;
            for (j = 0; j < m->p; j++) {
                float v = m->imgs[j].px[i][c];
                double wt = hat((double)pc_clampf(v, 0.0f, 1.0f) * 255.0);
                num += wt * (g_at(m->resp[c], v) - m->log_dt[j]);
                den += wt;
            }
            if (den > 1e-6) {
                le = num / den;
            } else {
                /* Saturated everywhere: darkest exposure if bright, brightest if dark. */
                float s = -0.0f;
                bool bright;
                size_t best = 0;
                for (j = 0; j < m->p; j++) {
                    s += m->imgs[j].px[i][c];
                }
                bright = s / (float)m->p > 0.5f;
                for (j = 1; j < m->p; j++) {
                    int cmp = pc_algo_l9u_total_cmp(m->log_dt[j], m->log_dt[best]);
                    if (bright ? cmp < 0 : cmp > 0) {
                        best = j;
                    }
                }
                le = g_at(m->resp[c], m->imgs[best].px[i][c]) - m->log_dt[best];
            }
            out[c] = pc_f64_to_f32(exp(le));
        }
        for (j = 0; j < m->p; j++) {
            alpha = pc_rs_maxf(alpha, m->imgs[j].px[i][3]);
        }
        out[3] = alpha;
    }
    return PC_OK;
}

static pc_status ghost_rows(void *vc, size_t chunk, size_t i0, size_t i1)
{
    merge_ctx *m = vc;
    size_t i, j;
    int c;
    (void)chunk;
    for (i = i0; i < i1; i++) {
        double s = 0.0, s2 = 0.0, sw = 0.0, mean;
        for (j = 0; j < m->p; j++) {
            const float *q = m->imgs[j].px[i];
            double wt = hat((double)luma(q) * 255.0), lr = -0.0;
            if (wt < 20.0) {
                continue;
            }
            for (c = 0; c < 3; c++) {
                lr += 0.33 * (g_at(m->resp[c], q[c]) - m->log_dt[j]);
            }
            s += wt * lr;
            s2 += wt * lr * lr;
            sw += wt;
        }
        if (sw <= 0.0) {
            m->score[i] = 0.0f;
            continue;
        }
        mean = s / sw;
        m->score[i] = pc_f64_to_f32(sqrt(pc_rs_max_k(s2 / sw - mean * mean, 0.0)));
    }
    return PC_OK;
}

void pc_algo_hdr_merged_fini(pc_algo_hdr_merged *m)
{
    if (m != NULL) {
        free(m->px);
        m->px = NULL;
        m->n = 0;
    }
}

static size_t well_exposed(const pc_algo_hdr_img *im)
{
    size_t i, c = 0;
    for (i = 0; i < im->n; i += 3) {
        float l = luma(im->px[i]);
        c += l >= 0.1f && l < 0.9f;
    }
    return c;
}

static pc_status recover_response(size_t w, size_t h, const pc_algo_hdr_img *imgs, size_t p,
                                  const pc_algo_hdr_merge_opts *opts, const double *log_dt,
                                  double resp[3][256], pc_error *err)
{
    size_t *order = NULL, *samples = NULL, ns = 0, j, s;
    uint8_t *z = NULL;
    pc_status st;
    int c;
    order = pc_malloc_array(p, sizeof *order);
    if (order == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if ((st = sorted_order(opts->exposures, p, order, err)) != PC_OK ||
        (st = pc_algo_hdr_sample_pixels(w, h, imgs[order[p / 2]].px, 160, &samples, &ns, err)) != PC_OK) {
        goto done;
    }
    z = pc_malloc_array(p * ns + 1, 1);
    if (z == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto done;
    }
    for (c = 0; c < 3; c++) {
        for (j = 0; j < p; j++) {
            for (s = 0; s < ns; s++) {
                z[j * ns + s] =
                    pc_f32_to_u8(roundf(pc_clampf(imgs[j].px[samples[s]][c], 0.0f, 1.0f) * 255.0f));
            }
        }
        if ((st = pc_algo_hdr_response_curve(z, p, ns, log_dt, 40.0, resp[c], err)) != PC_OK) {
            goto done;
        }
    }
done:
    free(order), free(samples), free(z);
    return st;
}

pc_status pc_algo_hdr_merge(size_t w, size_t h, const pc_algo_hdr_img *imgs, size_t n_imgs,
                            const pc_algo_hdr_merge_opts *opts, pc_algo_hdr_merged *out, pc_error *err)
{
    pc_algo_hdr_merged res;
    size_t p = n_imgs, n, j, i, base;
    double *log_dt = NULL, *r = NULL;
    float *ghost = NULL, *score = NULL, *l = NULL;
    merge_ctx m;
    pc_status st;
    int c;
    memset(&res, 0, sizeof res);
    if (p == 0) {
        return pc_fail(err, PC_ERR_INVALID, "no exposures to merge");
    }
    if (!pc_size_mul(w, h, &n)) {
        return pc_fail(err, PC_ERR_LIMIT, "image too large");
    }
    if (opts->n_exposures < p) {
        return pc_fail(err, PC_ERR_INVALID, "%zu exposures for %zu images", opts->n_exposures, p);
    }
    for (j = 0; j < p; j++) {
        if (imgs[j].n != n) {
            return pc_fail(err, PC_ERR_INVALID, "image %zu has %zu pixels, expected %zu", j, imgs[j].n, n);
        }
    }
    if ((st = pc_algo_l9u_alloc_d(p, false, &log_dt, err)) != PC_OK) {
        return st;
    }
    for (j = 0; j < p; j++) {
        log_dt[j] = log(pc_rs_max_k(opts->exposures[j], 1e-12));
    }
    if (opts->has_ghost_base && opts->ghost_base < p) {
        base = opts->ghost_base;
    } else {
        size_t best = 0, bw = 0;
        for (j = 0; j < p; j++) {
            size_t wv = well_exposed(&imgs[j]);
            if (j == 0 || wv >= bw) { /* max_by_key keeps the last maximum */
                best = j;
                bw = wv;
            }
        }
        base = best;
    }
    if (opts->response != NULL) {
        memcpy(res.response, opts->response, sizeof res.response);
    } else if ((st = recover_response(w, h, imgs, p, opts, log_dt, res.response, err)) != PC_OK) {
        goto fail;
    }
    res.px = pc_calloc_array(n == 0 ? 1 : n, sizeof *res.px);
    if (res.px == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto fail;
    }
    res.n = n;
    m.imgs = imgs;
    m.p = p;
    m.w = w;
    m.log_dt = log_dt;
    m.resp = (const double (*)[256])res.response;
    m.px = res.px;
    m.score = NULL;
    if (w > 0 && (st = pc_algo_par_for(h, 4, merge_rows, &m)) != PC_OK) {
        (void)pc_fail(err, st, "merge failed");
        goto fail;
    }
    if (opts->remove_ghosts && p > 1) {
        float rad;
        size_t cnt = 0;
        double ld = log_dt[base];
        /* Weighted variance of per-exposure log radiance (luminance). */
        if ((st = pc_algo_l9u_alloc_f(n, false, &score, err)) != PC_OK ||
            (st = pc_algo_l9u_alloc_f(n, false, &ghost, err)) != PC_OK) {
            goto fail;
        }
        m.score = score;
        if ((st = pc_algo_par_for(n, 4096, ghost_rows, &m)) != PC_OK) {
            (void)pc_fail(err, st, "merge failed");
            goto fail;
        }
        for (i = 0; i < n; i++) {
            ghost[i] = score[i] > 0.35f ? 1.0f : 0.0f;
        }
        /* Grow and feather the mask. */
        rad = pc_rs_maxf_k((float)(w > h ? w : h) / 200.0f, 2.0f);
        if ((st = pc_algo_gauss_blur_n(ghost, w, h, 1, rad, err)) != PC_OK) {
            goto fail;
        }
        for (i = 0; i < n; i++) {
            ghost[i] = pc_rs_minf_k(ghost[i] * 3.0f, 1.0f);
            cnt += ghost[i] > 0.5f;
        }
        res.ghost_fraction = (double)cnt / (double)(n > 1 ? n : 1);
        for (i = 0; i < n; i++) {
            const float *q = imgs[base].px[i];
            if (ghost[i] <= 0.0f) {
                continue;
            }
            for (c = 0; c < 3; c++) {
                float e = pc_f64_to_f32(exp(g_at(res.response[c], q[c]) - ld));
                res.px[i][c] = res.px[i][c] * (1.0f - ghost[i]) + e * ghost[i];
            }
        }
    }
    /* Scale per channel so the base exposure's well-exposed pixels map to their linear values. */
    if ((st = pc_algo_l9u_alloc_d(n / 37 + 1, false, &r, err)) != PC_OK) {
        goto fail;
    }
    for (c = 0; c < 3; c++) {
        size_t nr = 0;
        double k;
        for (i = 0; i < n; i += 37) {
            float v = imgs[base].px[i][c];
            if (v >= 0.15f && v < 0.85f && res.px[i][c] > 0.0f) {
                r[nr++] =
                    (double)pc_algo_photo_srgb_to_linear(v) / ((double)res.px[i][c] * opts->exposures[base]);
            }
        }
        pc_algo_l9u_sort(r, nr);
        k = (nr == 0 ? 1.0 : r[nr / 2]) * opts->exposures[base];
        for (i = 0; i < n; i++) {
            res.px[i][c] = pc_f64_to_f32((double)res.px[i][c] * k);
        }
    }
    {
        size_t nl = 0;
        if ((st = pc_algo_l9u_alloc_f(n / 13 + 1, false, &l, err)) != PC_OK) {
            goto fail;
        }
        for (i = 0; i < n; i += 13) {
            if (res.px[i][3] > 0.0f) {
                l[nl++] = pc_rs_maxf_k(luma(res.px[i]), 1e-9f);
            }
        }
        pc_algo_l9u_sortf(l, nl);
        res.stops =
            nl == 0 ? 0.0 : log2((double)l[pc_f64_to_size((double)nl * 0.999) % nl] / (double)l[nl / 100]);
    }
    res.ghost_base = base;
    free(log_dt), free(r), free(ghost), free(score), free(l);
    *out = res;
    return PC_OK;
fail:
    free(log_dt), free(r), free(ghost), free(score), free(l);
    pc_algo_hdr_merged_fini(&res);
    return st;
}

/* ---------- tone mapping ---------- */

pc_status pc_algo_hdr_tone_map(float (*px)[4], size_t n, size_t w, size_t h, const pc_algo_hdr_tone_method *m,
                               pc_error *err)
{
    size_t i, cnt = 0, wh;
    double s = -0.0;
    float log_mean;
    int c;
    if (!pc_size_mul(w, h, &wh) || wh != n) {
        return pc_fail(err, PC_ERR_INVALID, "pixel count %zu does not match %zu x %zu", n, w, h);
    }
    for (i = 0; i < n; i++) {
        if (px[i][3] > 0.0f) {
            cnt++;
            s += log((double)pc_rs_maxf_k(luma(px[i]), 1e-6f));
        }
    }
    cnt = cnt < 1 ? 1 : cnt;
    log_mean = pc_f64_to_f32(exp(s / (double)cnt));
    switch (m->kind) {
    case PC_ALGO_HDR_LOCAL_ADAPTATION: {
        /* Key the scene to middle grey, compress in log luminance, then encode. */
        float k = 0.18f / pc_rs_maxf_k(log_mean, 1e-6f);
        pc_status st;
        for (i = 0; i < n; i++) {
            for (c = 0; c < 3; c++) {
                px[i][c] *= k;
            }
        }
        if ((st = pc_algo_hdr_toning(px, n, w, h, &m->local, err)) != PC_OK) {
            return st;
        }
        for (i = 0; i < n; i++) {
            for (c = 0; c < 3; c++) {
                px[i][c] = pc_algo_photo_linear_to_srgb(pc_clampf(px[i][c], 0.0f, 1.0f));
            }
        }
        break;
    }
    case PC_ALGO_HDR_EXPOSURE_GAMMA: {
        float k = exp2f(m->exposure), g = pc_clampf(m->gamma, 0.1f, 10.0f);
        for (i = 0; i < n; i++) {
            for (c = 0; c < 3; c++) {
                px[i][c] = powf(pc_algo_photo_linear_to_srgb(pc_clampf(px[i][c] * k, 0.0f, 1.0f)), 1.0f / g);
            }
        }
        break;
    }
    case PC_ALGO_HDR_HIGHLIGHT_COMPRESSION: {
        float k = 0.18f / pc_rs_maxf_k(log_mean, 1e-6f);
        for (i = 0; i < n; i++) {
            float l = luma(px[i]) * k, ld = l / (1.0f + l), r = l > 1e-9f ? ld / l * k : 0.0f;
            for (c = 0; c < 3; c++) {
                px[i][c] = pc_algo_photo_linear_to_srgb(pc_clampf(px[i][c] * r, 0.0f, 1.0f));
            }
        }
        break;
    }
    case PC_ALGO_HDR_EQUALIZE_HISTOGRAM: {
        enum { BINS = 1024 };
        float *lv = NULL, lo = 3.40282347e38f, hi = -3.40282347e38f, span, cdf[BINS];
        double *hist = NULL, total = -0.0, acc = 0.0;
        pc_status st;
        size_t b;
        if ((st = pc_algo_l9u_alloc_f(n, false, &lv, err)) != PC_OK) {
            return st;
        }
        if ((st = pc_algo_l9u_alloc_d(BINS, true, &hist, err)) != PC_OK) {
            free(lv);
            return st;
        }
        for (i = 0; i < n; i++) {
            lv[i] = logf(pc_rs_maxf_k(luma(px[i]), 1e-7f));
            if (px[i][3] > 0.0f) {
                lo = pc_rs_minf(lo, lv[i]);
                hi = pc_rs_maxf(hi, lv[i]);
            }
        }
        span = pc_rs_maxf_k(hi - lo, 1e-6f);
        for (i = 0; i < n; i++) {
            if (px[i][3] > 0.0f) {
                b = pc_f32_to_size((lv[i] - lo) / span * (float)(BINS - 1));
                hist[b < BINS ? b : BINS - 1] += 1.0;
            }
        }
        for (b = 0; b < BINS; b++) {
            total += hist[b];
        }
        total = pc_rs_max_k(total, 1.0);
        for (b = 0; b < BINS; b++) {
            acc += hist[b];
            cdf[b] = pc_f64_to_f32(acc / total);
        }
        for (i = 0; i < n; i++) {
            size_t bi = pc_f32_to_size(pc_clampf((lv[i] - lo) / span, 0.0f, 1.0f) * (float)(BINS - 1));
            /* Display value from the CDF (encoded), keep colour ratios in linear light. */
            float target = cdf[bi < BINS ? bi : BINS - 1];
            float tl = pc_algo_photo_srgb_to_linear(target), l = pc_rs_maxf_k(luma(px[i]), 1e-7f);
            for (c = 0; c < 3; c++) {
                px[i][c] = pc_algo_photo_linear_to_srgb(pc_clampf(px[i][c] / l * tl, 0.0f, 1.0f));
            }
        }
        free(lv);
        free(hist);
        break;
    }
    }
    return PC_OK;
}
