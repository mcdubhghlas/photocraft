/* Pixel kernels of the SVG filters (port of resvg 0.45's filter/{box_blur, iir_blur,
 * color_matrix, component_transfer, composite, convolve_matrix, morphology, displacement_map,
 * turbulence}.rs). f32 / f64 exactly where resvg has them; Rust's saturating `as` casts and the
 * release build's wrapping integer arithmetic are spelled out.
 *
 * Work caps (hostile input; the reference has none):
 *   - morphology is computed separably with sliding windows: O(pixels) for any radius, results
 *     identical to resvg's O(pixels * radius^2) loop;
 *   - convolve matrix: pixels * kernel size above CONVOLVE_MAX_WORK fails the filter;
 *   - turbulence: at most TURB_STATIONARY octaves are evaluated (past them every further octave
 *     adds the same +-0 or NaN, so the result is exact), and pixels * 4 * octaves above
 *     TURB_MAX_WORK fails the filter. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "io/svg_filter.h"

#define CONVOLVE_MAX_WORK ((uint64_t)1 << 31)
#define TURB_STATIONARY 2100u
#define TURB_MAX_WORK ((uint64_t)1 << 30)

/* ---------------------------------------------------------------- box blur */

#define BOX_STEPS 5

static void create_box_gauss(float sigma, int32_t sizes[BOX_STEPS])
{
    int i;
    if (sigma > 0.0f) {
        float n = (float)BOX_STEPS;
        float w_ideal = sqrtf(12.0f * sigma * sigma / n) + 1.0f;
        int32_t wl = pc_f32_to_i32(floorf(w_ideal));
        int32_t wu;
        float wlf;
        float m_ideal;
        size_t m;
        if (wl % 2 == 0) {
            wl -= 1;
        }
        wu = pc_i32_wrapping_add(wl, 2);
        wlf = (float)wl;
        m_ideal = (12.0f * sigma * sigma - n * wlf * wlf - 4.0f * n * wlf - 3.0f * n) / (-4.0f * wlf - 4.0f);
        m = pc_f32_to_size(roundf(m_ideal));
        for (i = 0; i < BOX_STEPS; i++) {
            sizes[i] = (size_t)i < m ? wl : wu;
        }
    } else {
        for (i = 0; i < BOX_STEPS; i++) {
            sizes[i] = 1;
        }
    }
}

static float box_round(float x)
{
    volatile float v = x + 12582912.0f; /* keeps the two roundings */
    return v - 12582912.0f;
}

/* One direction of box_blur_{vert,horz}: `lines` lines of `len` pixels; pixel k of line i is at
 * i * lstep + k * step. br is the radius as Rust's usize (possibly a wrapped negative). */
static void blur_lines(uint64_t br, const uint8_t *in, uint8_t *out, size_t lines, size_t len, size_t lstep,
                       size_t step)
{
    float iarr;
    size_t i;
    if (br == 0) {
        memcpy(out, in, lines * len * 4);
        return;
    }
    iarr = 1.0f / (float)(br + br + 1);
    for (i = 0; i < lines; i++) {
        const uint8_t *src = in + i * lstep * 4;
        uint8_t *dst = out + i * lstep * 4;
        int64_t val[4] = {0, 0, 0, 0};
        uint64_t ti = 0;
        uint64_t li = 0;
        uint64_t ri = br;
        uint64_t j;
        uint64_t lim;
        int c;
        lim = br < len ? br : len;
        for (j = 0; j < lim; j++) {
            for (c = 0; c < 4; c++) {
                val[c] += src[j * step * 4 + (size_t)c];
            }
        }
        lim = br + 1 < len ? br + 1 : len;
        for (j = 0; j < lim; j++) {
            for (c = 0; c < 4; c++) {
                if (ri < len) {
                    val[c] += src[ri * step * 4 + (size_t)c];
                }
                dst[ti * step * 4 + (size_t)c] = pc_f32_to_u8(box_round((float)val[c] * iarr));
            }
            ri++;
            ti++;
        }
        if (len <= br) {
            continue;
        }
        for (j = br + 1; j < len - br; j++) {
            for (c = 0; c < 4; c++) {
                val[c] += (int64_t)src[ri * step * 4 + (size_t)c] - (int64_t)src[li * step * 4 + (size_t)c];
                dst[ti * step * 4 + (size_t)c] = pc_f32_to_u8(box_round((float)val[c] * iarr));
            }
            ri++;
            li++;
            ti++;
        }
        lim = len - br - 1 < br ? len - br - 1 : br;
        for (j = 0; j < lim; j++) {
            for (c = 0; c < 4; c++) {
                val[c] -= src[li * step * 4 + (size_t)c];
                dst[ti * step * 4 + (size_t)c] = pc_f32_to_u8(box_round((float)val[c] * iarr));
            }
            li++;
            ti++;
        }
    }
}

bool sv_fx_box_blur(double sx, double sy, uint8_t *px, uint32_t w, uint32_t h)
{
    int32_t bh[BOX_STEPS];
    int32_t bv[BOX_STEPS];
    size_t n = (size_t)w * h * 4;
    uint8_t *back;
    int i;
    if (n == 0) {
        return true;
    }
    back = calloc(n, 1);
    if (back == NULL) {
        return false;
    }
    memcpy(back, px, n);
    create_box_gauss((float)sx, bh);
    create_box_gauss((float)sy, bv);
    for (i = 0; i < BOX_STEPS; i++) {
        /* ((box - 1) / 2) as usize: i32 arithmetic, then a sign-extending cast */
        uint64_t rh = (uint64_t)(int64_t)(pc_i32_wrapping_sub(bh[i], 1) / 2);
        uint64_t rv = (uint64_t)(int64_t)(pc_i32_wrapping_sub(bv[i], 1) / 2);
        blur_lines(rv, px, back, w, h, 1, w);
        blur_lines(rh, back, px, h, w, w, 1);
    }
    free(back);
    return true;
}

/* ---------------------------------------------------------------- IIR blur */

static void gen_coefficients(double sigma, double *lambda, double *dnu)
{
    *lambda = (sigma * sigma) / (2.0 * 4.0);
    *dnu = (1.0 + 2.0 * *lambda - sqrt(1.0 + 4.0 * *lambda)) / (2.0 * *lambda);
}

static void gaussianiir2d(double sx, double sy, size_t w, size_t h, double *buf)
{
    size_t len = w * h;
    double lx = 1.0;
    double dx = 1.0;
    double ly = 1.0;
    double dy = 1.0;
    double ps;
    double p2;
    double p4;
    size_t i;
    int s;
    if (sx > 0.0) {
        size_t y;
        gen_coefficients(sx, &lx, &dx);
        for (y = 0; y < h; y++) {
            for (s = 0; s < 4; s++) {
                size_t idx = w * y;
                size_t x;
                for (x = 1; x < w; x++) {
                    buf[idx + x] += dx * buf[idx + x - 1];
                }
                for (x = w - 1; x > 0; x--) {
                    buf[idx + x - 1] += dx * buf[idx + x];
                }
            }
        }
    }
    if (sy > 0.0) {
        size_t x;
        gen_coefficients(sy, &ly, &dy);
        for (x = 0; x < w; x++) {
            for (s = 0; s < 4; s++) {
                size_t y;
                for (y = w; y < len; y += w) {
                    buf[x + y] += dy * buf[x + y - w];
                }
                for (y = len - w; y > 0; y -= w) {
                    buf[x + y - w] += dy * buf[x + y];
                }
            }
        }
    }
    /* (..).powi(8): repeated squaring, as LLVM expands it */
    ps = sqrt(dx * dy) / sqrt(lx * ly);
    p2 = ps * ps;
    p4 = p2 * p2;
    ps = p4 * p4;
    for (i = 0; i < len; i++) {
        buf[i] *= ps;
    }
}

bool sv_fx_iir_blur(double sx, double sy, uint8_t *px, uint32_t w, uint32_t h)
{
    size_t n = (size_t)w * h;
    double *buf;
    int c;
    if (n == 0) {
        return true;
    }
    buf = calloc(n, sizeof(double));
    if (buf == NULL) {
        return false;
    }
    for (c = 0; c < 4; c++) {
        size_t i;
        for (i = 0; i < n; i++) {
            buf[i] = (double)px[i * 4 + (size_t)c] / 255.0;
        }
        gaussianiir2d(sx, sy, w, h, buf);
        for (i = 0; i < n; i++) {
            px[i * 4 + (size_t)c] = pc_f64_to_u8(buf[i] * 255.0);
        }
    }
    free(buf);
    return true;
}

/* ---------------------------------------------------------------- colour matrix, transfer */

static uint8_t from_normalized(float c)
{
    return pc_f32_to_u8(sv_f32_bound(0.0f, c, 1.0f) * 255.0f);
}

static void matrix3(const float m[9], uint8_t *px, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t *p = px + i * 4;
        float r = (float)p[0] / 255.0f;
        float g = (float)p[1] / 255.0f;
        float b = (float)p[2] / 255.0f;
        p[0] = from_normalized(r * m[0] + g * m[1] + b * m[2]);
        p[1] = from_normalized(r * m[3] + g * m[4] + b * m[5]);
        p[2] = from_normalized(r * m[6] + g * m[7] + b * m[8]);
    }
}

void sv_fx_color_matrix(const sv_fprim *fe, uint8_t *px, size_t n)
{
    size_t i;
    switch (fe->cm) {
    case CM_MATRIX: {
        const float *m = fe->m;
        for (i = 0; i < n; i++) {
            uint8_t *p = px + i * 4;
            float r = (float)p[0] / 255.0f;
            float g = (float)p[1] / 255.0f;
            float b = (float)p[2] / 255.0f;
            float a = (float)p[3] / 255.0f;
            p[0] = from_normalized(r * m[0] + g * m[1] + b * m[2] + a * m[3] + m[4]);
            p[1] = from_normalized(r * m[5] + g * m[6] + b * m[7] + a * m[8] + m[9]);
            p[2] = from_normalized(r * m[10] + g * m[11] + b * m[12] + a * m[13] + m[14]);
            p[3] = from_normalized(r * m[15] + g * m[16] + b * m[17] + a * m[18] + m[19]);
        }
        break;
    }
    case CM_SATURATE: {
        float v = fe->cm_value > 0.0f ? fe->cm_value : 0.0f; /* v.max(0.0): v is never NaN */
        float m[9];
        m[0] = 0.213f + 0.787f * v;
        m[1] = 0.715f - 0.715f * v;
        m[2] = 0.072f - 0.072f * v;
        m[3] = 0.213f - 0.213f * v;
        m[4] = 0.715f + 0.285f * v;
        m[5] = 0.072f - 0.072f * v;
        m[6] = 0.213f - 0.213f * v;
        m[7] = 0.715f - 0.715f * v;
        m[8] = 0.072f + 0.928f * v;
        matrix3(m, px, n);
        break;
    }
    case CM_HUE_ROTATE: {
        float angle = fe->cm_value * (3.14159265358979323846f / 180.0f);
        float a1 = cosf(angle);
        float a2 = sinf(angle);
        float m[9];
        m[0] = 0.213f + 0.787f * a1 - 0.213f * a2;
        m[1] = 0.715f - 0.715f * a1 - 0.715f * a2;
        m[2] = 0.072f - 0.072f * a1 + 0.928f * a2;
        m[3] = 0.213f - 0.213f * a1 + 0.143f * a2;
        m[4] = 0.715f + 0.285f * a1 + 0.140f * a2;
        m[5] = 0.072f - 0.072f * a1 - 0.283f * a2;
        m[6] = 0.213f - 0.213f * a1 - 0.787f * a2;
        m[7] = 0.715f - 0.715f * a1 + 0.715f * a2;
        m[8] = 0.072f + 0.928f * a1 + 0.072f * a2;
        matrix3(m, px, n);
        break;
    }
    case CM_LUMINANCE_TO_ALPHA:
    default:
        for (i = 0; i < n; i++) {
            uint8_t *p = px + i * 4;
            float r = (float)p[0] / 255.0f;
            float g = (float)p[1] / 255.0f;
            float b = (float)p[2] / 255.0f;
            p[0] = p[1] = p[2] = 0;
            p[3] = from_normalized(r * 0.2125f + g * 0.7154f + b * 0.0721f);
        }
        break;
    }
}

static bool tf_dummy(const sv_tfunc *f)
{
    return f->kind == TF_IDENTITY || ((f->kind == TF_TABLE || f->kind == TF_DISCRETE) && f->n == 0);
}

static uint8_t transfer(const sv_tfunc *f, uint8_t v)
{
    float c = (float)v / 255.0f;
    switch (f->kind) {
    case TF_TABLE: {
        size_t n = f->n - 1;
        size_t k = pc_f32_to_size(floorf(c * (float)n));
        if (k > n) {
            k = n;
        }
        if (k == n) {
            c = f->v[k];
        } else {
            float vk = f->v[k];
            float vk1 = f->v[k + 1];
            float kf = (float)k;
            float nf = (float)n;
            c = vk + (c - kf / nf) * nf * (vk1 - vk);
        }
        break;
    }
    case TF_DISCRETE: {
        size_t n = f->n;
        size_t k = pc_f32_to_size(floorf(c * (float)n));
        c = f->v[k < n - 1 ? k : n - 1];
        break;
    }
    case TF_LINEAR:
        c = f->slope * c + f->intercept;
        break;
    case TF_GAMMA:
        c = f->amplitude * powf(c, f->exponent) + f->offset;
        break;
    case TF_IDENTITY:
    default:
        break;
    }
    return pc_f32_to_u8(sv_f32_bound(0.0f, c, 1.0f) * 255.0f);
}

void sv_fx_component_transfer(const sv_fprim *fe, uint8_t *px, size_t n)
{
    /* resvg's channel order: r, b, g, a (independent) */
    static const int ORDER[4] = {0, 2, 1, 3};
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        uint8_t *p = px + i * 4;
        for (k = 0; k < 4; k++) {
            int ch = ORDER[k];
            if (!tf_dummy(&fe->func[ch])) {
                p[ch] = transfer(&fe->func[ch], p[ch]);
            }
        }
    }
}

/* ---------------------------------------------------------------- composite arithmetic */

static float arith(float k1, float k2, float k3, float k4, uint8_t a, uint8_t b, float max)
{
    float i1 = (float)a / 255.0f;
    float i2 = (float)b / 255.0f;
    float r = k1 * i1 * i2 + k2 * i1 + k3 * i2 + k4;
    return sv_f32_bound(0.0f, r, max);
}

void sv_fx_arithmetic(float k1, float k2, float k3, float k4, const uint8_t *a, const uint8_t *b,
                      uint8_t *dst, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        const uint8_t *c1 = a + i * 4;
        const uint8_t *c2 = b + i * 4;
        float al = arith(k1, k2, k3, k4, c1[3], c2[3], 1.0f);
        if (sv_approx_eq_ulps(al, 0.0f, 4)) {
            continue;
        }
        dst[i * 4] = pc_f32_to_u8(arith(k1, k2, k3, k4, c1[0], c2[0], al) * 255.0f);
        dst[i * 4 + 1] = pc_f32_to_u8(arith(k1, k2, k3, k4, c1[1], c2[1], al) * 255.0f);
        dst[i * 4 + 2] = pc_f32_to_u8(arith(k1, k2, k3, k4, c1[2], c2[2], al) * 255.0f);
        dst[i * 4 + 3] = pc_f32_to_u8(al * 255.0f);
    }
}

/* ---------------------------------------------------------------- convolve matrix */

bool sv_fx_convolve(const sv_fprim *fe, uint8_t *px, uint32_t w, uint32_t h)
{
    size_t n = (size_t)w * h;
    uint8_t *buf;
    int64_t wmax = (int64_t)w - 1;
    int64_t hmax = (int64_t)h - 1;
    float div = fe->divisor;
    float bias = fe->bias;
    uint32_t x;
    uint32_t y;
    if ((uint64_t)fe->columns * fe->rows > CONVOLVE_MAX_WORK / (n > 0 ? n : 1)) {
        return false;
    }
    buf = calloc(n > 0 ? n : 1, 4);
    if (buf == NULL) {
        return false;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const uint8_t *in = px + ((size_t)y * w + x) * 4;
            uint8_t *o = buf + ((size_t)y * w + x) * 4;
            float nr = 0.0f;
            float ng = 0.0f;
            float nb = 0.0f;
            float na = 0.0f;
            float bounded;
            float v[3];
            uint32_t oy;
            uint32_t ox;
            int k;
            for (oy = 0; oy < fe->rows; oy++) {
                for (ox = 0; ox < fe->columns; ox++) {
                    int64_t tx = (int64_t)x - (int64_t)fe->target_x + (int64_t)ox;
                    int64_t ty = (int64_t)y - (int64_t)fe->target_y + (int64_t)oy;
                    float kv;
                    const uint8_t *p;
                    if (fe->edge == EDGE_NONE) {
                        if (tx < 0 || tx > wmax || ty < 0 || ty > hmax) {
                            continue;
                        }
                    } else if (fe->edge == EDGE_DUPLICATE) {
                        tx = tx < 0 ? 0 : tx > wmax ? wmax : tx;
                        ty = ty < 0 ? 0 : ty > hmax ? hmax : ty;
                    } else {
                        tx = ((tx % (int64_t)w) + (int64_t)w) % (int64_t)w;
                        ty = ((ty % (int64_t)h) + (int64_t)h) % (int64_t)h;
                    }
                    kv = fe->matrix[(size_t)(fe->rows - oy - 1) * fe->columns + (fe->columns - ox - 1)];
                    p = px + ((size_t)ty * w + (size_t)tx) * 4;
                    nr += (float)p[0] / 255.0f * kv;
                    ng += (float)p[1] / 255.0f * kv;
                    nb += (float)p[2] / 255.0f * kv;
                    if (!fe->preserve_alpha) {
                        na += (float)p[3] / 255.0f * kv;
                    }
                }
            }
            if (fe->preserve_alpha) {
                na = (float)in[3] / 255.0f;
            } else {
                na = na / div + bias;
            }
            bounded = sv_f32_bound(0.0f, na, 1.0f);
            v[0] = nr;
            v[1] = ng;
            v[2] = nb;
            for (k = 0; k < 3; k++) {
                float c = v[k] / div + bias * na;
                c = fe->preserve_alpha ? sv_f32_bound(0.0f, c, 1.0f) * bounded
                                       : sv_f32_bound(0.0f, c, bounded);
                o[k] = pc_f32_to_u8(c * 255.0f + 0.5f);
            }
            o[3] = pc_f32_to_u8(bounded * 255.0f + 0.5f);
        }
    }
    memcpy(px, buf, n * 4);
    free(buf);
    return true;
}

/* ---------------------------------------------------------------- morphology */

/* Sliding min / max over windows [k - t, k - t + span - 1] clipped to the line (monotonic deque),
 * for each of the 4 channels: equal to resvg's loop over the window, which skips pixels outside
 * the image. */
static void morph_lines(bool dilate, uint64_t span, uint64_t t, const uint8_t *in, uint8_t *out, size_t lines,
                        size_t len, size_t lstep, size_t step, size_t *dq)
{
    size_t i;
    int c;
    for (i = 0; i < lines; i++) {
        const uint8_t *src = in + i * lstep * 4;
        uint8_t *dst = out + i * lstep * 4;
        for (c = 0; c < 4; c++) {
            size_t head = 0;
            size_t tail = 0;
            size_t next = 0; /* next index to push */
            size_t k;
            for (k = 0; k < len; k++) {
                int64_t lo = (int64_t)k - (int64_t)t;
                int64_t hi = lo + (int64_t)span - 1;
                if (hi >= (int64_t)len) {
                    hi = (int64_t)len - 1;
                }
                while ((int64_t)next <= hi) {
                    uint8_t v = src[next * step * 4 + (size_t)c];
                    while (tail > head) {
                        uint8_t u = src[dq[tail - 1] * step * 4 + (size_t)c];
                        if (dilate ? u <= v : u >= v) {
                            tail--;
                        } else {
                            break;
                        }
                    }
                    dq[tail++] = next;
                    next++;
                }
                while (head < tail && (int64_t)dq[head] < lo) {
                    head++;
                }
                dst[k * step * 4 + (size_t)c] = src[dq[head] * step * 4 + (size_t)c];
            }
        }
    }
}

bool sv_fx_morphology(bool dilate, float rx, float ry, uint8_t *px, uint32_t w, uint32_t h)
{
    /* (rx.ceil() as u32 * 2): a u32 product that wraps */
    uint32_t cols = pc_f32_to_u32(ceilf(rx)) * 2u;
    uint32_t rows = pc_f32_to_u32(ceilf(ry)) * 2u;
    uint32_t tx;
    uint32_t ty;
    size_t n = (size_t)w * h;
    uint8_t *tmp;
    size_t *dq;
    cols = cols < w ? cols : w;
    rows = rows < h ? rows : h;
    if (cols == 0 || rows == 0) {
        /* an empty window leaves resvg's start value */
        memset(px, dilate ? 0 : 255, n * 4);
        return true;
    }
    tx = pc_f32_to_u32(floorf((float)cols / 2.0f));
    ty = pc_f32_to_u32(floorf((float)rows / 2.0f));
    tmp = calloc(n, 4);
    dq = calloc(w > h ? w : h, sizeof(size_t));
    if (tmp == NULL || dq == NULL) {
        free(tmp);
        free(dq);
        return false;
    }
    morph_lines(dilate, cols, tx, px, tmp, h, w, w, 1, dq);
    morph_lines(dilate, rows, ty, tmp, px, w, h, 1, w, dq);
    free(tmp);
    free(dq);
    return true;
}

/* ---------------------------------------------------------------- displacement map */

void sv_fx_displacement(const sv_fprim *fe, float sx, float sy, const uint8_t *src, const uint8_t *map,
                        uint8_t *dst, uint32_t w, uint32_t h)
{
    uint32_t x;
    uint32_t y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const uint8_t *p = map + ((size_t)y * w + x) * 4;
            float dx = (float)p[fe->xch == CH_R   ? 0
                                : fe->xch == CH_G ? 1
                                : fe->xch == CH_B ? 2
                                                  : 3] /
                           255.0f -
                       0.5f;
            float dy = (float)p[fe->ych == CH_R   ? 0
                                : fe->ych == CH_G ? 1
                                : fe->ych == CH_B ? 2
                                                  : 3] /
                           255.0f -
                       0.5f;
            int32_t ox = pc_f32_to_i32(roundf((float)x + dx * sx * fe->scale));
            int32_t oy = pc_f32_to_i32(roundf((float)y + dy * sy * fe->scale));
            if (ox >= 0 && (uint32_t)ox < w && oy >= 0 && (uint32_t)oy < h) {
                memcpy(dst + ((size_t)y * w + x) * 4, src + ((size_t)oy * w + (size_t)ox) * 4, 4);
            }
        }
    }
}

/* ---------------------------------------------------------------- turbulence */

#define RAND_M 2147483647
#define RAND_A 16807
#define RAND_Q 127773
#define RAND_R 2836
#define B_SIZE 256
#define B_LEN (B_SIZE + B_SIZE + 2)
#define BM 0xff
#define PERLIN_N 4096

typedef struct turb {
    size_t lattice[B_LEN];
    double grad[4][B_LEN][2];
} turb;

typedef struct stitch {
    int32_t width, height, wrap_x, wrap_y;
} stitch;

static int32_t trandom(int32_t seed)
{
    int32_t r = RAND_A * (seed % RAND_Q) - RAND_R * (seed / RAND_Q);
    if (r <= 0) {
        r += RAND_M;
    }
    return r;
}

static void turb_init(turb *t, int32_t seed)
{
    int k;
    int i;
    int j;
    if (seed <= 0) {
        int32_t neg = pc_i32_wrapping_sub(0, seed);
        seed = neg % (RAND_M - 1) + 1;
    }
    if (seed > RAND_M - 1) {
        seed = RAND_M - 1;
    }
    memset(t, 0, sizeof *t);
    for (k = 0; k < 4; k++) {
        for (i = 0; i < B_SIZE; i++) {
            double s;
            t->lattice[i] = (size_t)i;
            for (j = 0; j < 2; j++) {
                seed = trandom(seed);
                t->grad[k][i][j] = (double)((seed % (B_SIZE + B_SIZE)) - B_SIZE) / (double)B_SIZE;
            }
            s = sqrt(t->grad[k][i][0] * t->grad[k][i][0] + t->grad[k][i][1] * t->grad[k][i][1]);
            t->grad[k][i][0] /= s;
            t->grad[k][i][1] /= s;
        }
    }
    for (i = B_SIZE - 1; i >= 1; i--) {
        size_t kk = t->lattice[i];
        size_t jj;
        seed = trandom(seed);
        jj = (size_t)(seed % B_SIZE);
        t->lattice[i] = t->lattice[jj];
        t->lattice[jj] = kk;
    }
    for (i = 0; i < B_SIZE + 2; i++) {
        t->lattice[B_SIZE + i] = t->lattice[i];
        for (k = 0; k < 4; k++) {
            for (j = 0; j < 2; j++) {
                t->grad[k][B_SIZE + i][j] = t->grad[k][i][j];
            }
        }
    }
}

static double s_curve(double t)
{
    return t * t * (3.0 - 2.0 * t);
}

static double lerp(double t, double a, double b)
{
    return a + t * (b - a);
}

static double noise2(const turb *t, int ch, double x, double y, const stitch *st)
{
    double tt = x + (double)PERLIN_N;
    int32_t bx0 = pc_f64_to_i32(tt);
    int32_t bx1 = pc_i32_wrapping_add(bx0, 1);
    double rx0 = tt - (double)pc_f64_to_i64(tt);
    double rx1 = rx0 - 1.0;
    int32_t by0;
    int32_t by1;
    double ry0;
    double ry1;
    size_t i;
    size_t j;
    size_t b00;
    size_t b10;
    size_t b01;
    size_t b11;
    double sx;
    double sy;
    const double *q;
    double u;
    double v;
    double a;
    double b;
    tt = y + (double)PERLIN_N;
    by0 = pc_f64_to_i32(tt);
    by1 = pc_i32_wrapping_add(by0, 1);
    ry0 = tt - (double)pc_f64_to_i64(tt);
    ry1 = ry0 - 1.0;
    if (st != NULL) {
        if (bx0 >= st->wrap_x) {
            bx0 = pc_i32_wrapping_sub(bx0, st->width);
        }
        if (bx1 >= st->wrap_x) {
            bx1 = pc_i32_wrapping_sub(bx1, st->width);
        }
        if (by0 >= st->wrap_y) {
            by0 = pc_i32_wrapping_sub(by0, st->height);
        }
        if (by1 >= st->wrap_y) {
            by1 = pc_i32_wrapping_sub(by1, st->height);
        }
    }
    bx0 &= BM;
    bx1 &= BM;
    by0 &= BM;
    by1 &= BM;
    i = t->lattice[bx0];
    j = t->lattice[bx1];
    b00 = t->lattice[i + (size_t)by0];
    b10 = t->lattice[j + (size_t)by0];
    b01 = t->lattice[i + (size_t)by1];
    b11 = t->lattice[j + (size_t)by1];
    sx = s_curve(rx0);
    sy = s_curve(ry0);
    q = t->grad[ch][b00];
    u = rx0 * q[0] + ry0 * q[1];
    q = t->grad[ch][b10];
    v = rx1 * q[0] + ry0 * q[1];
    a = lerp(sx, u, v);
    q = t->grad[ch][b01];
    u = rx0 * q[0] + ry1 * q[1];
    q = t->grad[ch][b11];
    v = rx1 * q[0] + ry1 * q[1];
    b = lerp(sx, u, v);
    return lerp(sy, a, b);
}

static double turbulence(const turb *t, int ch, double x, double y, double tile_x, double tile_y,
                         double tile_w, double tile_h, double bfx, double bfy, uint32_t octaves, bool fractal,
                         bool do_stitch)
{
    stitch st;
    double sum = 0.0;
    double ratio = 1.0;
    uint32_t o;
    if (do_stitch) {
        if (!(bfx == 0.0)) { /* approx_zero_ulps(4) on an f64 that came from an f32 */
            double lo = floor(tile_w * bfx) / tile_w;
            double hi = ceil(tile_w * bfx) / tile_w;
            bfx = bfx / lo < hi / bfx ? lo : hi;
        }
        if (!(bfy == 0.0)) {
            double lo = floor(tile_h * bfy) / tile_h;
            double hi = ceil(tile_h * bfy) / tile_h;
            bfy = bfy / lo < hi / bfy ? lo : hi;
        }
        st.width = pc_f64_to_i32(tile_w * bfx + 0.5);
        st.height = pc_f64_to_i32(tile_h * bfy + 0.5);
        st.wrap_x = pc_f64_to_i32(tile_x * bfx + (double)PERLIN_N + (double)st.width);
        st.wrap_y = pc_f64_to_i32(tile_y * bfy + (double)PERLIN_N + (double)st.height);
    }
    x *= bfx;
    y *= bfy;
    for (o = 0; o < octaves; o++) {
        double nz = noise2(t, ch, x, y, do_stitch ? &st : NULL);
        sum += fractal ? nz / ratio : fabs(nz) / ratio;
        if (isnan(sum)) {
            break; /* NaN stays NaN */
        }
        x *= 2.0;
        y *= 2.0;
        ratio *= 2.0;
        if (do_stitch) {
            st.width = pc_i32_wrapping_mul(st.width, 2);
            st.wrap_x = pc_i32_wrapping_sub(pc_i32_wrapping_mul(st.wrap_x, 2), PERLIN_N);
            st.height = pc_i32_wrapping_mul(st.height, 2);
            st.wrap_y = pc_i32_wrapping_sub(pc_i32_wrapping_mul(st.wrap_y, 2), PERLIN_N);
        }
    }
    return sum;
}

bool sv_fx_turbulence(const sv_fprim *fe, double off_x, double off_y, double sx, double sy, uint8_t *px,
                      uint32_t w, uint32_t h)
{
    turb *t;
    uint32_t oct = fe->octaves < TURB_STATIONARY ? fe->octaves : TURB_STATIONARY;
    uint64_t n = (uint64_t)w * h;
    uint32_t x;
    uint32_t y;
    if (oct > 0 && n * 4 > TURB_MAX_WORK / oct) {
        return false;
    }
    t = malloc(sizeof *t);
    if (t == NULL) {
        return false;
    }
    turb_init(t, fe->seed);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t *p = px + ((size_t)y * w + x) * 4;
            double tx = ((double)x + off_x) / sx;
            double ty = ((double)y + off_y) / sy;
            int c;
            for (c = 0; c < 4; c++) {
                double v = turbulence(t, c, tx, ty, (double)x, (double)y, (double)w, (double)h,
                                      (double)fe->bfx, (double)fe->bfy, oct, fe->fractal, fe->stitch);
                v = fe->fractal ? (v * 255.0 + 255.0) / 2.0 : v * 255.0;
                p[c] = pc_f32_to_u8(sv_f32_bound(0.0f, pc_f64_to_f32(v), 255.0f) + 0.5f);
            }
        }
    }
    free(t);
    return true;
}
