/* Filter > Liquify rendering (port of apply_liquify and ProxyImage in crates/algo/src/liquify.rs):
 * the final bicubic resample through the field, tile by tile in parallel, and the preview proxy. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/liquify.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

static void catmull_rom(double t, double w[4])
{
    double t2 = t * t, t3 = t2 * t;
    w[0] = -0.5 * t3 + t2 - 0.5 * t;
    w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
    w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    w[3] = 0.5 * t3 - 0.5 * t2;
}

static int64_t wadd64(int64_t a, int64_t b)
{
    return (int64_t)((uint64_t)a + (uint64_t)b);
}

/* Catmull-Rom displacement at grid coordinates (nodes clamped to the field). */
static void sample_cubic(const pc_algo_liquify_field *f, double gx, double gy, const double wx[4],
                         const double wy[4], double out[2])
{
    int64_t ix = pc_f64_to_i64(floor(gx)), iy = pc_f64_to_i64(floor(gy));
    int64_t lw = (int64_t)f->w - 1, lh = (int64_t)f->h - 1;
    double acc[2] = {0.0, 0.0};
    int jj, ii;
    for (jj = 0; jj < 4; jj++) {
        size_t y = (size_t)pc_clamp_i64(wadd64(iy, jj - 1), 0, lh);
        const float (*row)[2] = (const float (*)[2])f->d + y * f->w;
        for (ii = 0; ii < 4; ii++) {
            size_t x = (size_t)pc_clamp_i64(wadd64(ix, ii - 1), 0, lw);
            double k = wx[ii] * wy[jj];
            acc[0] += (double)row[x][0] * k;
            acc[1] += (double)row[x][1] * k;
        }
    }
    out[0] = acc[0];
    out[1] = acc[1];
}

/* Whether any node influencing document rect r is displaced. */
static bool touches(const pc_algo_liquify_field *f, pc_rect r)
{
    double gx0 = ((double)r.x0 - (double)f->bounds.x0 - 0.5) / f->cell;
    double gy0 = ((double)r.y0 - (double)f->bounds.y0 - 0.5) / f->cell;
    double gx1 = ((double)r.x1 - (double)f->bounds.x0 - 0.5) / f->cell;
    double gy1 = ((double)r.y1 - (double)f->bounds.y0 - 0.5) / f->cell;
    int64_t lw = (int64_t)f->w - 1, lh = (int64_t)f->h - 1;
    size_t i0 = (size_t)pc_clamp_i64(wadd64(pc_f64_to_i64(floor(gx0)), -2), 0, lw);
    size_t j0 = (size_t)pc_clamp_i64(wadd64(pc_f64_to_i64(floor(gy0)), -2), 0, lh);
    size_t i1 = (size_t)pc_clamp_i64(wadd64(pc_f64_to_i64(ceil(gx1)), 2), 0, lw);
    size_t j1 = (size_t)pc_clamp_i64(wadd64(pc_f64_to_i64(ceil(gy1)), 2), 0, lh);
    size_t i, j;
    for (j = j0; j <= j1; j++) {
        for (i = i0; i <= i1; i++) {
            const float *v = f->d[j * f->w + i];
            if (v[0] != 0.0f || v[1] != 0.0f) {
                return true;
            }
        }
    }
    return false;
}

typedef struct apply_ctx {
    const pc_surface *src;
    const pc_algo_liquify_field *field;
    const pc_rect *tiles;
    float **out;    /* per tile: NULL when the tile is unchanged */
    pc_error *errs; /* per tile */
    size_t n;       /* channels */
    bool alpha;
    int32_t maxd;
} apply_ctx;

static double snap_whole(double t)
{
    return fabs(t - round(t)) < 1e-7 ? round(t) : t;
}

static pc_status apply_tile(const apply_ctx *c, size_t ti)
{
    const pc_algo_liquify_field *field = c->field;
    pc_rect t = c->tiles[ti], b = field->bounds, foot;
    size_t w = pc_rect_width(t), h = pc_rect_height(t), n = c->n, x, y, fw, plen = 0;
    double (*disp)[2];
    double fx0 = 1.7976931348623157e308, fy0 = 1.7976931348623157e308;
    double fx1 = -1.7976931348623157e308, fy1 = -1.7976931348623157e308;
    bool moved = false;
    float *px = NULL, *outp;
    pc_status st;
    disp = pc_malloc_array(w * h, sizeof *disp);
    if (disp == NULL) {
        return pc_fail(&c->errs[ti], PC_ERR_NOMEM, "liquify: out of memory");
    }
    /* Field per pixel (bicubic); rows share their y weights. */
    for (y = 0; y < h; y++) {
        double py = (double)t.y0 + (double)y + 0.5;
        double gy = (py - (double)b.y0 - 0.5) / field->cell, wy[4];
        catmull_rom(gy - floor(gy), wy);
        for (x = 0; x < w; x++) {
            double ppx = (double)t.x0 + (double)x + 0.5;
            double gx = (ppx - (double)b.x0 - 0.5) / field->cell, wx[4], *d = disp[y * w + x], sx, sy;
            catmull_rom(gx - floor(gx), wx);
            sample_cubic(field, gx, gy, wx, wy, d);
            if (d[0] != 0.0 || d[1] != 0.0) {
                moved = true;
            }
            sx = ppx + d[0];
            sy = py + d[1];
            fx0 = pc_rs_min(fx0, sx);
            fy0 = pc_rs_min(fy0, sy);
            fx1 = pc_rs_max(fx1, sx);
            fy1 = pc_rs_max(fy1, sy);
        }
    }
    if (!moved) {
        free(disp);
        return PC_OK;
    }
    foot = pc_rect_intersect(pc_rect_intersect(pc_rect_make(pc_i32_wrapping_sub(pc_f64_to_i32(floor(fx0)), 3),
                                                            pc_i32_wrapping_sub(pc_f64_to_i32(floor(fy0)), 3),
                                                            pc_i32_wrapping_add(pc_f64_to_i32(ceil(fx1)), 3),
                                                            pc_i32_wrapping_add(pc_f64_to_i32(ceil(fy1)), 3)),
                                               pc_rect_inflate(t, c->maxd)),
                             b);
    if (pc_rect_is_empty(foot)) {
        free(disp);
        return PC_OK;
    }
    st = pc_surface_read_region(c->src, foot, &px, &plen, &c->errs[ti]);
    if (st != PC_OK) {
        free(disp);
        return st;
    }
    if (c->alpha) {
        size_t i, k;
        for (i = 0; i + n <= plen; i += n) {
            float al = px[i + n - 1];
            for (k = 0; k + 1 < n; k++) {
                px[i + k] *= al;
            }
        }
    }
    fw = pc_rect_width(foot);
    outp = pc_calloc_array(w * h * n, sizeof *outp);
    if (outp == NULL) {
        free(disp);
        free(px);
        return pc_fail(&c->errs[ti], PC_ERR_NOMEM, "liquify: out of memory");
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const double *d = disp[y * w + x];
            double u = snap_whole((double)t.x0 + (double)x + d[0]);
            double v = snap_whole((double)t.y0 + (double)y + d[1]);
            int32_t ix = pc_f64_to_i32(floor(u)), iy = pc_f64_to_i32(floor(v));
            double wx[4], wy[4], acc[PC_ALGO_MAXC];
            size_t o = (y * w + x) * n, ch;
            int i, j;
            catmull_rom(u - (double)ix, wx);
            catmull_rom(v - (double)iy, wy);
            for (ch = 0; ch < n; ch++) {
                acc[ch] = 0.0;
            }
            for (j = 0; j < 4; j++) {
                for (i = 0; i < 4; i++) {
                    double k = wx[i] * wy[j];
                    int32_t sxp, syp;
                    size_t at;
                    if (k == 0.0) {
                        continue;
                    }
                    sxp = pc_clamp_i32(pc_i32_wrapping_add(pc_i32_wrapping_sub(ix, 1), i), foot.x0,
                                       foot.x1 - 1);
                    syp = pc_clamp_i32(pc_i32_wrapping_add(pc_i32_wrapping_sub(iy, 1), j), foot.y0,
                                       foot.y1 - 1);
                    at = ((size_t)(syp - foot.y0) * fw + (size_t)(sxp - foot.x0)) * n;
                    for (ch = 0; ch < n; ch++) {
                        acc[ch] += (double)px[at + ch] * k;
                    }
                }
            }
            if (c->alpha) {
                double al = pc_clampd(acc[n - 1], 0.0, 1.0);
                outp[o + n - 1] = (float)al;
                for (ch = 0; ch + 1 < n; ch++) {
                    outp[o + ch] = al > 0.0 ? pc_f64_to_f32(pc_clampd(acc[ch] / al, 0.0, 1.0)) : 0.0f;
                }
            } else {
                for (ch = 0; ch < n; ch++) {
                    outp[o + ch] = pc_f64_to_f32(pc_clampd(acc[ch], 0.0, 1.0));
                }
            }
        }
    }
    free(disp);
    free(px);
    c->out[ti] = outp;
    return PC_OK;
}

static pc_status apply_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const apply_ctx *c = ctx;
    pc_status first = PC_OK;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        pc_status st = apply_tile(c, i);
        if (st != PC_OK && first == PC_OK) {
            first = st;
        }
    }
    return first;
}

pc_status pc_algo_liquify_apply(const pc_surface *src, const pc_algo_liquify_field *field, pc_surface *out,
                                pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(src);
    pc_rect b = field->bounds;
    pc_surface res;
    apply_ctx c;
    pc_rect *tiles = NULL;
    size_t ntiles = 0, cap, i;
    pc_tile first, last;
    pc_status st;
    int32_t tx, ty;
    st = pc_surface_clone(src, &res, err);
    if (st != PC_OK) {
        return st;
    }
    if (pc_algo_liquify_is_identity(field) || pc_rect_is_empty(b) || !pc_rect_tile_range(b, &first, &last)) {
        *out = res;
        return PC_OK;
    }
    cap = (size_t)((int64_t)last.tx - first.tx + 1) * (size_t)((int64_t)last.ty - first.ty + 1);
    tiles = pc_malloc_array(cap, sizeof *tiles);
    if (tiles == NULL) {
        pc_surface_fini(&res);
        return pc_fail(err, PC_ERR_NOMEM, "liquify: out of memory");
    }
    for (ty = first.ty;; ty++) {
        for (tx = first.tx;; tx++) {
            pc_tile tc = {tx, ty};
            pc_rect r = pc_rect_intersect(pc_tile_rect(tc), b);
            if (!pc_rect_is_empty(r) && touches(field, r)) {
                tiles[ntiles++] = r;
            }
            if (tx == last.tx) {
                break;
            }
        }
        if (ty == last.ty) {
            break;
        }
    }
    memset(&c, 0, sizeof c);
    c.src = src;
    c.field = field;
    c.tiles = tiles;
    c.n = pc_surface_channels(src);
    c.alpha = fmt.alpha;
    c.maxd = pc_i32_wrapping_add(pc_f64_to_i32(ceil((double)pc_algo_liquify_max_displacement(field))), 3);
    c.out = pc_calloc_array(ntiles > 0 ? ntiles : 1, sizeof *c.out);
    c.errs = pc_calloc_array(ntiles > 0 ? ntiles : 1, sizeof *c.errs);
    if (c.out == NULL || c.errs == NULL || c.n == 0 || c.n > PC_ALGO_MAXC) {
        st = (c.n == 0 || c.n > PC_ALGO_MAXC)
                 ? pc_fail(err, PC_ERR_UNSUPPORTED, "liquify: unsupported format")
                 : pc_fail(err, PC_ERR_NOMEM, "liquify: out of memory");
        goto cleanup;
    }
    st = pc_algo_par_for(ntiles, 1, apply_chunk, &c);
    if (st != PC_OK) {
        for (i = 0; i < ntiles; i++) {
            if (c.errs[i].code != PC_OK) {
                if (err != NULL) {
                    *err = c.errs[i];
                }
                st = c.errs[i].code;
                break;
            }
        }
        goto cleanup;
    }
    for (i = 0; i < ntiles && st == PC_OK; i++) {
        if (c.out[i] != NULL) {
            pc_rect t = tiles[i];
            st = pc_surface_write_region(&res, t, c.out[i], pc_rect_width(t) * pc_rect_height(t) * c.n, err);
        }
    }
    if (st == PC_OK && fmt.alpha) {
        pc_surface_prune(&res);
    }
cleanup:
    if (c.out != NULL) {
        for (i = 0; i < ntiles; i++) {
            free(c.out[i]);
        }
    }
    free(c.out);
    free(c.errs);
    free(tiles);
    if (st != PC_OK) {
        pc_surface_fini(&res);
        return st;
    }
    *out = res;
    return PC_OK;
}

/* ---- proxy ---- */

typedef struct proxy_ctx {
    const pc_surface *src;
    pc_algo_liquify_proxy *p;
    int32_t k;
    bool alpha;
    pc_error *errs; /* per row */
} proxy_ctx;

static int32_t imin32(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static pc_status proxy_row(const proxy_ctx *c, size_t y)
{
    pc_algo_liquify_proxy *p = c->p;
    pc_rect bd = p->bounds;
    int32_t k = c->k, yi = (int32_t)y;
    pc_rect r = pc_rect_make(bd.x0, pc_i32_wrapping_add(bd.y0, pc_i32_wrapping_mul(yi, k)),
                             pc_i32_wrapping_add(bd.x0, pc_i32_wrapping_mul((int32_t)p->w, k)),
                             imin32(pc_i32_wrapping_add(bd.y0, pc_i32_wrapping_mul(yi + 1, k)), bd.y1));
    size_t rw = pc_rect_width(r), rh = pc_rect_height(r), n = rw * rh, x, xx, yy;
    float (*buf)[4] = pc_calloc_array(n > 0 ? n : 1, sizeof *buf);
    float (*row)[4] = p->px + y * p->w;
    pc_status st;
    if (buf == NULL) {
        return pc_fail(&c->errs[y], PC_ERR_NOMEM, "liquify: out of memory");
    }
    st = n > 0 ? pc_surface_read_rgba_into(c->src, r, buf, n, &c->errs[y]) : PC_OK;
    if (st != PC_OK) {
        free(buf);
        return st;
    }
    for (x = 0; x < p->w; x++) {
        float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f}, cnt = 0.0f;
        size_t xe = (x + 1) * (size_t)k < rw ? (x + 1) * (size_t)k : rw;
        for (yy = 0; yy < rh; yy++) {
            for (xx = x * (size_t)k; xx < xe; xx++) {
                const float *q = buf[yy * rw + xx];
                float a = c->alpha ? q[3] : 1.0f;
                acc[0] += q[0] * a;
                acc[1] += q[1] * a;
                acc[2] += q[2] * a;
                acc[3] += a;
                cnt += 1.0f;
            }
        }
        if (cnt > 0.0f) {
            int i;
            for (i = 0; i < 4; i++) {
                row[x][i] = acc[i] / cnt;
            }
        }
    }
    free(buf);
    return PC_OK;
}

static pc_status proxy_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    pc_status first = PC_OK;
    size_t y;
    (void)chunk;
    for (y = begin; y < end; y++) {
        pc_status st = proxy_row(ctx, y);
        if (st != PC_OK && first == PC_OK) {
            first = st;
        }
    }
    return first;
}

pc_status pc_algo_liquify_proxy_init(pc_algo_liquify_proxy *p, const pc_surface *src, pc_rect bounds,
                                     size_t max_side, pc_error *err)
{
    pc_algo_liquify_proxy q;
    uint32_t bw = pc_rect_width(bounds), bh = pc_rect_height(bounds);
    double side = (double)(bw > bh ? bw : bh), scale;
    size_t n, y;
    proxy_ctx c;
    pc_status st;
    if (side < 1.0) {
        side = 1.0;
    }
    scale = ceil(pc_rs_max_k(side / (double)(max_side > 1 ? max_side : 1), 1.0));
    q.bounds = bounds;
    q.scale = scale;
    q.w = pc_f64_to_size(ceil((double)bw / scale));
    q.h = pc_f64_to_size(ceil((double)bh / scale));
    q.w = q.w > 1 ? q.w : 1;
    q.h = q.h > 1 ? q.h : 1;
    if (!pc_size_mul(q.w, q.h, &n) || n > PC_MAX_ALLOC / sizeof *q.px || q.h > (size_t)INT32_MAX ||
        q.w > (size_t)INT32_MAX) {
        return pc_fail(err, PC_ERR_LIMIT, "liquify: the preview proxy is too large");
    }
    q.px = pc_calloc_array(n, sizeof *q.px);
    if (q.px == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "liquify: out of memory");
    }
    c.src = src;
    c.p = &q;
    c.k = pc_f64_to_i32(scale);
    c.alpha = pc_surface_format(src).alpha;
    c.errs = pc_calloc_array(q.h, sizeof *c.errs);
    if (c.errs == NULL) {
        free(q.px);
        return pc_fail(err, PC_ERR_NOMEM, "liquify: out of memory");
    }
    st = pc_algo_par_for(q.h, 1, proxy_chunk, &c);
    if (st != PC_OK) {
        for (y = 0; y < q.h; y++) {
            if (c.errs[y].code != PC_OK) {
                if (err != NULL) {
                    *err = c.errs[y];
                }
                st = c.errs[y].code;
                break;
            }
        }
        free(c.errs);
        free(q.px);
        return st;
    }
    free(c.errs);
    *p = q;
    return PC_OK;
}

void pc_algo_liquify_proxy_fini(pc_algo_liquify_proxy *p)
{
    if (p == NULL) {
        return;
    }
    free(p->px);
    p->px = NULL;
    p->w = p->h = 0;
}

typedef struct render_ctx {
    const pc_algo_liquify_proxy *p;
    const pc_algo_liquify_field *field;
    size_t x0, x1, y0;
    uint8_t (*out)[4];
} render_ctx;

static void render_row(const render_ctx *c, size_t y)
{
    const pc_algo_liquify_proxy *p = c->p;
    double s = p->scale, bx = (double)p->bounds.x0, by = (double)p->bounds.y0;
    double py = by + ((double)y + 0.5) * s;
    double lw = (double)(p->w - 1), lh = (double)(p->h - 1);
    size_t x;
    for (x = c->x0; x < c->x1; x++) {
        double ppx = bx + ((double)x + 0.5) * s, d[2], u, v, iu, iv;
        float fu, fv;
        const float *q[4];
        double xs[2], ys[2];
        int i, k;
        pc_algo_liquify_sample(c->field, ppx, py, d);
        u = (ppx + d[0] - bx) / s - 0.5;
        v = (py + d[1] - by) / s - 0.5;
        iu = floor(u);
        iv = floor(v);
        fu = pc_f64_to_f32(u - iu);
        fv = pc_f64_to_f32(v - iv);
        xs[0] = iu;
        xs[1] = iu + 1.0;
        ys[0] = iv;
        ys[1] = iv + 1.0;
        for (i = 0; i < 4; i++) {
            size_t yy = pc_f64_to_size(pc_clampd(ys[i / 2], 0.0, lh));
            size_t xx = pc_f64_to_size(pc_clampd(xs[i % 2], 0.0, lw));
            q[i] = p->px[yy * p->w + xx];
        }
        for (k = 0; k < 4; k++) {
            float val = (q[0][k] * (1.0f - fu) + q[1][k] * fu) * (1.0f - fv) +
                        (q[2][k] * (1.0f - fu) + q[3][k] * fu) * fv;
            c->out[y * p->w + x][k] = pc_f32_to_u8(pc_clampf(val, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
}

static pc_status render_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const render_ctx *c = ctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        render_row(c, c->y0 + i);
    }
    return PC_OK;
}

pc_status pc_algo_liquify_proxy_render(const pc_algo_liquify_proxy *p, const pc_algo_liquify_field *field,
                                       const size_t r[4], uint8_t (*out)[4], size_t out_len, pc_error *err)
{
    size_t x0 = r[0] < p->w ? r[0] : p->w, y0 = r[1] < p->h ? r[1] : p->h;
    size_t x1 = r[2] < p->w ? r[2] : p->w, y1 = r[3] < p->h ? r[3] : p->h, need;
    render_ctx c;
    if (x0 >= x1 || y0 >= y1) {
        return PC_OK;
    }
    if (!pc_size_mul(y1, p->w, &need) || out_len < need) {
        return pc_fail(err, PC_ERR_INVALID, "liquify: the preview buffer holds %zu of %zu pixels", out_len,
                       y1 * p->w);
    }
    c.p = p;
    c.field = field;
    c.x0 = x0;
    c.x1 = x1;
    c.y0 = y0;
    c.out = out;
    /* A dab's few thousand pixels are faster on this thread than waking the pool. */
    if ((x1 - x0) * (y1 - y0) > 150000) {
        return pc_algo_par_for(y1 - y0, 1, render_chunk, &c);
    }
    return render_chunk(&c, 0, 0, y1 - y0);
}

void pc_algo_liquify_proxy_rect(const pc_algo_liquify_proxy *p, pc_rect r, size_t out[4])
{
    double s = p->scale;
    double fx0 = pc_rs_max_k((double)pc_i32_wrapping_sub(r.x0, p->bounds.x0) / s, 0.0);
    double fy0 = pc_rs_max_k((double)pc_i32_wrapping_sub(r.y0, p->bounds.y0) / s, 0.0);
    double fx1 = pc_rs_max_k((double)pc_i32_wrapping_sub(r.x1, p->bounds.x0) / s, 0.0);
    double fy1 = pc_rs_max_k((double)pc_i32_wrapping_sub(r.y1, p->bounds.y0) / s, 0.0);
    size_t a = pc_f64_to_size(ceil(fx1)) + 1, b = pc_f64_to_size(ceil(fy1)) + 1;
    out[0] = pc_f64_to_size(floor(fx0));
    out[1] = pc_f64_to_size(floor(fy0));
    out[2] = a < p->w ? a : p->w;
    out[3] = b < p->h ? b : p->h;
}

/* ---- full-resolution preview ---- */

#define MAX_FULL_RES_SOURCE_PIXELS ((size_t)8 * 1024 * 1024)

typedef struct full_ctx {
    const pc_algo_liquify_field *field;
    pc_rect bounds, canvas, src_rect;
    const float (*scratch)[4];
    size_t sw, width, x0, x1, y0;
    uint8_t (*out)[4];
} full_ctx;

static void full_source_at(const full_ctx *c, double x, double y, float out[4])
{
    pc_rect cv = c->canvas, sr = c->src_rect;
    double fx = pc_clampd(x - 0.5, (double)cv.x0, (double)(cv.x1 - 1));
    double fy = pc_clampd(y - 0.5, (double)cv.y0, (double)(cv.y1 - 1));
    double ix = floor(fx), iy = floor(fy);
    float tx = pc_f64_to_f32(fx - ix), ty = pc_f64_to_f32(fy - iy);
    int32_t sx0 = pc_clamp_i32(pc_f64_to_i32(ix), sr.x0, sr.x1 - 1);
    int32_t sy0 = pc_clamp_i32(pc_f64_to_i32(iy), sr.y0, sr.y1 - 1);
    int32_t sx1 = sx0 + 1 < sr.x1 - 1 ? sx0 + 1 : sr.x1 - 1;
    int32_t sy1 = sy0 + 1 < sr.y1 - 1 ? sy0 + 1 : sr.y1 - 1;
    const float *q[4];
    int32_t xs[2] = {sx0, sx1}, ys[2] = {sy0, sy1};
    int i;
    for (i = 0; i < 4; i++) {
        q[i] = c->scratch[(size_t)(ys[i / 2] - sr.y0) * c->sw + (size_t)(xs[i % 2] - sr.x0)];
    }
    for (i = 0; i < 4; i++) {
        out[i] = (q[0][i] * (1.0f - tx) + q[1][i] * tx) * (1.0f - ty) +
                 (q[2][i] * (1.0f - tx) + q[3][i] * tx) * ty;
    }
}

static pc_status full_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const full_ctx *c = ctx;
    size_t r, x;
    (void)chunk;
    for (r = begin; r < end; r++) {
        size_t yo = c->y0 + r;
        int32_t y = c->bounds.y0 + (int32_t)yo;
        for (x = c->x0; x < c->x1; x++) {
            double p[2], d[2];
            float rgba[4];
            int k;
            p[0] = (double)(c->bounds.x0 + (int32_t)x) + 0.5;
            p[1] = (double)y + 0.5;
            pc_algo_liquify_sample(c->field, p[0], p[1], d);
            full_source_at(c, p[0] + d[0], p[1] + d[1], rgba);
            for (k = 0; k < 4; k++) {
                c->out[yo * c->width + x][k] = pc_f32_to_u8(pc_clampf(rgba[k], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
    }
    return PC_OK;
}

bool pc_algo_liquify_render_full_resolution(const pc_surface *src, const pc_algo_liquify_field *field,
                                            pc_rect bounds, pc_rect region, uint8_t (*out)[4], size_t out_len,
                                            pc_algo_liquify_scratch *scratch)
{
    float min_dx = INFINITY, min_dy = INFINITY, max_dx = -INFINITY, max_dy = -INFINITY;
    double gx0, gy0, gx1, gy1, x0, y0, x1, y1;
    size_t width, height, need, i0, j0, i1, j1, i, j, source_len, k;
    full_ctx c;
    pc_error err;
    bounds = pc_rect_intersect(bounds, field->bounds);
    region = pc_rect_intersect(region, bounds);
    width = pc_rect_width(bounds);
    height = pc_rect_height(bounds);
    if (!pc_size_mul(width, height, &need) || pc_rect_is_empty(region) || out_len != need || width == 0 ||
        height == 0) {
        return false;
    }
    /* Bilinear field interpolation is a convex combination of its surrounding nodes, so their
     * extrema bound every source sample in this output rectangle. */
    gx0 = ((double)region.x0 + 0.5 - (double)field->bounds.x0 - 0.5) / field->cell;
    gy0 = ((double)region.y0 + 0.5 - (double)field->bounds.y0 - 0.5) / field->cell;
    gx1 = ((double)region.x1 - 0.5 - (double)field->bounds.x0 - 0.5) / field->cell;
    gy1 = ((double)region.y1 - 0.5 - (double)field->bounds.y0 - 0.5) / field->cell;
    i0 = pc_f64_to_size(pc_rs_max_k(floor(gx0), 0.0));
    j0 = pc_f64_to_size(pc_rs_max_k(floor(gy0), 0.0));
    i1 = pc_f64_to_size(pc_rs_max_k(ceil(gx1), 0.0));
    j1 = pc_f64_to_size(pc_rs_max_k(ceil(gy1), 0.0));
    if (field->w > 0 && field->h > 0) {
        size_t je = j1 < field->h - 1 ? j1 : field->h - 1, ie = i1 < field->w - 1 ? i1 : field->w - 1;
        for (j = j0; j <= je; j++) {
            for (i = i0; i <= ie; i++) {
                const float *v = field->d[j * field->w + i];
                min_dx = pc_rs_minf(min_dx, v[0]);
                min_dy = pc_rs_minf(min_dy, v[1]);
                max_dx = pc_rs_maxf(max_dx, v[0]);
                max_dy = pc_rs_maxf(max_dy, v[1]);
            }
        }
    }
    if (!isfinite(min_dx) || !isfinite(min_dy) || !isfinite(max_dx) || !isfinite(max_dy)) {
        return false;
    }
    /* Source coordinates are pixel centres: one more sample on each side for bilinear, clamped to
     * the document as the regular proxy renderer does. */
    x0 = floor((double)region.x0 + (double)min_dx) - 1.0;
    y0 = floor((double)region.y0 + (double)min_dy) - 1.0;
    x1 = ceil((double)(region.x1 - 1) + (double)max_dx) + 2.0;
    y1 = ceil((double)(region.y1 - 1) + (double)max_dy) + 2.0;
    c.canvas = field->bounds;
    c.src_rect =
        pc_rect_make(pc_f64_to_i32(pc_rs_min(pc_rs_max(x0, (double)c.canvas.x0), (double)c.canvas.x1)),
                     pc_f64_to_i32(pc_rs_min(pc_rs_max(y0, (double)c.canvas.y0), (double)c.canvas.y1)),
                     pc_f64_to_i32(pc_rs_min(pc_rs_max(x1, (double)c.canvas.x0), (double)c.canvas.x1)),
                     pc_f64_to_i32(pc_rs_min(pc_rs_max(y1, (double)c.canvas.y0), (double)c.canvas.y1)));
    if (pc_rect_is_empty(c.src_rect)) {
        return false;
    }
    c.sw = pc_rect_width(c.src_rect);
    if (!pc_size_mul(c.sw, pc_rect_height(c.src_rect), &source_len) ||
        source_len > MAX_FULL_RES_SOURCE_PIXELS) {
        return false;
    }
    if (scratch->cap < source_len) {
        float (*grown)[4] = realloc(scratch->px, source_len * sizeof *grown);
        if (grown == NULL) {
            return false;
        }
        scratch->px = grown;
        scratch->cap = source_len;
    }
    memset(scratch->px, 0, source_len * sizeof *scratch->px);
    if (pc_surface_read_rgba_into(src, c.src_rect, scratch->px, source_len, &err) != PC_OK) {
        return false;
    }
    if (!pc_surface_format(src).alpha) {
        for (k = 0; k < source_len; k++) {
            scratch->px[k][3] = 1.0f;
        }
    }
    for (k = 0; k < source_len; k++) {
        float a = scratch->px[k][3];
        scratch->px[k][0] *= a;
        scratch->px[k][1] *= a;
        scratch->px[k][2] *= a;
    }
    c.field = field;
    c.bounds = bounds;
    c.scratch = (const float (*)[4])scratch->px;
    c.width = width;
    c.x0 = (size_t)(region.x0 - bounds.x0);
    c.x1 = (size_t)(region.x1 - bounds.x0);
    c.y0 = (size_t)(region.y0 - bounds.y0);
    c.out = out;
    j = (size_t)(region.y1 - bounds.y0) - c.y0;
    if ((c.x1 - c.x0) * j > 150000) {
        return pc_algo_par_for(j, 1, full_chunk, &c) == PC_OK;
    }
    return full_chunk(&c, 0, 0, j) == PC_OK;
}
