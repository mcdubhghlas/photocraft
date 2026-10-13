/* Content-Aware Fill's Auto sampling area (content_aware::auto_sampling); see content_aware.h.
 * Each cell (pixels box-averaged at a reduced scale for big windows) is described by its local mean
 * colour and local contrast (7 x 7); the ring around the hole is summarised by a few k-means
 * clusters; a cell near a cluster prefers to be sampled, one far from all of them not, and one
 * contrast-sensitive min cut settles the area. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/content_aware.h"
#include "algo/inpaint_int.h"
#include "algo/poisson.h"
#include "algo/segment.h"
#include "algo/selection.h"
#include "base/mem.h"
#include "base/rmath.h"

/* Largest side the area is worked out at: bigger windows are segmented at a reduced scale. */
#define AUTO_SIDE ((size_t)384)
#define NF_MAX 4

/* Box mean of radius r over a w x h plane (edge-clamped), by summed areas in f64. */
static float *box_mean(size_t w, size_t h, const float *v, size_t r)
{
    size_t w1 = w + 1, x, y;
    double *s = pc_calloc_array(w1 * (h + 1), sizeof *s);
    float *out = pc_malloc_array(w * h, sizeof *out);
    if (s == NULL || out == NULL) {
        free(s);
        free(out);
        return NULL;
    }
    for (y = 0; y < h; y++) {
        double row = 0.0;
        for (x = 0; x < w; x++) {
            row += (double)v[y * w + x];
            s[(y + 1) * w1 + x + 1] = s[y * w1 + x + 1] + row;
        }
    }
    for (y = 0; y < h; y++) {
        size_t y0 = y > r ? y - r : 0, y1 = y + r + 1 < h ? y + r + 1 : h;
        for (x = 0; x < w; x++) {
            size_t x0 = x > r ? x - r : 0, x1 = x + r + 1 < w ? x + r + 1 : w;
            double sum = s[y1 * w1 + x1] + s[y0 * w1 + x0] - s[y0 * w1 + x1] - s[y1 * w1 + x0];
            out[y * w + x] = (float)(sum / (double)((x1 - x0) * (y1 - y0)));
        }
    }
    free(s);
    return out;
}

static float cut_weight(void *ctx, float d2)
{
    (void)ctx;
    return 8.0f * expf(-d2 / (2.0f * 0.05f * 0.05f)) + 0.05f;
}

typedef struct feats {
    size_t nc;
    float *means[3];
    float *contrast;
} feats;

static void feat(const feats *f, size_t i, float *out)
{
    size_t c;
    for (c = 0; c < f->nc; c++) {
        out[c] = f->means[c][i];
    }
    out[f->nc] = 2.0f * f->contrast[i];
}

static float d2(const float *a, const float *b, size_t n)
{
    float s = -0.0f;
    size_t i;
    for (i = 0; i < n; i++) {
        s += (a[i] - b[i]) * (a[i] - b[i]);
    }
    return s;
}

/* f32::total_cmp(a, b) < 0. */
static bool total_lt(float a, float b)
{
    int32_t ia, ib;
    memcpy(&ia, &a, 4);
    memcpy(&ib, &b, 4);
    ia ^= (int32_t)((uint32_t)(ia >> 31) >> 1);
    ib ^= (int32_t)((uint32_t)(ib >> 31) >> 1);
    return ia < ib;
}

static float nearest(const feats *f, size_t i, float (*centres)[NF_MAX], size_t kc, float *buf)
{
    float m = INFINITY;
    size_t j;
    feat(f, i, buf);
    for (j = 0; j < kc; j++) {
        m = pc_rs_minf(m, d2(buf, centres[j], f->nc + 1));
    }
    return sqrtf(m);
}

/* everything outside the hole */
static pc_status everything(const bool *hole, size_t n, bool **out, pc_error *err)
{
    size_t i;
    bool *o = pc_malloc_array(n > 0 ? n : 1, sizeof *o);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < n; i++) {
        o[i] = !hole[i];
    }
    *out = o;
    return PC_OK;
}

pc_status pc_algo_ca_auto_sampling(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                   const bool *hole, size_t hole_len, const pc_interrupt *ctl, bool **out,
                                   pc_error *err)
{
    size_t n, i, k, sw, sh, sn, nc, nf, x, y, c, j, it, nring = 0, kc, q, n_hole = 0, n_out = 0;
    float *sums = NULL, *planes[3] = {NULL, NULL, NULL}, *luma = NULL, *luma2 = NULL, *lm = NULL, *lm2 = NULL;
    float *dist = NULL, *spread = NULL, *cost_fg = NULL, *cost_bg = NULL, ring_w, tau, s90;
    float centres[6][NF_MAX], fbuf[NF_MAX];
    bool *shole = NULL, *area = NULL, *o = NULL, any = false;
    size_t *ring = NULL;
    uint8_t *fixed = NULL;
    feats f;
    pc_algo_rgb_image rgb = {0, 0, NULL};
    pc_status st = pc_algo_heal_dims(w, h, ch, len, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (hole_len != n || (n > 0 && (img == NULL || hole == NULL))) {
        return pc_fail(err, PC_ERR_INVALID, "hole mask holds %zu entries, expected %zu", hole_len, n);
    }
    for (i = 0; i < n && !any; i++) {
        any = hole[i];
    }
    if (n == 0 || !any) {
        return everything(hole, n, out, err);
    }
    memset(&f, 0, sizeof f);
    /* Work at a reduced scale for big windows. */
    k = ((w > h ? w : h) + AUTO_SIDE - 1) / AUTO_SIDE;
    k = k > 1 ? k : 1;
    sw = (w + k - 1) / k, sh = (h + k - 1) / k, sn = sw * sh;
    nc = ch < 3 ? ch : 3;
    nf = nc + 1;
    f.nc = nc;
    /* Per cell: the sums of its colour planes, its pixel count and whether it holds the hole. */
    sums = pc_calloc_array(sn, (nc + 2) * sizeof *sums);
    shole = pc_malloc_array(sn, sizeof *shole);
    if (sums == NULL || shole == NULL) {
        goto nomem;
    }
    for (y = 0; y < h; y++) {
        if (y % k == 0 && pc_interrupt_cancelled(ctl)) {
            break;
        }
        for (x = 0; x < w; x++) {
            float *cell = &sums[((y / k) * sw + x / k) * (nc + 2)];
            size_t p = y * w + x;
            if (hole[p]) {
                cell[nc + 1] = 1.0f;
            }
            for (c = 0; c < nc; c++) {
                cell[c] += img[p * ch + c];
            }
            cell[nc] += 1.0f;
        }
    }
    if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
        goto cleanup;
    }
    for (c = 0; c < nc; c++) {
        planes[c] = pc_malloc_array(sn, sizeof(float));
        if (planes[c] == NULL) {
            goto nomem;
        }
        for (i = 0; i < sn; i++) {
            planes[c][i] = sums[i * (nc + 2) + c] / pc_rs_maxf_k(sums[i * (nc + 2) + nc], 1.0f);
        }
    }
    for (i = 0; i < sn; i++) {
        shole[i] = sums[i * (nc + 2) + nc + 1] > 0.0f;
    }
    free(sums);
    sums = NULL;
    /* Features: local means and local contrast (of the average of the colour planes). */
    luma = pc_malloc_array(sn, sizeof *luma);
    luma2 = pc_malloc_array(sn, sizeof *luma2);
    f.contrast = pc_malloc_array(sn, sizeof *f.contrast);
    if (luma == NULL || luma2 == NULL || f.contrast == NULL) {
        goto nomem;
    }
    for (c = 0; c < nc; c++) {
        if ((f.means[c] = box_mean(sw, sh, planes[c], 3)) == NULL) {
            goto nomem;
        }
    }
    for (i = 0; i < sn; i++) {
        float s = -0.0f;
        for (c = 0; c < nc; c++) {
            s += planes[c][i];
        }
        luma[i] = s / (float)nc;
        luma2[i] = luma[i] * luma[i];
    }
    lm = box_mean(sw, sh, luma, 3);
    lm2 = box_mean(sw, sh, luma2, 3);
    if (lm == NULL || lm2 == NULL) {
        goto nomem;
    }
    for (i = 0; i < sn; i++) {
        f.contrast[i] = sqrtf(pc_rs_maxf_k(lm2[i] - lm[i] * lm[i], 0.0f));
    }
    /* The ring: outside the hole, within a few pixels of it. */
    if ((st = pc_algo_selection_edt(shole, sw, sh, &dist, err)) != PC_OK) {
        goto cleanup;
    }
    ring_w = 6.0f / (float)k + 1.0f;
    ring = pc_malloc_array(sn, sizeof *ring);
    if (ring == NULL) {
        goto nomem;
    }
    for (i = 0; i < sn; i++) {
        if (!shole[i] && dist[i] <= ring_w) {
            ring[nring++] = i;
        }
    }
    if (nring == 0) {
        st = everything(hole, n, out, err);
        goto cleanup;
    }
    if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
        goto cleanup;
    }
    /* A few cluster centres of the ring's features (k-means, deterministic start). */
    kc = nring < 6 ? nring : 6;
    for (j = 0; j < kc; j++) {
        feat(&f, ring[j * nring / kc], centres[j]);
    }
    for (it = 0; it < 8; it++) {
        float acc[6][NF_MAX];
        size_t cnt[6];
        memset(acc, 0, sizeof acc);
        memset(cnt, 0, sizeof cnt);
        for (q = 0; q < nring; q++) {
            size_t bj = 0;
            float bd;
            feat(&f, ring[q], fbuf);
            bd = d2(fbuf, centres[0], nf);
            for (j = 1; j < kc; j++) {
                float dj = d2(fbuf, centres[j], nf);
                if (total_lt(dj, bd)) {
                    bj = j, bd = dj;
                }
            }
            for (c = 0; c < nf; c++) {
                acc[bj][c] += fbuf[c];
            }
            cnt[bj]++;
        }
        for (j = 0; j < kc; j++) {
            if (cnt[j] > 0) {
                for (c = 0; c < nf; c++) {
                    centres[j][c] = acc[j][c] / (float)cnt[j];
                }
            }
        }
    }
    /* How much the ring itself varies around its clusters sets the scale of "similar". */
    spread = pc_malloc_array(nring, sizeof *spread);
    if (spread == NULL) {
        goto nomem;
    }
    for (q = 0; q < nring; q++) {
        spread[q] = nearest(&f, ring[q], centres, kc, fbuf);
    }
    if ((st = pc_ip_nth_total(spread, nring, nring * 9 / 10 < nring - 1 ? nring * 9 / 10 : nring - 1, &s90, err)) !=
        PC_OK) {
        goto cleanup;
    }
    tau = pc_rs_maxf_k(s90 * 1.6f, 0.03f);
    cost_fg = pc_calloc_array(sn, sizeof *cost_fg);
    cost_bg = pc_calloc_array(sn, sizeof *cost_bg);
    fixed = pc_calloc_array(sn, sizeof *fixed);
    area = pc_malloc_array(sn, sizeof *area);
    if (cost_fg == NULL || cost_bg == NULL || fixed == NULL || area == NULL) {
        goto nomem;
    }
    for (i = 0; i < sn; i++) {
        float d;
        if (shole[i] || dist[i] <= 1.5f) {
            fixed[i] = PC_ALGO_SEGMENT_HARD_FG;
            continue;
        }
        d = nearest(&f, i, centres, kc, fbuf) / tau;
        cost_fg[i] = d * d;
        cost_bg[i] = 1.0f;
    }
    if ((st = pc_algo_rgb_image_init(&rgb, sw, sh, err)) != PC_OK) {
        goto cleanup;
    }
    for (i = 0; i < sn; i++) {
        for (c = 0; c < 3; c++) {
            rgb.px[i][c] = f.means[c < nc - 1 ? c : nc - 1][i];
        }
    }
    if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
        goto cleanup;
    }
    /* Contrast-sensitive smoothness, fitted to Photoshop's Auto overlays. */
    if ((st = pc_algo_segment_grid_cut_with(&rgb, cost_fg, cost_bg, fixed, cut_weight, NULL, ctl, area, err)) !=
        PC_OK) {
        goto cleanup;
    }
    o = pc_malloc_array(n, sizeof *o);
    if (o == NULL) {
        goto nomem;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            o[y * w + x] = !hole[y * w + x] && area[(y / k) * sw + x / k];
        }
    }
    for (i = 0; i < n; i++) {
        n_hole += hole[i] ? 1u : 0u;
        n_out += o[i] ? 1u : 0u;
    }
    /* Too little to copy from (fewer source pixels than twice the hole): sample everything. */
    if (n_out < 2 * (n_hole > 64 ? n_hole : 64)) {
        free(o);
        o = NULL;
        st = everything(hole, n, out, err);
        goto cleanup;
    }
    *out = o;
    o = NULL;
    goto cleanup;
nomem:
    st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
cleanup:
    free(sums);
    for (c = 0; c < 3; c++) {
        free(planes[c]);
        free(f.means[c]);
    }
    free(f.contrast);
    free(luma);
    free(luma2);
    free(lm);
    free(lm2);
    free(dist);
    free(spread);
    free(cost_fg);
    free(cost_bg);
    free(fixed);
    free(area);
    free(o);
    free(ring);
    free(shole);
    pc_algo_rgb_image_fini(&rgb);
    return st;
}
