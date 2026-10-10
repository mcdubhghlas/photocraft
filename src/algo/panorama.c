/* Panorama geometry: layouts, motion models, placements, detection, pair matching and alignment
 * with bundle adjustment (port of crates/algo/src/panorama.rs, first half; photometric, seams
 * and blending are in panorama_blend.c). */
#include "algo/panorama.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/features_ransac.h"
#include "algo/fxutil.h"
#include "algo/photo_util.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define DMAX 1.7976931348623157e308

static const struct {
    const char *name;
    pc_algo_layout l;
} LAYOUTS[] = {{"auto", PC_ALGO_LAYOUT_AUTO},
               {"perspective", PC_ALGO_LAYOUT_PERSPECTIVE},
               {"cylindrical", PC_ALGO_LAYOUT_CYLINDRICAL},
               {"spherical", PC_ALGO_LAYOUT_SPHERICAL},
               {"collage", PC_ALGO_LAYOUT_COLLAGE},
               {"reposition", PC_ALGO_LAYOUT_REPOSITION}};

bool pc_algo_layout_parse(const char *s, pc_algo_layout *out)
{
    size_t i;
    for (i = 0; s != NULL && i < sizeof LAYOUTS / sizeof LAYOUTS[0]; i++) {
        if (strcmp(s, LAYOUTS[i].name) == 0) {
            *out = LAYOUTS[i].l;
            return true;
        }
    }
    return false;
}

const char *pc_algo_layout_name(pc_algo_layout l)
{
    size_t i;
    for (i = 0; i < sizeof LAYOUTS / sizeof LAYOUTS[0]; i++) {
        if (LAYOUTS[i].l == l) {
            return LAYOUTS[i].name;
        }
    }
    return "auto";
}

static pc_algo_projection layout_projection(pc_algo_layout l)
{
    return l == PC_ALGO_LAYOUT_CYLINDRICAL
               ? PC_ALGO_PROJECTION_CYLINDER
               : (l == PC_ALGO_LAYOUT_SPHERICAL ? PC_ALGO_PROJECTION_SPHERE : PC_ALGO_PROJECTION_PLANE);
}

static pc_algo_motion layout_motion(pc_algo_layout l)
{
    switch (l) {
    case PC_ALGO_LAYOUT_CYLINDRICAL:
    case PC_ALGO_LAYOUT_SPHERICAL:
        return PC_ALGO_MOTION_EUCLIDEAN;
    case PC_ALGO_LAYOUT_COLLAGE:
        return PC_ALGO_MOTION_SIMILARITY;
    case PC_ALGO_LAYOUT_REPOSITION:
        return PC_ALGO_MOTION_TRANSLATION;
    default:
        return PC_ALGO_MOTION_HOMOGRAPHY;
    }
}

static size_t motion_min_points(pc_algo_motion m)
{
    return m == PC_ALGO_MOTION_TRANSLATION ? 1 : (m == PC_ALGO_MOTION_HOMOGRAPHY ? 4 : 2);
}

static size_t motion_n_params(pc_algo_motion m)
{
    switch (m) {
    case PC_ALGO_MOTION_TRANSLATION:
        return 2;
    case PC_ALGO_MOTION_EUCLIDEAN:
        return 3;
    case PC_ALGO_MOTION_SIMILARITY:
        return 4;
    default:
        return 8;
    }
}

static void motion_to_params(pc_algo_motion mo, const pc_algo_homography *h, double *p)
{
    double m[9];
    int i;
    for (i = 0; i < 9; i++) {
        m[i] = h->m[i] / h->m[8];
    }
    switch (mo) {
    case PC_ALGO_MOTION_TRANSLATION:
        p[0] = m[2], p[1] = m[5];
        break;
    case PC_ALGO_MOTION_EUCLIDEAN:
        p[0] = atan2(m[3], m[0]), p[1] = m[2], p[2] = m[5];
        break;
    case PC_ALGO_MOTION_SIMILARITY:
        p[0] = m[0], p[1] = m[3], p[2] = m[2], p[3] = m[5];
        break;
    default:
        memcpy(p, m, 8 * sizeof *p);
        break;
    }
}

static pc_algo_homography motion_to_homography(pc_algo_motion mo, const double *p)
{
    switch (mo) {
    case PC_ALGO_MOTION_TRANSLATION:
        return (pc_algo_homography){{1.0, 0.0, p[0], 0.0, 1.0, p[1], 0.0, 0.0, 1.0}};
    case PC_ALGO_MOTION_EUCLIDEAN: {
        double s = sin(p[0]), c = cos(p[0]);
        return (pc_algo_homography){{c, -s, p[1], s, c, p[2], 0.0, 0.0, 1.0}};
    }
    case PC_ALGO_MOTION_SIMILARITY:
        return (pc_algo_homography){{p[0], -p[1], p[2], p[1], p[0], p[3], 0.0, 0.0, 1.0}};
    default:
        return (pc_algo_homography){{p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], 1.0}};
    }
}

bool pc_algo_panorama_fit_motion(pc_algo_motion motion, const double (*src)[2], const double (*dst)[2],
                                 size_t n, pc_algo_homography *out)
{
    double nf, cs[2] = {0.0, 0.0}, cd[2] = {0.0, 0.0}, sxx = 0.0, sxy = 0.0, th, s, c;
    size_t i;
    switch (motion) {
    case PC_ALGO_MOTION_TRANSLATION:
        return pc_algo_features_fit(PC_ALGO_MODEL_TRANSLATION, src, dst, n, out);
    case PC_ALGO_MOTION_SIMILARITY:
        return pc_algo_features_fit(PC_ALGO_MODEL_SIMILARITY, src, dst, n, out);
    case PC_ALGO_MOTION_HOMOGRAPHY:
        return pc_algo_features_fit(PC_ALGO_MODEL_HOMOGRAPHY, src, dst, n, out);
    default:
        break;
    }
    if (n < 2) {
        return false;
    }
    nf = (double)n;
    for (i = 0; i < n; i++) {
        cs[0] = cs[0] + src[i][0] / nf, cs[1] = cs[1] + src[i][1] / nf;
    }
    for (i = 0; i < n; i++) {
        cd[0] = cd[0] + dst[i][0] / nf, cd[1] = cd[1] + dst[i][1] / nf;
    }
    for (i = 0; i < n; i++) {
        double a0 = src[i][0] - cs[0], a1 = src[i][1] - cs[1], b0 = dst[i][0] - cd[0], b1 = dst[i][1] - cd[1];
        sxx += a0 * b0 + a1 * b1;
        sxy += a0 * b1 - a1 * b0;
    }
    th = atan2(sxy, sxx);
    s = sin(th), c = cos(th);
    *out = (pc_algo_homography){
        {c, -s, cd[0] - (c * cs[0] - s * cs[1]), s, c, cd[1] - (s * cs[0] + c * cs[1]), 0.0, 0.0, 1.0}};
    return true;
}

static bool fit_motion_cb(void *ctx, const double (*src)[2], const double (*dst)[2], size_t n,
                          pc_algo_homography *out)
{
    return pc_algo_panorama_fit_motion(*(const pc_algo_motion *)ctx, src, dst, n, out);
}

pc_status pc_algo_panorama_ransac_motion(pc_algo_motion motion, const double (*src)[2],
                                         const double (*dst)[2], size_t n, size_t iters, double threshold,
                                         uint64_t seed, bool *found, pc_algo_homography *out,
                                         size_t **inliers, size_t *ninliers, pc_error *err)
{
    return pc_algo_ransac_generic(fit_motion_cb, &motion, motion_min_points(motion), false, src, dst, n,
                                  iters, threshold, seed ^ 0x5EED0FA1u, found, out, inliers, ninliers, err);
}

/* ---- placements ---- */

void pc_algo_placement_project(const pc_algo_placement *p, double x, double y, double out[2])
{
    double dx = x - p->center[0], dy = y - p->center[1], f = p->focal;
    if (p->k1 != 0.0) {
        double s = 1.0 + p->k1 * (dx * dx + dy * dy) / (p->focal * p->focal);
        dx *= s;
        dy *= s;
    }
    switch (p->projection) {
    case PC_ALGO_PROJECTION_CYLINDER:
        out[0] = f * atan2(dx, f);
        out[1] = f * dy / hypot(dx, f);
        break;
    case PC_ALGO_PROJECTION_SPHERE:
        out[0] = f * atan2(dx, f);
        out[1] = f * atan2(dy, hypot(dx, f));
        break;
    default:
        out[0] = dx;
        out[1] = dy;
        break;
    }
}

bool pc_algo_placement_unproject(const pc_algo_placement *p, double a, double b, double *x, double *y)
{
    double f = p->focal, dx, dy;
    switch (p->projection) {
    case PC_ALGO_PROJECTION_CYLINDER: {
        double th = a / f;
        if (fabs(th) >= 1.55) {
            return false;
        }
        dx = f * tan(th);
        dy = b / cos(th);
        break;
    }
    case PC_ALGO_PROJECTION_SPHERE: {
        double th = a / f, ph = b / f;
        if (fabs(th) >= 1.55 || fabs(ph) >= 1.55) {
            return false;
        }
        dx = f * tan(th);
        dy = f * tan(ph) / cos(th);
        break;
    }
    default:
        dx = a;
        dy = b;
        break;
    }
    if (p->k1 != 0.0) {
        /* Invert r_u = r_d (1 + k1 r_d^2 / f^2) by Newton steps on r_d. */
        double ru = hypot(dx, dy);
        if (ru > 1e-9) {
            double rd = ru;
            int i;
            for (i = 0; i < 8; i++) {
                double g = rd * (1.0 + p->k1 * rd * rd / (f * f)) - ru;
                double dg = 1.0 + 3.0 * p->k1 * rd * rd / (f * f);
                if (fabs(dg) < 1e-9) {
                    break;
                }
                rd -= g / dg;
            }
            if (!(isfinite(rd) && rd > 0.0)) {
                return false;
            }
            dx *= rd / ru;
            dy *= rd / ru;
        }
    }
    *x = dx + p->center[0];
    *y = dy + p->center[1];
    return true;
}

void pc_algo_placement_forward(const pc_algo_placement *p, double x, double y, double *u, double *v)
{
    double q[2];
    pc_algo_placement_project(p, x, y, q);
    pc_algo_homography_apply(&p->h, q[0], q[1], u, v);
}

bool pc_algo_placement_inverse(const pc_algo_placement *p, double u, double v, double *x, double *y)
{
    pc_algo_homography inv;
    double w, a, b;
    if (!pc_algo_homography_inverse(&p->h, &inv)) {
        return false;
    }
    w = inv.m[6] * u + inv.m[7] * v + inv.m[8];
    if (w <= 1e-12) {
        return false;
    }
    pc_algo_homography_apply(&inv, u, v, &a, &b);
    return pc_algo_placement_unproject(p, a, b, x, y);
}

pc_algo_placement pc_algo_placement_scaled(const pc_algo_placement *p, double k)
{
    pc_algo_homography s = {{k, 0.0, 0.0, 0.0, k, 0.0, 0.0, 0.0, 1.0}};
    pc_algo_homography si = {{1.0 / k, 0.0, 0.0, 0.0, 1.0 / k, 0.0, 0.0, 0.0, 1.0}};
    pc_algo_placement r = *p;
    r.focal = p->focal * k;
    r.center[0] = p->center[0] * k;
    r.center[1] = p->center[1] * k;
    r.h = pc_algo_homography_mul(&s, &p->h);
    r.h = pc_algo_homography_mul(&r.h, &si);
    return r;
}

pc_algo_placement pc_algo_placement_translated(const pc_algo_placement *p, double dx, double dy)
{
    pc_algo_homography t = {{1.0, 0.0, dx, 0.0, 1.0, dy, 0.0, 0.0, 1.0}};
    pc_algo_placement r = *p;
    r.h = pc_algo_homography_mul(&t, &p->h);
    return r;
}

void pc_algo_placement_bounds(const pc_algo_placement *p, double w, double h, double out[4])
{
    double b[4] = {DMAX, DMAX, -DMAX, -DMAX};
    int i, e;
    for (i = 0; i <= 32; i++) {
        double t = (double)i / 32.0;
        const double xy[4][2] = {{t * w, 0.0}, {t * w, h}, {0.0, t * h}, {w, t * h}};
        for (e = 0; e < 4; e++) {
            double u, v;
            pc_algo_placement_forward(p, xy[e][0], xy[e][1], &u, &v);
            if (isfinite(u) && isfinite(v)) {
                b[0] = pc_rs_min(b[0], u);
                b[1] = pc_rs_min(b[1], v);
                b[2] = pc_rs_max(b[2], u);
                b[3] = pc_rs_max(b[3], v);
            }
        }
    }
    memcpy(out, b, sizeof b);
}

/* ---- features ---- */

void pc_algo_image_features_fini(pc_algo_image_features *f)
{
    if (f != NULL) {
        free(f->points);
        free(f->desc);
        memset(f, 0, sizeof *f);
    }
}

pc_status pc_algo_image_features_copy(const pc_algo_image_features *src, pc_algo_image_features *out,
                                      pc_error *err)
{
    pc_algo_image_features f = *src;
    f.points = pc_malloc_array(src->n > 0 ? src->n : 1, sizeof *f.points);
    f.desc = pc_malloc_array(src->n > 0 ? src->n : 1, sizeof *f.desc);
    if (f.points == NULL || f.desc == NULL) {
        free(f.points), free(f.desc);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory copying features");
    }
    if (src->n > 0) {
        memcpy(f.points, src->points, src->n * sizeof *f.points);
        memcpy(f.desc, src->desc, src->n * sizeof *f.desc);
    }
    *out = f;
    return PC_OK;
}

pc_status pc_algo_panorama_detect(size_t w, size_t h, const float *luma, const bool *valid, size_t max,
                                  pc_algo_image_features *out, pc_error *err)
{
    float min_dist = pc_clampf((float)(w > h ? w : h) / 70.0f, 3.0f, 20.0f);
    pc_algo_corner *c = NULL;
    pc_algo_descriptor *d = NULL;
    size_t n = 0, i;
    pc_algo_image_features f;
    pc_status st = pc_algo_features_harris(w, h, luma, max, min_dist, valid, &c, &n, err);
    if (st == PC_OK) {
        st = pc_algo_features_describe(w, h, luma, c, n, false, &d, err);
    }
    if (st != PC_OK) {
        free(c);
        return st;
    }
    memset(&f, 0, sizeof f);
    f.w = w, f.h = h, f.n = n, f.desc = d;
    f.points = pc_malloc_array(n > 0 ? n : 1, sizeof *f.points);
    if (f.points == NULL) {
        free(c), free(d);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for keypoints");
    }
    for (i = 0; i < n; i++) {
        f.points[i][0] = (double)c[i].x;
        f.points[i][1] = (double)c[i].y;
    }
    free(c);
    *out = f;
    return PC_OK;
}

typedef struct detect_ctx {
    const pc_algo_prepared_image *imgs;
    size_t max;
    pc_algo_image_features *out;
    pc_status *st;
} detect_ctx;

static pc_status detect_chunk(void *vctx, size_t chunk, size_t begin, size_t end)
{
    detect_ctx *c = vctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        const pc_algo_prepared_image *im = &c->imgs[i];
        c->st[i] = pc_algo_panorama_detect(im->w, im->h, im->luma, im->valid, c->max, &c->out[i], NULL);
    }
    return PC_OK;
}

pc_status pc_algo_panorama_detect_many(const pc_algo_prepared_image *imgs, size_t n, size_t max,
                                       pc_algo_image_features *out, pc_error *err)
{
    detect_ctx c;
    size_t i;
    pc_status *st = pc_malloc_array(n > 0 ? n : 1, sizeof *st), first = PC_OK;
    pc_algo_image_features *tmp = pc_calloc_array(n > 0 ? n : 1, sizeof *tmp);
    if (st == NULL || tmp == NULL) {
        free(st), free(tmp);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory detecting features");
    }
    c.imgs = imgs, c.max = max, c.out = tmp, c.st = st;
    (void)pc_algo_par_for(n, 1, detect_chunk, &c);
    for (i = 0; i < n && first == PC_OK; i++) {
        first = st[i];
    }
    if (first != PC_OK) {
        /* Re-run the first failing image serially to report its message. */
        for (i = 0; i < n; i++) {
            pc_algo_image_features_fini(&tmp[i]);
        }
        for (i = 0; i < n; i++) {
            if (st[i] != PC_OK) {
                pc_algo_image_features f;
                pc_status s2 =
                    pc_algo_panorama_detect(imgs[i].w, imgs[i].h, imgs[i].luma, imgs[i].valid, max, &f, err);
                if (s2 == PC_OK) {
                    pc_algo_image_features_fini(&f);
                    s2 = pc_fail(err, PC_ERR_NOMEM, "out of memory detecting features");
                }
                free(st), free(tmp);
                return s2;
            }
        }
    }
    memcpy(out, tmp, n * sizeof *out);
    free(st), free(tmp);
    return PC_OK;
}

/* ---- pair matching ---- */

void pc_algo_pair_matches_free(pc_algo_pair_match *m, size_t count)
{
    size_t i;
    if (m == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(m[i].pts_a);
        free(m[i].pts_b);
    }
    free(m);
}

typedef struct pair_ctx {
    const pc_algo_image_features *feats;
    const size_t (*pairs)[2];
    pc_algo_pair_match *res;
    bool *ok;
    pc_status *st;
} pair_ctx;

static pc_status match_one(const pc_algo_image_features *fa, const pc_algo_image_features *fb, size_t a,
                           size_t b, pc_algo_pair_match *out, bool *ok)
{
    pc_algo_match *m = NULL;
    size_t nm = 0, i, *inl = NULL, ninl = 0;
    double (*src)[2] = NULL, (*dst)[2] = NULL, thr;
    pc_algo_homography hm;
    bool found = false;
    pc_status st;
    *ok = false;
    if (fa->n < 6 || fb->n < 6) {
        return PC_OK;
    }
    st = pc_algo_features_match(fb->desc, fb->n, fa->desc, fa->n, 0.8f, &m, &nm, NULL);
    if (st != PC_OK || nm < 8) {
        free(m);
        return st;
    }
    src = pc_malloc_array(nm, sizeof *src);
    dst = pc_malloc_array(nm, sizeof *dst);
    if (src == NULL || dst == NULL) {
        st = PC_ERR_NOMEM;
        goto cleanup;
    }
    for (i = 0; i < nm; i++) {
        memcpy(src[i], fb->points[m[i].a], sizeof src[i]);
        memcpy(dst[i], fa->points[m[i].b], sizeof dst[i]);
    }
    thr = pc_clampd((double)(fa->w > fa->h ? fa->w : fa->h) / 300.0, 1.5, 4.0);
    st = pc_algo_panorama_ransac_motion(PC_ALGO_MOTION_HOMOGRAPHY, (const double (*)[2])src,
                                        (const double (*)[2])dst, nm, 1200, thr, (uint64_t)(a * 131 + b),
                                        &found, &hm, &inl, &ninl, NULL);
    if (st != PC_OK || !found || (double)ninl < 8.0 + 0.3 * (double)nm || ninl < 10) {
        goto cleanup;
    }
    out->a = a, out->b = b, out->n = ninl, out->homography = hm;
    out->pts_a = pc_malloc_array(ninl, sizeof *out->pts_a);
    out->pts_b = pc_malloc_array(ninl, sizeof *out->pts_b);
    if (out->pts_a == NULL || out->pts_b == NULL) {
        free(out->pts_a), free(out->pts_b);
        out->pts_a = out->pts_b = NULL;
        st = PC_ERR_NOMEM;
        goto cleanup;
    }
    for (i = 0; i < ninl; i++) {
        memcpy(out->pts_a[i], dst[inl[i]], sizeof out->pts_a[i]);
        memcpy(out->pts_b[i], src[inl[i]], sizeof out->pts_b[i]);
    }
    *ok = true;
cleanup:
    free(m), free(src), free(dst), free(inl);
    return st;
}

static pc_status pair_chunk(void *vctx, size_t chunk, size_t begin, size_t end)
{
    pair_ctx *c = vctx;
    size_t k;
    (void)chunk;
    for (k = begin; k < end; k++) {
        size_t a = c->pairs[k][0], b = c->pairs[k][1];
        c->st[k] = match_one(&c->feats[a], &c->feats[b], a, b, &c->res[k], &c->ok[k]);
    }
    return PC_OK;
}

pc_status pc_algo_panorama_match_pairs(const pc_algo_image_features *feats, size_t n,
                                       pc_algo_pair_match **out, size_t *count, pc_error *err)
{
    size_t np, a, b, k = 0, nres = 0;
    size_t (*pairs)[2] = NULL;
    pc_algo_pair_match *res = NULL;
    bool *ok = NULL;
    pc_status *st = NULL, first = PC_OK;
    pair_ctx c;
    if (n < 2) {
        *out = NULL;
        *count = 0;
        return PC_OK;
    }
    if (n > 65536) {
        return pc_fail(err, PC_ERR_LIMIT, "too many images to match (%zu)", n);
    }
    np = n * (n - 1) / 2;
    pairs = pc_malloc_array(np, sizeof *pairs);
    res = pc_calloc_array(np, sizeof *res);
    ok = pc_calloc_array(np, sizeof *ok);
    st = pc_malloc_array(np, sizeof *st);
    if (pairs == NULL || res == NULL || ok == NULL || st == NULL) {
        free(pairs), free(res), free(ok), free(st);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory matching image pairs");
    }
    for (a = 0; a < n; a++) {
        for (b = a + 1; b < n; b++) {
            pairs[k][0] = a, pairs[k][1] = b, k++;
        }
    }
    c.feats = feats, c.pairs = (const size_t (*)[2])pairs, c.res = res, c.ok = ok, c.st = st;
    (void)pc_algo_par_for(np, 1, pair_chunk, &c);
    for (k = 0; k < np; k++) {
        if (first == PC_OK && st[k] != PC_OK) {
            first = st[k];
        }
        if (ok[k]) {
            res[nres++] = res[k];
        }
    }
    free(pairs), free(ok), free(st);
    if (first != PC_OK) {
        pc_algo_pair_matches_free(res, nres);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory matching image pairs");
    }
    if (nres == 0) {
        free(res);
        res = NULL;
    }
    *out = res;
    *count = nres;
    return PC_OK;
}

/* ---- focal length ---- */

/* One candidate (numerator, denominator) of f^2: usable when |d| > 1e-12 and n / d > 0. */
static bool focal_ok(double n, double d, double *f2, double *w)
{
    if (fabs(d) > 1e-12 && n / d > 0.0) {
        *f2 = n / d;
        *w = fabs(d);
        return true;
    }
    return false;
}

/* Prefer the better-conditioned candidate. */
static bool focal_pick(double n1, double d1, double n2, double d2, double *out)
{
    double a0, a1, b0, b1;
    bool oa = focal_ok(n1, d1, &a0, &a1), ob = focal_ok(n2, d2, &b0, &b1);
    if (oa && ob) {
        *out = a1 >= b1 ? a0 : b0;
        return true;
    }
    if (oa || ob) {
        *out = oa ? a0 : b0;
        return true;
    }
    return false;
}

bool pc_algo_panorama_focal_from_homography(const pc_algo_homography *h, const double ca[2],
                                            const double cb[2], double *out)
{
    pc_algo_homography ta = {{1.0, 0.0, -ca[0], 0.0, 1.0, -ca[1], 0.0, 0.0, 1.0}};
    pc_algo_homography tb = {{1.0, 0.0, cb[0], 0.0, 1.0, cb[1], 0.0, 0.0, 1.0}};
    pc_algo_homography mm = pc_algo_homography_mul(&ta, h);
    const double *m;
    double f0, f1;
    bool o0, o1;
    mm = pc_algo_homography_mul(&mm, &tb);
    m = mm.m;
    /* Columns of K^-1 H K orthogonal and of equal norm (focal of b) ... */
    o1 = focal_pick(-(m[0] * m[1] + m[3] * m[4]), m[6] * m[7],
                    m[1] * m[1] + m[4] * m[4] - m[0] * m[0] - m[3] * m[3], m[6] * m[6] - m[7] * m[7], &f1);
    /* ... rows likewise (focal of a). */
    o0 = focal_pick(-(m[2] * m[5]), m[0] * m[3] + m[1] * m[4], m[5] * m[5] - m[2] * m[2],
                    m[0] * m[0] + m[1] * m[1] - m[3] * m[3] - m[4] * m[4], &f0);
    if (o0 && o1) {
        *out = sqrt(sqrt(f0 * f1));
        return true;
    }
    if (o0 || o1) {
        *out = sqrt(o0 ? f0 : f1);
        return true;
    }
    return false;
}

/* ---- alignment ---- */

void pc_algo_alignment_fini(pc_algo_alignment *a)
{
    if (a != NULL) {
        free(a->placements);
        free(a->placed);
        free(a->pairs);
        memset(a, 0, sizeof *a);
    }
}

static int cmp_f64_total(const void *pa, const void *pb)
{
    double a = *(const double *)pa, b = *(const double *)pb;
    uint64_t ua, ub;
    int64_t ia, ib;
    memcpy(&ua, &a, 8);
    memcpy(&ub, &b, 8);
    ia = ua > (uint64_t)INT64_MAX ? -(int64_t)(~ua) - 1 : (int64_t)ua;
    ib = ub > (uint64_t)INT64_MAX ? -(int64_t)(~ub) - 1 : (int64_t)ub;
    ia ^= (int64_t)((uint64_t)(ia >> 63) >> 1);
    ib ^= (int64_t)((uint64_t)(ib >> 63) >> 1);
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

static pc_algo_placement base_placement(const pc_algo_image_features *f, pc_algo_projection projection,
                                        double focal, double k1)
{
    pc_algo_placement p;
    p.projection = projection;
    p.focal = focal;
    p.center[0] = (double)f->w / 2.0;
    p.center[1] = (double)f->h / 2.0;
    p.k1 = k1;
    p.h = PC_ALGO_HOMOGRAPHY_IDENTITY;
    return p;
}

/* Bundle-adjustment problem: pairs' selected correspondences (image px) and the parameter
 * layout [movable motions..., k1?, ln focal?]. */
typedef struct ba_pair {
    size_t a, b, start, n;
} ba_pair;

typedef struct ba {
    const pc_algo_image_features *feats;
    size_t n;
    pc_algo_projection projection;
    pc_algo_motion motion;
    double focal;
    bool geometric, refine_focal;
    size_t reference;
    pc_algo_homography tf_ref;
    size_t *mov_pos; /* per image: index in movable, SIZE_MAX when not movable */
    size_t nmov, mp, n_par, n_glob, m_res;
    ba_pair *pairs;
    size_t npairs;
    double (*pa)[2], (*pb)[2];
} ba;

static void ba_decode(const ba *b, const double *p, pc_algo_placement *pl, bool *placed)
{
    double k1 = b->geometric ? p[b->nmov * b->mp] : 0.0;
    double f = b->refine_focal ? exp(p[b->n_par - 1]) : b->focal;
    size_t i;
    for (i = 0; i < b->n; i++) {
        placed[i] = i == b->reference || b->mov_pos[i] != SIZE_MAX;
        if (!placed[i]) {
            continue;
        }
        pl[i] = base_placement(&b->feats[i], b->projection, f, k1);
        pl[i].h = i == b->reference ? b->tf_ref : motion_to_homography(b->motion, p + b->mov_pos[i] * b->mp);
    }
}

/* r gets b->m_res residuals; pl / placed are scratch of n entries. */
static void ba_residuals(const ba *b, const double *p, pc_algo_placement *pl, bool *placed, double *r)
{
    size_t k, i, m = 0;
    ba_decode(b, p, pl, placed);
    for (k = 0; k < b->npairs; k++) {
        const ba_pair *pd = &b->pairs[k];
        if (!placed[pd->a] || !placed[pd->b]) {
            continue;
        }
        for (i = pd->start; i < pd->start + pd->n; i++) {
            double ua, va, ub, vb;
            pc_algo_placement_forward(&pl[pd->a], b->pa[i][0], b->pa[i][1], &ua, &va);
            pc_algo_placement_forward(&pl[pd->b], b->pb[i][0], b->pb[i][1], &ub, &vb);
            r[m++] = ua - ub;
            r[m++] = va - vb;
        }
    }
    /* Soft priors keep the global terms sane when few pairs constrain them. */
    if (b->geometric) {
        r[m++] = p[b->nmov * b->mp] * 20.0;
    }
    if (b->refine_focal) {
        r[m++] = (p[b->n_par - 1] - log(b->focal)) * 2.0;
    }
}

static double cost_of(const double *r, size_t n)
{
    double s = -0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        s += r[i] * r[i];
    }
    return s;
}

typedef struct jac_ctx {
    const ba *b;
    const double *params;
    double *cols; /* n_par x m_res */
} jac_ctx;

static pc_status jac_chunk(void *vctx, size_t chunk, size_t begin, size_t end)
{
    jac_ctx *c = vctx;
    const ba *b = c->b;
    size_t j, i, m = b->m_res;
    double *pp = pc_malloc_array(b->n_par, sizeof(double));
    double *rp = pc_malloc_array(m > 0 ? m : 1, sizeof(double)),
           *rm = pc_malloc_array(m > 0 ? m : 1, sizeof(double));
    pc_algo_placement *pl = pc_malloc_array(b->n, sizeof *pl);
    bool *placed = pc_malloc_array(b->n, sizeof *placed);
    pc_status st = PC_OK;
    (void)chunk;
    if (pp == NULL || rp == NULL || rm == NULL || pl == NULL || placed == NULL) {
        st = PC_ERR_NOMEM;
        goto cleanup;
    }
    for (j = begin; j < end; j++) {
        double step = 1e-6 * (1.0 + fabs(c->params[j]));
        memcpy(pp, c->params, b->n_par * sizeof *pp);
        pp[j] += step;
        ba_residuals(b, pp, pl, placed, rp);
        pp[j] -= 2.0 * step;
        ba_residuals(b, pp, pl, placed, rm);
        for (i = 0; i < m; i++) {
            c->cols[j * m + i] = (rp[i] - rm[i]) / (2.0 * step);
        }
    }
cleanup:
    free(pp), free(rp), free(rm), free(pl), free(placed);
    return st;
}

/* Levenberg-Marquardt with a numeric Jacobian; params / cur updated in place. */
static pc_status ba_solve(const ba *b, double *params, double *cur, pc_error *err)
{
    size_t np = b->n_par, m = b->m_res, it, a, c, d, ncols;
    double *cols = NULL, *jtj = NULL, *jtr = NULL, *am = NULL, *bm = NULL, *delta = NULL, *trial = NULL;
    double *rt = NULL, lambda = 1e-3;
    pc_algo_placement *pl = NULL;
    bool *placed = NULL;
    pc_status st = PC_OK;
    if (!pc_size_mul(np, m, &ncols) || ncols > PC_MAX_ALLOC / sizeof(double)) {
        return pc_fail(err, PC_ERR_LIMIT, "bundle adjustment too large (%zu x %zu)", np, m);
    }
    cols = pc_malloc_array(ncols > 0 ? ncols : 1, sizeof(double));
    jtj = pc_malloc_array(np * np, sizeof(double));
    am = pc_malloc_array(np * np, sizeof(double));
    jtr = pc_malloc_array(np, sizeof(double));
    bm = pc_malloc_array(np, sizeof(double));
    delta = pc_malloc_array(np, sizeof(double));
    trial = pc_malloc_array(np, sizeof(double));
    rt = pc_malloc_array(m > 0 ? m : 1, sizeof(double));
    pl = pc_malloc_array(b->n, sizeof *pl);
    placed = pc_malloc_array(b->n, sizeof *placed);
    if (cols == NULL || jtj == NULL || am == NULL || jtr == NULL || bm == NULL || delta == NULL ||
        trial == NULL || rt == NULL || pl == NULL || placed == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for bundle adjustment");
        goto cleanup;
    }
    for (it = 0; it < 40; it++) {
        jac_ctx jc;
        bool improved = false;
        int tries;
        jc.b = b, jc.params = params, jc.cols = cols;
        st = pc_algo_par_for(np, 1, jac_chunk, &jc);
        if (st != PC_OK) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for bundle adjustment");
            goto cleanup;
        }
        for (a = 0; a < np; a++) {
            double s = -0.0;
            for (d = 0; d < m; d++) {
                s += cols[a * m + d] * cur[d];
            }
            jtr[a] = -s;
            for (c = a; c < np; c++) {
                double v = -0.0;
                for (d = 0; d < m; d++) {
                    v += cols[a * m + d] * cols[c * m + d];
                }
                jtj[a * np + c] = v;
                jtj[c * np + a] = v;
            }
        }
        for (tries = 0; tries < 8; tries++) {
            double cc, ct;
            memcpy(am, jtj, np * np * sizeof *am);
            for (d = 0; d < np; d++) {
                am[d * np + d] += lambda * (jtj[d * np + d] + 1e-9);
            }
            memcpy(bm, jtr, np * sizeof *bm);
            if (!pc_algo_photo_solve_dense(np, am, bm, delta)) {
                lambda *= 10.0;
                continue;
            }
            for (d = 0; d < np; d++) {
                trial[d] = params[d] + delta[d];
            }
            ba_residuals(b, trial, pl, placed, rt);
            cc = cost_of(cur, m);
            ct = cost_of(rt, m);
            if (ct < cc) {
                double gain = cc - ct;
                memcpy(params, trial, np * sizeof *params);
                memcpy(cur, rt, m * sizeof *cur);
                lambda = pc_rs_max_k(lambda * 0.3, 1e-9);
                improved = gain > 1e-10 * pc_rs_max_k(cost_of(cur, m), 1e-12);
                break;
            }
            lambda *= 10.0;
        }
        if (!improved) {
            break;
        }
    }
cleanup:
    free(cols), free(jtj), free(am), free(jtr), free(bm), free(delta), free(trial), free(rt), free(pl),
        free(placed);
    return st;
}

typedef struct edge {
    size_t a, b, inl;
    pc_algo_homography h; /* b -> a in projected space */
} edge;

static pc_status align_layout(const pc_algo_image_features *feats, size_t n,
                              const pc_algo_pair_match *matches, size_t nm, const pc_algo_align_options *opts,
                              pc_algo_layout layout, double focal, bool *found, pc_algo_alignment *out,
                              pc_error *err)
{
    pc_algo_projection projection = layout_projection(layout);
    pc_algo_motion motion = layout_motion(layout);
    edge *edges = NULL;
    size_t nedges = 0, k, i, npts = 0, *degree = NULL, nplaced = 0;
    double (*qa)[2] = NULL, (*qb)[2] = NULL, *params = NULL, *cur = NULL;
    pc_algo_homography *tf = NULL;
    bool *has_tf = NULL;
    ba b;
    pc_algo_alignment res;
    pc_status st = PC_OK;
    *found = false;
    memset(&b, 0, sizeof b);
    memset(&res, 0, sizeof res);
    for (k = 0; k < nm; k++) {
        if (!pc_size_add(npts, matches[k].n, &npts)) {
            return pc_fail(err, PC_ERR_LIMIT, "too many correspondences");
        }
    }
    edges = pc_malloc_array(nm > 0 ? nm : 1, sizeof *edges);
    b.pairs = pc_malloc_array(nm > 0 ? nm : 1, sizeof *b.pairs);
    b.pa = pc_malloc_array(npts > 0 ? npts : 1, sizeof *b.pa);
    b.pb = pc_malloc_array(npts > 0 ? npts : 1, sizeof *b.pb);
    if (edges == NULL || b.pairs == NULL || b.pa == NULL || b.pb == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory aligning images");
        goto cleanup;
    }
    /* Pairwise motions in projected space. */
    {
        size_t used = 0;
        for (k = 0; k < nm; k++) {
            const pc_algo_pair_match *m = &matches[k];
            pc_algo_placement pa, pb;
            size_t *inl = NULL, ninl = 0, step;
            pc_algo_homography hm;
            bool ok = false;
            double thr;
            if (m->a >= n || m->b >= n) {
                st = pc_fail(err, PC_ERR_INVALID, "pair (%zu, %zu) refers to a missing image", m->a, m->b);
                goto cleanup;
            }
            pa = base_placement(&feats[m->a], projection, focal, 0.0);
            pb = base_placement(&feats[m->b], projection, focal, 0.0);
            free(qa), free(qb);
            qa = pc_malloc_array(m->n > 0 ? m->n : 1, sizeof *qa);
            qb = pc_malloc_array(m->n > 0 ? m->n : 1, sizeof *qb);
            if (qa == NULL || qb == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory aligning images");
                goto cleanup;
            }
            for (i = 0; i < m->n; i++) {
                pc_algo_placement_project(&pa, m->pts_a[i][0], m->pts_a[i][1], qa[i]);
                pc_algo_placement_project(&pb, m->pts_b[i][0], m->pts_b[i][1], qb[i]);
            }
            thr = pc_clampd((double)(feats[m->a].w > feats[m->a].h ? feats[m->a].w : feats[m->a].h) / 250.0,
                            2.0, 5.0);
            st = pc_algo_panorama_ransac_motion(motion, (const double (*)[2])qb, (const double (*)[2])qa,
                                                m->n, 600, thr, (uint64_t)(m->a * 977 + m->b), &ok, &hm, &inl,
                                                &ninl, err);
            if (st != PC_OK) {
                goto cleanup;
            }
            if (!ok || ninl < 8 || (double)ninl < 0.5 * (double)m->n) {
                free(inl);
                continue;
            }
            edges[nedges].a = m->a, edges[nedges].b = m->b, edges[nedges].inl = ninl, edges[nedges].h = hm;
            nedges++;
            /* Bundle adjustment uses at most 80 evenly spread inliers per pair. */
            step = (ninl + 79) / 80;
            step = step > 0 ? step : 1;
            b.pairs[b.npairs].a = m->a, b.pairs[b.npairs].b = m->b, b.pairs[b.npairs].start = used;
            for (i = 0; i < ninl; i += step) {
                memcpy(b.pa[used], m->pts_a[inl[i]], sizeof b.pa[used]);
                memcpy(b.pb[used], m->pts_b[inl[i]], sizeof b.pb[used]);
                used++;
            }
            b.pairs[b.npairs].n = used - b.pairs[b.npairs].start;
            b.npairs++;
            free(inl);
        }
    }
    if (nedges == 0) {
        goto cleanup;
    }
    degree = pc_calloc_array(n, sizeof *degree);
    tf = pc_malloc_array(n, sizeof *tf);
    has_tf = pc_calloc_array(n, sizeof *has_tf);
    b.mov_pos = pc_malloc_array(n, sizeof *b.mov_pos);
    if (degree == NULL || tf == NULL || has_tf == NULL || b.mov_pos == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory aligning images");
        goto cleanup;
    }
    for (k = 0; k < nedges; k++) {
        degree[edges[k].a] += edges[k].inl;
        degree[edges[k].b] += edges[k].inl;
    }
    if (opts->has_reference && opts->reference < n) {
        b.reference = opts->reference;
    } else {
        /* Best connected; ties go to the middle image (max_by: the last maximum wins). */
        int64_t half = (int64_t)n / 2;
        size_t best = 0;
        for (i = 1; i < n; i++) {
            int64_t db = llabs((int64_t)best - half), di = llabs((int64_t)i - half);
            /* compare(best, i) = degree[best].cmp(degree[i]).then(|i - n/2|.cmp(|best - n/2|)) */
            int cmp = degree[best] < degree[i] ? -1 : (degree[best] > degree[i] ? 1 : 0);
            if (cmp == 0) {
                cmp = di < db ? -1 : (di > db ? 1 : 0);
            }
            if (cmp <= 0) {
                best = i;
            }
        }
        b.reference = best;
    }
    /* Maximum spanning tree (Prim) from the reference; transforms map projected -> panorama. */
    tf[b.reference] = (pc_algo_homography){{1.0, 0.0, (double)feats[b.reference].w / 2.0, 0.0, 1.0,
                                            (double)feats[b.reference].h / 2.0, 0.0, 0.0, 1.0}};
    has_tf[b.reference] = true;
    for (;;) {
        bool have = false;
        size_t node = 0, w = 0;
        pc_algo_homography t;
        for (k = 0; k < nedges; k++) {
            const edge *e = &edges[k];
            size_t cn;
            pc_algo_homography ct, inv;
            if (has_tf[e->a] && !has_tf[e->b]) {
                cn = e->b;
                ct = pc_algo_homography_mul(&tf[e->a], &e->h);
            } else if (!has_tf[e->a] && has_tf[e->b] && pc_algo_homography_inverse(&e->h, &inv)) {
                cn = e->a;
                ct = pc_algo_homography_mul(&tf[e->b], &inv);
            } else {
                continue;
            }
            if (!have || e->inl > w) {
                have = true;
                node = cn, w = e->inl, t = ct;
            }
        }
        if (!have) {
            break;
        }
        tf[node] = t;
        has_tf[node] = true;
    }
    for (i = 0; i < n; i++) {
        nplaced += has_tf[i] ? 1u : 0u;
    }
    if (nplaced < 2) {
        goto cleanup;
    }
    /* Bundle adjustment. */
    b.feats = feats, b.n = n, b.projection = projection, b.motion = motion, b.focal = focal;
    b.geometric = opts->geometric;
    b.refine_focal = projection != PC_ALGO_PROJECTION_PLANE;
    b.tf_ref = tf[b.reference];
    b.mp = motion_n_params(motion);
    b.n_glob = (b.geometric ? 1u : 0u) + (b.refine_focal ? 1u : 0u);
    for (i = 0; i < n; i++) {
        b.mov_pos[i] = SIZE_MAX;
        if (has_tf[i] && i != b.reference) {
            b.mov_pos[i] = b.nmov++;
        }
    }
    b.n_par = b.nmov * b.mp + b.n_glob;
    b.m_res = b.n_glob;
    for (k = 0; k < b.npairs; k++) {
        if (has_tf[b.pairs[k].a] && has_tf[b.pairs[k].b]) {
            b.m_res += 2 * b.pairs[k].n;
        }
    }
    params = pc_malloc_array(b.n_par > 0 ? b.n_par : 1, sizeof *params);
    cur = pc_malloc_array(b.m_res > 0 ? b.m_res : 1, sizeof *cur);
    res.placements = pc_calloc_array(n, sizeof *res.placements);
    res.placed = pc_calloc_array(n, sizeof *res.placed);
    res.pairs = pc_malloc_array(nedges, sizeof *res.pairs);
    if (params == NULL || cur == NULL || res.placements == NULL || res.placed == NULL || res.pairs == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory aligning images");
        goto cleanup;
    }
    for (i = 0; i < n; i++) {
        if (b.mov_pos[i] != SIZE_MAX) {
            motion_to_params(motion, &tf[i], params + b.mov_pos[i] * b.mp);
        }
    }
    if (b.geometric) {
        params[b.nmov * b.mp] = 0.0;
    }
    if (b.refine_focal) {
        params[b.n_par - 1] = log(focal);
    }
    ba_residuals(&b, params, res.placements, res.placed, cur);
    if (b.n_par > 0 && b.nmov > 0) {
        st = ba_solve(&b, params, cur, err);
        if (st != PC_OK) {
            goto cleanup;
        }
    }
    ba_decode(&b, params, res.placements, res.placed);
    {
        size_t n_res = 0, upto = b.m_res - b.n_glob;
        for (k = 0; k < b.npairs; k++) {
            n_res += b.pairs[k].n;
        }
        n_res = n_res > 0 ? n_res : 1;
        res.rms = sqrt(cost_of(cur, upto) / (double)n_res);
    }
    res.layout = layout;
    res.reference = b.reference;
    res.n = n;
    res.k1 = b.geometric ? params[b.nmov * b.mp] : 0.0;
    res.focal = b.refine_focal ? exp(params[b.n_par - 1]) : focal;
    for (k = 0; k < nedges; k++) {
        res.pairs[k].a = edges[k].a, res.pairs[k].b = edges[k].b, res.pairs[k].inliers = edges[k].inl;
    }
    res.npairs = nedges;
    *out = res;
    memset(&res, 0, sizeof res);
    *found = true;
cleanup:
    pc_algo_alignment_fini(&res);
    free(edges), free(b.pairs), free(b.pa), free(b.pb), free(qa), free(qb), free(degree), free(tf),
        free(has_tf);
    free(b.mov_pos), free(params), free(cur);
    return st;
}

/* A perspective panorama is too stretched when an image grows or shrinks a lot, flips, or the
 * field of view exceeds about 110 degrees. */
static bool too_stretched(const pc_algo_image_features *feats, const pc_algo_alignment *a)
{
    double x0 = DMAX, x1 = -DMAX, fov;
    size_t i;
    int q;
    for (i = 0; i < a->n; i++) {
        const pc_algo_placement *p = &a->placements[i];
        double w = (double)feats[i].w, h = (double)feats[i].h, c[4][2], area = -0.0, ratio;
        const double xy[4][2] = {{0.0, 0.0}, {w, 0.0}, {w, h}, {0.0, h}};
        const double *m = p->h.m;
        if (!a->placed[i]) {
            continue;
        }
        for (q = 0; q < 4; q++) {
            pc_algo_placement_forward(p, xy[q][0], xy[q][1], &c[q][0], &c[q][1]);
        }
        /* Signed area (shoelace); orientation must be preserved. */
        for (q = 0; q < 4; q++) {
            area += c[q][0] * c[(q + 1) % 4][1] - c[(q + 1) % 4][0] * c[q][1];
        }
        ratio = area / 2.0 / (w * h);
        if (!isfinite(ratio) || !(ratio >= 0.3 && ratio <= 3.0)) {
            return true;
        }
        for (q = 0; q < 4; q++) {
            double pq[2];
            pc_algo_placement_project(p, xy[q][0], xy[q][1], pq);
            if (m[6] * pq[0] + m[7] * pq[1] + m[8] <= 0.0) {
                return true;
            }
        }
        for (q = 0; q < 4; q++) {
            x0 = pc_rs_min(x0, c[q][0]);
            x1 = pc_rs_max(x1, c[q][0]);
        }
    }
    /* Horizontal field of view through the reference's focal length. */
    fov = 2.0 * atan((x1 - x0) / 2.0 / a->focal);
    return fov > 110.0 * (3.141592653589793 / 180.0);
}

pc_status pc_algo_panorama_align(const pc_algo_image_features *feats, size_t n,
                                 const pc_algo_pair_match *matches, size_t nmatches,
                                 const pc_algo_align_options *opts, bool *found, pc_algo_alignment *out,
                                 pc_error *err)
{
    double focal;
    pc_algo_alignment a, cyl;
    bool ok = false;
    pc_status st;
    size_t k;
    *found = false;
    if (n < 2 || nmatches == 0) {
        return PC_OK;
    }
    for (k = 0; k < nmatches; k++) {
        if (matches[k].a >= n || matches[k].b >= n) {
            return pc_fail(err, PC_ERR_INVALID, "pair (%zu, %zu) refers to a missing image", matches[k].a,
                           matches[k].b);
        }
    }
    if (opts->has_focal && opts->focal > 1.0) {
        focal = opts->focal;
    } else {
        double *fs = pc_malloc_array(nmatches, sizeof *fs), side = 0.0;
        size_t nf = 0, kept = 0, i, smax = 0;
        if (fs == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory estimating the focal length");
        }
        for (k = 0; k < nmatches; k++) {
            const pc_algo_image_features *fa = &feats[matches[k].a], *fb = &feats[matches[k].b];
            double ca[2] = {(double)fa->w / 2.0, (double)fa->h / 2.0},
                   cb[2] = {(double)fb->w / 2.0, (double)fb->h / 2.0};
            double f;
            if (pc_algo_panorama_focal_from_homography(&matches[k].homography, ca, cb, &f) && isfinite(f)) {
                fs[nf++] = f;
            }
        }
        for (i = 0; i < n; i++) {
            size_t s = feats[i].w > feats[i].h ? feats[i].w : feats[i].h;
            smax = s > smax ? s : smax;
        }
        side = (double)smax;
        for (i = 0; i < nf; i++) {
            if (fs[i] > side * 0.2 && fs[i] < side * 20.0) {
                fs[kept++] = fs[i];
            }
        }
        qsort(fs, kept, sizeof *fs, cmp_f64_total);
        focal = kept == 0 ? side : fs[kept / 2];
        free(fs);
    }
    if (opts->layout != PC_ALGO_LAYOUT_AUTO) {
        return align_layout(feats, n, matches, nmatches, opts, opts->layout, focal, found, out, err);
    }
    /* Auto: perspective unless it is too stretched (Photoshop's rule of thumb), then cylindrical,
     * or spherical when the panorama is also tall. */
    st = align_layout(feats, n, matches, nmatches, opts, PC_ALGO_LAYOUT_PERSPECTIVE, focal, &ok, &a, err);
    if (st != PC_OK) {
        return st;
    }
    if (ok) {
        if (!too_stretched(feats, &a)) {
            *out = a;
            *found = true;
            return PC_OK;
        }
        pc_algo_alignment_fini(&a);
    }
    st = align_layout(feats, n, matches, nmatches, opts, PC_ALGO_LAYOUT_CYLINDRICAL, focal, &ok, &cyl, err);
    if (st != PC_OK || !ok) {
        return st;
    }
    {
        double y0 = DMAX, y1 = -DMAX, bd[4];
        size_t i;
        for (i = 0; i < n; i++) {
            if (cyl.placed[i]) {
                pc_algo_placement_bounds(&cyl.placements[i], (double)feats[i].w, (double)feats[i].h, bd);
                y0 = pc_rs_min(y0, bd[1]);
                y1 = pc_rs_max(y1, bd[3]);
            }
        }
        if ((y1 - y0) / cyl.focal > 1.4) {
            double cf = cyl.focal;
            pc_algo_alignment_fini(&cyl);
            return align_layout(feats, n, matches, nmatches, opts, PC_ALGO_LAYOUT_SPHERICAL, cf, found, out,
                                err);
        }
    }
    *out = cyl;
    *found = true;
    return PC_OK;
}
