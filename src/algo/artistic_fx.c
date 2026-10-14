/* Filter Gallery pixel work, part 1: helpers, reaches, the driver and the Artistic and Brush
 * Strokes effects (port of crates/algo/src/artistic_fx.rs). */
#include "algo/artistic_fx.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fx_defs.h"
#include "algo/fxutil.h"
#include "base/mem.h"

/* Line blurs past this radius would take hours in the reference. */
#define MAX_LINE_RADIUS ((size_t)1 << 24)

/* ---- the working window ---- */

void pc_gfx_xy(const gwin *w, size_t i, int32_t *x, int32_t *y)
{
    *x = w->r.x0 + (int32_t)(i % w->w);
    *y = w->r.y0 + (int32_t)(i / w->w);
}

gpx pc_gfx_mix(gpx a, gpx b, float t)
{
    gpx r = {
        {a.c[0] + (b.c[0] - a.c[0]) * t, a.c[1] + (b.c[1] - a.c[1]) * t, a.c[2] + (b.c[2] - a.c[2]) * t}};
    return r;
}

/* Bilinear sample at document coordinates (pixel centres at .5), clamped to the window. */
gpx pc_gfx_sample(const gwin *w, float x, float y)
{
    float fx = pc_clampf(x - 0.5f - (float)w->r.x0, 0.0f, (float)(w->w - 1));
    float fy = pc_clampf(y - 0.5f - (float)w->r.y0, 0.0f, (float)(w->h - 1));
    size_t x0 = pc_f32_to_size(floorf(fx)), y0 = pc_f32_to_size(floorf(fy)), x1, y1;
    float ax, ay;
    gpx top, bot;
    if (x0 > w->w - 1) {
        x0 = w->w - 1;
    }
    if (y0 > w->h - 1) {
        y0 = w->h - 1;
    }
    x1 = x0 + 1 < w->w - 1 ? x0 + 1 : w->w - 1;
    y1 = y0 + 1 < w->h - 1 ? y0 + 1 : w->h - 1;
    ax = fx - (float)x0;
    ay = fy - (float)y0;
    top = pc_gfx_mix(w->c[y0 * w->w + x0], w->c[y0 * w->w + x1], ax);
    bot = pc_gfx_mix(w->c[y1 * w->w + x0], w->c[y1 * w->w + x1], ax);
    return pc_gfx_mix(top, bot, ay);
}

/* The pixel nearest to document (x, y), clamped to the window. */
gpx pc_gfx_at(const gwin *w, int32_t x, int32_t y)
{
    int32_t xx = pc_clamp_i32(pc_i32_wrapping_sub(x, w->r.x0), 0, (int32_t)w->w - 1);
    int32_t yy = pc_clamp_i32(pc_i32_wrapping_sub(y, w->r.y0), 0, (int32_t)w->h - 1);
    return w->c[(size_t)yy * w->w + (size_t)xx];
}

gpx *pc_gfx_clone(const gwin *w)
{
    gpx *c = pc_malloc_array(w->w * w->h, sizeof *c);
    if (c != NULL) {
        memcpy(c, w->c, w->w * w->h * sizeof *c);
    }
    return c;
}

/* ---- small maths ---- */

/* Hermite step from e0 to e1; e0 > e1 gives the falling step. */
float pc_gfx_smoothstep(float e0, float e1, float x)
{
    float t;
    if (fabsf(e1 - e0) < 1e-9f) {
        return x >= e1 ? 1.0f : 0.0f;
    }
    t = pc_clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float pc_gfx_lum(gpx p)
{
    return 0.299f * p.c[0] + 0.587f * p.c[1] + 0.114f * p.c[2];
}

gpx pc_gfx_grey(float v)
{
    gpx r = {{v, v, v}};
    return r;
}

gpx pc_gfx_rgb(const float c[4])
{
    gpx r = {{c[0], c[1], c[2]}};
    return r;
}

float pc_gfx_clamp01(float v)
{
    return pc_clampf(v, 0.0f, 1.0f);
}

float pc_gfx_posterize(float v, float levels)
{
    float l = pc_rs_maxf_k(levels - 1.0f, 1.0f);
    return roundf(pc_gfx_clamp01(v) * l) / l;
}

gpx pc_gfx_saturate(gpx p, float k)
{
    float l = pc_gfx_lum(p);
    gpx r = {{l + (p.c[0] - l) * k, l + (p.c[1] - l) * k, l + (p.c[2] - l) * k}};
    return r;
}

void pc_gfx_normalize3(const float v[3], float out[3])
{
    float l = pc_rs_maxf_k(sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), 1e-9f);
    out[0] = v[0] / l, out[1] = v[1] / l, out[2] = v[2] / l;
}

/* Pixels a Gaussian blur of sigma reads (three box passes per axis). */
int32_t pc_gfx_gs(float sigma)
{
    size_t r[3];
    if (sigma < 0.3f) {
        return 0;
    }
    pc_algo_gauss_box_radii(sigma, r);
    return pc_i32_wrapping_add(pc_i32_from_bits((uint32_t)(r[0] + r[1] + r[2])), 1);
}

pc_status pc_gfx_blur1(float *p, size_t w, size_t h, float sigma, pc_error *err)
{
    return pc_algo_gauss_blur_n(p, w, h, 1, sigma, err);
}

pc_status pc_gfx_blur3(gpx *c, size_t w, size_t h, float sigma, pc_error *err)
{
    return pc_algo_gauss_blur_n(&c[0].c[0], w, h, 3, sigma, err);
}

void pc_gfx_sobel(const float *p, size_t w, size_t h, float *gx, float *gy)
{
    size_t x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t xm = x > 0 ? x - 1 : 0, xp = x + 1 < w ? x + 1 : w - 1;
            size_t ym = y > 0 ? y - 1 : 0, yp = y + 1 < h ? y + 1 : h - 1;
#define AT(xx, yy) p[(yy) * w + (xx)]
            gx[y * w + x] = ((AT(xp, ym) + 2.0f * AT(xp, y) + AT(xp, yp)) -
                             (AT(xm, ym) + 2.0f * AT(xm, y) + AT(xm, yp))) /
                            8.0f;
            gy[y * w + x] = ((AT(xm, yp) + 2.0f * AT(x, yp) + AT(xp, yp)) -
                             (AT(xm, ym) + 2.0f * AT(x, ym) + AT(xp, ym))) /
                            8.0f;
#undef AT
        }
    }
}

float *pc_gfx_grad_mag(const float *p, size_t w, size_t h)
{
    size_t n = w * h, i;
    float *gx = pc_malloc_array(n > 0 ? n : 1, sizeof *gx), *gy = pc_malloc_array(n > 0 ? n : 1, sizeof *gy);
    if (gx == NULL || gy == NULL) {
        free(gx), free(gy);
        return NULL;
    }
    pc_gfx_sobel(p, w, h, gx, gy);
    for (i = 0; i < n; i++) {
        gx[i] = sqrtf(gx[i] * gx[i] + gy[i] * gy[i]);
    }
    free(gy);
    return gx;
}

float *pc_gfx_win_lum(const gwin *w)
{
    size_t n = w->w * w->h, i;
    float *l = pc_malloc_array(n > 0 ? n : 1, sizeof *l);
    if (l != NULL) {
        for (i = 0; i < n; i++) {
            l[i] = pc_gfx_lum(w->c[i]);
        }
    }
    return l;
}

/* Running-mean blur of radius r steps along direction (dx, dy) ((1,0), (0,1), (1,1) or (1,-1)) of
 * an interleaved n-channel buffer (f64 running sums, edges clamped along each line). */
pc_status pc_gfx_line_blur(float *buf, size_t n, size_t w, size_t h, int32_t dx, int32_t dy, size_t r,
                           pc_error *err)
{
    int64_t wi = (int64_t)w, hi = (int64_t)h, sx, sy;
    size_t cap = w > h ? w : h, *idx;
    double norm, *vals;
    if (r == 0 || w == 0 || h == 0) {
        return PC_OK;
    }
    if (r > MAX_LINE_RADIUS) {
        return pc_fail(err, PC_ERR_LIMIT, "Filter Gallery stroke of %zu px is too long", r);
    }
    norm = 1.0 / (double)(2 * r + 1);
    idx = pc_malloc_array(cap, sizeof *idx);
    vals = pc_malloc_array(cap, sizeof *vals);
    if (idx == NULL || vals == NULL) {
        free(idx), free(vals);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a Filter Gallery stroke");
    }
    for (sy = 0; sy < hi; sy++) {
        for (sx = 0; sx < wi; sx++) {
            int64_t x = sx, y = sy, px = sx - dx, py = sy - dy, m, k;
            size_t cnt = 0, c;
            if (px >= 0 && py >= 0 && px < wi && py < hi) {
                continue; /* not the start of a line */
            }
            while (x >= 0 && y >= 0 && x < wi && y < hi && cnt < cap) {
                idx[cnt++] = ((size_t)y * w + (size_t)x) * n;
                x += dx;
                y += dy;
            }
            m = (int64_t)cnt;
            for (c = 0; c < n; c++) {
                double acc = -0.0;
                size_t j;
                for (j = 0; j < cnt; j++) {
                    vals[j] = (double)buf[idx[j] + c];
                }
#define VAT(kk) vals[(kk) < 0 ? 0 : ((kk) > m - 1 ? m - 1 : (kk))]
                for (k = -(int64_t)r; k <= (int64_t)r; k++) {
                    acc += VAT(k);
                }
                for (k = 0; k < m; k++) {
                    int64_t a = k + (int64_t)r + 1, b = k - (int64_t)r;
                    buf[idx[k] + c] = (float)(acc * norm);
                    acc += VAT(a) - VAT(b);
                }
#undef VAT
            }
        }
    }
    free(idx);
    free(vals);
    return PC_OK;
}

pc_status pc_gfx_line_blur3(gpx *c, size_t w, size_t h, int32_t dx, int32_t dy, size_t r, pc_error *err)
{
    return pc_gfx_line_blur(&c[0].c[0], 3, w, h, dx, dy, r, err);
}

void pc_gfx_direction(const char *name, int32_t *dx, int32_t *dy)
{
    if (strcmp(name, "horizontal") == 0) {
        *dx = 1, *dy = 0;
    } else if (strcmp(name, "vertical") == 0) {
        *dx = 0, *dy = 1;
    } else if (strcmp(name, "leftDiagonal") == 0) {
        *dx = 1, *dy = 1;
    } else {
        *dx = 1, *dy = -1; /* rightDiagonal: bottom-left to top-right */
    }
}

/* Kuwahara smoothing (the flattest of four quadrant means): the painterly base of several
 * Artistic filters. Box means make the cost independent of r. */
pc_status pc_gfx_kuwahara(gwin *w, size_t r, pc_error *err)
{
    size_t h2 = r / 2 + (r % 2 != 0), n = w->w * w->h, i, nm;
    float *m;
    int64_t o, x, y;
    pc_status st;
    if (h2 < 1) {
        h2 = 1;
    }
    if (h2 > PC_ALGO_MAX_BOX_RADIUS) {
        return pc_fail(err, PC_ERR_LIMIT, "Filter Gallery brush of %zu px is too large", r);
    }
    if (!pc_size_mul(n, 5, &nm)) {
        return pc_fail(err, PC_ERR_LIMIT, "Filter Gallery window is too large");
    }
    m = pc_malloc_array(nm > 0 ? nm : 1, sizeof *m);
    if (m == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the Filter Gallery");
    }
    for (i = 0; i < n; i++) {
        float l = pc_gfx_lum(w->c[i]);
        m[i * 5] = w->c[i].c[0], m[i * 5 + 1] = w->c[i].c[1], m[i * 5 + 2] = w->c[i].c[2];
        m[i * 5 + 3] = l, m[i * 5 + 4] = l * l;
    }
    st = pc_algo_box_blur_n(m, w->w, w->h, 5, h2, err);
    if (st != PC_OK) {
        free(m);
        return st;
    }
    o = (int64_t)h2;
    for (y = 0; y < (int64_t)w->h; y++) {
        for (x = 0; x < (int64_t)w->w; x++) {
            const int64_t q4[4][2] = {{-o, -o}, {o, -o}, {-o, o}, {o, o}};
            float best = FLT_MAX;
            gpx col = {{0.0f, 0.0f, 0.0f}};
            int k;
            for (k = 0; k < 4; k++) {
                int64_t qx = x + q4[k][0], qy = y + q4[k][1];
                const float *q;
                float var;
                qx = qx < 0 ? 0 : (qx > (int64_t)w->w - 1 ? (int64_t)w->w - 1 : qx);
                qy = qy < 0 ? 0 : (qy > (int64_t)w->h - 1 ? (int64_t)w->h - 1 : qy);
                q = m + ((size_t)qy * w->w + (size_t)qx) * 5;
                var = q[4] - q[3] * q[3];
                if (var < best) {
                    best = var;
                    col.c[0] = q[0], col.c[1] = q[1], col.c[2] = q[2];
                }
            }
            w->c[(size_t)y * w->w + (size_t)x] = col;
        }
    }
    free(m);
    return PC_OK;
}

static int32_t kuwahara_reach(size_t r)
{
    size_t h2 = r / 2 + (r % 2 != 0);
    if (h2 < 1) {
        h2 = 1;
    }
    return pc_i32_wrapping_add(pc_i32_wrapping_mul(2, pc_i32_from_bits((uint32_t)h2)), 1);
}

pc_status pc_gfx_unsharp(gwin *w, float sigma, float amount, pc_error *err)
{
    gpx *b;
    size_t i, k;
    pc_status st;
    if (amount <= 0.0f) {
        return PC_OK;
    }
    b = pc_gfx_clone(w);
    if (b == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the Filter Gallery");
    }
    st = pc_gfx_blur3(b, w->w, w->h, sigma, err);
    if (st == PC_OK) {
        for (i = 0; i < w->w * w->h; i++) {
            for (k = 0; k < 3; k++) {
                w->c[i].c[k] = w->c[i].c[k] + (w->c[i].c[k] - b[i].c[k]) * amount;
            }
        }
    }
    free(b);
    return st;
}

/* ---- noise, strokes, textures ---- */

/* Smooth value noise (0..1) on a unit lattice. */
float pc_gfx_vnoise(float x, float y, uint32_t seed)
{
    int32_t ix = pc_f32_to_i32(floorf(x)), iy = pc_f32_to_i32(floorf(y));
    float tx = x - (float)ix, ty = y - (float)iy;
    float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
    int32_t ix1 = pc_i32_wrapping_add(ix, 1), iy1 = pc_i32_wrapping_add(iy, 1);
    float h00 = pc_algo_hash01(ix, iy, 0, seed), h10 = pc_algo_hash01(ix1, iy, 0, seed);
    float h01 = pc_algo_hash01(ix, iy1, 0, seed), h11 = pc_algo_hash01(ix1, iy1, 0, seed);
    float top = h00 + (h10 - h00) * sx, bot = h01 + (h11 - h01) * sx;
    return top + (bot - top) * sy;
}

/* Three-octave value noise (0..1). */
float pc_gfx_fbm(float x, float y, uint32_t seed)
{
    return (pc_gfx_vnoise(x, y, seed) * 0.57f + pc_gfx_vnoise(x * 2.03f, y * 2.03f, seed ^ 0x51u) * 0.29f +
            pc_gfx_vnoise(x * 4.1f, y * 4.1f, seed ^ 0xa3u) * 0.14f) /
           1.0f;
}

/* Stroke texture: value noise stretched len px along (dx, dy) and wid px across. */
float pc_gfx_strokes(int32_t x, int32_t y, int32_t dx, int32_t dy, float len, float wid, uint32_t seed)
{
    float fdx = (float)dx, fdy = (float)dy, l = pc_rs_maxf_k(sqrtf(fdx * fdx + fdy * fdy), 1e-6f);
    float ux = fdx / l, uy = fdy / l, xf = (float)x, yf = (float)y;
    float u = xf * ux + yf * uy, v = -xf * uy + yf * ux;
    return pc_gfx_vnoise(u / pc_rs_maxf_k(len, 0.5f), v / pc_rs_maxf_k(wid, 0.3f), seed);
}

/* Repaints in discrete strokes len px long and wid px wide along (dx, dy): every stroke takes the
 * colour of src at its middle and neighbouring strokes start at staggered points. *t is where the
 * pixel lies along its stroke (0 start, 1 end). */
gpx pc_gfx_paint_stroke(const gwin *src, int32_t x, int32_t y, int32_t dx, int32_t dy, float len, float wid,
                        uint32_t seed, float *t)
{
    float fdx = (float)dx, fdy = (float)dy, l = pc_rs_maxf_k(sqrtf(fdx * fdx + fdy * fdy), 1e-6f);
    float ux = fdx / l, uy = fdy / l, fx = (float)x + 0.5f, fy = (float)y + 0.5f, u, v, off, du;
    int32_t lane;
    len = pc_rs_maxf_k(len, 1.0f);
    wid = pc_rs_maxf_k(wid, 0.5f);
    u = fx * ux + fy * uy;
    v = -fx * uy + fy * ux;
    lane = pc_f32_to_i32(floorf(v / wid));
    off = pc_algo_hash01(lane, 0, 0, seed) * len;
    *t = pc_algo_rem_euclidf((u + off) / len, 1.0f);
    /* Back to the stroke's middle along the direction. */
    du = (0.5f - *t) * len;
    return pc_gfx_sample(src, fx + du * ux, fy + du * uy);
}

/* Unit vector toward the light in image coordinates (y down). */
void pc_gfx_light_dir(const char *name, float *lx, float *ly)
{
    const float s = PC_FX_FRAC_1_SQRT_2;
    float x = 0.0f, y = -1.0f;
    if (strcmp(name, "bottom") == 0) {
        x = 0.0f, y = 1.0f;
    } else if (strcmp(name, "bottomLeft") == 0) {
        x = -s, y = s;
    } else if (strcmp(name, "left") == 0) {
        x = -1.0f, y = 0.0f;
    } else if (strcmp(name, "topLeft") == 0) {
        x = -s, y = -s;
    } else if (strcmp(name, "topRight") == 0) {
        x = s, y = -s;
    } else if (strcmp(name, "right") == 0) {
        x = 1.0f, y = 0.0f;
    } else if (strcmp(name, "bottomRight") == 0) {
        x = s, y = s;
    }
    *lx = x;
    *ly = y;
}

/* Lambert shading factor (1 = flat) of a height field with gradient (gx, gy). */
float pc_gfx_shade(float gx, float gy, float lx, float ly, float k)
{
    float nx = -k * gx, ny = -k * gy;
    float nl = sqrtf(nx * nx + ny * ny + 1.0f * 1.0f), ll = sqrtf(lx * lx + ly * ly + 1.0f * 1.0f);
    float d = (nx * lx + ny * ly + 1.0f * 1.0f) / (nl * ll);
    return d / (1.0f / ll);
}

/* Basket weave of threads p px wide with `noise` irregularity. */
static float weave(float x, float y, float p, float noise)
{
    int32_t cx = pc_f32_to_i32(floorf(x / p)), cy = pc_f32_to_i32(floorf(y / p));
    bool over_h = pc_i32_wrapping_add(cx, cy) % 2 == 0;
    float u = pc_algo_rem_euclidf(x, p) / p, v = pc_algo_rem_euclidf(y, p) / p;
    /* A thread is a rounded ridge across its cell. */
    float ridge = over_h ? sinf(v * PC_FX_PI) : sinf(u * PC_FX_PI);
    float n = pc_gfx_vnoise(x / (p * 0.7f), y / (p * 2.0f), 97);
    return ridge * (1.0f - noise) + n * noise;
}

/* Procedural textures (0..1 heights) at document coordinates; s scales them (1 = 100 %). */
float pc_gfx_texture(const char *kind, float x, float y, float s)
{
    x = x / s;
    y = y / s;
    if (strcmp(kind, "brick") == 0) {
        const float bw = 24.0f, bh = 12.0f;
        float row = floorf(y / bh), xo, u, v, d;
        xo = x + (pc_f32_to_i32(row) % 2 == 0 ? 0.0f : bw / 2.0f);
        u = pc_algo_rem_euclidf(xo, bw);
        v = pc_algo_rem_euclidf(y, bh);
        d = pc_rs_minf(pc_rs_minf(u, bw - u), pc_rs_minf(v, bh - v));
        return pc_gfx_smoothstep(0.6f, 2.2f, d) * (0.85f + 0.15f * pc_gfx_vnoise(x / 3.0f, y / 3.0f, 91));
    }
    if (strcmp(kind, "burlap") == 0) {
        return weave(x, y, 7.0f, 0.35f);
    }
    if (strcmp(kind, "canvas") == 0) {
        return weave(x, y, 3.5f, 0.18f);
    }
    if (strcmp(kind, "sandstone") == 0) {
        return pc_gfx_fbm(x / 7.0f, y / 7.0f, 93) * 0.7f +
               pc_algo_hash01(pc_f32_to_i32(x), pc_f32_to_i32(y), 0, 94) * 0.3f;
    }
    if (strcmp(kind, "blocks") == 0) {
        const float b = 18.0f;
        float u = pc_algo_rem_euclidf(x, b) / b, v = pc_algo_rem_euclidf(y, b) / b;
        return pc_rs_minf_k(pc_rs_minf(pc_rs_minf(u, 1.0f - u), pc_rs_minf(v, 1.0f - v)) * 4.0f, 1.0f);
    }
    if (strcmp(kind, "frosted") == 0) {
        return pc_gfx_fbm(x / 2.5f, y / 2.5f, 95);
    }
    if (strcmp(kind, "tinyLens") == 0) {
        const float p = 12.0f;
        float u = (pc_algo_rem_euclidf(x, p) / p - 0.5f) * 2.0f,
              v = (pc_algo_rem_euclidf(y, p) / p - 0.5f) * 2.0f;
        return sqrtf(pc_rs_maxf_k(1.0f - (u * u + v * v), 0.0f));
    }
    return 0.5f;
}

void pc_gfx_texturize(gwin *w, const char *kind, float scaling, float relief, const char *light, bool invert,
                      int mode)
{
    float s, lx, ly, k, sign;
    size_t i, c;
    if (relief <= 0.0f) {
        return;
    }
    s = pc_clampf(scaling / 100.0f, 0.5f, 2.0f);
    pc_gfx_light_dir(light, &lx, &ly);
    k = relief / 50.0f * 3.0f;
    sign = invert ? -1.0f : 1.0f;
    for (i = 0; i < w->w * w->h; i++) {
        int32_t x, y;
        float fx, fy, gx, gy, f, wt;
        pc_gfx_xy(w, i, &x, &y);
        fx = (float)x;
        fy = (float)y;
        gx = (pc_gfx_texture(kind, fx + 1.0f, fy, s) - pc_gfx_texture(kind, fx - 1.0f, fy, s)) * 0.5f * sign;
        gy = (pc_gfx_texture(kind, fx, fy + 1.0f, s) - pc_gfx_texture(kind, fx, fy - 1.0f, s)) * 0.5f * sign;
        /* Rough Pastels: bright areas are thick chalk; in dark ones it's scraped off. */
        wt = mode == 1 ? 0.15f + 0.85f * (1.0f - pc_gfx_clamp01(pc_gfx_lum(w->c[i]))) : 1.0f;
        f = 1.0f + (pc_clampf(pc_gfx_shade(gx, gy, lx, ly, k), 0.0f, 2.0f) - 1.0f) * wt;
        for (c = 0; c < 3; c++) {
            w->c[i].c[c] = w->c[i].c[c] * f;
        }
    }
}

/* Closest and second-closest jittered cell points (distances and the closest point). */
void pc_gfx_voronoi(float x, float y, float cell, uint32_t seed, float *d1, float *d2, float *px, float *py)
{
    int32_t cx = pc_f32_to_i32(floorf(x / cell)), cy = pc_f32_to_i32(floorf(y / cell));
    float a = FLT_MAX, b = FLT_MAX, bx = x, by = y;
    int i, j;
    for (j = -1; j <= 1; j++) {
        for (i = -1; i <= 1; i++) {
            float qx, qy, d;
            pc_algo_cell_point(pc_i32_wrapping_add(cx, i), pc_i32_wrapping_add(cy, j), cell, seed, 0.0f, 0.0f,
                               &qx, &qy);
            d = sqrtf(pc_fx_sq(qx - x) + pc_fx_sq(qy - y));
            if (d < a) {
                b = a;
                a = d;
                bx = qx, by = qy;
            } else if (d < b) {
                b = d;
            }
        }
    }
    *d1 = a, *d2 = b, *px = bx, *py = by;
}

/* Brightens highlights (Film Grain / Smudge Stick "Highlight Area" and "Intensity"). */
gpx pc_gfx_highlights(gpx p, float l, float area, float intensity)
{
    float m;
    size_t c;
    if (area <= 0.0f) {
        return p;
    }
    m = pc_gfx_smoothstep(1.0f - area / 20.0f * 0.75f, 1.02f, l) * (1.0f - intensity / 10.0f * 0.6f);
    for (c = 0; c < 3; c++) {
        p.c[c] = p.c[c] + (1.0f - p.c[c]) * m;
    }
    return p;
}

/* ---- reaches ---- */

#define G(k) pc_algo_gallery_effect_get(e, (k))

static float cutout_sigma(const pc_algo_gallery_effect *e)
{
    return 0.5f + G("edgeSimplicity") * (1.1f - 0.3f * G("edgeFidelity"));
}

static size_t daubs_radius(const pc_algo_gallery_effect *e)
{
    char buf[PC_ALGO_GALLERY_NAME_MAX];
    const char *ty = pc_algo_gallery_effect_choice(e, "brushType", buf);
    bool wide = strcmp(ty, "wideSharp") == 0 || strcmp(ty, "wideBlurry") == 0;
    float r = G("brushSize") / 2.0f * (wide ? 1.5f : 1.0f);
    return pc_f32_to_size(pc_rs_maxf_k(r, 1.0f));
}

static float plastic_sigma(const pc_algo_gallery_effect *e)
{
    return 0.5f + G("smoothness") * 0.5f + (15.0f - G("detail")) * 0.2f;
}

static size_t watercolor_radius(const pc_algo_gallery_effect *e)
{
    return 1 + pc_f32_to_size((15.0f - G("brushDetail")) / 2.5f);
}

float pc_gfx_basrelief_sigma(const pc_algo_gallery_effect *e)
{
    return 0.3f + G("smoothness") * 0.5f + (15.0f - G("detail")) * 0.12f;
}

float pc_gfx_patch_size(const pc_algo_gallery_effect *e)
{
    return 3.0f + G("squareSize") * 1.5f;
}

float pc_gfx_glass_cell(const pc_algo_gallery_effect *e)
{
    return G("cellSize") * 1.6f + 1.0f;
}

static int32_t f2i(float v)
{
    return pc_f32_to_i32(v);
}

/* Sum with i32 wrapping (the reference's release-mode `+`). */
static int32_t add3(int32_t a, int32_t b, int32_t c)
{
    return pc_i32_wrapping_add(pc_i32_wrapping_add(a, b), c);
}

int32_t pc_algo_gallery_effect_reach(const pc_algo_gallery_effect *e)
{
    switch (e->filter) {
    case PC_ALGO_GAL_COLORED_PENCIL:
        return 2;
    case PC_ALGO_GAL_CUTOUT:
        return pc_gfx_gs(cutout_sigma(e));
    case PC_ALGO_GAL_DRY_BRUSH:
    case PC_ALGO_GAL_FRESCO:
        return add3(kuwahara_reach(1 + pc_f32_to_size(G("brushSize"))), pc_gfx_gs(1.0f), 2);
    case PC_ALGO_GAL_ROUGH_PASTELS:
        return add3(pc_gfx_gs(0.6f + 1.2f), f2i((2.0f + G("strokeLength")) / 2.0f), 3);
    case PC_ALGO_GAL_NEON_GLOW:
        return pc_gfx_gs(fabsf(G("glowSize")) * 0.6f + 0.5f);
    case PC_ALGO_GAL_PAINT_DAUBS:
        return add3(kuwahara_reach(daubs_radius(e)), pc_gfx_gs(1.5f), 1);
    case PC_ALGO_GAL_PALETTE_KNIFE:
        return pc_i32_wrapping_add(kuwahara_reach(pc_f32_to_size(pc_rs_maxf_k(G("strokeSize") / 3.0f, 1.0f))),
                                   pc_gfx_gs(G("softness") * 0.5f));
    case PC_ALGO_GAL_PLASTIC_WRAP:
        return pc_i32_wrapping_add(pc_gfx_gs(plastic_sigma(e)), 2);
    case PC_ALGO_GAL_POSTER_EDGES:
        return pc_i32_wrapping_add(pc_gfx_gs(0.6f + G("edgeThickness") * 0.6f), 2);
    case PC_ALGO_GAL_SMUDGE_STICK:
        return pc_i32_wrapping_add(f2i(G("strokeLength") * 1.5f), 2);
    case PC_ALGO_GAL_SPONGE:
        return kuwahara_reach(1 + pc_f32_to_size(G("brushSize")));
    case PC_ALGO_GAL_UNDERPAINTING:
        return pc_gfx_gs(0.5f + G("brushSize") * 0.4f);
    case PC_ALGO_GAL_WATERCOLOR:
        return pc_i32_wrapping_add(kuwahara_reach(watercolor_radius(e)), 2);
    case PC_ALGO_GAL_ACCENTED_EDGES:
        return add3(pc_gfx_gs(0.4f + G("smoothness") * 0.3f), pc_gfx_gs(G("edgeWidth") * 0.5f), 2);
    case PC_ALGO_GAL_ANGLED_STROKES:
        return add3(add3(pc_gfx_gs(0.8f), f2i(pc_rs_maxf_k(G("strokeLength"), 3.0f) / 2.0f), pc_gfx_gs(1.0f)),
                    3, 0);
    case PC_ALGO_GAL_CROSSHATCH:
        return add3(f2i(G("strokeLength") / 3.0f), pc_gfx_gs(1.0f), 2);
    case PC_ALGO_GAL_DARK_STROKES:
        return 9;
    case PC_ALGO_GAL_INK_OUTLINES:
        return pc_i32_wrapping_add(f2i(G("strokeLength") / 2.0f), 3);
    case PC_ALGO_GAL_SPATTER:
        return pc_i32_wrapping_add(f2i(ceilf(G("sprayRadius") * 1.5f)), 2);
    case PC_ALGO_GAL_SPRAYED_STROKES:
        return add3(f2i(ceilf(G("sprayRadius"))), f2i(G("strokeLength") / 2.0f), 3);
    case PC_ALGO_GAL_SUMI_E:
        return add3(pc_gfx_gs(G("strokeWidth") * 0.2f), f2i(G("strokeWidth") / 2.0f), 2);
    case PC_ALGO_GAL_GLASS:
        return pc_i32_wrapping_add(f2i(ceilf(G("distortion") * 2.0f)), 2);
    case PC_ALGO_GAL_OCEAN_RIPPLE:
        return pc_i32_wrapping_add(f2i(ceilf(G("rippleMagnitude") * 0.9f)), 2);
    case PC_ALGO_GAL_BAS_RELIEF:
        return pc_i32_wrapping_add(pc_gfx_gs(pc_gfx_basrelief_sigma(e)), 2);
    case PC_ALGO_GAL_CHARCOAL:
        return pc_i32_wrapping_add(pc_gfx_gs(1.0f), 2);
    case PC_ALGO_GAL_CHROME:
        return pc_i32_wrapping_add(pc_gfx_gs(1.0f + G("smoothness") * 0.4f), 2);
    case PC_ALGO_GAL_GRAPHIC_PEN:
        return pc_i32_wrapping_add(f2i(G("strokeLength") / 2.0f), 2);
    case PC_ALGO_GAL_HALFTONE_PATTERN:
        return pc_gfx_gs(0.5f + G("size") * 0.3f);
    case PC_ALGO_GAL_NOTE_PAPER:
        return add3(pc_gfx_gs(1.0f), pc_gfx_gs(1.2f), 2);
    case PC_ALGO_GAL_PHOTOCOPY:
        return pc_gfx_gs(1.0f + G("detail") * 0.5f);
    case PC_ALGO_GAL_PLASTER:
        return add3(pc_gfx_gs(0.5f + G("smoothness") * 0.8f), pc_gfx_gs(1.5f), 2);
    case PC_ALGO_GAL_STAMP:
        return pc_gfx_gs(0.3f + G("smoothness") * 0.3f);
    case PC_ALGO_GAL_TORN_EDGES:
        return pc_gfx_gs(1.0f);
    case PC_ALGO_GAL_WATER_PAPER:
        return pc_i32_wrapping_add(pc_i32_wrapping_mul(2, f2i(G("fiberLength") / 6.0f)), 2);
    case PC_ALGO_GAL_GLOWING_EDGES:
        return add3(pc_gfx_gs(0.3f + G("smoothness") * 0.25f), pc_gfx_gs(G("edgeWidth") * 0.4f), 2);
    case PC_ALGO_GAL_PATCHWORK:
        return pc_i32_wrapping_add(f2i(ceilf(pc_gfx_patch_size(e))), 2);
    case PC_ALGO_GAL_STAINED_GLASS:
        return pc_i32_wrapping_add(f2i(ceilf(pc_gfx_glass_cell(e) * 2.5f)), 2);
    default:
        /* Film Grain, Diffuse Glow, Chalk & Charcoal, Reticulation, Grain, Mosaic Tiles,
         * Texturizer, Conte Crayon, Craquelure. */
        return 0;
    }
}

/* ---- the driver ---- */

pc_status pc_algo_gallery_run(const pc_algo_gallery_effects *effects, const pc_algo_image *src, pc_rect out,
                              const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    size_t n = src->ch, i, k;
    gwin w;
    pc_status st = PC_OK;
    w.r = src->rect;
    w.w = pc_rect_width(src->rect);
    w.h = pc_rect_height(src->rect);
    pc_algo_image_crop_into(src, out, dst);
    if (effects->n == 0 || w.w == 0 || w.h == 0 || n == 0 || src->data == NULL) {
        return PC_OK;
    }
    w.c = pc_malloc_array(w.w * w.h, sizeof *w.c);
    if (w.c == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the Filter Gallery");
    }
    for (i = 0; i < w.w * w.h; i++) {
        float c[4];
        pc_algo_rgba(ctx, src->data + i * n, n, c);
        w.c[i] = pc_gfx_rgb(c);
    }
    for (k = 0; k < effects->n && st == PC_OK; k++) {
        bool handled = false;
        st = pc_gfx_apply1(&effects->v[k], &w, ctx, &handled, err);
        if (st == PC_OK && !handled) {
            st = pc_gfx_apply2(&effects->v[k], &w, ctx, err);
        }
    }
    if (st == PC_OK) {
        for (i = 0; i < pc_fx_npx(out); i++) {
            float *px = dst + i * n, a = ctx->alpha ? px[n - 1] : 1.0f;
            int32_t x, y;
            gpx c;
            pc_algo_xy(out, i, &x, &y);
            c = pc_gfx_at(&w, x, y);
            {
                const float col[4] = {pc_gfx_clamp01(c.c[0]), pc_gfx_clamp01(c.c[1]), pc_gfx_clamp01(c.c[2]),
                                      a};
                pc_algo_set_rgba(ctx, px, n, col);
            }
            if (ctx->alpha) {
                px[n - 1] = a;
            }
        }
    }
    free(w.c);
    return st;
}

/* ---- Artistic and Brush Strokes ---- */

#define NOMEM() pc_fail(err, PC_ERR_NOMEM, "out of memory for the Filter Gallery")
#define TRY(x)                                                                                               \
    do {                                                                                                     \
        st = (x);                                                                                            \
        if (st != PC_OK) {                                                                                   \
            goto done;                                                                                       \
        }                                                                                                    \
    } while (0)

static gpx scale_px(gpx p, float k)
{
    gpx r = {{p.c[0] * k, p.c[1] * k, p.c[2] * k}};
    return r;
}

pc_status pc_gfx_apply1(const pc_algo_gallery_effect *e, gwin *w, const pc_algo_ctx *ctx, bool *handled,
                        pc_error *err)
{
    size_t W = w->w, H = w->h, N = W * H, i, c;
    gpx fg = pc_gfx_rgb(e->foreground), bg = pc_gfx_rgb(e->background);
    float *l = NULL, *m = NULL, *e2 = NULL;
    gpx *tmp = NULL;
    char cb[PC_ALGO_GALLERY_NAME_MAX], cb2[PC_ALGO_GALLERY_NAME_MAX];
    pc_status st = PC_OK;
    int32_t x, y;
    (void)ctx;
    *handled = true;
    switch (e->filter) {
    case PC_ALGO_GAL_COLORED_PENCIL: {
        float pw = G("pencilWidth"), pressure = G("strokePressure") / 15.0f;
        float pk = 0.7f + 0.3f * G("paperBrightness") / 50.0f;
        gpx paper = scale_px(bg, pk);
        l = pc_gfx_win_lum(w);
        e2 = l != NULL ? pc_gfx_grad_mag(l, W, H) : NULL;
        if (e2 == NULL) {
            TRY(NOMEM());
        }
        for (i = 0; i < N; i++) {
            float d = 1.0f - l[i], h1, h2, hatch, cov;
            pc_gfx_xy(w, i, &x, &y);
            h1 = pc_gfx_strokes(x, y, 1, 1, 5.0f + pw * 3.0f, 0.5f + pw * 0.45f, 11);
            h2 = pc_gfx_strokes(x, y, 1, -1, 5.0f + pw * 3.0f, 0.5f + pw * 0.45f, 12);
            hatch = pc_rs_maxf(h1, h2 * pc_gfx_smoothstep(0.3f, 0.8f, d));
            cov = pc_gfx_clamp01(d * (0.55f + pressure) + e2[i] * 3.0f - (1.0f - hatch) * 0.8f + 0.25f);
            w->c[i] = pc_gfx_mix(paper, scale_px(w->c[i], 0.88f), cov);
        }
        break;
    }
    case PC_ALGO_GAL_CUTOUT: {
        float levels;
        TRY(pc_gfx_blur3(w->c, W, H, cutout_sigma(e), err));
        levels = roundf(G("numberOfLevels"));
        for (i = 0; i < N; i++) {
            /* Flat tones per luminance level, keeping each region's hue. */
            float lv = pc_rs_maxf_k(pc_gfx_lum(w->c[i]), 1e-4f), k = pc_gfx_posterize(lv, levels) / lv;
            for (c = 0; c < 3; c++) {
                w->c[i].c[c] = pc_gfx_posterize(pc_gfx_clamp01(w->c[i].c[c] * k), levels * 2.0f);
            }
        }
        break;
    }
    case PC_ALGO_GAL_DRY_BRUSH:
    case PC_ALGO_GAL_FRESCO: {
        bool fresco = e->filter == PC_ALGO_GAL_FRESCO;
        float levels, amp;
        TRY(pc_gfx_kuwahara(w, 1 + pc_f32_to_size(G("brushSize")), err));
        TRY(pc_gfx_unsharp(w, 1.0f, fresco ? 0.9f : 0.5f, err));
        levels = 3.0f + G("brushDetail");
        amp = 0.02f + (G("texture") - 1.0f) * 0.05f;
        if (fresco) {
            float *lum = pc_gfx_win_lum(w);
            l = lum != NULL ? pc_gfx_grad_mag(lum, W, H) : NULL;
            free(lum);
            if (l == NULL) {
                TRY(NOMEM());
            }
        }
        for (i = 0; i < N; i++) {
            float nz;
            pc_gfx_xy(w, i, &x, &y);
            nz = (pc_algo_hash01(x, y, 0, 21) - 0.5f) * amp;
            for (c = 0; c < 3; c++) {
                float v = w->c[i].c[c];
                v = pc_gfx_posterize(v, levels) * 0.6f + v * 0.4f + nz;
                if (fresco) {
                    v = powf(pc_gfx_clamp01((v - 0.5f) * 1.3f + 0.45f), 1.15f) *
                        (1.0f - pc_gfx_clamp01(l[i] * 2.5f) * 0.6f);
                }
                w->c[i].c[c] = v;
            }
        }
        break;
    }
    case PC_ALGO_GAL_FILM_GRAIN: {
        float grain = G("grain") / 20.0f, area = G("highlightArea"), inten = G("intensity");
        for (i = 0; i < N; i++) {
            float lv = pc_gfx_lum(w->c[i]), nz;
            gpx p;
            pc_gfx_xy(w, i, &x, &y);
            nz = (pc_algo_hash01(x, y, 0, 31) - 0.5f) * grain * 0.5f * (0.3f + 2.8f * lv * (1.0f - lv));
            for (c = 0; c < 3; c++) {
                p.c[c] = w->c[i].c[c] + nz;
            }
            w->c[i] = pc_gfx_highlights(p, lv, area, inten);
        }
        break;
    }
    case PC_ALGO_GAL_NEON_GLOW: {
        float size = G("glowSize"), br = G("glowBrightness") / 50.0f;
        gpx glow = pc_gfx_rgb(e->color);
        l = pc_gfx_win_lum(w);
        m = pc_malloc_array(N, sizeof *m);
        if (l == NULL || m == NULL) {
            TRY(NOMEM());
        }
        for (i = 0; i < N; i++) {
            m[i] = size >= 0.0f ? l[i] : 1.0f - l[i];
        }
        TRY(pc_gfx_blur1(m, W, H, fabsf(size) * 0.6f + 0.5f, err));
        for (i = 0; i < N; i++) {
            gpx base = pc_gfx_mix(fg, bg, l[i]);
            float k = pc_gfx_clamp01(m[i] * 1.2f) * br * 2.0f;
            for (c = 0; c < 3; c++) {
                w->c[i].c[c] = 1.0f - (1.0f - base.c[c]) * (1.0f - pc_gfx_clamp01(glow.c[c] * k));
            }
        }
        break;
    }
    case PC_ALGO_GAL_PAINT_DAUBS: {
        const char *ty = pc_algo_gallery_effect_choice(e, "brushType", cb);
        float sharp;
        bool sparkle = strcmp(ty, "sparkle") == 0, light = strcmp(ty, "lightRough") == 0,
             dark = strcmp(ty, "darkRough") == 0;
        TRY(pc_gfx_kuwahara(w, daubs_radius(e), err));
        sharp = G("sharpness") / 40.0f;
        if (strcmp(ty, "wideBlurry") == 0) {
            TRY(pc_gfx_blur3(w->c, W, H, 1.5f, err));
        } else if (strcmp(ty, "wideSharp") == 0) {
            TRY(pc_gfx_unsharp(w, 1.5f, sharp * 4.0f, err));
        } else {
            TRY(pc_gfx_unsharp(w, 1.2f, sharp * 2.5f, err));
        }
        if (light || dark || sparkle) {
            if (sparkle) {
                float *lum = pc_gfx_win_lum(w);
                e2 = lum != NULL ? pc_gfx_grad_mag(lum, W, H) : NULL;
                free(lum);
                if (e2 == NULL) {
                    TRY(NOMEM());
                }
            }
            for (i = 0; i < N; i++) {
                float nz;
                pc_gfx_xy(w, i, &x, &y);
                nz = pc_gfx_fbm((float)x / 2.0f, (float)y / 2.0f, 41) - 0.5f;
                for (c = 0; c < 3; c++) {
                    float v = w->c[i].c[c];
                    if (light) {
                        v = v + pc_rs_maxf_k(nz, 0.0f) * 0.35f;
                    } else if (dark) {
                        v = v * (1.0f - pc_rs_maxf_k(nz, 0.0f) * 0.7f);
                    } else {
                        v = v + pc_gfx_clamp01(e2[i] * 4.0f) *
                                    pc_gfx_smoothstep(0.75f, 0.95f, pc_algo_hash01(x, y, 0, 42)) * 0.8f;
                    }
                    w->c[i].c[c] = v;
                }
            }
        }
        break;
    }
    case PC_ALGO_GAL_PALETTE_KNIFE: {
        float levels;
        TRY(pc_gfx_kuwahara(w, pc_f32_to_size(pc_rs_maxf_k(G("strokeSize") / 3.0f, 1.0f)), err));
        levels = 2.0f + G("strokeDetail") * 3.0f;
        for (i = 0; i < N; i++) {
            for (c = 0; c < 3; c++) {
                float v = w->c[i].c[c];
                w->c[i].c[c] = pc_gfx_posterize(v, levels) * 0.5f + v * 0.5f;
            }
        }
        TRY(pc_gfx_blur3(w->c, W, H, G("softness") * 0.5f, err));
        break;
    }
    case PC_ALGO_GAL_PLASTIC_WRAP: {
        float hs = G("highlightStrength") / 20.0f, lv[3], hv[3];
        const float lv0[3] = {-0.45f, -0.65f, 1.0f};
        float *gx, *gy;
        l = pc_gfx_win_lum(w);
        m = pc_malloc_array(N, sizeof *m);
        e2 = pc_malloc_array(N, sizeof *e2);
        if (l == NULL || m == NULL || e2 == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_blur1(l, W, H, plastic_sigma(e), err));
        gx = m, gy = e2;
        pc_gfx_sobel(l, W, H, gx, gy);
        pc_gfx_normalize3(lv0, lv);
        {
            const float v[3] = {lv[0], lv[1], lv[2] + 1.0f};
            pc_gfx_normalize3(v, hv);
        }
        for (i = 0; i < N; i++) {
            const float k = 14.0f, v[3] = {-k * gx[i], -k * gy[i], 1.0f};
            float nn[3], ndh, tilt, spec;
            pc_gfx_normalize3(v, nn);
            ndh = pc_rs_maxf_k(nn[0] * hv[0] + nn[1] * hv[1] + nn[2] * hv[2], 0.0f);
            tilt = 1.0f - nn[2];
            spec = powf(ndh, 30.0f) * pc_gfx_smoothstep(0.01f, 0.12f, tilt) +
                   pc_gfx_smoothstep(0.25f, 0.6f, tilt) * 0.25f;
            for (c = 0; c < 3; c++) {
                w->c[i].c[c] = w->c[i].c[c] * 0.88f + spec * hs * 1.3f;
            }
        }
        break;
    }
    case PC_ALGO_GAL_POSTER_EDGES: {
        float inten, levels;
        l = pc_gfx_win_lum(w);
        if (l == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_blur1(l, W, H, 0.6f + G("edgeThickness") * 0.6f, err));
        e2 = pc_gfx_grad_mag(l, W, H);
        if (e2 == NULL) {
            TRY(NOMEM());
        }
        inten = G("edgeIntensity") / 10.0f;
        levels = G("posterization") + 2.0f;
        for (i = 0; i < N; i++) {
            float ink =
                pc_gfx_smoothstep(0.015f, 0.06f, e2[i] * (1.0f + inten * 3.0f)) * (0.55f + inten * 0.45f);
            for (c = 0; c < 3; c++) {
                w->c[i].c[c] = pc_gfx_posterize(w->c[i].c[c], levels) * (1.0f - ink);
            }
        }
        break;
    }
    case PC_ALGO_GAL_ROUGH_PASTELS: {
        /* Chalk strokes up to the right; Stroke Detail keeps more of the image inside them. */
        float len = G("strokeLength"), det = (G("strokeDetail") - 1.0f) / 19.0f;
        if (len >= 1.0f) {
            gwin src;
            tmp = pc_gfx_clone(w);
            if (tmp == NULL) {
                TRY(NOMEM());
            }
            TRY(pc_gfx_blur3(tmp, W, H, 0.6f + (1.0f - det) * 1.2f, err));
            src = *w;
            src.c = tmp;
            for (i = 0; i < N; i++) {
                float t, nz;
                gpx p;
                pc_gfx_xy(w, i, &x, &y);
                p = pc_gfx_paint_stroke(&src, x, y, 1, -1, 2.0f + len, 1.5f, 51, &t);
                /* A chalk stroke is a little lighter in its middle, grainy along its length. */
                nz = (pc_gfx_strokes(x, y, 1, -1, 2.0f + len * 0.5f, 0.8f, 52) - 0.5f) * 0.12f +
                     (0.5f - fabsf(t - 0.5f)) * 0.06f;
                p = pc_gfx_mix(p, w->c[i], det * 0.6f);
                for (c = 0; c < 3; c++) {
                    w->c[i].c[c] = p.c[c] + nz;
                }
            }
        }
        pc_gfx_texturize(w, pc_algo_gallery_effect_choice(e, "texture", cb), G("scaling"), G("relief"),
                         pc_algo_gallery_effect_choice(e, "light", cb2),
                         pc_algo_gallery_effect_flag(e, "invert"), 1);
        break;
    }
    case PC_ALGO_GAL_SMUDGE_STICK: {
        float area, inten;
        TRY(pc_gfx_line_blur3(w->c, W, H, 1, 1, pc_f32_to_size(G("strokeLength") * 1.5f), err));
        area = G("highlightArea");
        inten = G("intensity");
        for (i = 0; i < N; i++) {
            float lv = pc_gfx_lum(w->c[i]);
            w->c[i] = pc_gfx_highlights(scale_px(w->c[i], 0.7f + 0.3f * lv), lv, area, inten);
        }
        break;
    }
    case PC_ALGO_GAL_SPONGE: {
        float def, sc;
        TRY(pc_gfx_kuwahara(w, 1 + pc_f32_to_size(G("brushSize")), err));
        def = G("definition") / 25.0f;
        sc = 1.5f + G("smoothness") * 0.5f;
        for (i = 0; i < N; i++) {
            float s, hole;
            pc_gfx_xy(w, i, &x, &y);
            s = pc_gfx_fbm((float)x / sc, (float)y / sc, 61);
            hole = pc_gfx_smoothstep(0.58f, 0.75f, s);
            for (c = 0; c < 3; c++) {
                w->c[i].c[c] = pc_gfx_clamp01((w->c[i].c[c] - 0.5f) * (1.0f + def * 0.6f) + 0.5f) *
                               (1.0f - hole * def * 0.4f);
            }
        }
        break;
    }
    case PC_ALGO_GAL_UNDERPAINTING: {
        float cov;
        tmp = pc_gfx_clone(w);
        if (tmp == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_blur3(w->c, W, H, 0.5f + G("brushSize") * 0.4f, err));
        for (i = 0; i < N; i++) {
            w->c[i] = pc_gfx_saturate(pc_gfx_mix(tmp[i], w->c[i], 0.8f), 1.15f);
        }
        cov = G("textureCoverage") / 40.0f;
        pc_gfx_texturize(w, pc_algo_gallery_effect_choice(e, "texture", cb), G("scaling"),
                         G("relief") * (0.5f + cov * 1.5f), pc_algo_gallery_effect_choice(e, "light", cb2),
                         pc_algo_gallery_effect_flag(e, "invert"), 0);
        break;
    }
    case PC_ALGO_GAL_WATERCOLOR: {
        float sh, amp;
        TRY(pc_gfx_kuwahara(w, watercolor_radius(e), err));
        sh = G("shadowIntensity") / 10.0f;
        l = pc_gfx_win_lum(w);
        e2 = l != NULL ? pc_gfx_grad_mag(l, W, H) : NULL;
        if (e2 == NULL) {
            TRY(NOMEM());
        }
        amp = (G("texture") - 1.0f) * 0.04f;
        for (i = 0; i < N; i++) {
            float nz, k;
            gpx p;
            pc_gfx_xy(w, i, &x, &y);
            nz = (pc_gfx_fbm((float)x / 3.0f, (float)y / 3.0f, 71) - 0.5f) * amp;
            k = 1.0f - pc_gfx_clamp01(e2[i] * (3.0f + sh * 8.0f)) * 0.6f;
            p = pc_gfx_saturate(w->c[i], 1.2f);
            for (c = 0; c < 3; c++) {
                w->c[i].c[c] = powf(pc_gfx_clamp01(p.c[c] * k + nz), 1.1f + sh * 0.6f);
            }
        }
        break;
    }
    case PC_ALGO_GAL_ACCENTED_EDGES: {
        float eb;
        l = pc_gfx_win_lum(w);
        if (l == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_blur1(l, W, H, 0.4f + G("smoothness") * 0.3f, err));
        e2 = pc_gfx_grad_mag(l, W, H);
        if (e2 == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_blur1(e2, W, H, G("edgeWidth") * 0.5f, err));
        eb = G("edgeBrightness") / 50.0f;
        for (i = 0; i < N; i++) {
            float k = pc_gfx_clamp01(e2[i] * 6.0f);
            for (c = 0; c < 3; c++) {
                float v = w->c[i].c[c];
                w->c[i].c[c] = eb >= 0.5f ? v + (1.0f - v) * k * (eb - 0.5f) * 2.0f
                                          : v * (1.0f - k * (0.5f - eb) * 2.0f);
            }
        }
        break;
    }
    case PC_ALGO_GAL_ANGLED_STROKES: {
        /* Light areas are painted in strokes going one way, dark areas the other way. */
        float len = pc_rs_maxf_k(G("strokeLength"), 3.0f), bal = G("directionBalance") / 100.0f;
        gwin src;
        tmp = pc_gfx_clone(w);
        if (tmp == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_blur3(tmp, W, H, 0.8f, err));
        src = *w;
        src.c = tmp;
        l = pc_gfx_win_lum(&src);
        if (l == NULL) {
            TRY(NOMEM());
        }
        for (i = 0; i < N; i++) {
            float k, t, nz;
            gpx down, up, p;
            pc_gfx_xy(w, i, &x, &y);
            k = pc_gfx_smoothstep(1.0f - bal - 0.05f, 1.0f - bal + 0.05f, l[i]);
            down = pc_gfx_paint_stroke(&src, x, y, 1, 1, len, 2.0f, 83, &t);
            up = pc_gfx_paint_stroke(&src, x, y, 1, -1, len, 2.0f, 84, &t);
            nz = (pc_gfx_strokes(x, y, 1, 1, len * 0.5f, 0.7f, 85) - 0.5f) * (1.0f - k) +
                 (pc_gfx_strokes(x, y, 1, -1, len * 0.5f, 0.7f, 86) - 0.5f) * k;
            p = pc_gfx_mix(down, up, k);
            for (c = 0; c < 3; c++) {
                w->c[i].c[c] = p.c[c] + nz * 0.1f;
            }
        }
        TRY(pc_gfx_unsharp(w, 1.0f, 0.2f + G("sharpness") / 10.0f * 2.3f, err));
        break;
    }
    case PC_ALGO_GAL_CROSSHATCH: {
        size_t r = pc_f32_to_size(G("strokeLength") / 3.0f);
        float st_ = G("strength");
        for (i = 0; i < N; i++) {
            float nz;
            pc_gfx_xy(w, i, &x, &y);
            nz = (pc_algo_hash01(x, y, 0, 81) - 0.5f) * 0.12f * st_;
            for (c = 0; c < 3; c++) {
                w->c[i].c[c] = w->c[i].c[c] + nz;
            }
        }
        tmp = pc_gfx_clone(w);
        if (tmp == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_line_blur3(tmp, W, H, 1, 1, r, err));
        TRY(pc_gfx_line_blur3(w->c, W, H, 1, -1, r, err));
        for (i = 0; i < N; i++) {
            w->c[i] = pc_gfx_mix(tmp[i], w->c[i], 0.5f);
        }
        TRY(pc_gfx_unsharp(w, 1.0f, G("sharpness") / 20.0f * 3.0f, err));
        break;
    }
    case PC_ALGO_GAL_DARK_STROKES: {
        float bal = G("balance") / 10.0f, bi = G("blackIntensity") / 10.0f, wi = G("whiteIntensity") / 10.0f,
              t;
        l = pc_gfx_win_lum(w);
        tmp = pc_gfx_clone(w);
        if (l == NULL || tmp == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_line_blur3(tmp, W, H, 1, 1, 3, err));
        TRY(pc_gfx_line_blur3(w->c, W, H, 1, -1, 8, err));
        t = 0.25f + bal * 0.5f;
        for (i = 0; i < N; i++) {
            gpx d = scale_px(tmp[i], 1.0f - bi * 0.85f), li;
            for (c = 0; c < 3; c++) {
                li.c[c] = w->c[i].c[c] + (1.0f - w->c[i].c[c]) * wi * 0.5f;
            }
            w->c[i] = pc_gfx_mix(d, li, pc_gfx_smoothstep(t - 0.1f, t + 0.1f, l[i]));
        }
        break;
    }
    case PC_ALGO_GAL_INK_OUTLINES: {
        float di = G("darkIntensity") / 50.0f, li = G("lightIntensity") / 50.0f;
        l = pc_gfx_win_lum(w);
        e2 = l != NULL ? pc_gfx_grad_mag(l, W, H) : NULL;
        if (e2 == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_line_blur3(w->c, W, H, 1, 1, pc_f32_to_size(G("strokeLength") / 2.0f), err));
        for (i = 0; i < N; i++) {
            float ink = pc_gfx_clamp01(e2[i] * 5.0f) * di * 1.5f;
            float lt = pc_gfx_smoothstep(0.5f, 1.0f, l[i]) * li,
                  dk = pc_gfx_smoothstep(0.5f, 0.0f, l[i]) * di * 0.8f;
            for (c = 0; c < 3; c++) {
                float v = w->c[i].c[c];
                w->c[i].c[c] = (v + (1.0f - v) * lt) * (1.0f - dk) * (1.0f - pc_gfx_clamp01(ink));
            }
        }
        break;
    }
    case PC_ALGO_GAL_SPATTER: {
        /* Smoothness smooths the random displacement: a larger feature size and, as with any
         * averaged noise, a smaller reach. Low smoothness sprays specks out to the full radius;
         * high smoothness leaves a torn but attached, hard edge. */
        float rad = G("sprayRadius");
        float feature = 1.0f + G("smoothness") * 0.35f;
        float reach = pc_rs_minf(rad * 1.2f / feature, rad);
        gwin src;
        tmp = pc_gfx_clone(w);
        if (tmp == NULL) {
            TRY(NOMEM());
        }
        src = *w;
        src.c = tmp;
        for (i = 0; i < N; i++) {
            float fx, fy, dx, dy;
            pc_gfx_xy(w, i, &x, &y);
            fx = (float)x / feature;
            fy = (float)y / feature;
            dx = (pc_gfx_fbm(fx, fy, 91) - 0.5f) * 3.0f * reach;
            dy = (pc_gfx_fbm(fx, fy, 92) - 0.5f) * 3.0f * reach;
            w->c[i] = pc_gfx_sample(&src, (float)x + 0.5f + dx, (float)y + 0.5f + dy);
        }
        break;
    }
    case PC_ALGO_GAL_SPRAYED_STROKES: {
        float rad = G("sprayRadius");
        float sc = 1.2f;
        int32_t ddx, ddy;
        gwin src;
        tmp = pc_gfx_clone(w);
        if (tmp == NULL) {
            TRY(NOMEM());
        }
        src = *w;
        src.c = tmp;
        for (i = 0; i < N; i++) {
            float fx, fy, dx, dy;
            pc_gfx_xy(w, i, &x, &y);
            fx = (float)x / sc;
            fy = (float)y / sc;
            dx = (pc_gfx_vnoise(fx, fy, 91) - 0.5f) * 2.0f * rad;
            dy = (pc_gfx_vnoise(fx, fy, 92) - 0.5f) * 2.0f * rad;
            w->c[i] = pc_gfx_sample(&src, (float)x + 0.5f + dx, (float)y + 0.5f + dy);
        }
        pc_gfx_direction(pc_algo_gallery_effect_choice(e, "strokeDirection", cb), &ddx, &ddy);
        TRY(pc_gfx_line_blur3(w->c, W, H, ddx, ddy, pc_f32_to_size(G("strokeLength") / 2.0f), err));
        break;
    }
    case PC_ALGO_GAL_SUMI_E: {
        float sw = G("strokeWidth"), pr = G("strokePressure") / 15.0f, con = G("contrast") / 40.0f;
        TRY(pc_gfx_blur3(w->c, W, H, sw * 0.2f, err));
        TRY(pc_gfx_line_blur3(w->c, W, H, 1, 1, pc_f32_to_size(sw / 2.0f), err));
        for (i = 0; i < N; i++) {
            float lv = pc_gfx_lum(w->c[i]), ink = powf(pc_gfx_smoothstep(0.65f, 0.15f, lv), 1.0f - pr * 0.5f);
            for (c = 0; c < 3; c++) {
                w->c[i].c[c] = pc_gfx_clamp01((w->c[i].c[c] - 0.5f) * (1.0f + con) + 0.5f) *
                               (1.0f - ink * 0.9f * (0.5f + con));
            }
        }
        break;
    }
    default:
        *handled = false;
        break;
    }
done:
    free(l);
    free(m);
    free(e2);
    free(tmp);
    return st;
}
