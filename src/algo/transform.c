/* Projective warps of surfaces (port of crates/algo/src/transform.rs). */
#include "algo/transform.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/image.h"
#include "algo/resample.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "base/rmath.h"

void pc_algo_homography_apply(const pc_algo_homography *h, double x, double y, double *ox, double *oy)
{
    const double *m = h->m;
    double w = m[6] * x + m[7] * y + m[8];
    *ox = (m[0] * x + m[1] * y + m[2]) / w;
    *oy = (m[3] * x + m[4] * y + m[5]) / w;
}

pc_algo_homography pc_algo_homography_mul(const pc_algo_homography *a, const pc_algo_homography *b)
{
    pc_algo_homography r;
    int i, j, k;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            double s = -0.0; /* Iterator::sum */
            for (k = 0; k < 3; k++) {
                s += a->m[i * 3 + k] * b->m[k * 3 + j];
            }
            r.m[i * 3 + j] = s;
        }
    }
    return r;
}

bool pc_algo_homography_inverse(const pc_algo_homography *h, pc_algo_homography *out)
{
    const double *m = h->m;
    double c[9], det;
    int i;
    c[0] = m[4] * m[8] - m[5] * m[7];
    c[1] = m[2] * m[7] - m[1] * m[8];
    c[2] = m[1] * m[5] - m[2] * m[4];
    c[3] = m[5] * m[6] - m[3] * m[8];
    c[4] = m[0] * m[8] - m[2] * m[6];
    c[5] = m[2] * m[3] - m[0] * m[5];
    c[6] = m[3] * m[7] - m[4] * m[6];
    c[7] = m[1] * m[6] - m[0] * m[7];
    c[8] = m[0] * m[4] - m[1] * m[3];
    det = m[0] * c[0] + m[1] * c[3] + m[2] * c[6];
    if (fabs(det) < 1e-12) {
        return false;
    }
    for (i = 0; i < 9; i++) {
        out->m[i] = c[i] / det;
    }
    return true;
}

/* Unit square -> quad (corners (0,0), (1,0), (1,1), (0,1)). */
static bool square_to_quad(const double q[4][2], pc_algo_homography *out)
{
    double x0 = q[0][0], y0 = q[0][1], x1 = q[1][0], y1 = q[1][1];
    double x2 = q[2][0], y2 = q[2][1], x3 = q[3][0], y3 = q[3][1];
    double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
    double dx1, dx2, dy1, dy2, den, g, h;
    if (fabs(sx) < 1e-12 && fabs(sy) < 1e-12) {
        pc_algo_homography a = {{x1 - x0, x3 - x0, x0, y1 - y0, y3 - y0, y0, 0.0, 0.0, 1.0}};
        *out = a;
        return true;
    }
    dx1 = x1 - x2;
    dx2 = x3 - x2;
    dy1 = y1 - y2;
    dy2 = y3 - y2;
    den = dx1 * dy2 - dx2 * dy1;
    if (fabs(den) < 1e-12) {
        return false;
    }
    g = (sx * dy2 - dx2 * sy) / den;
    h = (dx1 * sy - sx * dy1) / den;
    {
        pc_algo_homography r = {
            {x1 - x0 + g * x1, x3 - x0 + h * x3, x0, y1 - y0 + g * y1, y3 - y0 + h * y3, y0, g, h, 1.0}};
        *out = r;
    }
    return true;
}

bool pc_algo_homography_rect_to_quad(const double r[4], const double quad[4][2], pc_algo_homography *out)
{
    double w = r[2] - r[0], h = r[3] - r[1];
    pc_algo_homography sq;
    if (w <= 0.0 || h <= 0.0) {
        return false;
    }
    if (!square_to_quad(quad, &sq)) {
        return false;
    }
    {
        pc_algo_homography to_unit = {{1.0 / w, 0.0, -r[0] / w, 0.0, 1.0 / h, -r[1] / h, 0.0, 0.0, 1.0}};
        *out = pc_algo_homography_mul(&sq, &to_unit);
    }
    return true;
}

double pc_algo_homography_local_scale(const pc_algo_homography *h, double x, double y)
{
    const double e = 0.5;
    double ax, ay, bx, by, cx, cy, dx, dy;
    pc_algo_homography_apply(h, x - e, y, &ax, &ay);
    pc_algo_homography_apply(h, x + e, y, &bx, &by);
    pc_algo_homography_apply(h, x, y - e, &cx, &cy);
    pc_algo_homography_apply(h, x, y + e, &dx, &dy);
    {
        double jx0 = bx - ax, jx1 = by - ay, jy0 = dx - cx, jy1 = dy - cy;
        return sqrt(fabs(jx0 * jy1 - jx1 * jy0));
    }
}

pc_algo_interp pc_algo_interp_parse(const char *s)
{
    if (s != NULL && (strcmp(s, "nearest") == 0 || strcmp(s, "nearestNeighbor") == 0)) {
        return PC_ALGO_INTERP_NEAREST;
    }
    if (s != NULL && strcmp(s, "bilinear") == 0) {
        return PC_ALGO_INTERP_BILINEAR;
    }
    return PC_ALGO_INTERP_BICUBIC;
}

static void catmull_rom(double t, double out[4])
{
    double t2 = t * t, t3 = t2 * t;
    out[0] = -0.5 * t3 + t2 - 0.5 * t;
    out[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
    out[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    out[3] = 0.5 * t3 - 0.5 * t2;
}

typedef struct warp_ctx {
    const pc_surface *src;
    pc_rect src_rect, dst;
    pc_algo_homography inv;
    pc_algo_interp interp;
    size_t n;
    pc_tile first;
    int64_t tiles_x;
    size_t group0;
    pc_rect *rects; /* per tile of the group */
    float **data;   /* per tile; NULL when the tile is empty */
    pc_error *errs;
} warp_ctx;

/* Min / max over values as `fold(f64::MAX, f64::min)` / `fold(f64::MIN, f64::max)` do. */
static double fold_min(const double *v, int n)
{
    double m = 1.7976931348623157e308;
    int i;
    for (i = 0; i < n; i++) {
        m = pc_rs_min(m, v[i]);
    }
    return m;
}

static double fold_max(const double *v, int n)
{
    double m = -1.7976931348623157e308;
    int i;
    for (i = 0; i < n; i++) {
        m = pc_rs_max(m, v[i]);
    }
    return m;
}

/* One destination tile; *out stays NULL when nothing in it is covered. */
static pc_status warp_tile(const warp_ctx *w, pc_rect t, float **out, pc_error *err)
{
    double xs[4], ys[4];
    int32_t fx0, fy0, fx1, fy1;
    pc_rect foot;
    float *px = NULL, *outp = NULL;
    size_t plen = 0, fw, tw, n = w->n, a = n - 1, olen, c;
    bool any = false;
    int32_t x, y;
    double acc[PC_RASTER_MAX_CHANNELS] = {0.0, 0.0, 0.0, 0.0, 0.0};
    pc_status st;
    *out = NULL;
    if (n < 1 || n > PC_RASTER_MAX_CHANNELS) {
        return pc_fail(err, PC_ERR_INVALID, "warp of a %zu-channel surface", n);
    }
    /* Source footprint of this tile (inverse-mapped corners), padded for the filter. */
    pc_algo_homography_apply(&w->inv, (double)t.x0, (double)t.y0, &xs[0], &ys[0]);
    pc_algo_homography_apply(&w->inv, (double)t.x1, (double)t.y0, &xs[1], &ys[1]);
    pc_algo_homography_apply(&w->inv, (double)t.x1, (double)t.y1, &xs[2], &ys[2]);
    pc_algo_homography_apply(&w->inv, (double)t.x0, (double)t.y1, &xs[3], &ys[3]);
    fx0 = pc_i32_wrapping_sub(pc_f64_to_i32(floor(fold_min(xs, 4))), 3);
    fy0 = pc_i32_wrapping_sub(pc_f64_to_i32(floor(fold_min(ys, 4))), 3);
    fx1 = pc_i32_wrapping_add(pc_f64_to_i32(ceil(fold_max(xs, 4))), 3);
    fy1 = pc_i32_wrapping_add(pc_f64_to_i32(ceil(fold_max(ys, 4))), 3);
    foot = pc_rect_intersect(pc_rect_make(fx0, fy0, fx1, fy1), w->src_rect);
    if (pc_rect_is_empty(foot) || !pc_surface_has_tiles_in(w->src, foot)) {
        return PC_OK;
    }
    /* Premultiplied source window. */
    st = pc_surface_read_region(w->src, foot, &px, &plen, err);
    if (st != PC_OK) {
        return st;
    }
    pc_algo_premultiply(px, plen, n, true);
    fw = pc_rect_width(foot);
    tw = pc_rect_width(t);
    olen = tw * pc_rect_height(t) * n;
    outp = pc_calloc_array(olen, sizeof(float));
    if (outp == NULL) {
        free(px);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory warping");
    }
#define AT(xx, yy, cc)                                                                                       \
    (((xx) < foot.x0 || (yy) < foot.y0 || (xx) >= foot.x1 || (yy) >= foot.y1)                                \
         ? 0.0                                                                                               \
         : (double)                                                                                          \
               px[((size_t)((int64_t)(yy) - foot.y0) * fw + (size_t)((int64_t)(xx) - foot.x0)) * n + (cc)])
    for (y = t.y0; y < t.y1; y++) {
        for (x = t.x0; x < t.x1; x++) {
            double u, v, al;
            size_t o;
            pc_algo_homography_apply(&w->inv, (double)x + 0.5, (double)y + 0.5, &u, &v);
            u -= 0.5;
            v -= 0.5;
            if (u < (double)w->src_rect.x0 - 1.0 || v < (double)w->src_rect.y0 - 1.0 ||
                u > (double)w->src_rect.x1 || v > (double)w->src_rect.y1) {
                continue;
            }
            for (c = 0; c < n; c++) {
                acc[c] = 0.0;
            }
            if (w->interp == PC_ALGO_INTERP_NEAREST) {
                int32_t ix = pc_f64_to_i32(floor(u + 0.5)), iy = pc_f64_to_i32(floor(v + 0.5));
                for (c = 0; c < n; c++) {
                    acc[c] = AT(ix, iy, c);
                }
            } else if (w->interp == PC_ALGO_INTERP_BILINEAR) {
                int32_t ix = pc_f64_to_i32(floor(u)), iy = pc_f64_to_i32(floor(v));
                double fx = u - (double)ix, fy = v - (double)iy;
                double wy[2], wx[2];
                int dy, dx;
                wy[0] = 1.0 - fy, wy[1] = fy, wx[0] = 1.0 - fx, wx[1] = fx;
                for (dy = 0; dy < 2; dy++) {
                    for (dx = 0; dx < 2; dx++) {
                        int32_t sx = pc_i32_wrapping_add(ix, dx), sy = pc_i32_wrapping_add(iy, dy);
                        for (c = 0; c < n; c++) {
                            acc[c] += AT(sx, sy, c) * wx[dx] * wy[dy];
                        }
                    }
                }
            } else {
                int32_t ix = pc_f64_to_i32(floor(u)), iy = pc_f64_to_i32(floor(v));
                double wx[4], wy[4];
                int i, j;
                catmull_rom(u - (double)ix, wx);
                catmull_rom(v - (double)iy, wy);
                for (j = 0; j < 4; j++) {
                    for (i = 0; i < 4; i++) {
                        double k = wx[i] * wy[j];
                        int32_t sx = pc_i32_wrapping_add(pc_i32_wrapping_sub(ix, 1), i);
                        int32_t sy = pc_i32_wrapping_add(pc_i32_wrapping_sub(iy, 1), j);
                        for (c = 0; c < n; c++) {
                            acc[c] += AT(sx, sy, c) * k;
                        }
                    }
                }
            }
            al = pc_clampd(acc[a], 0.0, 1.0);
            if (al <= 0.0) {
                continue;
            }
            any = true;
            o = ((size_t)((int64_t)y - t.y0) * tw + (size_t)((int64_t)x - t.x0)) * n;
            for (c = 0; c < a; c++) {
                outp[o + c] = (float)pc_clampd(acc[c] / al, 0.0, 1.0);
            }
            outp[o + a] = (float)al;
        }
    }
#undef AT
    free(px);
    if (!any) {
        free(outp);
        return PC_OK;
    }
    *out = outp;
    return PC_OK;
}

static pc_status warp_job(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const warp_ctx *w = vctx;
    size_t k;
    (void)chunk;
    for (k = begin; k < end; k++) {
        pc_status st = warp_tile(w, w->rects[k], &w->data[k], &w->errs[k]);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

pc_status pc_algo_warp_surface(const pc_surface *src, pc_rect src_rect, const pc_algo_homography *h,
                               pc_algo_interp interp, pc_surface *out, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(src);
    pc_surface converted, reduced, res;
    const pc_surface *s = src;
    bool have_conv = false, have_red = false;
    pc_algo_homography hh = *h, inv;
    double cx, cy, scale, xs[4], ys[4], lim = (double)(1 << 20);
    int32_t bx0, by0, bx1, by1;
    pc_tile first, last;
    warp_ctx w;
    size_t group, total, g0, i;
    pc_status st = PC_OK;
    memset(&w, 0, sizeof w);
    if (!fmt.alpha) {
        fmt.alpha = true;
        st = pc_surface_convert(src, fmt, &converted, err);
        if (st != PC_OK) {
            return st;
        }
        have_conv = true;
        s = &converted;
    }
    pc_surface_init(&res, fmt);
    if (pc_rect_is_empty(src_rect)) {
        goto finish;
    }
    /* Pre-reduce for large downscales (bicubic alone aliases below ~50%). */
    cx = (double)pc_i32_wrapping_add(src_rect.x0, src_rect.x1) / 2.0;
    cy = (double)pc_i32_wrapping_add(src_rect.y0, src_rect.y1) / 2.0;
    scale = pc_algo_homography_local_scale(&hh, cx, cy);
    if (interp != PC_ALGO_INTERP_NEAREST && scale < 0.5 && scale > 0.0) {
        /* 2f64.powf(x) is exp2(x) after LLVM; the remaining scale lands in [0.5, 1). */
        double f = pc_rs_min_k(exp2(ceil(log2(scale))), 1.0);
        pc_algo_homography d = {{1.0 / f, 0.0, 0.0, 0.0, 1.0 / f, 0.0, 0.0, 0.0, 1.0}};
        st = pc_algo_resize_surface(s, f, f, PC_ALGO_RESAMPLE_BICUBIC, &reduced, err);
        if (st != PC_OK) {
            goto finish;
        }
        have_red = true;
        s = &reduced;
        hh = pc_algo_homography_mul(&hh, &d);
        src_rect = pc_algo_scaled_rect(src_rect, f, f);
    }
    if (!pc_algo_homography_inverse(&hh, &inv)) {
        goto finish;
    }
    /* Destination bounds: the warped corners (plus a pixel for filter support). */
    pc_algo_homography_apply(&hh, (double)src_rect.x0, (double)src_rect.y0, &xs[0], &ys[0]);
    pc_algo_homography_apply(&hh, (double)src_rect.x1, (double)src_rect.y0, &xs[1], &ys[1]);
    pc_algo_homography_apply(&hh, (double)src_rect.x1, (double)src_rect.y1, &xs[2], &ys[2]);
    pc_algo_homography_apply(&hh, (double)src_rect.x0, (double)src_rect.y1, &xs[3], &ys[3]);
    for (i = 0; i < 4; i++) {
        if (!isfinite(xs[i]) || !isfinite(ys[i])) {
            goto finish;
        }
    }
    bx0 = pc_f64_to_i32(pc_rs_max_k(floor(fold_min(xs, 4)), -lim)) - 1;
    by0 = pc_f64_to_i32(pc_rs_max_k(floor(fold_min(ys, 4)), -lim)) - 1;
    bx1 = pc_f64_to_i32(pc_rs_min_k(ceil(fold_max(xs, 4)), lim)) + 1;
    by1 = pc_f64_to_i32(pc_rs_min_k(ceil(fold_max(ys, 4)), lim)) + 1;
    w.dst = pc_rect_make(bx0, by0, bx1, by1);
    if (!pc_rect_tile_range(w.dst, &first, &last)) {
        goto finish;
    }
    w.src = s;
    w.src_rect = src_rect;
    w.inv = inv;
    w.interp = interp;
    w.n = pc_pixel_format_channels(fmt);
    w.first = first;
    w.tiles_x = (int64_t)last.tx - first.tx + 1;
    total = (size_t)(((int64_t)last.ty - first.ty + 1) * w.tiles_x);
    group = pc_pool_threads(pc_pool_global()) * 4;
    if (group > total) {
        group = total;
    }
    w.rects = pc_calloc_array(group, sizeof *w.rects);
    w.data = pc_calloc_array(group, sizeof *w.data);
    w.errs = pc_calloc_array(group, sizeof *w.errs);
    if (w.rects == NULL || w.data == NULL || w.errs == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory warping");
        goto finish;
    }
    /* Tiles run a group at a time so the results held at once stay bounded; each covered tile's
     * pixels are written in tile order after its group. */
    for (g0 = 0; g0 < total && st == PC_OK; g0 += group) {
        size_t gn = total - g0 < group ? total - g0 : group;
        for (i = 0; i < gn; i++) {
            size_t idx = g0 + i;
            pc_tile tc;
            tc.tx = (int32_t)(first.tx + (int64_t)(idx % (size_t)w.tiles_x));
            tc.ty = (int32_t)(first.ty + (int64_t)(idx / (size_t)w.tiles_x));
            w.rects[i] = pc_rect_intersect(pc_tile_rect(tc), w.dst);
            w.data[i] = NULL;
            w.errs[i].code = PC_OK;
        }
        st = pc_pool_for(pc_pool_global(), gn, 1, warp_job, &w);
        if (st != PC_OK) {
            for (i = 0; i < gn; i++) {
                if (w.errs[i].code != PC_OK) {
                    if (err != NULL) {
                        *err = w.errs[i];
                    }
                    st = w.errs[i].code;
                    break;
                }
            }
        }
        for (i = 0; i < gn && st == PC_OK; i++) {
            if (w.data[i] != NULL) {
                size_t len = pc_rect_width(w.rects[i]) * pc_rect_height(w.rects[i]) * w.n;
                st = pc_surface_write_region(&res, w.rects[i], w.data[i], len, err);
            }
        }
        for (i = 0; i < gn; i++) {
            free(w.data[i]);
            w.data[i] = NULL;
        }
    }
    if (st == PC_OK) {
        pc_surface_prune(&res);
    }
finish:
    free(w.rects);
    free(w.data);
    free(w.errs);
    if (have_red) {
        pc_surface_fini(&reduced);
    }
    if (have_conv) {
        pc_surface_fini(&converted);
    }
    if (st != PC_OK) {
        pc_surface_fini(&res);
        return st;
    }
    *out = res;
    return PC_OK;
}
