/* Mesh warps (port of crates/algo/src/warp.rs). */
#include "algo/warp.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/resample.h"
#include "algo/transform.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Grid cell size (source px) used to approximate the map. */
#define CELL 4.0
/* Most cells along one axis (bounds memory and time on huge layers). */
#define MAX_CELLS 512
/* Most subsamples per axis when a warp shrinks the source; also the longest source step per
 * destination pixel the filter follows (bounds the footprint each tile reads). */
#define MAX_SS 8.0

static int32_t f2i(double v)
{
    return pc_f64_to_i32(v);
}

static int32_t div_euclid(int32_t a, int32_t b)
{
    int32_t q = a / b;
    return a % b < 0 ? q - 1 : q;
}

typedef struct tri_job {
    const pc_surface *src;
    pc_rect src_rect;
    const pc_algo_warp_vertex *verts;
    const size_t (*tris)[3];
    pc_algo_interp interp;
    size_t n;
    const pc_rect *tiles;
    size_t **bins; /* triangle indices per tile */
    size_t *nbin;
    float **results; /* per tile, NULL when nothing was drawn */
} tri_job;

/* Subsamples along one destination axis for the source step `jv` it makes: one where the warp keeps
 * or enlarges the source, else about one per source pixel the output pixel spans (an area
 * average; the subsamples are bilinear, the box does the filtering). */
static size_t ss_count(pc_algo_interp interp, const double jv[2])
{
    if (interp == PC_ALGO_INTERP_NEAREST) {
        return 1;
    }
    return (size_t)pc_clampd(round(hypot(jv[0], jv[1])), 1.0, MAX_SS);
}

/* Caps a source step at MAX_SS, so a sliver triangle can't ask for an unbounded footprint. */
static void ss_cap(double jv[2])
{
    double l = hypot(jv[0], jv[1]);
    if (l > MAX_SS) {
        jv[0] = jv[0] * MAX_SS / l;
        jv[1] = jv[1] * MAX_SS / l;
    }
}

/* Per triangle: its source step per destination pixel along x and y ({dx_u, dx_v, dy_u, dy_v},
 * the inverse map's Jacobian, zero when point-sampled) and the subsamples along each. */
typedef struct tri_jac {
    double j[4];
    size_t nx, ny;
} tri_jac;

typedef struct foot_src {
    const float *px;
    pc_rect foot;
    size_t fw, n;
} foot_src;

static double foot_at(const foot_src *f, int32_t xx, int32_t yy, size_t cc)
{
    if (xx < f->foot.x0 || yy < f->foot.y0 || xx >= f->foot.x1 || yy >= f->foot.y1) {
        return 0.0;
    }
    return (double)f->px[((size_t)(yy - f->foot.y0) * f->fw + (size_t)(xx - f->foot.x0)) * f->n + cc];
}

/* One premultiplied sample at source position (u, v) (pixel centres at integers). */
static void point_sample(const foot_src *f, pc_algo_interp interp, double u, double v, double acc[8])
{
    size_t c, n = f->n;
    for (c = 0; c < n; c++) {
        acc[c] = 0.0;
    }
    if (interp == PC_ALGO_INTERP_NEAREST) {
        int32_t ix = f2i(floor(u + 0.5)), iy = f2i(floor(v + 0.5));
        for (c = 0; c < n; c++) {
            acc[c] = foot_at(f, ix, iy, c);
        }
    } else if (interp == PC_ALGO_INTERP_BILINEAR) {
        int32_t ix = f2i(floor(u)), iy = f2i(floor(v)), dx, dy;
        double fx = u - (double)ix, fy = v - (double)iy;
        for (dy = 0; dy < 2; dy++) {
            double wy = dy == 0 ? 1.0 - fy : fy;
            for (dx = 0; dx < 2; dx++) {
                double wx = dx == 0 ? 1.0 - fx : fx;
                int32_t sx = pc_i32_wrapping_add(ix, dx), sy = pc_i32_wrapping_add(iy, dy);
                for (c = 0; c < n; c++) {
                    acc[c] += foot_at(f, sx, sy, c) * wx * wy;
                }
            }
        }
    } else {
        /* Snap to the pixel grid when within float noise, so identity is exact. */
        double wx[4], wy[4];
        int32_t ix, iy, ii, jj;
        if (fabs(u - round(u)) < 1e-7) {
            u = round(u);
        }
        if (fabs(v - round(v)) < 1e-7) {
            v = round(v);
        }
        ix = f2i(floor(u));
        iy = f2i(floor(v));
        pc_algo_transform_bicubic(u - (double)ix, wx);
        pc_algo_transform_bicubic(v - (double)iy, wy);
        for (jj = 0; jj < 4; jj++) {
            for (ii = 0; ii < 4; ii++) {
                double kk = wx[ii] * wy[jj];
                int32_t sx = pc_i32_wrapping_add(pc_i32_wrapping_sub(ix, 1), ii);
                int32_t sy = pc_i32_wrapping_add(pc_i32_wrapping_sub(iy, 1), jj);
                if (kk == 0.0) {
                    continue;
                }
                for (c = 0; c < n; c++) {
                    acc[c] += foot_at(f, sx, sy, c) * kk;
                }
            }
        }
    }
}

/* Rasterizes one destination tile. */
static pc_status tri_tile(const tri_job *j, size_t k, pc_error *err)
{
    pc_rect t = j->tiles[k];
    size_t w = pc_rect_width(t), h = pc_rect_height(t), n = j->n, a = n - 1, o, b, c, nb = j->nbin[k];
    double (*uv)[2], fx0 = DBL_MAX, fy0 = DBL_MAX, fx1 = -DBL_MAX, fy1 = -DBL_MAX;
    float *px = NULL, *outp = NULL;
    size_t pxlen = 0, ti;
    uint32_t *owner;
    tri_jac *jac;
    bool any = false;
    foot_src fs;
    pc_status st;
    if (nb == 0) {
        return PC_OK;
    }
    uv = pc_malloc_array(w * h, sizeof *uv);
    owner = pc_calloc_array(w * h, sizeof *owner);
    jac = pc_malloc_array(nb, sizeof *jac);
    if (uv == NULL || owner == NULL || jac == NULL) {
        free(uv);
        free(owner);
        free(jac);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a mesh warp");
    }
    for (o = 0; o < w * h; o++) {
        uv[o][0] = NAN, uv[o][1] = NAN;
    }
    /* Source position per destination pixel (NaN = not covered) and the triangle it came from. */
    for (ti = 0; ti < nb; ti++) {
        const size_t *tri = j->tris[j->bins[k][ti]];
        const double *p0 = j->verts[tri[0]].dst, *p1 = j->verts[tri[1]].dst, *p2 = j->verts[tri[2]].dst;
        const double *s0 = j->verts[tri[0]].src, *s1 = j->verts[tri[1]].src, *s2 = j->verts[tri[2]].src;
        double det = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1]);
        double d1[2], d2[2], l1x, l1y, l2x, l2y, jx[2], jy[2];
        size_t nx, ny;
        int32_t bx0, by0, bx1, by1, x, y;
        memset(&jac[ti], 0, sizeof jac[ti]);
        jac[ti].nx = 1, jac[ti].ny = 1;
        if (fabs(det) < 1e-12) {
            continue;
        }
        d1[0] = s1[0] - s0[0], d1[1] = s1[1] - s0[1];
        d2[0] = s2[0] - s0[0], d2[1] = s2[1] - s0[1];
        l1x = (p2[1] - p0[1]) / det;
        l1y = (p0[0] - p2[0]) / det;
        l2x = (p0[1] - p1[1]) / det;
        l2y = (p1[0] - p0[0]) / det;
        jx[0] = l1x * d1[0] + l2x * d2[0], jx[1] = l1x * d1[1] + l2x * d2[1];
        jy[0] = l1y * d1[0] + l2y * d2[0], jy[1] = l1y * d1[1] + l2y * d2[1];
        ss_cap(jx);
        ss_cap(jy);
        nx = ss_count(j->interp, jx);
        ny = ss_count(j->interp, jy);
        if (nx * ny != 1) {
            jac[ti].j[0] = jx[0], jac[ti].j[1] = jx[1], jac[ti].j[2] = jy[0], jac[ti].j[3] = jy[1];
            jac[ti].nx = nx, jac[ti].ny = ny;
        }
        bx0 = f2i(pc_rs_max(floor(pc_rs_min(pc_rs_min(p0[0], p1[0]), p2[0]) - 0.5), (double)t.x0));
        by0 = f2i(pc_rs_max(floor(pc_rs_min(pc_rs_min(p0[1], p1[1]), p2[1]) - 0.5), (double)t.y0));
        bx1 = f2i(pc_rs_min(ceil(pc_rs_max(pc_rs_max(p0[0], p1[0]), p2[0]) + 0.5), (double)t.x1));
        by1 = f2i(pc_rs_min(ceil(pc_rs_max(pc_rs_max(p0[1], p1[1]), p2[1]) + 0.5), (double)t.y1));
        for (y = by0; y < by1; y++) {
            double py = (double)y + 0.5;
            for (x = bx0; x < bx1; x++) {
                double pxx = (double)x + 0.5;
                double l1 = ((pxx - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (py - p0[1])) / det;
                double l2 = ((p1[0] - p0[0]) * (py - p0[1]) - (pxx - p0[0]) * (p1[1] - p0[1])) / det;
                double l0 = 1.0 - l1 - l2;
                const double eps = -1e-9;
                if (l0 < eps || l1 < eps || l2 < eps) {
                    continue;
                }
                o = (size_t)(y - t.y0) * w + (size_t)(x - t.x0);
                uv[o][0] = l0 * s0[0] + l1 * s1[0] + l2 * s2[0];
                uv[o][1] = l0 * s0[1] + l1 * s1[1] + l2 * s2[1];
                owner[o] = (uint32_t)ti;
            }
        }
    }
    /* Source footprint of the covered pixels, including the spread of their subsamples. */
    for (o = 0; o < w * h; o++) {
        const double *jj;
        double ex, ey;
        if (isnan(uv[o][0])) {
            continue;
        }
        jj = jac[owner[o]].j;
        ex = 0.5 * (fabs(jj[0]) + fabs(jj[2]));
        ey = 0.5 * (fabs(jj[1]) + fabs(jj[3]));
        fx0 = pc_rs_min(fx0, uv[o][0] - ex);
        fy0 = pc_rs_min(fy0, uv[o][1] - ey);
        fx1 = pc_rs_max(fx1, uv[o][0] + ex);
        fy1 = pc_rs_max(fy1, uv[o][1] + ey);
    }
    if (fx0 > fx1) {
        goto done;
    }
    fs.foot = pc_rect_intersect(
        pc_rect_make(pc_i32_wrapping_sub(f2i(floor(fx0)), 3), pc_i32_wrapping_sub(f2i(floor(fy0)), 3),
                     pc_i32_wrapping_add(f2i(ceil(fx1)), 3), pc_i32_wrapping_add(f2i(ceil(fy1)), 3)),
        j->src_rect);
    if (pc_rect_is_empty(fs.foot) || !pc_surface_has_tiles_in(j->src, fs.foot)) {
        goto done;
    }
    st = pc_surface_read_region(j->src, fs.foot, &px, &pxlen, err);
    if (st != PC_OK) {
        free(uv);
        free(owner);
        free(jac);
        return st;
    }
    for (o = 0; o + n <= pxlen; o += n) {
        for (c = 0; c < a; c++) {
            px[o + c] *= px[o + a];
        }
    }
    fs.px = px;
    fs.fw = pc_rect_width(fs.foot);
    fs.n = n;
    outp = pc_calloc_array(w * h * n, sizeof *outp);
    if (outp == NULL) {
        free(uv);
        free(owner);
        free(jac);
        free(px);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a mesh warp");
    }
    for (o = 0; o < w * h; o++) {
        double acc[8] = {0}, one[8] = {0}, al;
        const tri_jac *tj;
        if (isnan(uv[o][0])) {
            continue;
        }
        tj = &jac[owner[o]];
        if (tj->nx * tj->ny == 1) {
            point_sample(&fs, j->interp, uv[o][0] - 0.5, uv[o][1] - 0.5, acc);
        } else {
            size_t sx, sy;
            double kk;
            for (sy = 0; sy < tj->ny; sy++) {
                double tt = ((double)sy + 0.5) / (double)tj->ny - 0.5;
                for (sx = 0; sx < tj->nx; sx++) {
                    double ss = ((double)sx + 0.5) / (double)tj->nx - 0.5;
                    point_sample(&fs, PC_ALGO_INTERP_BILINEAR, uv[o][0] + ss * tj->j[0] + tt * tj->j[2] - 0.5,
                                 uv[o][1] + ss * tj->j[1] + tt * tj->j[3] - 0.5, one);
                    for (c = 0; c < n; c++) {
                        acc[c] += one[c];
                    }
                }
            }
            kk = 1.0 / (double)(tj->nx * tj->ny);
            for (c = 0; c < n; c++) {
                acc[c] *= kk;
            }
        }
        al = pc_clampd(acc[a], 0.0, 1.0);
        /* Float noise (a vertex a hair off the pixel grid) must not leave invisible colour behind
         * in transparent pixels. */
        if (al <= 1e-6) {
            continue;
        }
        any = true;
        b = o * n;
        for (c = 0; c < a; c++) {
            outp[b + c] = (float)pc_clampd(acc[c] / al, 0.0, 1.0);
        }
        outp[b + a] = (float)al;
    }
done:
    free(uv);
    free(owner);
    free(jac);
    free(px);
    if (any) {
        j->results[k] = outp;
    } else {
        free(outp);
    }
    return PC_OK;
}

typedef struct tri_par {
    const tri_job *job;
    pc_status *status;
    pc_error *errs;
} tri_par;

static pc_status tri_chunk(void *vc, size_t chunk, size_t begin, size_t end)
{
    tri_par *p = vc;
    size_t k;
    (void)chunk;
    for (k = begin; k < end; k++) {
        p->status[k] = tri_tile(p->job, k, &p->errs[k]);
    }
    return PC_OK;
}

pc_status pc_algo_warp_triangles(const pc_surface *src, pc_rect src_rect, const pc_algo_warp_vertex *verts,
                                 size_t nverts, const size_t (*tris)[3], size_t ntris, pc_algo_interp interp,
                                 pc_surface *out, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(src);
    pc_surface converted, result;
    const pc_surface *s = src;
    const double lim = (double)(1 << 20);
    double x0 = DBL_MAX, y0 = DBL_MAX, x1 = -DBL_MAX, y1 = -DBL_MAX;
    pc_rect dst, *tiles = NULL;
    pc_tile first, last;
    size_t ntiles = 0, i, k, *lookup = NULL, nlook = 0, cols = 0, rows = 0, **bins = NULL, *nbin = NULL,
           *capbin = NULL;
    int32_t ts = PC_TILE_SIZE, tx0, ty0;
    float **results = NULL;
    pc_status st = PC_OK, *tst = NULL;
    pc_error *terr = NULL;
    tri_job job;
    tri_par par;
    bool conv = false;
    for (i = 0; i < ntris; i++) {
        if (tris[i][0] >= nverts || tris[i][1] >= nverts || tris[i][2] >= nverts) {
            return pc_fail(err, PC_ERR_INVALID, "mesh warp: triangle %zu refers to a missing vertex", i);
        }
    }
    if (!fmt.alpha) {
        fmt.alpha = true;
        st = pc_surface_convert(src, fmt, &converted, err);
        if (st != PC_OK) {
            return st;
        }
        s = &converted;
        conv = true;
    }
    pc_surface_init(&result, fmt);
    if (pc_rect_is_empty(src_rect) || ntris == 0) {
        goto finish;
    }
    for (i = 0; i < nverts; i++) {
        if (!isfinite(verts[i].dst[0]) || !isfinite(verts[i].dst[1])) {
            goto finish;
        }
    }
    for (i = 0; i < ntris; i++) {
        for (k = 0; k < 3; k++) {
            const double *p = verts[tris[i][k]].dst;
            x0 = pc_rs_min(x0, p[0]);
            y0 = pc_rs_min(y0, p[1]);
            x1 = pc_rs_max(x1, p[0]);
            y1 = pc_rs_max(y1, p[1]);
        }
    }
    dst = pc_rect_make(pc_i32_wrapping_sub(f2i(floor(pc_rs_max_k(x0, -lim))), 1),
                       pc_i32_wrapping_sub(f2i(floor(pc_rs_max_k(y0, -lim))), 1),
                       pc_i32_wrapping_add(f2i(ceil(pc_rs_min_k(x1, lim))), 1),
                       pc_i32_wrapping_add(f2i(ceil(pc_rs_min_k(y1, lim))), 1));
    /* Destination tiles clipped to dst, and the triangles binned per tile. */
    if (pc_rect_tile_range(dst, &first, &last)) {
        size_t cap = (size_t)((int64_t)last.tx - first.tx + 1) * (size_t)((int64_t)last.ty - first.ty + 1);
        int32_t tx, ty;
        tiles = pc_malloc_array(cap, sizeof *tiles);
        if (tiles == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a mesh warp");
            goto finish;
        }
        for (ty = first.ty; ty <= last.ty; ty++) {
            for (tx = first.tx; tx <= last.tx; tx++) {
                pc_tile tc = {tx, ty};
                pc_rect r = pc_rect_intersect(pc_tile_rect(tc), dst);
                if (!pc_rect_is_empty(r)) {
                    tiles[ntiles++] = r;
                }
            }
        }
    }
    tx0 = 0, ty0 = 0;
    {
        int32_t mx = INT32_MAX, my = INT32_MAX, cmax = 0, rmax = 0;
        for (k = 0; k < ntiles; k++) {
            mx = tiles[k].x0 < mx ? tiles[k].x0 : mx;
            my = tiles[k].y0 < my ? tiles[k].y0 : my;
        }
        if (ntiles > 0) {
            tx0 = div_euclid(mx, ts) * ts;
            ty0 = div_euclid(my, ts) * ts;
        }
        for (k = 0; k < ntiles; k++) {
            int32_t cx = (tiles[k].x0 - tx0) / ts, cy = (tiles[k].y0 - ty0) / ts;
            cmax = cx > cmax ? cx : cmax;
            rmax = cy > rmax ? cy : rmax;
        }
        cols = (size_t)cmax + 1;
        rows = (size_t)rmax + 1;
    }
    nlook = cols * rows;
    lookup = pc_malloc_array(nlook, sizeof *lookup);
    bins = pc_calloc_array(ntiles > 0 ? ntiles : 1, sizeof *bins);
    nbin = pc_calloc_array(ntiles > 0 ? ntiles : 1, sizeof *nbin);
    capbin = pc_calloc_array(ntiles > 0 ? ntiles : 1, sizeof *capbin);
    results = pc_calloc_array(ntiles > 0 ? ntiles : 1, sizeof *results);
    tst = pc_calloc_array(ntiles > 0 ? ntiles : 1, sizeof *tst);
    terr = pc_calloc_array(ntiles > 0 ? ntiles : 1, sizeof *terr);
    if (lookup == NULL || bins == NULL || nbin == NULL || capbin == NULL || results == NULL || tst == NULL ||
        terr == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a mesh warp");
        goto finish;
    }
    for (k = 0; k < nlook; k++) {
        lookup[k] = SIZE_MAX;
    }
    for (k = 0; k < ntiles; k++) {
        lookup[(size_t)((tiles[k].y0 - ty0) / ts) * cols + (size_t)((tiles[k].x0 - tx0) / ts)] = k;
    }
    for (i = 0; i < ntris; i++) {
        const double *p0 = verts[tris[i][0]].dst, *p1 = verts[tris[i][1]].dst, *p2 = verts[tris[i][2]].dst;
        int32_t bx0 = f2i(floor(pc_rs_min(pc_rs_min(pc_rs_min(DBL_MAX, p0[0]), p1[0]), p2[0])));
        int32_t by0 = f2i(floor(pc_rs_min(pc_rs_min(pc_rs_min(DBL_MAX, p0[1]), p1[1]), p2[1])));
        int32_t bx1 = f2i(ceil(pc_rs_max(pc_rs_max(pc_rs_max(-DBL_MAX, p0[0]), p1[0]), p2[0])));
        int32_t by1 = f2i(ceil(pc_rs_max(pc_rs_max(pc_rs_max(-DBL_MAX, p0[1]), p1[1]), p2[1])));
        int32_t cx0 = div_euclid(pc_i32_wrapping_sub(bx0 > dst.x0 ? bx0 : dst.x0, tx0), ts);
        int32_t cy0 = div_euclid(pc_i32_wrapping_sub(by0 > dst.y0 ? by0 : dst.y0, ty0), ts);
        int32_t cx1 = div_euclid(pc_i32_wrapping_sub(bx1 < dst.x1 - 1 ? bx1 : dst.x1 - 1, tx0), ts);
        int32_t cy1 = div_euclid(pc_i32_wrapping_sub(by1 < dst.y1 - 1 ? by1 : dst.y1 - 1, ty0), ts);
        int64_t cx, cy;
        for (cy = cy0 > 0 ? cy0 : 0; cy <= cy1; cy++) {
            for (cx = cx0 > 0 ? cx0 : 0; cx <= cx1; cx++) {
                size_t g = (size_t)cy * cols + (size_t)cx, kk;
                if (g >= nlook || lookup[g] == SIZE_MAX) {
                    continue;
                }
                kk = lookup[g];
                if (nbin[kk] == capbin[kk]) {
                    size_t *nb = pc_grow_array(bins[kk], &capbin[kk], nbin[kk] + 1, sizeof **bins);
                    if (nb == NULL) {
                        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a mesh warp");
                        goto finish;
                    }
                    bins[kk] = nb;
                }
                bins[kk][nbin[kk]++] = i;
            }
        }
    }
    job.src = s;
    job.src_rect = src_rect;
    job.verts = verts;
    job.tris = tris;
    job.interp = interp;
    job.n = pc_pixel_format_channels(fmt);
    job.tiles = tiles;
    job.bins = bins;
    job.nbin = nbin;
    job.results = results;
    par.job = &job;
    par.status = tst;
    par.errs = terr;
    (void)pc_algo_par_for(ntiles, 1, tri_chunk, &par);
    for (k = 0; k < ntiles; k++) {
        if (tst[k] != PC_OK) {
            if (err != NULL) {
                *err = terr[k];
            }
            st = tst[k];
            goto finish;
        }
    }
    for (k = 0; k < ntiles && st == PC_OK; k++) {
        if (results[k] != NULL) {
            st = pc_surface_write_region(&result, tiles[k], results[k],
                                         (size_t)pc_rect_width(tiles[k]) * pc_rect_height(tiles[k]) * job.n,
                                         err);
        }
    }
    pc_surface_prune(&result);
finish:
    if (bins != NULL) {
        for (k = 0; k < ntiles; k++) {
            free(bins[k]);
        }
    }
    if (results != NULL) {
        for (k = 0; k < ntiles; k++) {
            free(results[k]);
        }
    }
    free(bins), free(nbin), free(capbin), free(results), free(tst), free(terr), free(lookup), free(tiles);
    if (conv) {
        pc_surface_fini(&converted);
    }
    if (st != PC_OK) {
        pc_surface_fini(&result);
        return st;
    }
    *out = result;
    return PC_OK;
}

pc_status pc_algo_warp_mesh_surface(const pc_surface *src, pc_rect src_rect, pc_algo_map_fn f, void *ctx,
                                    pc_algo_interp interp, pc_surface *out, pc_error *err)
{
    double sw, sh;
    size_t nx, ny, i, j, w1, nv, nt = 0;
    pc_algo_warp_vertex *verts;
    size_t (*tris)[3];
    pc_status st;
    if (pc_rect_is_empty(src_rect)) {
        pc_pixel_format fmt = pc_surface_format(src);
        fmt.alpha = true;
        pc_surface_init(out, fmt);
        return PC_OK;
    }
    sw = (double)pc_rect_width(src_rect);
    sh = (double)pc_rect_height(src_rect);
    nx = pc_f64_to_size(ceil(sw / CELL));
    ny = pc_f64_to_size(ceil(sh / CELL));
    nx = nx < 1 ? 1 : (nx > MAX_CELLS ? MAX_CELLS : nx);
    ny = ny < 1 ? 1 : (ny > MAX_CELLS ? MAX_CELLS : ny);
    w1 = nx + 1;
    nv = (nx + 1) * (ny + 1);
    verts = pc_malloc_array(nv, sizeof *verts);
    tris = pc_malloc_array(nx * ny * 2, sizeof *tris);
    if (verts == NULL || tris == NULL) {
        free(verts), free(tris);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a mesh warp");
    }
    /* Forward-mapped vertices (and their source positions). */
    for (j = 0; j <= ny; j++) {
        for (i = 0; i <= nx; i++) {
            pc_algo_warp_vertex *v = &verts[j * w1 + i];
            v->src[0] = (double)src_rect.x0 + sw * (double)i / (double)nx;
            v->src[1] = (double)src_rect.y0 + sh * (double)j / (double)ny;
            f(ctx, v->src[0], v->src[1], &v->dst[0], &v->dst[1]);
        }
    }
    for (j = 0; j < ny; j++) {
        for (i = 0; i < nx; i++) {
            size_t a = j * w1 + i;
            tris[nt][0] = a, tris[nt][1] = a + 1, tris[nt][2] = a + w1 + 1;
            nt++;
            tris[nt][0] = a, tris[nt][1] = a + w1 + 1, tris[nt][2] = a + w1;
            nt++;
        }
    }
    st = pc_algo_warp_triangles(src, src_rect, verts, nv, (const size_t (*)[3])tris, nt, interp, out, err);
    free(verts);
    free(tris);
    return st;
}

pc_status pc_algo_warp_mesh_gray(const pc_surface *s, pc_algo_map_fn f, void *ctx, pc_algo_interp interp,
                                 pc_surface *out, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(s), wa = fmt;
    float defpx[PC_RASTER_MAX_CHANNELS], def;
    pc_rect src = pc_surface_content_bounds(s), b;
    pc_surface res, tmp, warped;
    float *v = NULL, *pairs = NULL;
    size_t len = 0, i, npx;
    pc_status st;
    if (pc_surface_channels(s) != 1) {
        return pc_fail(err, PC_ERR_INVALID, "mesh warp: a mask must have one channel");
    }
    pc_surface_default_pixel(s, defpx);
    def = defpx[0];
    pc_surface_init_default(&res, fmt, &def, 1);
    if (pc_rect_is_empty(src)) {
        *out = res;
        return PC_OK;
    }
    wa.alpha = true;
    pc_surface_init(&tmp, wa);
    st = pc_surface_read_region(s, src, &v, &len, err);
    if (st != PC_OK) {
        goto fail;
    }
    pairs = pc_malloc_array(len * 2 + 1, sizeof *pairs);
    if (pairs == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a mesh warp");
        goto fail;
    }
    for (i = 0; i < len; i++) {
        pairs[2 * i] = v[i];
        pairs[2 * i + 1] = 1.0f;
    }
    st = pc_surface_write_region(&tmp, src, pairs, len * 2, err);
    if (st != PC_OK) {
        goto fail;
    }
    st = pc_algo_warp_mesh_surface(&tmp, src, f, ctx, interp, &warped, err);
    if (st != PC_OK) {
        goto fail;
    }
    /* Clear the old region and write the warped one separately: one dense union buffer would grow
     * with the square of how far a mesh moves the content. */
    for (i = 0; i < len; i++) {
        v[i] = def;
    }
    st = pc_surface_write_region(&res, src, v, len, err);
    b = pc_surface_content_bounds(&warped);
    if (st == PC_OK && !pc_rect_is_empty(b)) {
        float *wp = NULL;
        size_t wl = 0;
        st = pc_surface_read_region(&warped, b, &wp, &wl, err);
        if (st == PC_OK) {
            npx = wl / 2;
            for (i = 0; i < npx; i++) {
                wp[i] = wp[2 * i] * wp[2 * i + 1] + def * (1.0f - wp[2 * i + 1]);
            }
            st = pc_surface_write_region(&res, b, wp, npx, err);
        }
        free(wp);
    }
    pc_surface_fini(&warped);
    if (st != PC_OK) {
        goto fail;
    }
    pc_surface_prune(&res);
    free(v);
    free(pairs);
    pc_surface_fini(&tmp);
    *out = res;
    return PC_OK;
fail:
    free(v);
    free(pairs);
    pc_surface_fini(&tmp);
    pc_surface_fini(&res);
    return st;
}

typedef struct place_ctx {
    const pc_warp *w;
    const double *m;             /* affine, or NULL */
    const pc_algo_homography *h; /* projective, or NULL */
} place_ctx;

static void place_map(void *vc, double x, double y, double *ox, double *oy)
{
    const place_ctx *p = vc;
    pc_point q = pc_warp_map(p->w, x, y);
    if (p->m != NULL) {
        const double *m = p->m;
        *ox = m[0] * q.x + m[2] * q.y + m[4];
        *oy = m[1] * q.x + m[3] * q.y + m[5];
    } else {
        pc_algo_homography_apply(p->h, q.x, q.y, ox, oy);
    }
}

static bool near(double x, double y)
{
    return fabs(x - y) < 1e-9;
}

pc_status pc_algo_place_source(const pc_surface *src, pc_rect src_rect, const pc_affine *t,
                               const pc_warp *warp, pc_surface *out, pc_error *err)
{
    const double *m = t->m;
    if (warp != NULL && !pc_warp_is_identity(warp)) {
        place_ctx pc = {warp, m, NULL};
        return pc_algo_warp_mesh_surface(src, src_rect, place_map, &pc, PC_ALGO_INTERP_BICUBIC, out, err);
    }
    if (near(m[0], 1.0) && near(m[1], 0.0) && near(m[2], 0.0) && near(m[3], 1.0) && near(m[4], round(m[4])) &&
        near(m[5], round(m[5]))) {
        return pc_algo_translate_surface(src, f2i(round(m[4])), f2i(round(m[5])), out, err);
    }
    {
        pc_algo_homography h = {{m[0], m[2], m[4], m[1], m[3], m[5], 0.0, 0.0, 1.0}};
        return pc_algo_warp_surface(src, src_rect, &h, PC_ALGO_INTERP_BICUBIC, out, err);
    }
}

pc_status pc_algo_place_source_projective(const pc_surface *src, pc_rect src_rect,
                                          const pc_algo_homography *h, const pc_warp *warp, pc_surface *out,
                                          pc_error *err)
{
    if (warp != NULL && !pc_warp_is_identity(warp)) {
        place_ctx pc = {warp, NULL, h};
        return pc_algo_warp_mesh_surface(src, src_rect, place_map, &pc, PC_ALGO_INTERP_BICUBIC, out, err);
    }
    return pc_algo_warp_surface(src, src_rect, h, PC_ALGO_INTERP_BICUBIC, out, err);
}
