/* Content-Aware Fill (port of crates/algo/src/content_aware.rs); see content_aware.h. */
#include "algo/content_aware.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/inpaint.h"
#include "algo/poisson.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

pc_algo_ca_fill_options pc_algo_ca_fill_options_default(void)
{
    return (pc_algo_ca_fill_options){0.35f, NULL, 0, false, false, 1};
}

size_t pc_algo_ca_rotation_level(const char *level, float out[3])
{
    if (level == NULL) {
        return 0;
    }
    if (strcmp(level, "low") == 0) {
        out[0] = -10.0f, out[1] = 10.0f;
        return 2;
    }
    if (strcmp(level, "medium") == 0) {
        out[0] = -20.0f, out[1] = 20.0f;
        return 2;
    }
    if (strcmp(level, "high") == 0) {
        out[0] = -35.0f, out[1] = 35.0f;
        return 2;
    }
    if (strcmp(level, "full") == 0) {
        out[0] = 90.0f, out[1] = 180.0f, out[2] = 270.0f;
        return 3;
    }
    return 0;
}

float pc_algo_ca_color_level(const char *level)
{
    if (level != NULL && strcmp(level, "none") == 0) {
        return 0.0f;
    }
    if (level != NULL && strcmp(level, "high") == 0) {
        return 0.65f;
    }
    if (level != NULL && strcmp(level, "veryHigh") == 0) {
        return 0.9f;
    }
    return 0.35f;
}

/* Sample the source bilinearly at (x, y); false when any tap is unusable. */
static bool sample(size_t w, size_t h, size_t ch, const float *img, const bool *ok, float x, float y,
                   float *out)
{
    size_t x0, y0, x1, y1, c;
    float tx, ty;
    if (x < 0.0f || y < 0.0f || x > (float)(w - 1) || y > (float)(h - 1)) {
        return false;
    }
    x0 = pc_f32_to_size(floorf(x)), y0 = pc_f32_to_size(floorf(y));
    x1 = x0 + 1 < w - 1 ? x0 + 1 : w - 1;
    y1 = y0 + 1 < h - 1 ? y0 + 1 : h - 1;
    tx = x - (float)x0, ty = y - (float)y0;
    if (!ok[y0 * w + x0] || !ok[y0 * w + x1] || !ok[y1 * w + x0] || !ok[y1 * w + x1]) {
        return false;
    }
    for (c = 0; c < ch; c++) {
        float p00 = img[(y0 * w + x0) * ch + c], p10 = img[(y0 * w + x1) * ch + c];
        float p01 = img[(y1 * w + x0) * ch + c], p11 = img[(y1 * w + x1) * ch + c];
        float top = p00 + (p10 - p00) * tx;
        float bot = p01 + (p11 - p01) * tx;
        out[c] = top + (bot - top) * ty;
    }
    return true;
}

pc_status pc_algo_ca_variant(size_t w, size_t h, size_t ch, const float *img, const bool *ok, float deg,
                             float scale, bool mirror, size_t *ow_out, size_t *oh_out, float **out_img,
                             bool **out_ok, pc_error *err)
{
    /* f32::to_radians multiplies by the f32 constant PI / 180. */
    float a = -(deg * (3.14159265358979323846f / 180.0f));
    float s = sinf(a), c = cosf(a), fw, fh, icx, icy, ocx, ocy, px[PC_ALGO_HEAL_MAX_CH];
    size_t ow, oh, x, y, n;
    float *vi;
    bool *vo;
    pc_status st = pc_algo_heal_dims(w, h, ch, SIZE_MAX, NULL, err);
    if (st != PC_OK) {
        return st;
    }
    if (w == 0 || h == 0) {
        return pc_fail(err, PC_ERR_INVALID, "empty source");
    }
    /* Output size: the rotated, scaled bounding box. */
    fw = (float)w * scale, fh = (float)h * scale;
    ow = pc_f32_to_size(roundf(fw * fabsf(c) + fh * fabsf(s)));
    oh = pc_f32_to_size(roundf(fw * fabsf(s) + fh * fabsf(c)));
    ow = ow > 1 ? ow : 1;
    oh = oh > 1 ? oh : 1;
    if ((st = pc_algo_heal_dims(ow, oh, ch, SIZE_MAX, &n, err)) != PC_OK) {
        return st;
    }
    vi = pc_calloc_array(n * ch, sizeof *vi);
    vo = pc_calloc_array(n, sizeof *vo);
    if (vi == NULL || vo == NULL) {
        free(vi);
        free(vo);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    icx = ((float)w - 1.0f) / 2.0f, icy = ((float)h - 1.0f) / 2.0f;
    ocx = ((float)ow - 1.0f) / 2.0f, ocy = ((float)oh - 1.0f) / 2.0f;
    for (y = 0; y < oh; y++) {
        for (x = 0; x < ow; x++) {
            float dx = ((float)x - ocx) / scale, dy = ((float)y - ocy) / scale;
            float sx = icx + dx * c - dy * s;
            float sy = icy + dx * s + dy * c;
            if (mirror) {
                sx = ((float)w - 1.0f) - sx;
            }
            if (sample(w, h, ch, img, ok, sx, sy, px)) {
                size_t i = y * ow + x;
                vo[i] = true;
                memcpy(&vi[i * ch], px, ch * sizeof(float));
            }
        }
    }
    *ow_out = ow, *oh_out = oh, *out_img = vi, *out_ok = vo;
    return PC_OK;
}

/* Separable box blur of radius r (edge-clamped), in place through a scratch buffer. */
static pc_status box_blur(size_t w, size_t h, size_t ch, float *v, size_t r, pc_error *err)
{
    size_t len = w * h * ch, pass;
    float *tmp, norm;
    if (r == 0) {
        return PC_OK;
    }
    tmp = pc_malloc_array(len, sizeof *tmp);
    if (tmp == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    norm = 1.0f / (float)(2 * r + 1);
    for (pass = 0; pass < 2; pass++) {
        bool horizontal = pass == 0;
        const float *src = horizontal ? v : tmp;
        float *dst = horizontal ? tmp : v;
        size_t nl = horizontal ? h : w, m = horizontal ? w : h, line, c, k;
        int64_t ri = (int64_t)r, mm = (int64_t)m - 1;
        for (line = 0; line < nl; line++) {
            for (c = 0; c < ch; c++) {
                float acc = -0.0f;
                int64_t q;
#define AT(kk)                                                                                               \
    src[((horizontal ? line * w + (size_t)pc_clamp_i64((kk), 0, mm)                                          \
                     : (size_t)pc_clamp_i64((kk), 0, mm) * w + line)) *                                      \
            ch +                                                                                             \
        c]
                for (q = -ri; q <= ri; q++) {
                    acc += AT(q);
                }
                for (k = 0; k < m; k++) {
                    size_t di = horizontal ? line * w + k : k * w + line;
                    dst[di * ch + c] = acc * norm;
                    acc += AT((int64_t)k + ri + 1) - AT((int64_t)k - ri);
                }
#undef AT
            }
        }
    }
    free(tmp);
    return PC_OK;
}

/* Wraps the caller's interrupt for the completion: same cancel, progress scaled into 0..0.9. */
typedef struct scaled_ctl {
    const pc_interrupt *outer;
    float scale;
} scaled_ctl;

static bool scaled_cancel(void *ctx)
{
    const scaled_ctl *s = ctx;
    return pc_interrupt_cancelled(s->outer);
}

static void scaled_progress(void *ctx, float f)
{
    const scaled_ctl *s = ctx;
    pc_interrupt_progress(s->outer, f * s->scale);
}

/* Pull the fill's low frequencies towards the membrane interpolation of the hole boundary. */
static pc_status adapt_colors(size_t w, size_t h, size_t ch, const float *img, const bool *hole, float *out,
                              float k, const pc_interrupt *ctl, pc_error *err)
{
    size_t x0 = w, y0 = h, x1 = 0, y1 = 0, x, y, r, i, c, n = w * h;
    float *membrane = NULL, *low;
    pc_status st;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            if (hole[y * w + x]) {
                x0 = x < x0 ? x : x0, y0 = y < y0 ? y : y0;
                x1 = x + 1 > x1 ? x + 1 : x1, y1 = y + 1 > y1 ? y + 1 : y1;
            }
        }
    }
    r = (x1 - x0 > y1 - y0 ? x1 - x0 : y1 - y0) / 6;
    r = r < 1 ? 1 : (r > 32 ? 32 : r);
    st = pc_algo_poisson_membrane_fill(w, h, ch, img, n * ch, hole, n, ctl, &membrane, err);
    if (st != PC_OK) {
        return st;
    }
    pc_interrupt_progress(ctl, 0.97f);
    if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
        free(membrane);
        return st;
    }
    low = pc_malloc_array(n * ch, sizeof *low);
    if (low == NULL) {
        free(membrane);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    memcpy(low, out, n * ch * sizeof *low);
    if ((st = box_blur(w, h, ch, low, r, err)) != PC_OK) {
        free(membrane);
        free(low);
        return st;
    }
    for (i = 0; i < n; i++) {
        if (hole[i]) {
            for (c = 0; c < ch; c++) {
                size_t j = i * ch + c;
                out[j] = pc_clampf(out[j] + k * (membrane[j] - low[j]), 0.0f, 1.0f);
            }
        }
    }
    free(membrane);
    free(low);
    return PC_OK;
}

typedef struct variant {
    size_t w, h;
    float *img;
    bool *ok;
} variant;

pc_status pc_algo_ca_fill(size_t w, size_t h, size_t ch, const float *img, size_t len, const bool *hole,
                          const bool *source, size_t mask_len, const pc_algo_ca_fill_options *opts,
                          const pc_interrupt *ctl, float **out, pc_error *err)
{
    pc_algo_complete_params params = pc_algo_complete_params_default();
    variant extra[PC_ALGO_CA_MAX_ROTATIONS + 3];
    size_t n, nextra = 0, gap, i, x, y, col_w = 0, col_h = 0, mw, mh, mn, oy, e;
    bool *ok = NULL, *unknown = NULL, any = false;
    float *mimg = NULL, *filled = NULL, *o = NULL;
    scaled_ctl sc;
    pc_interrupt inner;
    pc_status st = pc_algo_heal_dims(w, h, ch, len, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (opts == NULL || mask_len != n || (n > 0 && (img == NULL || hole == NULL || source == NULL))) {
        return pc_fail(err, PC_ERR_INVALID, "masks hold %zu entries, expected %zu", mask_len, n);
    }
    if (opts->rotation_count > PC_ALGO_CA_MAX_ROTATIONS ||
        (opts->rotation_count > 0 && opts->rotations == NULL)) {
        return pc_fail(err, PC_ERR_LIMIT, "too many source rotations (%zu)", opts->rotation_count);
    }
    for (i = 0; i < n && !any; i++) {
        any = hole[i];
    }
    o = pc_malloc_array(n * ch, sizeof *o);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (n > 0) {
        memcpy(o, img, n * ch * sizeof *o);
    }
    if (!any) {
        *out = o;
        return PC_OK;
    }
    params.seed = opts->seed;
    gap = 2 * params.patch_radius + 2;
    ok = pc_malloc_array(n, sizeof *ok);
    if (ok == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    for (i = 0; i < n; i++) {
        ok[i] = !hole[i] && source[i];
    }
    /* Variants: the identity (which holds the hole) first, then the adaptations. */
    {
        float degs[PC_ALGO_CA_MAX_ROTATIONS + 3], scales[PC_ALGO_CA_MAX_ROTATIONS + 3];
        bool mirrors[PC_ALGO_CA_MAX_ROTATIONS + 3];
        size_t nv = 0, k;
        if (opts->mirror) {
            degs[nv] = 0.0f, scales[nv] = 1.0f, mirrors[nv++] = true;
        }
        for (k = 0; k < opts->rotation_count; k++) {
            degs[nv] = opts->rotations[k], scales[nv] = 1.0f, mirrors[nv++] = false;
        }
        if (opts->scale) {
            degs[nv] = 0.0f, scales[nv] = 0.8f, mirrors[nv++] = false;
            degs[nv] = 0.0f, scales[nv] = 1.25f, mirrors[nv++] = false;
        }
        for (k = 0; k < nv; k++) {
            variant *v = &extra[nextra];
            st = pc_algo_ca_variant(w, h, ch, img, ok, degs[k], scales[k], mirrors[k], &v->w, &v->h, &v->img,
                                    &v->ok, err);
            if (st != PC_OK) {
                goto cleanup;
            }
            nextra++;
            col_w = v->w > col_w ? v->w : col_w;
            col_h += v->h + gap;
        }
    }
    /* Mosaic: identity at the left, variants in a column to its right. */
    mw = nextra == 0 ? w : w + gap + col_w;
    mh = h > col_h ? h : col_h;
    if ((st = pc_algo_heal_dims(mw, mh, ch, SIZE_MAX, &mn, err)) != PC_OK) {
        goto cleanup;
    }
    mimg = pc_calloc_array(mn * ch, sizeof *mimg);
    unknown = pc_malloc_array(mn, sizeof *unknown);
    if (mimg == NULL || unknown == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    for (i = 0; i < mn; i++) {
        unknown[i] = true;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t s = y * w + x, d = y * mw + x;
            memcpy(&mimg[d * ch], &img[s * ch], ch * sizeof(float));
            unknown[d] = !ok[s];
        }
    }
    oy = 0;
    for (e = 0; e < nextra; e++) {
        const variant *v = &extra[e];
        for (y = 0; y < v->h; y++) {
            for (x = 0; x < v->w; x++) {
                size_t s = y * v->w + x, d = (oy + y) * mw + w + gap + x;
                memcpy(&mimg[d * ch], &v->img[s * ch], ch * sizeof(float));
                unknown[d] = !v->ok[s];
            }
        }
        oy += v->h + gap;
    }
    if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
        goto cleanup;
    }
    /* The completion is nearly all the work: it reports 0-90 %. */
    sc = (scaled_ctl){ctl, 0.9f};
    inner = (pc_interrupt){scaled_cancel, scaled_progress, &sc};
    st = pc_algo_inpaint_complete(mw, mh, ch, mimg, mn * ch, unknown, mn, &params, &inner, &filled, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    if (filled != NULL) {
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                if (hole[y * w + x]) {
                    memcpy(&o[(y * w + x) * ch], &filled[(y * mw + x) * ch], ch * sizeof(float));
                }
            }
        }
    } else {
        float *m = NULL;
        if ((st = pc_algo_poisson_membrane_fill(w, h, ch, img, len, hole, n, ctl, &m, err)) != PC_OK) {
            goto cleanup;
        }
        free(o);
        o = m;
    }
    if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
        goto cleanup;
    }
    if (opts->color_adaptation > 0.0f &&
        (st = adapt_colors(w, h, ch, img, hole, o, opts->color_adaptation, ctl, err)) != PC_OK) {
        goto cleanup;
    }
    pc_interrupt_progress(ctl, 1.0f);
cleanup:
    for (e = 0; e < nextra; e++) {
        free(extra[e].img);
        free(extra[e].ok);
    }
    free(ok);
    free(unknown);
    free(mimg);
    free(filled);
    if (st != PC_OK) {
        free(o);
        return st;
    }
    *out = o;
    return PC_OK;
}
