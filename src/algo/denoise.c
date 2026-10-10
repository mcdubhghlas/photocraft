/* Reduce Noise (port of crates/algo/src/denoise.rs): edge-preserving luminance smoothing, chroma
 * smoothing guided by the luminance, and optional detail sharpening, all with the guided filter
 * (He, Sun & Tang, "Guided Image Filtering", ECCV 2010): O(1) per pixel whatever the radius. */
#include "algo/blur_int.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define MAX_LUMA_R 5
#define MAX_CHROMA_R 9

float pc_algo_denoise_reach(void)
{
    /* Two box passes per guided filter, plus sharpening. */
    return (float)(2 * MAX_LUMA_R + 2 * MAX_CHROMA_R + 6);
}

/* Guided filter of p with guide g (both w x h), box radius r, into out. */
static pc_status guided(const float *g, const float *p, size_t w, size_t h, size_t r, float eps, float *out,
                        pc_error *err)
{
    size_t len = w * h, i;
    float *m = pc_malloc_array(len * 4 + 1, sizeof(float)), *ab = pc_malloc_array(len * 2 + 1, sizeof(float));
    pc_status st;
    if (m == NULL || ab == NULL) {
        free(m);
        free(ab);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Reduce Noise");
    }
    /* Interleave the four box-filtered moments to run one pass. */
    for (i = 0; i < len; i++) {
        m[i * 4] = g[i];
        m[i * 4 + 1] = p[i];
        m[i * 4 + 2] = g[i] * g[i];
        m[i * 4 + 3] = g[i] * p[i];
    }
    st = pc_algo_box_blur_n(m, w, h, 4, r, err);
    if (st == PC_OK) {
        for (i = 0; i < len; i++) {
            float mg = m[i * 4], mp = m[i * 4 + 1], mgg = m[i * 4 + 2], mgp = m[i * 4 + 3];
            float var = pc_rs_maxf_k(mgg - mg * mg, 0.0f), a = (mgp - mg * mp) / (var + eps);
            ab[i * 2] = a;
            ab[i * 2 + 1] = mp - a * mg;
        }
        st = pc_algo_box_blur_n(ab, w, h, 2, r, err);
    }
    if (st == PC_OK) {
        for (i = 0; i < len; i++) {
            out[i] = ab[i * 2] * g[i] + ab[i * 2 + 1];
        }
    }
    free(m);
    free(ab);
    return st;
}

typedef enum model { M_GRAY, M_YCC, M_LAB, M_VIA_RGB } model;

pc_status pc_algo_reduce_noise_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                                 const pc_algo_reduce_noise_params *p, float *dst, pc_error *err)
{
    size_t n = src->ch, ww = pc_rect_width(src->rect), wh = pc_rect_height(src->rect), len, i, c, xx, yy;
    size_t npx = (size_t)pc_rect_width(out) * pc_rect_height(out);
    float s = pc_clampf(p->strength, 0.0f, 10.0f) / 10.0f,
          col = pc_clampf(p->reduce_color_noise, 0.0f, 100.0f) / 100.0f;
    float sh = pc_clampf(p->sharpen_details, 0.0f, 100.0f) / 100.0f, px[PC_ALGO_MAXC] = {0};
    float *yv = NULL, *c1 = NULL, *c2 = NULL, *tmp = NULL;
    pc_rect win = src->rect;
    pc_status st = PC_OK;
    model md;
    len = ww * wh;
    if ((s <= 0.0f && col <= 0.0f && sh <= 0.0f && !p->remove_jpeg_artifact) || len == 0) {
        pc_algo_image_crop_into(src, out, dst);
        return PC_OK;
    }
    if (n == 0 || n > PC_ALGO_MAXC) {
        return pc_fail(err, PC_ERR_INVALID, "unsupported channel count %zu", n);
    }
    if (len > PC_MAX_ALLOC / 64) {
        return pc_fail(err, PC_ERR_LIMIT, "Reduce Noise area too large");
    }
    switch (ctx->mode) {
    case PC_COLOR_MODE_GRAYSCALE:
    case PC_COLOR_MODE_BITMAP:
    case PC_COLOR_MODE_DUOTONE:
        md = M_GRAY;
        break;
    case PC_COLOR_MODE_RGB:
        md = M_YCC;
        break;
    case PC_COLOR_MODE_LAB:
        md = M_LAB;
        break;
    default:
        md = M_VIA_RGB;
        break;
    }
    /* Decompose into luma + two chroma planes. */
    yv = pc_malloc_array(len, sizeof(float));
    tmp = pc_malloc_array(len, sizeof(float));
    if (md != M_GRAY) {
        c1 = pc_malloc_array(len, sizeof(float));
        c2 = pc_malloc_array(len, sizeof(float));
    }
    if (yv == NULL || tmp == NULL || (md != M_GRAY && (c1 == NULL || c2 == NULL))) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Reduce Noise");
        goto done;
    }
    for (i = 0; i < len; i++) {
        int32_t x = (int32_t)((int64_t)win.x0 + (int64_t)(i % ww)),
                y = (int32_t)((int64_t)win.y0 + (int64_t)(i / ww));
        for (c = 0; c < n; c++) {
            px[c] = pc_algo_image_get(src, x, y, c);
        }
        switch (md) {
        case M_GRAY:
            yv[i] = px[0];
            break;
        case M_LAB:
            yv[i] = px[0];
            c1[i] = px[1];
            c2[i] = px[2];
            break;
        case M_YCC:
        case M_VIA_RGB: {
            float cv[4], l;
            if (md == M_YCC) {
                cv[0] = px[0], cv[1] = px[1], cv[2] = px[2], cv[3] = 1.0f;
            } else {
                pc_algo_rgba(ctx, px, n, cv);
            }
            l = 0.299f * cv[0] + 0.587f * cv[1] + 0.114f * cv[2];
            yv[i] = l;
            c1[i] = cv[2] - l;
            c2[i] = cv[0] - l;
            break;
        }
        }
    }
    /* JPEG deblocking: soften luma across the 8x8 grid lines (document-anchored). */
    if (p->remove_jpeg_artifact) {
        memcpy(tmp, yv, len * sizeof(float));
        for (yy = 1; yy + 1 < wh; yy++) {
            for (xx = 1; xx + 1 < ww; xx++) {
                int64_t dx = (((int64_t)win.x0 + (int64_t)xx) % 8 + 8) % 8;
                int64_t dy = (((int64_t)win.y0 + (int64_t)yy) % 8 + 8) % 8;
                if (dx == 0 || dx == 7 || dy == 0 || dy == 7) {
                    float a = 0.0f;
                    size_t jj, kk;
                    for (jj = 0; jj < 3; jj++) {
                        for (kk = 0; kk < 3; kk++) {
                            a += tmp[(yy + jj - 1) * ww + xx + kk - 1];
                        }
                    }
                    yv[yy * ww + xx] = 0.5f * tmp[yy * ww + xx] + 0.5f * a / 9.0f;
                }
            }
        }
    }
    /* Luminance: edge-preserving smoothing; Preserve Details shrinks the regularisation. */
    if (s > 0.0f) {
        size_t r = pc_f32_to_size(roundf(1.0f + pc_clampf(p->strength, 0.0f, 10.0f) * 0.4f));
        float keep = pc_clampf(p->preserve_details, 0.0f, 100.0f) / 100.0f, base = 0.004f + 0.05f * s;
        float eps = base * base * (1.05f - keep), k = sqrtf(s);
        st = guided(yv, yv, ww, wh, r < MAX_LUMA_R ? r : MAX_LUMA_R, eps, tmp, err);
        if (st != PC_OK) {
            goto done;
        }
        for (i = 0; i < len; i++) {
            yv[i] += (tmp[i] - yv[i]) * k;
        }
    }
    /* Chroma: guided by the cleaned luma so colour edges stay put. */
    if (col > 0.0f && md != M_GRAY) {
        size_t r = pc_f32_to_size(roundf(1.0f + col * 8.0f)), q;
        float base = 0.01f + 0.05f * col, eps = base * base;
        for (q = 0; q < 2; q++) {
            float *plane = q == 0 ? c1 : c2;
            st = guided(yv, plane, ww, wh, r < MAX_CHROMA_R ? r : MAX_CHROMA_R, eps, tmp, err);
            if (st != PC_OK) {
                goto done;
            }
            for (i = 0; i < len; i++) {
                plane[i] += (tmp[i] - plane[i]) * col;
            }
        }
    }
    /* Sharpen Details: unsharp mask on luma. */
    if (sh > 0.0f) {
        memcpy(tmp, yv, len * sizeof(float));
        st = pc_algo_gauss_blur_n(tmp, ww, wh, 1, 1.0f, err);
        if (st != PC_OK) {
            goto done;
        }
        for (i = 0; i < len; i++) {
            yv[i] += (yv[i] - tmp[i]) * sh * 1.2f;
        }
    }
    pc_algo_image_crop_into(src, out, dst);
    for (i = 0; i < npx; i++) {
        float *q = dst + i * n;
        int32_t x, y;
        size_t k;
        pc_algo_xy(out, i, &x, &y);
        /* The reference indexes its planes here (out lies inside src); keep a pixel outside. */
        if (!pc_rect_contains(win, x, y)) {
            continue;
        }
        k = (size_t)((int64_t)y - win.y0) * ww + (size_t)((int64_t)x - win.x0);
        switch (md) {
        case M_GRAY:
            q[0] = yv[k];
            break;
        case M_LAB:
            q[0] = yv[k];
            if (n >= 3) {
                q[1] = c1[k];
                q[2] = c2[k];
            }
            break;
        case M_YCC:
        case M_VIA_RGB: {
            float l = yv[k], b = c1[k] + l, r = c2[k] + l, g = (l - 0.299f * r - 0.114f * b) / 0.587f;
            if (md == M_YCC && n >= 3) {
                q[0] = r;
                q[1] = g;
                q[2] = b;
            } else if (md == M_VIA_RGB) {
                float rgba[4];
                rgba[0] = r, rgba[1] = g, rgba[2] = b, rgba[3] = ctx->alpha ? q[n - 1] : 1.0f;
                pc_algo_set_rgba(ctx, q, n, rgba);
            }
            break;
        }
        }
    }
done:
    free(yv), free(c1), free(c2), free(tmp);
    return st;
}
