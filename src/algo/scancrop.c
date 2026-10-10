/* Crop and Straighten Photos (port of crates/algo/src/scancrop.rs). */
#include "algo/scancrop.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define DEG2RAD (3.141592653589793 / 180.0)
#define RAD2DEG (180.0 / 3.141592653589793)

void pc_algo_found_photo_corners(const pc_algo_found_photo *p, double out[4][2])
{
    double a = p->angle * DEG2RAD, s = sin(a), c = cos(a), hw = p->width / 2.0, hh = p->height / 2.0;
    const double xy[4][2] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
    int i;
    for (i = 0; i < 4; i++) {
        out[i][0] = p->center[0] + c * xy[i][0] - s * xy[i][1];
        out[i][1] = p->center[1] + s * xy[i][0] + c * xy[i][1];
    }
}

static int64_t total_key64(double f)
{
    uint64_t u;
    int64_t i;
    memcpy(&u, &f, sizeof u);
    i = u > (uint64_t)INT64_MAX ? -(int64_t)(~u) - 1 : (int64_t)u; /* bit-cast without UB */
    return i ^ (int64_t)((uint64_t)(i >> 63) >> 1);
}

static int32_t total_key32(float f)
{
    uint32_t u;
    int32_t i;
    memcpy(&u, &f, sizeof u);
    i = pc_u32_as_i32(u);
    return i ^ pc_u32_as_i32((uint32_t)(i >> 31) >> 1);
}

static int cmp_total32(const void *pa, const void *pb)
{
    int32_t a = total_key32(*(const float *)pa), b = total_key32(*(const float *)pb);
    return a < b ? -1 : (a > b ? 1 : 0);
}

static float otsu(const float *v, size_t n)
{
    double hist[256] = {0}, total = -0.0, sum = -0.0, wb = 0.0, sb = 0.0, best = 0.0;
    size_t i, thr = 0;
    for (i = 0; i < n; i++) {
        hist[pc_f32_to_size(pc_clampf(v[i], 0.0f, 1.0f) * 255.0f)] += 1.0;
    }
    for (i = 0; i < 256; i++) {
        total += hist[i];
    }
    for (i = 0; i < 256; i++) {
        sum += (double)i * hist[i];
    }
    for (i = 0; i < 256; i++) {
        double wf, mb, mf, between;
        wb += hist[i];
        if (wb == 0.0) {
            continue;
        }
        wf = total - wb;
        if (wf == 0.0) {
            break;
        }
        sb += (double)i * hist[i];
        mb = sb / wb;
        mf = (sum - sb) / wf;
        between = wb * wf * (mb - mf) * (mb - mf);
        if (between > best) {
            best = between;
            thr = i;
        }
    }
    return ((float)thr + 0.5f) / 255.0f;
}

/* One separable pass of a square structuring element (outside the image counts as false). */
static void morph_pass(size_t w, size_t h, const bool *src, bool *out, int64_t r, bool dilate,
                       bool horizontal)
{
    size_t x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            bool acc = !dilate;
            int64_t d;
            for (d = -r; d <= r; d++) {
                int64_t xx = horizontal ? (int64_t)x + d : (int64_t)x;
                int64_t yy = horizontal ? (int64_t)y : (int64_t)y + d;
                bool v = !(xx < 0 || yy < 0 || xx >= (int64_t)w || yy >= (int64_t)h) &&
                         src[(size_t)yy * w + (size_t)xx];
                if (dilate) {
                    acc = acc || v;
                } else {
                    acc = acc && v;
                }
            }
            out[y * w + x] = acc;
        }
    }
}

/* m = morph(m): horizontal into tmp, vertical back into m. */
static void morph(size_t w, size_t h, bool *m, bool *tmp, int64_t r, bool dilate)
{
    morph_pass(w, h, m, tmp, r, dilate, true);
    morph_pass(w, h, tmp, m, r, dilate, false);
}

static int cmp_pt(const void *pa, const void *pb)
{
    const double *a = pa, *b = pb;
    int64_t ka = total_key64(a[0]), kb = total_key64(b[0]);
    if (ka != kb) {
        return ka < kb ? -1 : 1;
    }
    ka = total_key64(a[1]), kb = total_key64(b[1]);
    return ka < kb ? -1 : (ka > kb ? 1 : 0);
}

static double cross(const double *o, const double *a, const double *b)
{
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}

/* Andrew's monotone chain; pts (np, reordered) -> hull (at most np + 1 entries); returns its size. */
static size_t convex_hull(double (*pts)[2], size_t np, double (*hull)[2], double (*upper)[2])
{
    size_t n = 0, i, nl = 0, nu = 0;
    if (np > 1) {
        qsort(pts, np, sizeof *pts, cmp_pt);
    }
    for (i = 0; i < np; i++) { /* dedup (consecutive, by value) */
        if (n == 0 || !(pts[n - 1][0] == pts[i][0] && pts[n - 1][1] == pts[i][1])) {
            pts[n][0] = pts[i][0], pts[n][1] = pts[i][1];
            n++;
        }
    }
    if (n < 3) {
        memcpy(hull, pts, n * sizeof *pts);
        return n;
    }
    for (i = 0; i < n; i++) {
        while (nl >= 2 && cross(hull[nl - 2], hull[nl - 1], pts[i]) <= 0.0) {
            nl--;
        }
        hull[nl][0] = pts[i][0], hull[nl][1] = pts[i][1];
        nl++;
    }
    for (i = n; i-- > 0;) {
        while (nu >= 2 && cross(upper[nu - 2], upper[nu - 1], pts[i]) <= 0.0) {
            nu--;
        }
        upper[nu][0] = pts[i][0], upper[nu][1] = pts[i][1];
        nu++;
    }
    nl--;
    nu--;
    memcpy(hull + nl, upper, nu * sizeof *upper);
    return nl + nu;
}

/* Minimum-area rectangle around a convex hull: centre, width, height, angle (degrees). */
static void min_rect(const double (*hull)[2], size_t n, double center[2], double *w, double *h, double *ang)
{
    double best_area = 1.7976931348623157e308, bc[2] = {0.0, 0.0}, bw = 0.0, bh = 0.0, ba = 0.0, t;
    size_t i, j;
    for (i = 0; i < n; i++) {
        const double *a = hull[i], *b = hull[(i + 1) % n];
        double th = atan2(b[1] - a[1], b[0] - a[0]), s = sin(th), c = cos(th);
        double x0 = 1.7976931348623157e308, y0 = x0, x1 = -x0, y1 = -x0, area;
        for (j = 0; j < n; j++) {
            double u = c * hull[j][0] + s * hull[j][1], v = -s * hull[j][0] + c * hull[j][1];
            x0 = pc_rs_min(x0, u);
            x1 = pc_rs_max(x1, u);
            y0 = pc_rs_min(y0, v);
            y1 = pc_rs_max(y1, v);
        }
        area = (x1 - x0) * (y1 - y0);
        if (area < best_area) {
            double cu = (x0 + x1) / 2.0, cv = (y0 + y1) / 2.0;
            best_area = area;
            bc[0] = c * cu - s * cv;
            bc[1] = s * cu + c * cv;
            bw = x1 - x0;
            bh = y1 - y0;
            ba = th * RAD2DEG;
        }
    }
    /* Normalise to -45..45, swapping the sides when turning by 90 degrees. */
    while (ba > 45.0) {
        ba -= 90.0;
        t = bw, bw = bh, bh = t;
    }
    while (ba < -45.0) {
        ba += 90.0;
        t = bw, bw = bh, bh = t;
    }
    center[0] = bc[0], center[1] = bc[1];
    *w = bw, *h = bh, *ang = ba;
}

static double band_key(const pc_algo_found_photo *p, double band)
{
    return floor(p->center[1] / band);
}

pc_status pc_algo_scancrop_find_photos(size_t w, size_t h, const float (*rgba)[4], size_t n,
                                       pc_algo_found_photo **out, size_t *count, pc_error *err)
{
    size_t k, sw, sh, ns, x, y, i, c, nborder = 0, ncomp = 0, nres = 0, *order = NULL, *comp_end = NULL;
    size_t total_px;
    float (*small)[3] = NULL, *border = NULL, bg[3], *dist = NULL, thr;
    uint32_t *cnt = NULL, *label = NULL;
    bool *mask = NULL, *tmp = NULL;
    double (*pts)[2] = NULL, (*hull)[2] = NULL, (*upper)[2] = NULL, total, band;
    pc_algo_found_photo *res = NULL;
    int64_t r;
    pc_status st = PC_OK;
    if (w < 8 || h < 8) {
        *out = NULL;
        *count = 0;
        return PC_OK;
    }
    if (!pc_size_mul(w, h, &total_px) || n < total_px || rgba == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "scan buffer holds fewer than %zux%zu pixels", w, h);
    }
    /* Work on <= 800 px. */
    k = ((w > h ? w : h) + 799) / 800;
    k = k > 0 ? k : 1;
    sw = (w + k - 1) / k;
    sh = (h + k - 1) / k;
    ns = sw * sh;
    small = pc_calloc_array(ns, sizeof *small);
    cnt = pc_calloc_array(ns, sizeof *cnt);
    border = pc_malloc_array(2 * (sw + sh), sizeof(float[4])); /* 3 channels + one scratch value */
    dist = pc_malloc_array(ns, sizeof *dist);
    mask = pc_malloc_array(ns, sizeof *mask);
    tmp = pc_malloc_array(ns, sizeof *tmp);
    label = pc_malloc_array(ns, sizeof *label);
    order = pc_malloc_array(ns, sizeof *order);
    comp_end = pc_malloc_array(ns, sizeof *comp_end);
    if (small == NULL || cnt == NULL || border == NULL || dist == NULL || mask == NULL || tmp == NULL ||
        label == NULL || order == NULL || comp_end == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory finding photos");
        goto cleanup;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t q = (y / k) * sw + x / k;
            const float *p = rgba[y * w + x];
            for (c = 0; c < 3; c++) {
                small[q][c] += p[c];
            }
            cnt[q] += 1;
        }
    }
    for (i = 0; i < ns; i++) {
        for (c = 0; c < 3; c++) {
            small[i][c] /= (float)(cnt[i] > 0 ? cnt[i] : 1);
        }
    }
    /* Background = per-channel median of the border. */
    {
        float (*bd)[3] = (float (*)[3])border, *sc = border + 6 * (sw + sh);
        for (x = 0; x < sw; x++) {
            memcpy(bd[nborder++], small[x], sizeof bd[0]);
            memcpy(bd[nborder++], small[(sh - 1) * sw + x], sizeof bd[0]);
        }
        for (y = 0; y < sh; y++) {
            memcpy(bd[nborder++], small[y * sw], sizeof bd[0]);
            memcpy(bd[nborder++], small[y * sw + sw - 1], sizeof bd[0]);
        }
        for (c = 0; c < 3; c++) {
            for (i = 0; i < nborder; i++) {
                sc[i] = bd[i][c];
            }
            qsort(sc, nborder, sizeof *sc, cmp_total32);
            bg[c] = sc[nborder / 2];
        }
    }
    for (i = 0; i < ns; i++) {
        float d0 = small[i][0] - bg[0], d1 = small[i][1] - bg[1], d2 = small[i][2] - bg[2];
        dist[i] = sqrtf(d0 * d0 + d1 * d1 + d2 * d2) / sqrtf(3.0f);
    }
    thr = pc_rs_maxf_k(otsu(dist, ns), 0.04f);
    for (i = 0; i < ns; i++) {
        mask[i] = dist[i] > thr;
    }
    r = (int64_t)((sw > sh ? sw : sh) / 200);
    r = r > 1 ? r : 1;
    morph(sw, sh, mask, tmp, r, true);
    morph(sw, sh, mask, tmp, r, false);
    morph(sw, sh, mask, tmp, 1, false);
    morph(sw, sh, mask, tmp, 1, true);
    /* Connected components (8-connected), depth-first with an explicit stack; `order` collects
     * each component's pixels in visiting order. */
    {
        size_t *stack = (size_t *)NULL, nord = 0;
        stack = pc_malloc_array(ns, sizeof *stack);
        if (stack == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory finding photos");
            goto cleanup;
        }
        for (i = 0; i < ns; i++) {
            label[i] = UINT32_MAX;
        }
        for (i = 0; i < ns; i++) {
            size_t nst = 0;
            uint32_t id;
            if (!mask[i] || label[i] != UINT32_MAX) {
                continue;
            }
            id = (uint32_t)ncomp;
            stack[nst++] = i;
            label[i] = id;
            while (nst > 0) {
                size_t q = stack[--nst];
                int64_t qx = (int64_t)(q % sw), qy = (int64_t)(q / sw), dy, dx;
                order[nord++] = q;
                for (dy = -1; dy <= 1; dy++) {
                    for (dx = -1; dx <= 1; dx++) {
                        int64_t xx = qx + dx, yy = qy + dy;
                        size_t j;
                        if (xx < 0 || yy < 0 || xx >= (int64_t)sw || yy >= (int64_t)sh) {
                            continue;
                        }
                        j = (size_t)yy * sw + (size_t)xx;
                        if (mask[j] && label[j] == UINT32_MAX) {
                            label[j] = id;
                            stack[nst++] = j;
                        }
                    }
                }
            }
            comp_end[ncomp++] = nord;
        }
        free(stack);
    }
    total = (double)ns;
    res = pc_malloc_array(ncomp > 0 ? ncomp : 1, sizeof *res);
    if (res == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory finding photos");
        goto cleanup;
    }
    for (c = 0; c < ncomp; c++) {
        size_t start = c > 0 ? comp_end[c - 1] : 0, len = comp_end[c] - start, np = 0, nh;
        double center[2], wd, ht, ang, inset = (double)k;
        if (!((double)len > total * 0.01)) {
            continue;
        }
        free(pts), free(hull), free(upper);
        pts = pc_malloc_array(4 * len, sizeof *pts);
        hull = pc_malloc_array(4 * len + 1, sizeof *hull);
        upper = pc_malloc_array(4 * len + 1, sizeof *upper);
        if (pts == NULL || hull == NULL || upper == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory finding photos");
            goto cleanup;
        }
        /* Hull of the boundary pixels' corners (in full-resolution px). */
        for (i = start; i < comp_end[c]; i++) {
            size_t q = order[i], qx = q % sw, qy = q / sw;
            double fx, fy;
            if (!(qx == 0 || qy == 0 || qx + 1 == sw || qy + 1 == sh || !mask[q - 1] || !mask[q + 1] ||
                  !mask[q - sw] || !mask[q + sw])) {
                continue;
            }
            fx = (double)qx, fy = (double)qy;
            pts[np][0] = fx * (double)k, pts[np][1] = fy * (double)k, np++;
            pts[np][0] = (fx + 1.0) * (double)k, pts[np][1] = fy * (double)k, np++;
            pts[np][0] = fx * (double)k, pts[np][1] = (fy + 1.0) * (double)k, np++;
            pts[np][0] = (fx + 1.0) * (double)k, pts[np][1] = (fy + 1.0) * (double)k, np++;
        }
        nh = convex_hull(pts, np, hull, upper);
        min_rect((const double (*)[2])hull, nh, center, &wd, &ht, &ang);
        res[nres].center[0] = center[0];
        res[nres].center[1] = center[1];
        res[nres].width = pc_rs_max_k(wd - 2.0 * inset, 1.0);
        res[nres].height = pc_rs_max_k(ht - 2.0 * inset, 1.0);
        res[nres].angle = ang;
        res[nres].area = (double)len / total;
        nres++;
    }
    /* Reading order: rows by centre y (bands of a third of the smallest photo), then x. */
    band = 1.7976931348623157e308;
    for (i = 0; i < nres; i++) {
        band = pc_rs_min(band, pc_rs_min(res[i].height, res[i].width));
    }
    band = pc_rs_max_k(band, 1.0) / 3.0;
    for (i = 1; i < nres; i++) { /* stable insertion sort (few photos) */
        pc_algo_found_photo v = res[i];
        size_t j = i;
        while (j > 0) {
            const pc_algo_found_photo *p = &res[j - 1];
            int64_t a0 = total_key64(band_key(p, band)), b0 = total_key64(band_key(&v, band));
            bool after = a0 > b0 || (a0 == b0 && total_key64(p->center[0]) > total_key64(v.center[0]));
            if (!after) {
                break;
            }
            res[j] = res[j - 1];
            j--;
        }
        res[j] = v;
    }
    if (nres == 0) {
        free(res);
        res = NULL;
    }
    *out = res;
    *count = nres;
    res = NULL;
cleanup:
    free(small), free(cnt), free(border), free(dist), free(mask), free(tmp), free(label), free(order);
    free(comp_end), free(pts), free(hull), free(upper), free(res);
    return st;
}
