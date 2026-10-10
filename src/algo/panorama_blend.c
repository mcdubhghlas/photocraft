/* Panorama photometric correction, seams and multi-band blending (port of
 * crates/algo/src/panorama.rs, second half). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/panorama.h"
#include "algo/photo_util.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* ---- photometric ---- */

void pc_algo_photometric_fini(pc_algo_photometric *p)
{
    if (p != NULL) {
        free(p->gains);
        memset(p, 0, sizeof *p);
    }
}

float pc_algo_photometric_factor(const pc_algo_photometric *p, size_t i, size_t c, double r2)
{
    double v = exp(p->vignette[0] * r2 + p->vignette[1] * r2 * r2);
    if (i >= p->n || c >= p->ch) {
        return 1.0f;
    }
    return (float)(1.0 / ((double)p->gains[i * p->ch + c] * v));
}

/* RoiImage::at. */
static bool roi_at(const pc_algo_roi_image *im, int64_t x, int64_t y, size_t *k)
{
    int64_t lx = x - im->x0, ly = y - im->y0;
    if (lx >= 0 && ly >= 0 && (uint64_t)lx < (uint64_t)im->w && (uint64_t)ly < (uint64_t)im->h) {
        *k = (size_t)ly * im->w + (size_t)lx;
        return true;
    }
    return false;
}

static bool roi_valid(const pc_algo_roi_image *im, size_t ch)
{
    size_t px;
    return im->ch == ch && pc_size_mul3(im->w, im->h, ch, &px) && px <= PC_MAX_ALLOC / sizeof(float) &&
           ((im->px != NULL && im->alpha != NULL) || im->w * im->h == 0);
}

typedef struct psample {
    size_t a, b;
    double ra, rb;
} psample;

static bool r2_of(const pc_algo_placement *placements, const size_t (*sizes)[2], size_t i, int64_t u,
                  int64_t v, double *r2)
{
    double sx, sy, w, h, dx, dy;
    if (placements == NULL) {
        *r2 = 0.0;
        return true;
    }
    if (!pc_algo_placement_inverse(&placements[i], (double)u + 0.5, (double)v + 0.5, &sx, &sy)) {
        return false;
    }
    w = (double)sizes[i][0], h = (double)sizes[i][1];
    dx = sx - w / 2.0, dy = sy - h / 2.0;
    *r2 = (dx * dx + dy * dy) / (w * w / 4.0 + h * h / 4.0);
    return true;
}

pc_status pc_algo_panorama_photometric(const pc_algo_roi_image *imgs, size_t n,
                                       const pc_algo_placement *placements, const size_t (*sizes)[2],
                                       bool gain, bool vignette, pc_algo_photometric *out, pc_error *err)
{
    size_t ch = n > 0 ? imgs[0].ch : 1, i, c, ns = 0, cap = 0, ccap = 0, nv, *cov_i = NULL, *cov_k = NULL;
    int64_t x0 = INT64_MAX, y0 = INT64_MAX, x1 = INT64_MIN, y1 = INT64_MIN, step, x, y, area;
    psample *s = NULL;
    float *cols = NULL, *gains = NULL;
    double *sol = NULL;
    pc_algo_photometric res;
    pc_algo_photo_normal ne;
    const double sigma_n = 0.05, sigma_g = gain ? 0.15 : 1e-4;
    pc_status st = PC_OK;
    bool geometry = placements != NULL && sizes != NULL;
    if (!geometry) {
        placements = NULL;
    }
    vignette = vignette && geometry;
    memset(&ne, 0, sizeof ne);
    if (ch == 0 || ch > 64) {
        return pc_fail(err, PC_ERR_INVALID, "photometric needs 1 to 64 colour channels");
    }
    for (i = 0; i < n; i++) {
        if (!roi_valid(&imgs[i], ch)) {
            return pc_fail(err, PC_ERR_INVALID, "image %zu does not match the first image's channels", i);
        }
    }
    gains = pc_malloc_array(n > 0 ? n * ch : 1, sizeof *gains);
    if (gains == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for gains");
    }
    for (i = 0; i < n * ch; i++) {
        gains[i] = 1.0f;
    }
    res.gains = gains, res.n = n, res.ch = ch, res.vignette[0] = res.vignette[1] = 0.0;
    if (n == 0 || (!gain && !vignette)) {
        *out = res;
        return PC_OK;
    }
    /* Overlap samples on a grid: (a, b, colours, radii^2). */
    for (i = 0; i < n; i++) {
        x0 = imgs[i].x0 < x0 ? imgs[i].x0 : x0;
        y0 = imgs[i].y0 < y0 ? imgs[i].y0 : y0;
        x1 = (int64_t)imgs[i].x0 + (int64_t)imgs[i].w > x1 ? (int64_t)imgs[i].x0 + (int64_t)imgs[i].w : x1;
        y1 = (int64_t)imgs[i].y0 + (int64_t)imgs[i].h > y1 ? (int64_t)imgs[i].y0 + (int64_t)imgs[i].h : y1;
    }
    area = (x1 - x0) * (y1 - y0) / 40000;
    step = (int64_t)ceil(sqrt((double)(area > 1 ? area : 1)));
    cov_i = pc_malloc_array(n, sizeof *cov_i);
    cov_k = pc_malloc_array(n, sizeof *cov_k);
    if (cov_i == NULL || cov_k == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for photometric samples");
        goto cleanup;
    }
    for (y = y0; y < y1; y += step) {
        for (x = x0; x < x1; x += step) {
            size_t ncov = 0, p, q, k;
            for (i = 0; i < n; i++) {
                if (roi_at(&imgs[i], x, y, &k) && imgs[i].alpha[k] > 0.999f) {
                    cov_i[ncov] = i, cov_k[ncov] = k, ncov++;
                }
            }
            for (p = 0; p < ncov; p++) {
                for (q = p + 1; q < ncov; q++) {
                    const float *ca = imgs[cov_i[p]].px + cov_k[p] * ch,
                                *cb = imgs[cov_i[q]].px + cov_k[q] * ch;
                    bool skip = false;
                    double ra, rb;
                    for (c = 0; c < ch && !skip; c++) {
                        skip = !(ca[c] >= 0.02f && ca[c] <= 0.98f);
                    }
                    for (c = 0; c < ch && !skip; c++) {
                        skip = !(cb[c] >= 0.02f && cb[c] <= 0.98f);
                    }
                    /* Disagreeing content (parallax, moving objects) says nothing about exposure. */
                    for (c = 0; c < ch && !skip; c++) {
                        skip = fabsf(logf(ca[c] / cb[c])) > 1.0f;
                    }
                    if (skip || !r2_of(placements, sizes, cov_i[p], x, y, &ra) ||
                        !r2_of(placements, sizes, cov_i[q], x, y, &rb)) {
                        continue;
                    }
                    if (ns + 1 > cap) {
                        psample *s2 = pc_grow_array(s, &cap, ns + 1, sizeof *s);
                        if (s2 == NULL) {
                            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for photometric samples");
                            goto cleanup;
                        }
                        s = s2;
                    }
                    if ((ns + 1) * 2 * ch > ccap) {
                        float *c2 = pc_grow_array(cols, &ccap, (ns + 1) * 2 * ch, sizeof *cols);
                        if (c2 == NULL) {
                            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for photometric samples");
                            goto cleanup;
                        }
                        cols = c2;
                    }
                    s[ns].a = cov_i[p], s[ns].b = cov_i[q], s[ns].ra = ra, s[ns].rb = rb;
                    memcpy(cols + ns * 2 * ch, ca, ch * sizeof *cols);
                    memcpy(cols + ns * 2 * ch + ch, cb, ch * sizeof *cols);
                    ns++;
                }
            }
        }
    }
    if (ns < 20) {
        goto done;
    }
    /* Unknowns: log gain per image, then a, b (shared, from the mean log intensity). Vignetting
     * is solved on the channel mean, then gains per channel with it fixed. */
    nv = vignette ? 2 : 0;
    st = pc_algo_photo_normal_init(&ne, n + nv, err);
    sol = pc_malloc_array(n + nv, sizeof *sol);
    if (st != PC_OK || sol == NULL) {
        st = st != PC_OK ? st : pc_fail(err, PC_ERR_NOMEM, "out of memory for photometric equations");
        goto cleanup;
    }
    for (i = 0; i < ns; i++) {
        const float *ca = cols + i * 2 * ch, *cb = ca + ch;
        double la = -0.0, lb = -0.0, ra = s[i].ra, rb = s[i].rb;
        pc_algo_photo_term row[4];
        for (c = 0; c < ch; c++) {
            la += log((double)ca[c]);
        }
        for (c = 0; c < ch; c++) {
            lb += log((double)cb[c]);
        }
        la /= (double)ch;
        lb /= (double)ch;
        row[0].i = s[i].a, row[0].c = 1.0;
        row[1].i = s[i].b, row[1].c = -1.0;
        row[2].i = n, row[2].c = ra - rb;
        row[3].i = n + 1, row[3].c = ra * ra - rb * rb;
        pc_algo_photo_normal_add(&ne, row, vignette ? 4 : 2, la - lb, 1.0 / sigma_n);
    }
    for (i = 0; i < n; i++) {
        pc_algo_photo_term t = {i, 1.0};
        pc_algo_photo_normal_add(&ne, &t, 1, 0.0, 1.0 / sigma_g);
    }
    if (vignette) {
        pc_algo_photo_term t0 = {n, 1.0}, t1 = {n + 1, 1.0};
        pc_algo_photo_normal_add(&ne, &t0, 1, 0.0, 0.5);
        pc_algo_photo_normal_add(&ne, &t1, 1, 0.0, 0.5);
    }
    if (!pc_algo_photo_normal_solve(&ne, sol)) {
        goto done;
    }
    pc_algo_photo_normal_fini(&ne);
    if (vignette) {
        /* Vignetting only darkens; clamp a runaway brightening fit. */
        res.vignette[0] = pc_clampd(sol[n], -2.0, 0.5);
        res.vignette[1] = pc_clampd(sol[n + 1], -2.0, 2.0);
    }
    if (gain) {
        for (c = 0; c < ch; c++) {
            st = pc_algo_photo_normal_init(&ne, n, err);
            if (st != PC_OK) {
                goto cleanup;
            }
            for (i = 0; i < ns; i++) {
                const float *ca = cols + i * 2 * ch, *cb = ca + ch;
                double ra = s[i].ra, rb = s[i].rb;
                double va = res.vignette[0] * ra + res.vignette[1] * ra * ra;
                double vb = res.vignette[0] * rb + res.vignette[1] * rb * rb;
                pc_algo_photo_term row[2] = {{s[i].a, 1.0}, {s[i].b, -1.0}};
                pc_algo_photo_normal_add(&ne, row, 2, (log((double)ca[c]) - va) - (log((double)cb[c]) - vb),
                                         1.0 / sigma_n);
            }
            for (i = 0; i < n; i++) {
                pc_algo_photo_term t = {i, 1.0};
                pc_algo_photo_normal_add(&ne, &t, 1, 0.0, 1.0 / sigma_g);
            }
            if (pc_algo_photo_normal_solve(&ne, sol)) {
                for (i = 0; i < n; i++) {
                    gains[i * ch + c] = (float)pc_clampd(exp(sol[i]), 0.2, 5.0);
                }
            }
            pc_algo_photo_normal_fini(&ne);
        }
    }
done:
    *out = res;
    gains = NULL;
cleanup:
    pc_algo_photo_normal_fini(&ne);
    free(s), free(cols), free(sol), free(cov_i), free(cov_k), free(gains);
    return st;
}

/* ---- seams ---- */

static int32_t total_key32(float f)
{
    uint32_t u;
    int32_t i;
    memcpy(&u, &f, sizeof u);
    i = pc_u32_as_i32(u);
    return i ^ pc_u32_as_i32((uint32_t)(i >> 31) >> 1);
}

pc_status pc_algo_panorama_seam_labels(size_t w, size_t h, const float *const *luma, const bool *const *cover,
                                       size_t count, const size_t *order, size_t norder, int32_t **out,
                                       pc_error *err)
{
    size_t wh, o, i;
    int32_t *label;
    bool *overlap;
    float *acc;
    size_t *from, *seam;
    size_t maxlines = w > h ? w : h;
    if (!pc_size_mul(w, h, &wh) || wh > PC_MAX_ALLOC / 16) {
        return pc_fail(err, PC_ERR_LIMIT, "seam grid of %zux%zu is too large", w, h);
    }
    for (o = 0; o < norder; o++) {
        if (order[o] >= count || order[o] > (size_t)INT32_MAX) {
            return pc_fail(err, PC_ERR_INVALID, "seam order refers to a missing image (%zu)", order[o]);
        }
    }
    label = pc_malloc_array(wh > 0 ? wh : 1, sizeof *label);
    overlap = pc_malloc_array(wh > 0 ? wh : 1, sizeof *overlap);
    acc = pc_malloc_array(wh > 0 ? wh : 1, sizeof *acc);
    from = pc_malloc_array(wh > 0 ? wh : 1, sizeof *from);
    seam = pc_malloc_array(maxlines > 0 ? maxlines : 1, sizeof *seam);
    if (label == NULL || overlap == NULL || acc == NULL || from == NULL || seam == NULL) {
        free(label), free(overlap), free(acc), free(from), free(seam);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for seams");
    }
    for (i = 0; i < wh; i++) {
        label[i] = -1;
    }
    for (o = 0; o < norder; o++) {
        size_t img = order[o], no = 0, nn = 0, lines, len, line, k;
        const bool *cov = cover[img];
        const float *li = luma[img];
        double sx = 0.0, sy = 0.0, nx = 0.0, ny = 0.0, ox, oy, cx, cy;
        bool vertical, positive, have_prev = false;
        size_t prev_line = 0;
        for (i = 0; i < wh; i++) {
            overlap[i] = false;
            if (!cov[i]) {
                continue;
            }
            nx += (double)(i % w);
            ny += (double)(i / w);
            nn += 1;
            if (label[i] >= 0) {
                overlap[i] = true;
                sx += (double)(i % w);
                sy += (double)(i / w);
                no += 1;
            } else {
                label[i] = (int32_t)img;
            }
        }
        if (no == 0 || nn == no) {
            continue;
        }
        ox = sx / (double)no, oy = sy / (double)no;
        cx = nx / (double)nn, cy = ny / (double)nn;
        vertical = fabs(cx - ox) >= fabs(cy - oy);
        /* Image on the increasing side along the seam's cross axis? */
        positive = vertical ? cx > ox : cy > oy;
        lines = vertical ? h : w;
        len = vertical ? w : h;
#define IDX(ln, kk) (vertical ? (ln) * w + (kk) : (kk) * w + (ln))
        for (i = 0; i < wh; i++) {
            acc[i] = (float)INFINITY;
            from[i] = SIZE_MAX;
        }
        /* DP over lines (rows for a vertical seam), within the overlap. */
        for (line = 0; line < lines; line++) {
            bool any = false;
            for (k = 0; k < len && !any; k++) {
                any = overlap[IDX(line, k)];
            }
            if (!any) {
                continue;
            }
            for (k = 0; k < len; k++) {
                size_t ii = IDX(line, k), bk = SIZE_MAX;
                float bv = 0.0f;
                if (!overlap[ii]) {
                    continue;
                }
                if (have_prev && prev_line + 1 == line) {
                    int dk;
                    bv = (float)INFINITY;
                    for (dk = -1; dk <= 1; dk++) {
                        int64_t kk = (int64_t)k + dk;
                        size_t j;
                        if (kk < 0 || kk >= (int64_t)len) {
                            continue;
                        }
                        j = IDX(prev_line, (size_t)kk);
                        if (acc[j] < bv) {
                            bv = acc[j];
                            bk = (size_t)kk;
                        }
                    }
                    if (bk == SIZE_MAX) {
                        bv = 0.0f;
                    }
                }
                acc[ii] = bv + (0.002f + fabsf(li[ii] - luma[label[ii]][ii]));
                from[ii] = bk;
            }
            prev_line = line;
            have_prev = true;
        }
        /* Backtrack from the cheapest end; lines not reached keep their own best cell. */
        for (line = 0; line < lines; line++) {
            seam[line] = SIZE_MAX;
        }
        for (line = lines; line-- > 0;) {
            size_t best = SIZE_MAX, l, kk;
            if (seam[line] != SIZE_MAX) {
                continue;
            }
            for (k = 0; k < len; k++) { /* min_by: the first minimum wins */
                if (overlap[IDX(line, k)] && (best == SIZE_MAX || total_key32(acc[IDX(line, best)]) >
                                                                      total_key32(acc[IDX(line, k)]))) {
                    best = k;
                }
            }
            if (best == SIZE_MAX) {
                continue;
            }
            l = line, kk = best;
            for (;;) {
                size_t f;
                seam[l] = kk;
                f = from[IDX(l, kk)];
                if (f == SIZE_MAX || l == 0) {
                    break;
                }
                l--;
                if (seam[l] != SIZE_MAX) {
                    break;
                }
                kk = f;
            }
        }
        for (line = 0; line < lines; line++) {
            size_t sk = seam[line];
            if (sk == SIZE_MAX) {
                continue;
            }
            for (k = 0; k < len; k++) {
                size_t ii = IDX(line, k);
                if (overlap[ii] && ((positive && k > sk) || (!positive && k < sk))) {
                    label[ii] = (int32_t)img;
                }
            }
        }
#undef IDX
    }
    free(overlap), free(acc), free(from), free(seam);
    *out = label;
    return PC_OK;
}

/* ---- multi-band blending ---- */

size_t pc_algo_panorama_blend_levels(size_t w, size_t h)
{
    size_t s = w < h ? w : h, n = 1;
    while (s > 16 && n < 7) {
        s /= 2;
        n++;
    }
    return n;
}

static const float KB[5] = {1.0f, 4.0f, 6.0f, 4.0f, 1.0f};

/* Separable 5-tap binomial, then decimate; *out gets ceil(w/2) x ceil(h/2) x ch (w, h >= 1). */
static float *reduce(size_t w, size_t h, size_t ch, const float *src, size_t *ow, size_t *oh)
{
    size_t nw = (w + 1) / 2, nh = (h + 1) / 2, x, y, c;
    float *tmp = pc_malloc_array(nw * h * ch, sizeof(float)),
          *out = pc_malloc_array(nw * nh * ch, sizeof(float));
    if (tmp == NULL || out == NULL) {
        free(tmp), free(out);
        return NULL;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < nw; x++) {
            for (c = 0; c < ch; c++) {
                float s = 0.0f;
                int t;
                for (t = 0; t < 5; t++) {
                    size_t xx = (size_t)pc_clamp_i64((int64_t)(2 * x) + t - 2, 0, (int64_t)w - 1);
                    s += KB[t] * src[(y * w + xx) * ch + c];
                }
                tmp[(y * nw + x) * ch + c] = s / 16.0f;
            }
        }
    }
    for (y = 0; y < nh; y++) {
        for (x = 0; x < nw; x++) {
            for (c = 0; c < ch; c++) {
                float s = 0.0f;
                int t;
                for (t = 0; t < 5; t++) {
                    size_t yy = (size_t)pc_clamp_i64((int64_t)(2 * y) + t - 2, 0, (int64_t)h - 1);
                    s += KB[t] * tmp[(yy * nw + x) * ch + c];
                }
                out[(y * nw + x) * ch + c] = s / 16.0f;
            }
        }
    }
    free(tmp);
    *ow = nw, *oh = nh;
    return out;
}

/* Bilinear upsampling at half-pixel-aligned positions (sw, sh >= 1). */
static float *expand(size_t sw, size_t sh, size_t ch, const float *src, size_t w, size_t h)
{
    float *out = pc_malloc_array(w * h * ch > 0 ? w * h * ch : 1, sizeof(float));
    size_t x, y, c;
    if (out == NULL) {
        return NULL;
    }
    for (y = 0; y < h; y++) {
        float fy = pc_clampf(((float)y + 0.5f) / 2.0f - 0.5f, 0.0f, (float)(sh - 1));
        size_t y0 = pc_f32_to_size(floorf(fy)), y1;
        float ty = fy - floorf(fy);
        y1 = y0 + 1 < sh - 1 ? y0 + 1 : sh - 1;
        for (x = 0; x < w; x++) {
            float fx = pc_clampf(((float)x + 0.5f) / 2.0f - 0.5f, 0.0f, (float)(sw - 1));
            size_t x0 = pc_f32_to_size(floorf(fx)), x1;
            float tx = fx - floorf(fx);
            x1 = x0 + 1 < sw - 1 ? x0 + 1 : sw - 1;
            for (c = 0; c < ch; c++) {
#define P(xx, yy) src[((yy) * sw + (xx)) * ch + c]
                float top = P(x0, y0) + (P(x1, y0) - P(x0, y0)) * tx;
                float bot = P(x0, y1) + (P(x1, y1) - P(x0, y1)) * tx;
#undef P
                out[(y * w + x) * ch + c] = top + (bot - top) * ty;
            }
        }
    }
    return out;
}

/* Fills pixels with zero weight from their surroundings (push-pull), so pyramid levels don't
 * pull in black from outside an image. Depth is bounded by log2 of the size. */
static bool push_pull(size_t w, size_t h, size_t ch, float *px, const float *wt)
{
    size_t nw, nh, x, y, c, j;
    float *cpx, *cw;
    bool has_zero = false, has_pos = false, ok = true;
    if (w <= 1 && h <= 1) {
        return true;
    }
    nw = (w + 1) / 2, nh = (h + 1) / 2;
    cpx = pc_calloc_array(nw * nh * ch, sizeof(float));
    cw = pc_calloc_array(nw * nh, sizeof(float));
    if (cpx == NULL || cw == NULL) {
        free(cpx), free(cw);
        return false;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t i = y * w + x;
            float a = wt[i];
            j = (y / 2) * nw + x / 2;
            cw[j] += a;
            for (c = 0; c < ch; c++) {
                cpx[j * ch + c] += a * px[i * ch + c];
            }
        }
    }
    for (j = 0; j < nw * nh; j++) {
        if (cw[j] > 0.0f) {
            for (c = 0; c < ch; c++) {
                cpx[j * ch + c] /= cw[j];
            }
            cw[j] = 1.0f;
        }
    }
    for (j = 0; j < nw * nh; j++) {
        has_zero = has_zero || cw[j] == 0.0f;
        has_pos = has_pos || cw[j] > 0.0f;
    }
    if (has_zero && has_pos) {
        ok = push_pull(nw, nh, ch, cpx, cw);
    }
    for (y = 0; y < h && ok; y++) {
        for (x = 0; x < w; x++) {
            size_t i = y * w + x;
            if (wt[i] < 1.0f) {
                float a = wt[i];
                j = (y / 2) * nw + x / 2;
                for (c = 0; c < ch; c++) {
                    px[i * ch + c] = a * px[i * ch + c] + (1.0f - a) * cpx[j * ch + c];
                }
            }
        }
    }
    free(cpx), free(cw);
    return ok;
}

/* One image's pyramids on its margin-grown ROI grid. */
typedef struct band_level {
    size_t w, h;
    float *lap; /* w * h * ch */
    float *gau; /* w * h */
} band_level;

typedef struct band {
    int64_t gx0, gy0;
    band_level *lv;
    pc_status st;
} band;

typedef struct mb_ctx {
    size_t w, h, ch, levels, first;
    int64_t align, margin;
    const pc_algo_roi_image *imgs;
    const float *const *weights;
    band *bands;
} mb_ctx;

static void band_free(band *b, size_t levels)
{
    size_t l;
    if (b->lv != NULL) {
        for (l = 0; l < levels; l++) {
            free(b->lv[l].lap);
            free(b->lv[l].gau);
        }
        free(b->lv);
        b->lv = NULL;
    }
}

static pc_status build_band(const mb_ctx *m, size_t k, band *out)
{
    const pc_algo_roi_image *im = &m->imgs[k];
    int64_t gx0 = (pc_clamp_i64(im->x0 - m->margin, 0, INT64_MAX) / m->align) * m->align;
    int64_t gy0 = (pc_clamp_i64(im->y0 - m->margin, 0, INT64_MAX) / m->align) * m->align;
    int64_t gx1 = (int64_t)im->x0 + (int64_t)im->w + m->margin,
            gy1 = (int64_t)im->y0 + (int64_t)im->h + m->margin;
    size_t gw, gh, x, y, l, cw, chh, ch = m->ch, ng;
    float *px = NULL, *cov = NULL, *wt = NULL, *bin = NULL, *cur, *cwt;
    gx1 = gx1 < (int64_t)m->w ? gx1 : (int64_t)m->w;
    gy1 = gy1 < (int64_t)m->h ? gy1 : (int64_t)m->h;
    gw = (size_t)(gx1 - gx0 > 1 ? gx1 - gx0 : 1);
    gh = (size_t)(gy1 - gy0 > 1 ? gy1 - gy0 : 1);
    out->gx0 = gx0, out->gy0 = gy0;
    out->lv = pc_calloc_array(m->levels, sizeof *out->lv);
    if (!pc_size_mul3(gw, gh, ch, &ng) || ng > PC_MAX_ALLOC / sizeof(float)) {
        return PC_ERR_LIMIT;
    }
    px = pc_calloc_array(ng, sizeof(float));
    cov = pc_calloc_array(gw * gh, sizeof(float));
    wt = pc_calloc_array(gw * gh, sizeof(float));
    bin = pc_malloc_array(gw * gh, sizeof(float));
    if (out->lv == NULL || px == NULL || cov == NULL || wt == NULL || bin == NULL) {
        free(px), free(cov), free(wt), free(bin);
        return PC_ERR_NOMEM;
    }
    for (y = 0; y < im->h; y++) {
        for (x = 0; x < im->w; x++) {
            int64_t gx = (int64_t)im->x0 + (int64_t)x - gx0, gy = (int64_t)im->y0 + (int64_t)y - gy0;
            size_t i, g;
            if (gx < 0 || gy < 0 || (size_t)gx >= gw || (size_t)gy >= gh) {
                continue;
            }
            i = y * im->w + x, g = (size_t)gy * gw + (size_t)gx;
            cov[g] = im->alpha[i];
            wt[g] = m->weights[k][i] * pc_rs_minf_k(im->alpha[i], 1.0f);
            memcpy(px + g * ch, im->px + i * ch, ch * sizeof(float));
        }
    }
    for (x = 0; x < gw * gh; x++) {
        bin[x] = cov[x] > 0.5f ? 1.0f : 0.0f;
    }
    free(cov);
    if (!push_pull(gw, gh, ch, px, bin)) {
        free(px), free(wt), free(bin);
        return PC_ERR_NOMEM;
    }
    free(bin);
    /* Laplacian of the colours, Gaussian of the weights. */
    cw = gw, chh = gh, cur = px, cwt = wt;
    for (l = 0; l < m->levels; l++) {
        size_t nw, nh, n2, n3, q;
        float *down, *up, *wdown;
        if (l + 1 == m->levels) {
            out->lv[l].w = cw, out->lv[l].h = chh, out->lv[l].lap = cur, out->lv[l].gau = cwt;
            return PC_OK;
        }
        down = reduce(cw, chh, ch, cur, &nw, &nh);
        up = down != NULL ? expand(nw, nh, ch, down, cw, chh) : NULL;
        wdown = up != NULL ? reduce(cw, chh, 1, cwt, &n2, &n3) : NULL;
        if (wdown == NULL) {
            free(down), free(up), free(cur), free(cwt);
            return PC_ERR_NOMEM;
        }
        for (q = 0; q < cw * chh * ch; q++) {
            cur[q] -= up[q];
        }
        free(up);
        out->lv[l].w = cw, out->lv[l].h = chh, out->lv[l].lap = cur, out->lv[l].gau = cwt;
        cw = nw, chh = nh, cur = down, cwt = wdown;
    }
    free(cur), free(cwt);
    return PC_OK;
}

static pc_status band_chunk(void *vctx, size_t chunk, size_t begin, size_t end)
{
    mb_ctx *m = vctx;
    size_t k;
    (void)chunk;
    for (k = begin; k < end; k++) {
        band *b = &m->bands[k];
        b->st = build_band(m, m->first + k, b);
    }
    return PC_OK;
}

pc_status pc_algo_panorama_multiband(size_t w, size_t h, const pc_algo_roi_image *imgs, size_t n,
                                     const float *const *weights, size_t levels, float **out_px,
                                     float **out_cov, pc_error *err)
{
    size_t ch = n > 0 ? imgs[0].ch : 1, l, i, (*sizes)[2] = NULL, g0, gn, wh;
    float **acc = NULL, **wsum = NULL, *cur = NULL, *cov = NULL;
    band *bands = NULL;
    mb_ctx m;
    pc_status st = PC_OK;
    levels = levels > 0 ? levels : 1;
    if (levels > 30) {
        return pc_fail(err, PC_ERR_LIMIT, "%zu pyramid levels are too many", levels);
    }
    if (ch == 0 || !pc_size_mul3(w, h, ch, &wh) || wh > PC_MAX_ALLOC / sizeof(float) || w > INT32_MAX ||
        h > INT32_MAX) {
        return pc_fail(err, PC_ERR_LIMIT, "panorama of %zux%zu x %zu is too large", w, h, ch);
    }
    for (i = 0; i < n; i++) {
        if (!roi_valid(&imgs[i], ch) || (imgs[i].w * imgs[i].h > 0 && weights[i] == NULL)) {
            return pc_fail(err, PC_ERR_INVALID, "image %zu does not match the first image's channels", i);
        }
    }
    sizes = pc_malloc_array(levels, sizeof *sizes);
    acc = pc_calloc_array(levels, sizeof *acc);
    wsum = pc_calloc_array(levels, sizeof *wsum);
    cov = pc_calloc_array(w * h > 0 ? w * h : 1, sizeof *cov);
    if (sizes == NULL || acc == NULL || wsum == NULL || cov == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for blending");
        goto cleanup;
    }
    /* Level sizes of the panorama. */
    sizes[0][0] = w, sizes[0][1] = h;
    for (l = 1; l < levels; l++) {
        sizes[l][0] = (sizes[l - 1][0] + 1) / 2, sizes[l][1] = (sizes[l - 1][1] + 1) / 2;
    }
    for (l = 0; l < levels; l++) {
        size_t lw = sizes[l][0] * sizes[l][1];
        acc[l] = pc_calloc_array(lw * ch > 0 ? lw * ch : 1, sizeof(float));
        wsum[l] = pc_calloc_array(lw > 0 ? lw : 1, sizeof(float));
        if (acc[l] == NULL || wsum[l] == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for blending");
            goto cleanup;
        }
    }
    m.w = w, m.h = h, m.ch = ch, m.levels = levels, m.imgs = imgs, m.weights = weights;
    m.align = (int64_t)1 << (levels - 1);
    m.margin = m.align * 4;
    /* Each image on its ROI grown by a margin and aligned to the coarsest level's grid. Bands are
     * built in parallel in groups (bounding memory) and accumulated in image order. */
    bands = pc_calloc_array(8, sizeof *bands);
    if (bands == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for blending");
        goto cleanup;
    }
    m.bands = bands;
    for (g0 = 0; g0 < n; g0 += 8) {
        gn = n - g0 < 8 ? n - g0 : 8;
        m.first = g0;
        memset(bands, 0, 8 * sizeof *bands);
        (void)pc_algo_par_for(gn, 1, band_chunk, &m);
        for (i = 0; i < gn && st == PC_OK; i++) {
            const band *b = &bands[i];
            if (b->st != PC_OK) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory for blending");
                break;
            }
            for (l = 0; l < levels; l++) {
                const band_level *bl = &b->lv[l];
                size_t ox = (size_t)(b->gx0 >> l), oy = (size_t)(b->gy0 >> l), pw = sizes[l][0],
                       ph = sizes[l][1];
                size_t x, y, c;
                for (y = 0; y < bl->h; y++) {
                    size_t py = oy + y;
                    if (py >= ph) {
                        break;
                    }
                    for (x = 0; x < bl->w; x++) {
                        size_t pxx = ox + x, q = y * bl->w + x, p;
                        float k;
                        if (pxx >= pw) {
                            break;
                        }
                        p = py * pw + pxx;
                        k = bl->gau[q];
                        if (k <= 0.0f) {
                            continue;
                        }
                        wsum[l][p] += k;
                        for (c = 0; c < ch; c++) {
                            acc[l][p * ch + c] += k * bl->lap[q * ch + c];
                        }
                    }
                }
            }
        }
        for (i = 0; i < gn; i++) {
            band_free(&bands[i], levels);
        }
        if (st != PC_OK) {
            goto cleanup;
        }
    }
    for (l = 0; l < levels; l++) {
        size_t p, c, np = sizes[l][0] * sizes[l][1];
        for (p = 0; p < np; p++) {
            float k = wsum[l][p];
            for (c = 0; c < ch; c++) {
                acc[l][p * ch + c] = k > 1e-8f ? acc[l][p * ch + c] / k : 0.0f;
            }
        }
    }
    /* Collapse. */
    cur = acc[levels - 1];
    acc[levels - 1] = NULL;
    for (l = levels - 1; l-- > 0;) {
        size_t q, np = sizes[l][0] * sizes[l][1] * ch;
        float *up = expand(sizes[l + 1][0], sizes[l + 1][1], ch, cur, sizes[l][0], sizes[l][1]);
        if (up == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for blending");
            goto cleanup;
        }
        for (q = 0; q < np; q++) {
            up[q] = acc[l][q] + up[q];
        }
        free(cur);
        cur = up;
    }
    /* Coverage: union of the images. */
    for (i = 0; i < n; i++) {
        const pc_algo_roi_image *im = &imgs[i];
        size_t x, y;
        for (y = 0; y < im->h; y++) {
            for (x = 0; x < im->w; x++) {
                int64_t px = (int64_t)im->x0 + (int64_t)x, py = (int64_t)im->y0 + (int64_t)y;
                size_t p;
                if (px < 0 || py < 0 || (size_t)px >= w || (size_t)py >= h) {
                    continue;
                }
                p = (size_t)py * w + (size_t)px;
                cov[p] = pc_rs_maxf(cov[p], im->alpha[y * im->w + x]);
            }
        }
    }
    *out_px = cur;
    *out_cov = cov;
    cur = NULL;
    cov = NULL;
cleanup:
    if (acc != NULL) {
        for (l = 0; l < levels; l++) {
            free(acc[l]);
        }
    }
    if (wsum != NULL) {
        for (l = 0; l < levels; l++) {
            free(wsum[l]);
        }
    }
    free(acc), free(wsum), free(sizes), free(cur), free(cov), free(bands);
    return st;
}
