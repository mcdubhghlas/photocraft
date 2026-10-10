/* Shared filter helpers (port of crates/algo/src/fxutil.rs and the small shared helpers listed in
 * fxutil.h). */
#include "algo/fxutil.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "raster/pixel.h"

pc_pixel_format pc_algo_ctx_format(const pc_algo_ctx *ctx)
{
    pc_pixel_format f;
    f.mode = ctx->mode;
    f.sample = PC_SAMPLE_TYPE_F32;
    f.alpha = ctx->alpha;
    return f;
}

void pc_algo_xy(pc_rect out, size_t i, int32_t *x, int32_t *y)
{
    size_t w = pc_rect_width(out);
    if (w == 0) {
        *x = out.x0;
        *y = out.y0;
        return;
    }
    *x = (int32_t)((int64_t)out.x0 + (int64_t)(i % w));
    *y = (int32_t)((int64_t)out.y0 + (int64_t)(i / w));
}

void pc_algo_rgba(const pc_algo_ctx *ctx, const float *px, size_t n, float out[4])
{
    pc_to_rgba(pc_algo_ctx_format(ctx), px, n, out);
}

void pc_algo_set_rgba(const pc_algo_ctx *ctx, float *px, size_t n, const float c[4])
{
    float tmp[PC_ALGO_MAXC] = {0};
    size_t k = pc_from_rgba(pc_algo_ctx_format(ctx), c, tmp);
    size_t m = k < n ? k : n;
    if (m > 0) {
        memcpy(px, tmp, m * sizeof(float));
    }
}

void pc_algo_native(const pc_algo_ctx *ctx, const float c[4], float out[PC_ALGO_MAXC])
{
    memset(out, 0, PC_ALGO_MAXC * sizeof(float));
    (void)pc_from_rgba(pc_algo_ctx_format(ctx), c, out);
}

float pc_algo_luma(const pc_algo_ctx *ctx, const float *px, size_t n)
{
    float c[4];
    switch (ctx->mode) {
    case PC_COLOR_MODE_GRAYSCALE:
    case PC_COLOR_MODE_BITMAP:
    case PC_COLOR_MODE_DUOTONE:
    case PC_COLOR_MODE_LAB:
        return n > 0 ? px[0] : 0.0f;
    case PC_COLOR_MODE_RGB:
        if (n >= 3) {
            return 0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2];
        }
        break;
    default:
        break;
    }
    pc_algo_rgba(ctx, px, n, c);
    return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
}

pc_status pc_algo_premul_window(const pc_algo_image *src, pc_rect win, bool alpha, float **out, size_t *len,
                                pc_error *err)
{
    size_t n = src->ch, total, k = 0, c;
    float *v;
    int32_t x, y;
    if (pc_rect_is_empty(win)) {
        win = PC_RECT_EMPTY;
    }
    if (!pc_algo_float_count(win, n, &total)) {
        return pc_fail(err, PC_ERR_LIMIT, "window of %ux%u is too large", pc_rect_width(win),
                       pc_rect_height(win));
    }
    v = pc_malloc_array(total > 0 ? total : 1, sizeof(float));
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a %zu-sample window", total);
    }
    for (y = win.y0; y < win.y1; y++) {
        for (x = win.x0; x < win.x1; x++) {
            float a = alpha && n > 0 ? pc_algo_image_get(src, x, y, n - 1) : 1.0f;
            for (c = 0; c < n; c++) {
                float s = pc_algo_image_get(src, x, y, c);
                v[k++] = alpha && c + 1 < n ? s * a : s;
            }
        }
    }
    if (total == 0) {
        free(v);
        v = NULL;
    }
    *out = v;
    *len = total;
    return PC_OK;
}

void pc_algo_unpremul_px(float *px, size_t n, bool alpha)
{
    size_t c;
    float a;
    if (!alpha || n == 0) {
        return;
    }
    a = px[n - 1];
    for (c = 0; c + 1 < n; c++) {
        px[c] = a > 1e-7f ? px[c] / a : 0.0f;
    }
}

/* One running-sum box pass of radius r over `len` elements of n channels read at stride `step`
 * (in floats) from line[0]; the source is copied to scratch first. f64 sums: rounding must not
 * depend on where a window starts (tile independence). */
static void box_line(float *line, size_t len, size_t step, size_t n, size_t r, float *scratch)
{
    double norm = 1.0 / (double)(2 * r + 1);
    size_t i, c;
    int64_t ri = (int64_t)r, last = (int64_t)len - 1;
    for (i = 0; i < len; i++) {
        memcpy(scratch + i * n, line + i * step, n * sizeof(float));
    }
    for (c = 0; c < n; c++) {
        double acc = -0.0; /* Iterator::sum starts at -0.0 */
        int64_t k;
        for (k = -ri; k <= ri; k++) {
            acc += (double)scratch[(size_t)(k < 0 ? 0 : (k > last ? last : k)) * n + c];
        }
        for (i = 0; i < len; i++) {
            int64_t a = (int64_t)i + ri + 1, b = (int64_t)i - ri;
            line[i * step + c] = (float)(acc * norm);
            acc += (double)scratch[(size_t)(a > last ? last : a) * n + c] -
                   (double)scratch[(size_t)(b < 0 ? 0 : b) * n + c];
        }
    }
}

static pc_status box_rows(float *buf, size_t w, size_t h, size_t n, size_t r, float *scratch)
{
    size_t y;
    if (r == 0 || w == 0) {
        return PC_OK;
    }
    for (y = 0; y < h; y++) {
        box_line(buf + y * w * n, w, n, n, r, scratch);
    }
    return PC_OK;
}

static pc_status box_cols(float *buf, size_t w, size_t h, size_t n, size_t r, float *scratch)
{
    size_t x;
    if (r == 0 || h == 0) {
        return PC_OK;
    }
    for (x = 0; x < w; x++) {
        box_line(buf + x * n, h, w * n, n, r, scratch);
    }
    return PC_OK;
}

static pc_status box_scratch(size_t w, size_t h, size_t n, float **out, pc_error *err)
{
    size_t m = w > h ? w : h;
    *out = pc_malloc_array(m * n > 0 ? m * n : 1, sizeof(float));
    if (*out == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a blur line");
    }
    return PC_OK;
}

static pc_status check_radius(size_t r, pc_error *err)
{
    if (r > PC_ALGO_MAX_BOX_RADIUS) {
        return pc_fail(err, PC_ERR_LIMIT, "blur radius %zu is too large", r);
    }
    return PC_OK;
}

pc_status pc_algo_box_blur_n(float *buf, size_t w, size_t h, size_t n, size_t r, pc_error *err)
{
    float *scratch;
    pc_status st = check_radius(r, err);
    if (st != PC_OK || w == 0 || h == 0 || n == 0) {
        return st;
    }
    st = box_scratch(w, h, n, &scratch, err);
    if (st != PC_OK) {
        return st;
    }
    (void)box_rows(buf, w, h, n, r, scratch);
    (void)box_cols(buf, w, h, n, r, scratch);
    free(scratch);
    return PC_OK;
}

void pc_algo_gauss_box_radii(float sigma, size_t out[3])
{
    const float n = 3.0f;
    float w_ideal = sqrtf(12.0f * sigma * sigma / n + 1.0f);
    int32_t wl = pc_f32_to_i32(floorf(w_ideal)), wu;
    float mf;
    size_t m, i;
    if (wl % 2 == 0) {
        wl = pc_i32_wrapping_sub(wl, 1);
    }
    if (wl < 1) {
        wl = 1;
    }
    wu = pc_i32_wrapping_add(wl, 2);
    mf = (12.0f * sigma * sigma - n * (float)pc_i32_wrapping_mul(wl, wl) - 4.0f * n * (float)wl - 3.0f * n) /
         (-4.0f * (float)wl - 4.0f);
    m = pc_f32_to_size(pc_clampf(roundf(mf), 0.0f, n));
    for (i = 0; i < 3; i++) {
        /* `wl as usize - 1`: a negative (wrapped) i32 sign-extends, as in the reference. */
        out[i] = i < m ? ((size_t)(int64_t)wl - 1) / 2 : ((size_t)(int64_t)wu - 1) / 2;
    }
    /* Kovesi's rounded triple degenerates below sigma ~0.577 (every radius 0): one radius-1 pass
     * is the smallest real blur (issue #706). */
    if (out[0] == 0 && out[1] == 0 && out[2] == 0) {
        out[0] = 1;
    }
}

pc_status pc_algo_gauss_blur_n(float *buf, size_t w, size_t h, size_t n, float sigma, pc_error *err)
{
    size_t radii[3], i;
    float *scratch;
    pc_status st;
    if (sigma < 0.3f) {
        return PC_OK;
    }
    pc_algo_gauss_box_radii(sigma, radii);
    for (i = 0; i < 3; i++) {
        st = check_radius(radii[i], err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (w == 0 || h == 0 || n == 0) {
        return PC_OK;
    }
    st = box_scratch(w, h, n, &scratch, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < 3; i++) {
        (void)box_rows(buf, w, h, n, radii[i], scratch);
    }
    for (i = 0; i < 3; i++) {
        (void)box_cols(buf, w, h, n, radii[i], scratch);
    }
    free(scratch);
    return PC_OK;
}

float pc_algo_hash01(int32_t x, int32_t y, uint32_t c, uint32_t seed)
{
    uint32_t h =
        ((uint32_t)x * 0x8da6b343u) ^ ((uint32_t)y * 0xd8163841u) ^ (c * 0xcb1ab31fu) ^ (seed * 0x9e3779b9u);
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return (float)(h >> 8) / (float)(1u << 24);
}

void pc_algo_cell_point(int32_t cx, int32_t cy, float cell, uint32_t seed, float ox, float oy, float *px,
                        float *py)
{
    *px = ox + ((float)cx + 0.1f + 0.8f * pc_algo_hash01(cx, cy, 11, seed)) * cell;
    *py = oy + ((float)cy + 0.1f + 0.8f * pc_algo_hash01(cx, cy, 12, seed)) * cell;
}

float pc_algo_smoothstep(float e0, float e1, float x)
{
    float t;
    if (e1 <= e0) {
        return x >= e1 ? 1.0f : 0.0f;
    }
    t = pc_clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void pc_algo_add_sample_premul(const pc_algo_image *src, float x, float y, pc_rect clip, bool alpha,
                               float *acc)
{
    size_t n = src->ch, w = pc_rect_width(src->rect), q, c;
    float fx, fy, ax, ay, k[4];
    int32_t x0, y0, x1, y1, xs[4], ys[4];
    if (!isfinite(x) || !isfinite(y) || pc_rect_is_empty(clip) || n == 0) {
        return;
    }
    fx = pc_clampf(x - 0.5f, (float)clip.x0, (float)(clip.x1 - 1));
    fy = pc_clampf(y - 0.5f, (float)clip.y0, (float)(clip.y1 - 1));
    x0 = pc_f32_to_i32(floorf(fx));
    y0 = pc_f32_to_i32(floorf(fy));
    x1 = pc_i32_wrapping_add(x0, 1) < clip.x1 - 1 ? pc_i32_wrapping_add(x0, 1) : clip.x1 - 1;
    y1 = pc_i32_wrapping_add(y0, 1) < clip.y1 - 1 ? pc_i32_wrapping_add(y0, 1) : clip.y1 - 1;
    ax = fx - (float)x0;
    ay = fy - (float)y0;
    xs[0] = x0, ys[0] = y0, k[0] = (1.0f - ax) * (1.0f - ay);
    xs[1] = x1, ys[1] = y0, k[1] = ax * (1.0f - ay);
    xs[2] = x0, ys[2] = y1, k[2] = (1.0f - ax) * ay;
    xs[3] = x1, ys[3] = y1, k[3] = ax * ay;
    for (q = 0; q < 4; q++) {
        int64_t dy = (int64_t)ys[q] - src->rect.y0, dx = (int64_t)xs[q] - src->rect.x0;
        size_t i;
        const float *px;
        float a;
        if (k[q] <= 0.0f) {
            continue;
        }
        if (dy < 0 || dx < 0 || (uint64_t)dx >= w) {
            continue; /* the reference requires clip inside src.rect (it would panic) */
        }
        i = ((size_t)dy * w + (size_t)dx) * n;
        if (i + n > src->len) {
            continue;
        }
        px = src->data + i;
        a = alpha ? px[n - 1] * k[q] : k[q];
        if (alpha) {
            for (c = 0; c + 1 < n; c++) {
                acc[c] += px[c] * a;
            }
            acc[n - 1] += a;
        } else {
            for (c = 0; c < n; c++) {
                acc[c] += px[c] * a;
            }
        }
    }
}

float pc_algo_rem_euclidf(float a, float b)
{
    float r = fmodf(a, b);
    return r < 0.0f ? r + fabsf(b) : r;
}

static float hue_of(const float c[3], float mx, float d)
{
    float h;
    if (d <= 0.0f) {
        return 0.0f;
    }
    if (mx == c[0]) {
        h = pc_algo_rem_euclidf((c[1] - c[2]) / d, 6.0f);
    } else if (mx == c[1]) {
        h = (c[2] - c[0]) / d + 2.0f;
    } else {
        h = (c[0] - c[1]) / d + 4.0f;
    }
    return h / 6.0f;
}

void pc_algo_rgb_to_hsl(const float c[3], float out[3])
{
    float mx = pc_rs_maxf(pc_rs_maxf(c[0], c[1]), c[2]);
    float mn = pc_rs_minf(pc_rs_minf(c[0], c[1]), c[2]);
    float d = mx - mn;
    float l = (mx + mn) / 2.0f;
    float s = d <= 0.0f ? 0.0f : d / pc_rs_maxf_k(1.0f - fabsf(2.0f * l - 1.0f), 1e-7f);
    out[0] = hue_of(c, mx, d);
    out[1] = pc_clampf(s, 0.0f, 1.0f);
    out[2] = l;
}

static void from_hcm(float h, float chroma, float m, float out[3])
{
    float hp = pc_algo_rem_euclidf(h, 1.0f) * 6.0f;
    float x = chroma * (1.0f - fabsf(pc_algo_rem_euclidf(hp, 2.0f) - 1.0f));
    float r, g, b;
    switch (pc_f32_to_i32(hp)) {
    case 0:
        r = chroma, g = x, b = 0.0f;
        break;
    case 1:
        r = x, g = chroma, b = 0.0f;
        break;
    case 2:
        r = 0.0f, g = chroma, b = x;
        break;
    case 3:
        r = 0.0f, g = x, b = chroma;
        break;
    case 4:
        r = x, g = 0.0f, b = chroma;
        break;
    default:
        r = chroma, g = 0.0f, b = x;
        break;
    }
    out[0] = r + m;
    out[1] = g + m;
    out[2] = b + m;
}

void pc_algo_hsl_to_rgb(const float c[3], float out[3])
{
    float chroma = (1.0f - fabsf(2.0f * c[2] - 1.0f)) * c[1];
    from_hcm(c[0], chroma, c[2] - chroma / 2.0f, out);
}

void pc_algo_hsb_to_rgb(const float c[3], float out[3])
{
    float chroma = c[2] * c[1];
    from_hcm(c[0], chroma, c[2] - chroma, out);
}

void pc_algo_color_range(const float (*px)[4], size_t n, const float color[3], float fuzziness, float *mask)
{
    float f = pc_rs_maxf_k(fuzziness, 0.0f) + 1.0f;
    size_t i;
    int c;
    for (i = 0; i < n; i++) {
        float d = 0.0f;
        if (px[i][3] <= 0.0f) {
            mask[i] = 0.0f;
            continue;
        }
        for (c = 0; c < 3; c++) {
            d = pc_rs_maxf(d, fabsf(px[i][c] - color[c]) * 255.0f);
        }
        mask[i] = pc_clampf(1.0f - d / f, 0.0f, 1.0f);
    }
}

pc_status pc_algo_par_for(size_t n, size_t grain, pc_pool_fn fn, void *ctx)
{
    return pc_pool_for(pc_pool_global(), n, grain, fn, ctx);
}
