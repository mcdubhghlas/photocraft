/* SLIC superpixels (port of crates/algo/src/segment/slic.rs).
 *
 * Achanta et al., "SLIC Superpixels Compared to State-of-the-Art Superpixel Methods", PAMI 2012:
 * k-means in CIELAB + xy restricted to a 2S x 2S window per centre, centres seeded on a grid and
 * moved to the lowest gradient in their 3x3 neighbourhood, then connectivity enforcement. */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/segment.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

static float d2(const float a[3], const float b[3])
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

static float lin(float v)
{
    return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

static float labf(float t)
{
    return t > 0.008856f ? pc_cbrtf(t) : 7.787f * t + 16.0f / 116.0f;
}

void pc_algo_slic_rgb_to_lab(const float c[3], float out[3])
{
    float r = lin(c[0]), g = lin(c[1]), b = lin(c[2]);
    float x = (0.4124564f * r + 0.3575761f * g + 0.1804375f * b) / 0.95047f;
    float y = 0.2126729f * r + 0.7151522f * g + 0.072175f * b;
    float z = (0.0193339f * r + 0.119192f * g + 0.9503041f * b) / 1.08883f;
    float fx = labf(x), fy = labf(y), fz = labf(z);
    out[0] = 116.0f * fy - 16.0f;
    out[1] = 500.0f * (fx - fy);
    out[2] = 200.0f * (fy - fz);
}

void pc_algo_superpixels_fini(pc_algo_superpixels *sp)
{
    if (sp == NULL) {
        return;
    }
    free(sp->labels);
    sp->labels = NULL;
    sp->count = 0;
}

/* Relabels 4-connected components; components smaller than min_size merge into the adjacent
 * component found before them. */
static pc_status enforce_connectivity(const uint32_t *labels, size_t w, size_t h, size_t min_size,
                                      uint32_t **out_p, size_t *count, pc_error *err)
{
    size_t n = w * h, s, *comp, nc, k;
    uint32_t next = 0;
    uint32_t *out = pc_malloc_array(n ? n : 1, sizeof(uint32_t));
    comp = pc_malloc_array(n ? n : 1, sizeof(size_t));
    if (out == NULL || comp == NULL) {
        free(out);
        free(comp);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for superpixels");
    }
    for (s = 0; s < n; s++) {
        out[s] = UINT32_MAX;
    }
    for (s = 0; s < n; s++) {
        size_t sx = s % w, sy = s / w;
        bool has_adj = false;
        uint32_t adj = 0;
        if (out[s] != UINT32_MAX) {
            continue;
        }
        /* Adjacent already-labelled component (to merge into if this one is small). */
        if (sx > 0 && out[s - 1] != UINT32_MAX) {
            adj = out[s - 1];
            has_adj = true;
        }
        if (sy > 0 && out[s - w] != UINT32_MAX) {
            adj = out[s - w];
            has_adj = true;
        }
        nc = 0;
        comp[nc++] = s;
        out[s] = next;
        for (k = 0; k < nc; k++) {
            size_t i = comp[k], x = i % w, y = i / w, nb[4], cnt = 0, j;
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
            for (j = 0; j < cnt; j++) {
                if (out[nb[j]] == UINT32_MAX && labels[nb[j]] == labels[s]) {
                    out[nb[j]] = next;
                    comp[nc++] = nb[j];
                }
            }
        }
        if (has_adj && nc < min_size) {
            for (k = 0; k < nc; k++) {
                out[comp[k]] = adj;
            }
        } else {
            next++;
        }
    }
    free(comp);
    *out_p = out;
    *count = next;
    return PC_OK;
}

pc_status pc_algo_slic_lab(const float (*lab)[3], size_t w, size_t h, size_t target, float compactness,
                           size_t iters, pc_algo_superpixels *out, pc_error *err)
{
    size_t n, nc = 0, ccap = 0, i, it, k;
    float s, wxy, yf, q;
    int64_t r;
    float (*centers)[5] = NULL;
    uint32_t *labels = NULL, *final_labels = NULL;
    float *dist = NULL;
    double (*sums)[6] = NULL;
    size_t count = 0;
    pc_status st = PC_OK;
    if (!pc_size_mul(w, h, &n) || n > PC_MAX_ALLOC / sizeof(float[4])) {
        return pc_fail(err, PC_ERR_LIMIT, "image too large for superpixels");
    }
    if (n == 0) {
        out->w = w;
        out->h = h;
        out->labels = NULL;
        out->count = 0;
        out->step = 1.0f;
        return PC_OK;
    }
    s = pc_rs_maxf_k(sqrtf((float)n / (float)(target > 1 ? target : 1)), 1.0f);
#define GRAD(gx, gy)                                                                                         \
    (d2(lab[(gy < h - 1 ? gy : h - 1) * w + (gx > 0 ? gx - 1 : 0)],                                          \
        lab[(gy < h - 1 ? gy : h - 1) * w + (gx + 1 < w - 1 ? gx + 1 : w - 1)]) +                            \
     d2(lab[(gy > 0 ? gy - 1 : 0) * w + (gx < w - 1 ? gx : w - 1)],                                          \
        lab[(gy + 1 < h - 1 ? gy + 1 : h - 1) * w + (gx < w - 1 ? gx : w - 1)]))
    /* Seed centres: [L, a, b, x, y]. */
    for (yf = s / 2.0f; yf < (float)h; yf += s) {
        float xf;
        for (xf = s / 2.0f; xf < (float)w; xf += s) {
            size_t cx = pc_f32_to_size(xf), cy = pc_f32_to_size(yf), bx = cx, by = cy, xx, yy;
            float bg = FLT_MAX;
            for (yy = cy > 0 ? cy - 1 : 0; yy <= (cy + 1 < h - 1 ? cy + 1 : h - 1); yy++) {
                for (xx = cx > 0 ? cx - 1 : 0; xx <= (cx + 1 < w - 1 ? cx + 1 : w - 1); xx++) {
                    float g = GRAD(xx, yy);
                    if (g < bg) {
                        bx = xx;
                        by = yy;
                        bg = g;
                    }
                }
            }
            if (nc == ccap) {
                void *gp = pc_grow_array(centers, &ccap, nc + 1, sizeof *centers);
                if (gp == NULL) {
                    st = pc_fail(err, PC_ERR_NOMEM, "out of memory for superpixels");
                    goto done;
                }
                centers = gp;
            }
            centers[nc][0] = lab[by * w + bx][0];
            centers[nc][1] = lab[by * w + bx][1];
            centers[nc][2] = lab[by * w + bx][2];
            centers[nc][3] = (float)bx;
            centers[nc][4] = (float)by;
            nc++;
        }
    }
#undef GRAD
    labels = pc_malloc_array(n, sizeof(uint32_t));
    dist = pc_malloc_array(n, sizeof(float));
    sums = pc_malloc_array(nc ? nc : 1, sizeof *sums);
    if (labels == NULL || dist == NULL || sums == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for superpixels");
        goto done;
    }
    for (i = 0; i < n; i++) {
        labels[i] = UINT32_MAX;
        dist[i] = FLT_MAX;
    }
    q = compactness / s;
    wxy = q * q;
    r = pc_f32_to_i64(ceilf(s));
    for (it = 0; it < (iters > 1 ? iters : 1); it++) {
        for (i = 0; i < n; i++) {
            dist[i] = FLT_MAX;
        }
        for (k = 0; k < nc; k++) {
            const float *c = centers[k];
            int64_t cx = pc_f32_to_i64(roundf(c[3])), cy = pc_f32_to_i64(roundf(c[4]));
            int64_t y0 = cy - r > 0 ? cy - r : 0, y1 = cy + r + 1 < (int64_t)h ? cy + r + 1 : (int64_t)h;
            int64_t x0 = cx - r > 0 ? cx - r : 0, x1 = cx + r + 1 < (int64_t)w ? cx + r + 1 : (int64_t)w;
            int64_t yy, xx;
            for (yy = y0; yy < y1; yy++) {
                for (xx = x0; xx < x1; xx++) {
                    size_t ii = (size_t)yy * w + (size_t)xx;
                    float dc = d2(lab[ii], c), dx = (float)xx - c[3], dy = (float)yy - c[4];
                    float d = dc + (dx * dx + dy * dy) * wxy;
                    if (d < dist[ii]) {
                        dist[ii] = d;
                        labels[ii] = (uint32_t)k;
                    }
                }
            }
        }
        memset(sums, 0, (nc ? nc : 1) * sizeof *sums);
        for (i = 0; i < n; i++) {
            double *sm;
            if (labels[i] == UINT32_MAX) {
                continue;
            }
            sm = sums[labels[i]];
            sm[0] += (double)lab[i][0];
            sm[1] += (double)lab[i][1];
            sm[2] += (double)lab[i][2];
            sm[3] += (double)(i % w);
            sm[4] += (double)(i / w);
            sm[5] += 1.0;
        }
        for (k = 0; k < nc; k++) {
            if (sums[k][5] > 0.0) {
                int j;
                for (j = 0; j < 5; j++) {
                    centers[k][j] = pc_f64_to_f32(sums[k][j] / sums[k][5]);
                }
            }
        }
    }
    /* Unreached pixels (possible at the far edges): the label of the previous pixel. */
    for (i = 0; i < n; i++) {
        if (labels[i] == UINT32_MAX) {
            labels[i] = i > 0 ? labels[i - 1] : 0;
        }
    }
    st = enforce_connectivity(labels, w, h, pc_f32_to_size((s * s) / 4.0f), &final_labels, &count, err);
    if (st != PC_OK) {
        goto done;
    }
    out->w = w;
    out->h = h;
    out->labels = final_labels;
    out->count = count;
    out->step = s;
done:
    free(centers);
    free(labels);
    free(dist);
    free(sums);
    return st;
}

pc_status pc_algo_slic(const pc_algo_rgb_image *img, size_t target, float compactness, size_t iters,
                       pc_algo_superpixels *out, pc_error *err)
{
    size_t n = img->w * img->h, i;
    float (*lab)[3] = pc_malloc_array(n ? n : 1, sizeof *lab);
    pc_status st;
    if (lab == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for superpixels");
    }
    for (i = 0; i < n; i++) {
        pc_algo_slic_rgb_to_lab(img->px[i], lab[i]);
    }
    st = pc_algo_slic_lab((const float (*)[3])lab, img->w, img->h, target, compactness, iters, out, err);
    free(lab);
    return st;
}
