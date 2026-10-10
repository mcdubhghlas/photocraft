/* Oil Paint: flow-guided smoothing plus a lit bristle relief (port of crates/algo/src/oil.rs).
 *
 * The structure tensor of the luminance (Brox et al.) gives a smooth stroke direction field, a
 * line integral convolution (Cabral & Leedom 1993) smears colour along it, as in flow-based
 * abstraction (Kyprianidis & Doellner 2008), and white noise convolved along the same field gives
 * the bristle streaks, which with the smoothed luminance form a height map shaded by a light. */
#include <stdlib.h>
#include <string.h>

#include "algo/fx.h"
#include "algo/fx_defs.h"
#include "base/mem.h"

static float stroke_len(float stylization)
{
    return 1.5f + pc_clampf(stylization, 0.1f, 10.0f) * 2.0f;
}

static float tensor_sigma(float scale)
{
    return 0.8f + pc_clampf(scale, 0.1f, 10.0f) * 0.6f;
}

float pc_algo_oil_reach(float stylization, float scale)
{
    return stroke_len(stylization) + 3.0f * tensor_sigma(scale) + 3.0f * 2.5f + 4.0f;
}

typedef struct oil_work {
    size_t ww, wh, n;
    float *p;        /* premultiplied, cleaned colour of the window */
    float (*tan)[2]; /* stroke direction per window pixel */
} oil_work;

/* Bilinear sample of the window at (x, y) (clamped), weighted by k, added to acc. */
static void oil_sample(const oil_work *o, float x, float y, float *acc, float k)
{
    size_t ww = o->ww, wh = o->wh, n = o->n, x0, y0, x1, y1, c, i00, i10, i01, i11;
    float ax, ay, w00, w10, w01, w11;
    x = pc_clampf(x, 0.0f, (float)(ww - 1));
    y = pc_clampf(y, 0.0f, (float)(wh - 1));
    x0 = pc_f32_to_size(floorf(x));
    y0 = pc_f32_to_size(floorf(y));
    if (x0 > ww - 1) {
        x0 = ww - 1;
    }
    if (y0 > wh - 1) {
        y0 = wh - 1;
    }
    x1 = x0 + 1 < ww - 1 ? x0 + 1 : ww - 1;
    y1 = y0 + 1 < wh - 1 ? y0 + 1 : wh - 1;
    ax = x - (float)x0;
    ay = y - (float)y0;
    w00 = (1.0f - ax) * (1.0f - ay) * k;
    w10 = ax * (1.0f - ay) * k;
    w01 = (1.0f - ax) * ay * k;
    w11 = ax * ay * k;
    i00 = (y0 * ww + x0) * n, i10 = (y0 * ww + x1) * n, i01 = (y1 * ww + x0) * n, i11 = (y1 * ww + x1) * n;
    for (c = 0; c < n; c++) {
        acc[c] += o->p[i00 + c] * w00 + o->p[i10 + c] * w10 + o->p[i01 + c] * w01 + o->p[i11 + c] * w11;
    }
}

static size_t round_index(float v, size_t len)
{
    return pc_f32_to_size(pc_clampf(roundf(v), 0.0f, (float)(len - 1)));
}

/* Height of the relief at r1-local (rx, ry). */
static float oil_height(const float *col, const float *bristle, size_t rw, size_t n, size_t cc, bool sub,
                        float amp, size_t rx, size_t ry)
{
    size_t o = (ry * rw + rx) * n, c;
    float l = -0.0f;
    for (c = 0; c < cc; c++) {
        l += col[o + c];
    }
    l /= (float)cc;
    l = sub ? 1.0f - l : l;
    return l * 2.0f + bristle[ry * rw + rx] * amp * 6.0f;
}

static pc_status oil_k(const void *vp, const void *prep, const pc_algo_image *src, pc_rect out,
                       const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_oil_params *spec = vp;
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), ww = pc_rect_width(src->rect),
           wh = pc_rect_height(src->rect);
    size_t plen = 0, npx, i, c, rw, rh, x, y;
    pc_rect win = src->rect, r1;
    bool alpha = ctx->alpha, sub = ctx->mode == PC_COLOR_MODE_CMYK, lab = ctx->mode == PC_COLOR_MODE_LAB;
    float *lum = NULL, *efg = NULL, *col = NULL, *bristle = NULL, len, h, light[3], half[3], amp, shine,
          flat_spec, ls, lc, elev;
    int32_t steps;
    oil_work o = {ww, wh, n, NULL, NULL};
    pc_status st;
    (void)prep;
    if (pc_rect_is_empty(out) || n == 0 || n > PC_ALGO_MAXC || cc == 0) {
        pc_algo_image_crop_into(src, out, dst);
        return PC_OK;
    }
    if (!pc_rect_contains_rect(win, out)) {
        return pc_fail(err, PC_ERR_INVALID, "Oil Paint: the source must cover the output and its halo");
    }
    npx = ww * wh;
    /* 1. Premultiplied colour, cleaned (pre-smoothed) by `cleanliness`. */
    st = pc_algo_premul_window(src, win, alpha, &o.p, &plen, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_algo_gauss_blur_n(o.p, ww, wh, n, pc_clampf(spec->cleanliness, 0.0f, 10.0f) * 0.25f, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    lum = pc_malloc_array(npx, sizeof *lum);
    efg = pc_calloc_array(npx * 3, sizeof *efg);
    o.tan = pc_malloc_array(npx, sizeof *o.tan);
    if (lum == NULL || efg == NULL || o.tan == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Oil Paint");
        goto cleanup;
    }
    /* 2. Luminance-like intensity for the structure tensor (channel mean works in every model). */
    for (i = 0; i < npx; i++) {
        float s = -0.0f;
        for (c = 0; c < cc; c++) {
            s += o.p[i * n + c];
        }
        lum[i] = s / (float)cc;
    }
    /* 3. Smoothed structure tensor (E, F, G). */
    for (y = 1; y + 1 < wh; y++) {
        for (x = 1; x + 1 < ww; x++) {
#define L(dx, dy) lum[(y + (size_t)(dy) - 1) * ww + (x + (size_t)(dx) - 1)]
            float gx = (L(2, 0) + 2.0f * L(2, 1) + L(2, 2)) - (L(0, 0) + 2.0f * L(0, 1) + L(0, 2));
            float gy = (L(0, 2) + 2.0f * L(1, 2) + L(2, 2)) - (L(0, 0) + 2.0f * L(1, 0) + L(2, 0));
#undef L
            size_t k = (y * ww + x) * 3;
            efg[k] = gx * gx;
            efg[k + 1] = gx * gy;
            efg[k + 2] = gy * gy;
        }
    }
    st = pc_algo_gauss_blur_n(efg, ww, wh, 3, tensor_sigma(spec->scale), err);
    if (st != PC_OK) {
        goto cleanup;
    }
    /* 4. Stroke direction = minor eigenvector (along edges). */
    for (i = 0; i < npx; i++) {
        float e = efg[i * 3], f = efg[i * 3 + 1], g = efg[i * 3 + 2];
        float l1 = 0.5f * (e + g + sqrtf((e - g) * (e - g) + 4.0f * f * f));
        float tx = l1 - e, ty = -f, m = sqrtf(tx * tx + ty * ty);
        /* Near-isotropic structure has no stable direction: gentle diagonal strokes. */
        if (m < 1e-6f || (l1 - (e + g - l1)) < 1e-6f) {
            o.tan[i][0] = PC_FX_FRAC_1_SQRT_2;
            o.tan[i][1] = PC_FX_FRAC_1_SQRT_2;
        } else {
            o.tan[i][0] = tx / m;
            o.tan[i][1] = ty / m;
        }
    }
    /* 5. Line integral convolution over `out` grown by 1 (for the relief gradient). */
    r1 = pc_rect_intersect(pc_rect_inflate(out, 1), win);
    rw = pc_rect_width(r1);
    rh = pc_rect_height(r1);
    len = stroke_len(spec->stylization);
    h = pc_rs_maxf_k(len / 10.0f, 1.0f);
    steps = pc_f32_to_i32(ceilf(len / h));
    col = pc_malloc_array(rw * rh * n, sizeof *col);
    bristle = pc_malloc_array(rw * rh, sizeof *bristle);
    if (col == NULL || bristle == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Oil Paint");
        goto cleanup;
    }
    for (y = 0; y < rh; y++) {
        for (x = 0; x < rw; x++) {
            float dx = (float)(r1.x0 - win.x0) + (float)x, dy = (float)(r1.y0 - win.y0) + (float)y;
            float acc[PC_ALGO_MAXC] = {0}, wsum = 0.0f, nsum = 0.0f;
            int d;
            size_t k0 = (y * rw + x) * n;
            oil_sample(&o, dx, dy, acc, 1.0f);
            wsum += 1.0f;
            nsum += pc_algo_hash01(pc_i32_wrapping_add(win.x0, pc_f32_to_i32(dx)),
                                   pc_i32_wrapping_add(win.y0, pc_f32_to_i32(dy)), 71, 0);
            for (d = 0; d < 2; d++) {
                float dir = d == 0 ? 1.0f : -1.0f, qx = dx, qy = dy, px = 0.0f, py = 0.0f;
                int32_t k;
                for (k = 1; k <= steps; k++) {
                    size_t ti = round_index(qy, wh) * ww + round_index(qx, ww);
                    float tx = o.tan[ti][0], ty = o.tan[ti][1], wk;
                    if (k == 1) {
                        tx *= dir;
                        ty *= dir;
                    } else if (tx * px + ty * py < 0.0f) {
                        tx = -tx;
                        ty = -ty;
                    }
                    px = tx;
                    py = ty;
                    qx += tx * h;
                    qy += ty * h;
                    wk = 1.0f - (float)k / (float)(steps + 1);
                    oil_sample(&o, qx, qy, acc, wk);
                    wsum += wk;
                    nsum +=
                        wk * pc_algo_hash01(pc_i32_wrapping_add(win.x0, pc_f32_to_i32(roundf(qx))),
                                            pc_i32_wrapping_add(win.y0, pc_f32_to_i32(roundf(qy))), 71, 0);
                }
            }
            for (c = 0; c < n; c++) {
                col[k0 + c] = acc[c] / wsum;
            }
            bristle[y * rw + x] = nsum / wsum - 0.5f;
        }
    }
    /* 6. Relief lighting. */
    ls = sinf(pc_fx_rad(spec->angle));
    lc = cosf(pc_fx_rad(spec->angle));
    elev = 45.0f * PC_FX_RAD_PER_DEG;
    light[0] = lc * cosf(elev);
    light[1] = -ls * cosf(elev);
    light[2] = sinf(elev);
    {
        float v[3] = {light[0], light[1], light[2] + 1.0f},
              m = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        half[0] = v[0] / m, half[1] = v[1] / m, half[2] = v[2] / m;
    }
    amp = pc_clampf(spec->bristle_detail, 0.0f, 10.0f) / 10.0f;
    shine = pc_clampf(spec->shine, 0.0f, 10.0f) / 10.0f;
    flat_spec = powf(half[2], 40.0f);
    i = 0;
    for (y = 0; y < pc_rect_height(out); y++) {
        for (x = 0; x < pc_rect_width(out); x++) {
            size_t rx = (size_t)(out.x0 - r1.x0) + x, ry = (size_t)(out.y0 - r1.y0) + y,
                   k0 = (ry * rw + rx) * n;
            float *px = dst + i;
            memcpy(px, col + k0, n * sizeof(float));
            pc_algo_unpremul_px(px, n, alpha);
            /* Painting restyles colour, not coverage: keep the layer's own alpha. */
            if (alpha) {
                px[n - 1] = pc_algo_image_get(src, out.x0 + (int32_t)x, out.y0 + (int32_t)y, n - 1);
            }
            if (spec->lighting) {
                float hx =
                    oil_height(col, bristle, rw, n, cc, sub, amp, rx + 1 < rw - 1 ? rx + 1 : rw - 1, ry) -
                    oil_height(col, bristle, rw, n, cc, sub, amp, rx > 0 ? rx - 1 : 0, ry);
                float hy =
                    oil_height(col, bristle, rw, n, cc, sub, amp, rx, ry + 1 < rh - 1 ? ry + 1 : rh - 1) -
                    oil_height(col, bristle, rw, n, cc, sub, amp, rx, ry > 0 ? ry - 1 : 0);
                float nv0 = -hx * 0.5f, nv1 = -hy * 0.5f;
                float m = sqrtf(nv0 * nv0 + nv1 * nv1 + 1.0f);
                float ndl = (nv0 * light[0] + nv1 * light[1] + 1.0f * light[2]) / m;
                float ndh = pc_rs_maxf_k((nv0 * half[0] + nv1 * half[1] + 1.0f * half[2]) / m, 0.0f);
                float shade = 1.0f + (ndl - light[2]) * 1.2f;
                float spec_v = shine * pc_rs_maxf_k(powf(ndh, 40.0f) - flat_spec, 0.0f) * 0.8f;
                size_t chans = lab ? 1 : cc;
                for (c = 0; c < chans; c++) {
                    px[c] = sub ? 1.0f - ((1.0f - px[c]) * shade + spec_v) : px[c] * shade + spec_v;
                }
            }
            i += n;
        }
    }
    st = PC_OK;
cleanup:
    free(o.p);
    free(o.tan);
    free(lum);
    free(efg);
    free(col);
    free(bristle);
    return st;
}

static pc_algo_halo oil_halo(const void *p, const pc_rect *b)
{
    const pc_algo_oil_params *o = p;
    (void)b;
    return pc_algo_halo_reach(pc_algo_oil_reach(o->stylization, o->scale));
}

static const pc_algo_jfield OIL_F[] = {
    PC_ALGO_JF("stylization", pc_algo_oil_params, stylization, PC_ALGO_JF_F32),
    PC_ALGO_JF("cleanliness", pc_algo_oil_params, cleanliness, PC_ALGO_JF_F32),
    PC_ALGO_JF("scale", pc_algo_oil_params, scale, PC_ALGO_JF_F32),
    PC_ALGO_JF("bristleDetail", pc_algo_oil_params, bristle_detail, PC_ALGO_JF_F32),
    PC_ALGO_JF("lighting", pc_algo_oil_params, lighting, PC_ALGO_JF_BOOL),
    PC_ALGO_JF("angle", pc_algo_oil_params, angle, PC_ALGO_JF_F32),
    PC_ALGO_JF("shine", pc_algo_oil_params, shine, PC_ALGO_JF_F32),
};

PC_FX_DEF(pc_algo_def_oil_paint, "oilPaint", pc_algo_oil_params, OIL_F, NULL, oil_halo, oil_k);
