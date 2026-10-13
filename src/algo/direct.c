/* Direct registration for Auto-Align Layers (port of crates/algo/src/direct.rs); see direct.h. */
#include "algo/direct.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* The search box's half-width around the start, per parameter: 10 % of the layer in shift, 10 %
 * in scale, 5 degrees in rotation, 5 % in aspect and shear, 5 % in each perspective term. */
static const double SPAN[PC_ALGO_POSE_N] = {0.10, 0.10, 0.10, 0.08726646259971647, 0.05, 0.05, 0.05, 0.05};
/* The simplex's first step and stopping size at a level, in pixels of that level. */
#define STEP_PX 1.0
#define TOL_PX 0.1

void pc_algo_pose_to_vec(const pc_algo_pose *p, double out[PC_ALGO_POSE_N])
{
    out[0] = p->dx, out[1] = p->dy, out[2] = p->scale, out[3] = p->rot;
    out[4] = p->aspect, out[5] = p->shear, out[6] = p->px, out[7] = p->py;
}

pc_algo_pose pc_algo_pose_from_vec(const double v[PC_ALGO_POSE_N])
{
    pc_algo_pose p;
    p.dx = v[0], p.dy = v[1], p.scale = v[2], p.rot = v[3];
    p.aspect = v[4], p.shear = v[5], p.px = v[6], p.py = v[7];
    return p;
}

void pc_algo_pose_free(pc_algo_motion motion, bool out[PC_ALGO_POSE_N])
{
    size_t k;
    for (k = 0; k < PC_ALGO_POSE_N; k++) {
        out[k] = motion == PC_ALGO_MOTION_HOMOGRAPHY;
    }
    out[0] = out[1] = true;
    if (motion == PC_ALGO_MOTION_EUCLIDEAN || motion == PC_ALGO_MOTION_SIMILARITY) {
        out[3] = true;
    }
    if (motion == PC_ALGO_MOTION_SIMILARITY) {
        out[2] = true;
    }
}

pc_algo_homography pc_algo_pose_matrix(const pc_algo_pose *p, size_t w, size_t h)
{
    double cx = (double)w / 2.0, cy = (double)h / 2.0, c = cos(p->rot), s = sin(p->rot), sc = p->scale;
    double a = sc * c, b = sc * (c * p->shear - s * p->aspect), d = sc * s,
           e = sc * (s * p->shear + c * p->aspect);
    double gx = p->px / (double)w, gy = p->py / (double)h;
    /* T(c) . [[a, b, t], [d, e, t'], [gx, gy, 1]] . T(-c), written out. */
    double a2 = a + cx * gx, b2 = b + cx * gy, d2 = d + cy * gx, e2 = e + cy * gy;
    double tx = cx + p->dx * (double)w - (a2 * cx + b2 * cy);
    double ty = cy + p->dy * (double)h - (d2 * cx + e2 * cy);
    pc_algo_homography m = {{a2, b2, tx, d2, e2, ty, gx, gy, 1.0 - gx * cx - gy * cy}};
    return m;
}

/* ---- pyramid ---- */

typedef struct lvl {
    float *luma;
    bool *valid; /* NULL: covered everywhere */
    size_t w, h;
} lvl;

static void lvl_free(lvl *l)
{
    free(l->luma);
    free(l->valid);
    l->luma = NULL;
    l->valid = NULL;
}

static const float K5[5] = {1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f};

/* One REDUCE pass (5-tap binomial, clamped borders, phase-0 subsample) of a w x h plane into
 * (w+1)/2 x (h+1)/2. */
static bool reduce_pass(const float *src, size_t sw, size_t sh, size_t w, size_t h, float **out)
{
    float *tmp = pc_malloc_array(w * sh, sizeof *tmp), *o = pc_calloc_array(w * h, sizeof *o);
    size_t x, y;
    int k;
    if (tmp == NULL || o == NULL) {
        free(tmp), free(o);
        return false;
    }
    for (y = 0; y < sh; y++) {
        const float *s = src + y * sw;
        for (x = 0; x < w; x++) {
            float acc = -0.0f;
            for (k = 0; k < 5; k++) {
                size_t i = 2 * x + (size_t)k;
                i = i >= 2 ? i - 2 : 0;
                acc += K5[k] * s[i < sw - 1 ? i : sw - 1];
            }
            tmp[y * w + x] = acc;
        }
    }
    for (y = 0; y < h; y++) {
        for (k = 0; k < 5; k++) {
            size_t sy = 2 * y + (size_t)k;
            const float *row;
            sy = sy >= 2 ? sy - 2 : 0;
            sy = sy < sh - 1 ? sy : sh - 1;
            row = tmp + sy * w;
            for (x = 0; x < w; x++) {
                o[y * w + x] += K5[k] * row[x];
            }
        }
    }
    free(tmp);
    *out = o;
    return true;
}

/* Gaussian REDUCE of a plane and its coverage; a reduced pixel is covered when more than half
 * its support is. */
static bool reduce(const lvl *l, lvl *out)
{
    size_t w = (l->w + 1) / 2, h = (l->h + 1) / 2, i;
    memset(out, 0, sizeof *out);
    out->w = w;
    out->h = h;
    if (!reduce_pass(l->luma, l->w, l->h, w, h, &out->luma)) {
        return false;
    }
    if (l->valid != NULL) {
        float *cover = pc_malloc_array(l->w * l->h, sizeof *cover), *red = NULL;
        if (cover == NULL) {
            lvl_free(out);
            return false;
        }
        for (i = 0; i < l->w * l->h; i++) {
            cover[i] = l->valid[i] ? 1.0f : 0.0f;
        }
        if (!reduce_pass(cover, l->w, l->h, w, h, &red)) {
            free(cover);
            lvl_free(out);
            return false;
        }
        free(cover);
        out->valid = pc_malloc_array(w * h, sizeof *out->valid);
        if (out->valid == NULL) {
            free(red);
            lvl_free(out);
            return false;
        }
        for (i = 0; i < w * h; i++) {
            out->valid[i] = red[i] > 0.5f;
        }
        free(red);
    }
    return true;
}

static void levels_free(lvl *l, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        lvl_free(&l[i]);
    }
    free(l);
}

/* The search pyramid: level 0 is the plane, each next level half its size, while the height
 * exceeds 64 and the width 8. NULL on NOMEM. */
static lvl *levels(const pc_algo_direct_plane *p, size_t *n)
{
    size_t cap = 1, count = 1, s = p->w < p->h ? p->w : p->h;
    lvl *out;
    while (s > 1) {
        s /= 2;
        cap++;
    }
    cap += 2;
    out = pc_calloc_array(cap, sizeof *out);
    if (out == NULL) {
        return NULL;
    }
    out[0].w = p->w;
    out[0].h = p->h;
    out[0].luma = pc_malloc_array(p->w * p->h, sizeof *out[0].luma);
    if (out[0].luma == NULL) {
        free(out);
        return NULL;
    }
    memcpy(out[0].luma, p->luma, p->w * p->h * sizeof *p->luma);
    if (p->valid != NULL) {
        out[0].valid = pc_malloc_array(p->w * p->h, sizeof *out[0].valid);
        if (out[0].valid == NULL) {
            levels_free(out, 1);
            return NULL;
        }
        memcpy(out[0].valid, p->valid, p->w * p->h * sizeof *p->valid);
    }
    while (count < cap && out[count - 1].h > 64 && out[count - 1].w > 8) {
        if (!reduce(&out[count - 1], &out[count])) {
            levels_free(out, count);
            return NULL;
        }
        count++;
    }
    *n = count;
    return out;
}

/* ---- sampling ---- */

/* The cubic B-spline (4 taps) at fraction t. */
static void spline4(double t, double out[4])
{
    out[0] = ((-1.0 / 3.0 * t + 0.8) * t - 0.46666667) * t;
    out[1] = ((t - 1.8) * t - 0.2) * t + 1.0;
    out[2] = ((1.2 - t) * t + 0.8) * t;
    out[3] = ((1.0 / 3.0 * t - 0.2) * t - 0.13333334) * t;
}

/* src sampled at (sx, sy) with the cubic B-spline, edge-clamped. */
static float sample(const float *src, size_t w, size_t h, double sx, double sy)
{
    double x0 = floor(sx), y0 = floor(sy), wx[4], wy[4], acc = 0.0;
    int64_t ix = pc_f64_to_i64(x0), iy = pc_f64_to_i64(y0);
    int i, j;
    spline4(sx - x0, wx);
    spline4(sy - y0, wy);
    for (j = 0; j < 4; j++) {
        size_t yy = (size_t)pc_clamp_i64(iy + j - 1, 0, (int64_t)h - 1);
        double r = 0.0;
        for (i = 0; i < 4; i++) {
            size_t xx = (size_t)pc_clamp_i64(ix + i - 1, 0, (int64_t)w - 1);
            r += wx[i] * (double)src[yy * w + xx];
        }
        acc += wy[j] * r;
    }
    return pc_f64_to_f32(acc);
}

/* The source points along output row y under an affine inv: pixel x reads (x0 + dx x, y0 + dy x);
 * false for a projective inv. */
static bool affine_row(const pc_algo_homography *inv, double y, double r[4])
{
    const double *m = inv->m;
    if (!(m[6] == 0.0 && m[7] == 0.0 && m[8] == 1.0)) {
        return false;
    }
    r[0] = m[1] * y + m[2];
    r[1] = m[4] * y + m[5];
    r[2] = m[0];
    r[3] = m[3];
    return true;
}

/* Whether the target covers the source point (nearest pixel of its coverage). */
static bool covered(const bool *valid, size_t w, double sx, double sy)
{
    return valid == NULL || valid[pc_f64_to_size(sy + 0.5) * w + pc_f64_to_size(sx + 0.5)];
}

/* ---- cost ---- */

typedef struct row_sums {
    double sr, st, sr2, st2, srt;
    size_t cnt;
} row_sums;

typedef struct cost_ctx {
    const lvl *ref, *tgt;
    pc_algo_homography inv;
    row_sums *rows;
} cost_ctx;

static pc_status cost_rows(void *vc, size_t chunk, size_t y0, size_t y1)
{
    const cost_ctx *c = vc;
    const lvl *ref = c->ref, *tgt = c->tgt;
    double xmax = (double)(tgt->w - 1), ymax = (double)(tgt->h - 1);
    size_t y, x;
    (void)chunk;
    for (y = y0; y < y1; y++) {
        row_sums s = {0.0, 0.0, 0.0, 0.0, 0.0, 0};
        double yf = (double)y, aff[4];
        bool is_aff = affine_row(&c->inv, yf, aff);
        const float *row = ref->luma + y * ref->w;
        for (x = 0; x < ref->w; x++) {
            double sx, sy, r, t;
            if (ref->valid != NULL && !ref->valid[y * ref->w + x]) {
                continue;
            }
            if (is_aff) {
                sx = aff[0] + aff[2] * (double)x;
                sy = aff[1] + aff[3] * (double)x;
            } else {
                pc_algo_homography_apply(&c->inv, (double)x, yf, &sx, &sy);
            }
            if (!(sx >= 0.0 && sx <= xmax && sy >= 0.0 && sy <= ymax) ||
                !covered(tgt->valid, tgt->w, sx, sy)) {
                continue;
            }
            r = (double)row[x];
            t = (double)sample(tgt->luma, tgt->w, tgt->h, sx, sy);
            s.sr += r;
            s.st += t;
            s.sr2 += r * r;
            s.st2 += t * t;
            s.srt += r * t;
            s.cnt++;
        }
        c->rows[y] = s;
    }
    return PC_OK;
}

/* The cost of pose: the RMS of reference - target(pose), the mean removed, over the pixels both
 * cover (1e9 when fewer than 16 do), the overlap and the correlation. False on NOMEM. */
static bool cost(const lvl *ref, const lvl *tgt, const pc_algo_pose *pose, double *rms, size_t *overlap,
                 double *corr)
{
    pc_algo_homography m = pc_algo_pose_matrix(pose, tgt->w, tgt->h);
    double sr = 0.0, st = 0.0, sr2 = 0.0, st2 = 0.0, srt = 0.0, n, vr, vt, cov;
    size_t cnt = 0, y;
    cost_ctx c;
    *rms = 1e9, *overlap = 0, *corr = 0.0;
    if (!pc_algo_homography_inverse(&m, &c.inv)) {
        return true;
    }
    c.ref = ref;
    c.tgt = tgt;
    c.rows = pc_malloc_array(ref->h > 0 ? ref->h : 1, sizeof *c.rows);
    if (c.rows == NULL) {
        return false;
    }
    (void)pc_algo_par_for(ref->h, 1, cost_rows, &c);
    for (y = 0; y < ref->h; y++) {
        sr = sr + c.rows[y].sr;
        st = st + c.rows[y].st;
        sr2 = sr2 + c.rows[y].sr2;
        st2 = st2 + c.rows[y].st2;
        srt = srt + c.rows[y].srt;
        cnt += c.rows[y].cnt;
    }
    free(c.rows);
    *overlap = cnt;
    if (cnt < 16) {
        return true;
    }
    n = (double)cnt;
    /* Central moments. */
    vr = pc_rs_max_k(sr2 - sr * sr / n, 0.0);
    vt = pc_rs_max_k(st2 - st * st / n, 0.0);
    cov = srt - sr * st / n;
    *rms = sqrt(pc_rs_max_k((vr + vt - 2.0 * cov) / n, 0.0));
    *corr = vr > 0.0 && vt > 0.0 ? pc_clampd(cov / sqrt(vr * vt), -1.0, 1.0) : 0.0;
    return true;
}

/* ---- Nelder-Mead ---- */

static int total_cmp(double a, double b)
{
    int64_t ia, ib;
    memcpy(&ia, &a, 8);
    memcpy(&ib, &b, 8);
    ia ^= (int64_t)((uint64_t)(ia >> 63) >> 1);
    ib ^= (int64_t)((uint64_t)(ib >> 63) >> 1);
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

static bool converged(double (*simplex)[PC_ALGO_POSE_N], const double *fv, const double *tol, size_t n)
{
    size_t v, k;
    if (fabs(fv[n] - fv[0]) > 1e-4 * (1.0 + fabs(fv[0]))) {
        return false;
    }
    for (v = 1; v <= n; v++) {
        for (k = 0; k < n; k++) {
            if (!(fabs(simplex[v][k] - simplex[0][k]) <= tol[k])) {
                return false;
            }
        }
    }
    return true;
}

static void clamp_box(double *v, const double *lo, const double *hi, size_t n)
{
    size_t k;
    for (k = 0; k < n; k++) {
        v[k] = pc_clampd(v[k], lo[k], hi[k]);
    }
}

bool pc_algo_nelder_mead(pc_algo_nm_fn f, void *ctx, size_t n, const double *x0, const double *lo,
                         const double *hi, const double *step, const double *tol, double *out)
{
    double simplex[PC_ALGO_POSE_N + 1][PC_ALGO_POSE_N], fv[PC_ALGO_POSE_N + 1], c[PC_ALGO_POSE_N];
    double xr[PC_ALGO_POSE_N], xe[PC_ALGO_POSE_N], xc[PC_ALGO_POSE_N];
    const double alpha = 1.0, gamma = 2.0, rho = 0.5, sigma = 0.5;
    size_t v, k, it, best;
    if (n == 0) {
        return true;
    }
    if (n > PC_ALGO_POSE_N) {
        return false;
    }
    memcpy(simplex[0], x0, n * sizeof *x0);
    for (k = 0; k < n; k++) {
        memcpy(simplex[k + 1], x0, n * sizeof *x0);
        simplex[k + 1][k] += step[k];
        clamp_box(simplex[k + 1], lo, hi, n);
    }
    for (v = 0; v <= n; v++) {
        fv[v] = f(ctx, simplex[v]);
    }
    for (it = 0; it < 200; it++) {
        double fr;
        /* Stable sort by value (insertion sort: at most 9 vertices). */
        for (v = 1; v <= n; v++) {
            double kv[PC_ALGO_POSE_N], kf = fv[v];
            size_t j = v;
            memcpy(kv, simplex[v], sizeof kv);
            while (j > 0 && total_cmp(fv[j - 1], kf) > 0) {
                memcpy(simplex[j], simplex[j - 1], sizeof kv);
                fv[j] = fv[j - 1];
                j--;
            }
            memcpy(simplex[j], kv, sizeof kv);
            fv[j] = kf;
        }
        if (converged(simplex, fv, tol, n)) {
            break;
        }
        for (k = 0; k < n; k++) {
            c[k] = 0.0;
        }
        for (v = 0; v < n; v++) {
            for (k = 0; k < n; k++) {
                c[k] += simplex[v][k] / (double)n;
            }
        }
        for (k = 0; k < n; k++) {
            xr[k] = c[k] + alpha * (c[k] - simplex[n][k]);
        }
        clamp_box(xr, lo, hi, n);
        fr = f(ctx, xr);
        if (fr < fv[0]) {
            double fe;
            for (k = 0; k < n; k++) {
                xe[k] = c[k] + gamma * (xr[k] - c[k]);
            }
            clamp_box(xe, lo, hi, n);
            fe = f(ctx, xe);
            if (fe < fr) {
                memcpy(simplex[n], xe, n * sizeof *xe);
                fv[n] = fe;
            } else {
                memcpy(simplex[n], xr, n * sizeof *xr);
                fv[n] = fr;
            }
        } else if (fr < fv[n - 1]) {
            memcpy(simplex[n], xr, n * sizeof *xr);
            fv[n] = fr;
        } else {
            double fc;
            for (k = 0; k < n; k++) {
                xc[k] = c[k] + rho * (simplex[n][k] - c[k]);
            }
            clamp_box(xc, lo, hi, n);
            fc = f(ctx, xc);
            if (fc < fv[n]) {
                memcpy(simplex[n], xc, n * sizeof *xc);
                fv[n] = fc;
            } else {
                for (v = 1; v <= n; v++) {
                    for (k = 0; k < n; k++) {
                        simplex[v][k] = simplex[0][k] + sigma * (simplex[v][k] - simplex[0][k]);
                    }
                    clamp_box(simplex[v], lo, hi, n);
                    fv[v] = f(ctx, simplex[v]);
                }
            }
        }
    }
    /* min_by: the first of equal minima. */
    best = 0;
    for (v = 1; v <= n; v++) {
        if (total_cmp(fv[v], fv[best]) < 0) {
            best = v;
        }
    }
    memcpy(out, simplex[best], n * sizeof *out);
    return true;
}

/* ---- registration ---- */

typedef struct reg_ctx {
    const lvl *rl, *tl;
    double snap[PC_ALGO_POSE_N];
    size_t free_idx[PC_ALGO_POSE_N], nfree;
    bool nomem;
} reg_ctx;

static double reg_cost(void *vc, const double *xf)
{
    reg_ctx *c = vc;
    double v[PC_ALGO_POSE_N], rms, corr;
    size_t k, ov;
    pc_algo_pose p;
    memcpy(v, c->snap, sizeof v);
    for (k = 0; k < c->nfree; k++) {
        v[c->free_idx[k]] = xf[k];
    }
    p = pc_algo_pose_from_vec(v);
    if (!cost(c->rl, c->tl, &p, &rms, &ov, &corr)) {
        c->nomem = true;
    }
    return rms;
}

pc_status pc_algo_direct_register(const pc_algo_direct_plane *reference, const pc_algo_direct_plane *target,
                                  pc_algo_motion motion, pc_algo_pose init, size_t coarsen,
                                  pc_algo_registration *out, pc_error *err)
{
    size_t w = target->w, h = target->h, nref = 0, ntgt = 0, n, finest, k, lv;
    lvl *pref = NULL, *ptgt = NULL;
    bool fr[PC_ALGO_POSE_N];
    double iv[PC_ALGO_POSE_N], cur[PC_ALGO_POSE_N], lo[PC_ALGO_POSE_N], hi[PC_ALGO_POSE_N];
    double rms = 1e9, corr = 0.0;
    size_t overlap = 0;
    reg_ctx rc;
    pc_algo_pose pose;
    if (w == 0 || h == 0 || reference->w != w || reference->h != h || reference->luma == NULL ||
        target->luma == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "auto-align: the planes must be non-empty and of one size");
    }
    pref = levels(reference, &nref);
    ptgt = pref != NULL ? levels(target, &ntgt) : NULL;
    if (pref == NULL || ptgt == NULL) {
        if (pref != NULL) {
            levels_free(pref, nref);
        }
        return pc_fail(err, PC_ERR_NOMEM, "auto-align: out of memory");
    }
    n = nref < ntgt ? nref : ntgt;
    pc_algo_pose_free(motion, fr);
    rc.nfree = 0;
    for (k = 0; k < PC_ALGO_POSE_N; k++) {
        if (fr[k]) {
            rc.free_idx[rc.nfree++] = k;
        }
    }
    pc_algo_pose_to_vec(&init, iv);
    memcpy(cur, iv, sizeof cur);
    for (k = 0; k < rc.nfree; k++) {
        lo[k] = iv[rc.free_idx[k]] - SPAN[rc.free_idx[k]];
        hi[k] = iv[rc.free_idx[k]] + SPAN[rc.free_idx[k]];
    }
    finest = coarsen < n - 1 ? coarsen : n - 1;
    rc.nomem = false;
    for (lv = n; lv-- > finest;) {
        double x0[PC_ALGO_POSE_N], step[PC_ALGO_POSE_N], tol[PC_ALGO_POSE_N], best[PC_ALGO_POSE_N];
        double gain[PC_ALGO_POSE_N], px = (double)((uint64_t)1 << (lv < 60 ? lv : 60));
        double wd = (double)w, hd = (double)h, r = pc_rs_max(wd, hd) / 2.0;
        gain[0] = wd, gain[1] = hd, gain[2] = r, gain[3] = r;
        gain[4] = hd / 2.0, gain[5] = hd / 2.0, gain[6] = wd / 4.0, gain[7] = hd / 4.0;
        rc.rl = &pref[lv];
        rc.tl = &ptgt[lv];
        memcpy(rc.snap, cur, sizeof cur);
        for (k = 0; k < rc.nfree; k++) {
            x0[k] = cur[rc.free_idx[k]];
            step[k] = STEP_PX * px / gain[rc.free_idx[k]];
            tol[k] = TOL_PX * px / gain[rc.free_idx[k]];
        }
        (void)pc_algo_nelder_mead(reg_cost, &rc, rc.nfree, x0, lo, hi, step, tol, best);
        for (k = 0; k < rc.nfree; k++) {
            cur[rc.free_idx[k]] = best[k];
        }
        pose = pc_algo_pose_from_vec(cur);
        if (rc.nomem || !cost(rc.rl, rc.tl, &pose, &rms, &overlap, &corr)) {
            levels_free(pref, nref);
            levels_free(ptgt, ntgt);
            return pc_fail(err, PC_ERR_NOMEM, "auto-align: out of memory");
        }
    }
    levels_free(pref, nref);
    levels_free(ptgt, ntgt);
    pose = pc_algo_pose_from_vec(cur);
    out->pose = pose;
    out->h = pc_algo_pose_matrix(&pose, w, h);
    out->rms = rms;
    out->overlap = overlap;
    out->correlation = corr;
    return PC_OK;
}

pc_status pc_algo_direct_warp(const pc_algo_direct_plane *src, const pc_algo_pose *pose, float **out_luma,
                              bool **out_valid, pc_error *err)
{
    size_t w = src->w, h = src->h, x, y;
    float *o = pc_calloc_array(w * h > 0 ? w * h : 1, sizeof *o);
    bool *v = pc_calloc_array(w * h > 0 ? w * h : 1, sizeof *v);
    pc_algo_homography m = pc_algo_pose_matrix(pose, w, h), inv;
    if (o == NULL || v == NULL) {
        free(o), free(v);
        return pc_fail(err, PC_ERR_NOMEM, "auto-align: out of memory");
    }
    if (w > 0 && h > 0 && pc_algo_homography_inverse(&m, &inv)) {
        double xmax = (double)(w - 1), ymax = (double)(h - 1);
        for (y = 0; y < h; y++) {
            double yf = (double)y, aff[4];
            bool is_aff = affine_row(&inv, yf, aff);
            for (x = 0; x < w; x++) {
                double sx, sy;
                bool inside;
                if (is_aff) {
                    sx = aff[0] + aff[2] * (double)x;
                    sy = aff[1] + aff[3] * (double)x;
                } else {
                    pc_algo_homography_apply(&inv, (double)x, yf, &sx, &sy);
                }
                inside = sx >= 0.0 && sx <= xmax && sy >= 0.0 && sy <= ymax;
                v[y * w + x] = inside && covered(src->valid, w, sx, sy);
                if (inside) {
                    o[y * w + x] = sample(src->luma, w, h, sx, sy);
                }
            }
        }
    }
    *out_luma = o;
    *out_valid = v;
    return PC_OK;
}
