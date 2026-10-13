/* Content-Aware Fill (port of crates/algo/src/content_aware.rs); see content_aware.h. The Auto
 * sampling area is in content_aware_auto.c. */
#include "algo/content_aware.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/inpaint.h"
#include "algo/poisson.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

pc_algo_ca_fill_options pc_algo_ca_fill_options_default(void)
{
    pc_algo_ca_fill_options o = {pc_algo_ca_color_level("default"), NULL, 0, false, false, 1};
    return o;
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

pc_algo_adapt pc_algo_ca_color_level(const char *level)
{
    if (level != NULL && strcmp(level, "none") == 0) {
        return PC_ALGO_ADAPT_NONE;
    }
    if (level != NULL && strcmp(level, "high") == 0) {
        return (pc_algo_adapt){0.8f, 1.25f, 0.08f};
    }
    if (level != NULL && strcmp(level, "veryHigh") == 0) {
        return (pc_algo_adapt){0.67f, 1.5f, 0.14f};
    }
    return (pc_algo_adapt){0.9f, 1.1f, 0.04f};
}

pc_algo_adapt pc_algo_ca_color_amount(uint8_t color)
{
    float c;
    if (color == 0) {
        return PC_ALGO_ADAPT_NONE;
    }
    c = (float)(color < 10 ? color : 10);
    return (pc_algo_adapt){1.0f - 0.033f * c, 1.0f + 0.05f * c, 0.04f * c};
}

/* One axis of the sampling window: twice the centre c2, the selection's extent, the canvas lo..hi. */
static void span(int64_t side, int64_t c2, uint32_t extent, int32_t lo32, int32_t hi32, int32_t *a0, int32_t *a1)
{
    int64_t lo = lo32, hi = hi32, a;
    side = side > (int64_t)extent ? side : (int64_t)extent;
    if (side >= hi - lo) {
        *a0 = lo32, *a1 = hi32;
        return;
    }
    a = (c2 - side) / 2;
    a = a < lo ? lo : (a > hi - side ? hi - side : a);
    *a0 = (int32_t)a, *a1 = (int32_t)(a + side);
}

pc_rect pc_algo_ca_sampling_window(pc_rect hole, pc_rect canvas)
{
    double w, h, sd;
    int64_t side;
    pc_rect r;
    if (pc_rect_is_empty(hole) || pc_rect_is_empty(canvas)) {
        return PC_RECT_EMPTY;
    }
    w = (double)(pc_rect_width(hole) > 50 ? pc_rect_width(hole) : 50);
    h = (double)(pc_rect_height(hole) > 50 ? pc_rect_height(hole) : 50);
    sd = pc_rs_min_k(round(4.0 * sqrt(w * h)), (double)INT32_MAX);
    side = (int64_t)sd;
    span(side, (int64_t)hole.x0 + hole.x1, pc_rect_width(hole), canvas.x0, canvas.x1, &r.x0, &r.x1);
    span(side, (int64_t)hole.y0 + hole.y1, pc_rect_height(hole), canvas.y0, canvas.y1, &r.y0, &r.y1);
    return r;
}

/* ---- variants ------------------------------------------------------------------------------------ */

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

typedef struct var_job {
    size_t w, h, ch, ow;
    const float *img;
    const bool *ok;
    float s, c, scale, icx, icy, ocx, ocy;
    bool mirror;
    float *vi;
    bool *vo;
    const pc_interrupt *ctl;
} var_job;

static pc_status var_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const var_job *j = ctx;
    float px[PC_ALGO_HEAL_MAX_CH];
    size_t x, y;
    (void)chunk;
    for (y = begin; y < end; y++) {
        if (pc_interrupt_cancelled(j->ctl)) {
            continue;
        }
        for (x = 0; x < j->ow; x++) {
            float dx = ((float)x - j->ocx) / j->scale, dy = ((float)y - j->ocy) / j->scale;
            float sx = j->icx + dx * j->c - dy * j->s;
            float sy = j->icy + dx * j->s + dy * j->c;
            if (j->mirror) {
                sx = ((float)j->w - 1.0f) - sx;
            }
            if (sample(j->w, j->h, j->ch, j->img, j->ok, sx, sy, px)) {
                size_t i = y * j->ow + x;
                j->vo[i] = true;
                memcpy(&j->vi[i * j->ch], px, j->ch * sizeof(float));
            }
        }
    }
    return PC_OK;
}

pc_status pc_algo_ca_variant(size_t w, size_t h, size_t ch, const float *img, const bool *ok, float deg,
                             float scale, bool mirror, const pc_interrupt *ctl, size_t *ow_out, size_t *oh_out,
                             float **out_img, bool **out_ok, pc_error *err)
{
    /* f32::to_radians multiplies by the f32 constant PI / 180. */
    float a = -(deg * (3.14159265358979323846f / 180.0f));
    float s = sinf(a), c = cosf(a), fw, fh;
    size_t ow, oh, n;
    var_job j;
    pc_status st = pc_algo_heal_dims(w, h, ch, SIZE_MAX, NULL, err);
    if (st != PC_OK) {
        return st;
    }
    if (w == 0 || h == 0) {
        return pc_fail(err, PC_ERR_INVALID, "empty source");
    }
    if (!(scale > 0.0f) || !isfinite(scale) || !isfinite(deg)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid variant transform");
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
    j.vi = pc_calloc_array(n * ch, sizeof *j.vi);
    j.vo = pc_calloc_array(n, sizeof *j.vo);
    if (j.vi == NULL || j.vo == NULL) {
        free(j.vi);
        free(j.vo);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    j.w = w, j.h = h, j.ch = ch, j.ow = ow, j.img = img, j.ok = ok, j.s = s, j.c = c, j.scale = scale;
    j.icx = ((float)w - 1.0f) / 2.0f, j.icy = ((float)h - 1.0f) / 2.0f;
    j.ocx = ((float)ow - 1.0f) / 2.0f, j.ocy = ((float)oh - 1.0f) / 2.0f;
    j.mirror = mirror, j.ctl = ctl;
    (void)pc_algo_par_for(oh, 1, var_rows, &j);
    if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
        free(j.vi);
        free(j.vo);
        return st;
    }
    *ow_out = ow, *oh_out = oh, *out_img = j.vi, *out_ok = j.vo;
    return PC_OK;
}

/* ---- the fill ------------------------------------------------------------------------------------ */

/* The caller's interrupt for the completion: same cancel, progress scaled into 0..0.99. */
static bool inner_cancel(void *ctx)
{
    return pc_interrupt_cancelled(ctx);
}

static void inner_progress(void *ctx, float f)
{
    pc_interrupt_progress(ctx, f * 0.99f);
}

typedef struct variant {
    size_t w, h;
    float *img;
    bool *ok;
} variant;

/* Fill the hole pixels of img from the membrane (nothing to copy from). */
static pc_status membrane_into(size_t w, size_t h, size_t ch, float *img, const bool *hole, const pc_interrupt *ctl,
                               pc_error *err)
{
    size_t n = w * h, i;
    float *m = NULL;
    pc_status st = pc_algo_poisson_membrane_fill(w, h, ch, img, n * ch, hole, n, ctl, &m, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < n; i++) {
        if (hole[i]) {
            memcpy(&img[i * ch], &m[i * ch], ch * sizeof(float));
        }
    }
    free(m);
    return PC_OK;
}

pc_status pc_algo_ca_fill_inplace(size_t w, size_t h, size_t ch, float *img, size_t len, const bool *hole,
                                  const bool *source, size_t mask_len, const pc_algo_ca_fill_options *opts,
                                  const pc_interrupt *ctl, pc_error *err)
{
    pc_algo_complete_params params = pc_algo_complete_params_default();
    size_t n, i, nv = 0, gap, k, x, y, mw = w, mh = h, mn;
    variant vars[PC_ALGO_CA_MAX_ROTATIONS + 3];
    bool *ok = NULL, any = false, filled = false;
    uint8_t *roles = NULL;
    float *mimg = NULL, *f = NULL;
    pc_interrupt inner = {inner_cancel, inner_progress, (void *)(uintptr_t)ctl};
    pc_status st = pc_algo_heal_dims(w, h, ch, len, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (opts == NULL || mask_len != n || (n > 0 && (img == NULL || hole == NULL || source == NULL)) ||
        (opts->rotation_count > 0 && opts->rotations == NULL)) {
        return pc_fail(err, PC_ERR_INVALID, "masks hold %zu entries, expected %zu", mask_len, n);
    }
    if (opts->rotation_count > PC_ALGO_CA_MAX_ROTATIONS) {
        return pc_fail(err, PC_ERR_LIMIT, "%zu rotations is too many", opts->rotation_count);
    }
    if ((st = pc_algo_adapt_check(opts->color_adaptation, err)) != PC_OK) {
        return st;
    }
    for (i = 0; i < n && !any; i++) {
        any = hole[i];
    }
    if (!any) {
        return PC_OK;
    }
    params.seed = opts->seed;
    params.adapt = opts->color_adaptation;
    gap = 2 * params.patch_radius + 2;
    ok = pc_malloc_array(n, sizeof *ok);
    if (ok == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < n; i++) {
        ok[i] = !hole[i] && source[i];
    }
    /* Variants: the identity (which holds the hole) first, then the adaptations. */
    {
        float degs[PC_ALGO_CA_MAX_ROTATIONS + 3], scales[PC_ALGO_CA_MAX_ROTATIONS + 3];
        bool mirrors[PC_ALGO_CA_MAX_ROTATIONS + 3];
        size_t want = 0;
        if (opts->mirror) {
            degs[want] = 0.0f, scales[want] = 1.0f, mirrors[want++] = true;
        }
        for (k = 0; k < opts->rotation_count; k++) {
            degs[want] = opts->rotations[k], scales[want] = 1.0f, mirrors[want++] = false;
        }
        if (opts->scale) {
            degs[want] = 0.0f, scales[want] = 0.8f, mirrors[want++] = false;
            degs[want] = 0.0f, scales[want] = 1.25f, mirrors[want++] = false;
        }
        for (k = 0; k < want; k++) {
            if ((st = pc_algo_ca_variant(w, h, ch, img, ok, degs[k], scales[k], mirrors[k], ctl, &vars[nv].w,
                                         &vars[nv].h, &vars[nv].img, &vars[nv].ok, err)) != PC_OK) {
                goto cleanup;
            }
            nv++;
        }
    }
    if (nv > 0) {
        size_t col_w = 0, col_h = 0;
        for (k = 0; k < nv; k++) {
            col_w = vars[k].w > col_w ? vars[k].w : col_w;
            col_h += vars[k].h + gap;
        }
        mw = w + gap + col_w;
        mh = h > col_h ? h : col_h;
    }
    if ((st = pc_algo_heal_dims(mw, mh, ch, SIZE_MAX, &mn, err)) != PC_OK) {
        goto cleanup;
    }
    roles = pc_calloc_array(mn, 1);
    if (roles == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t s = y * w + x;
            roles[y * mw + x] = hole[s] ? PC_ALGO_PX_HOLE : (ok[s] ? PC_ALGO_PX_SOURCE : PC_ALGO_PX_KNOWN);
        }
    }
    if (nv > 0) {
        size_t oy = 0;
        /* Mosaic: identity at the left, variants in a column to its right, gaps that are no image. */
        mimg = pc_calloc_array(mn * ch, sizeof *mimg);
        if (mimg == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            goto cleanup;
        }
        for (y = 0; y < h; y++) {
            memcpy(&mimg[y * mw * ch], &img[y * w * ch], w * ch * sizeof(float));
        }
        for (k = 0; k < nv; k++) {
            const variant *v = &vars[k];
            if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
                goto cleanup;
            }
            for (y = 0; y < v->h; y++) {
                for (x = 0; x < v->w; x++) {
                    size_t s = y * v->w + x, d = (oy + y) * mw + w + gap + x;
                    memcpy(&mimg[d * ch], &v->img[s * ch], ch * sizeof(float));
                    if (v->ok[s]) {
                        roles[d] = PC_ALGO_PX_SOURCE;
                    }
                }
            }
            oy += v->h + gap;
        }
    }
    for (k = 0; k < nv; k++) {
        free(vars[k].img);
        free(vars[k].ok);
    }
    nv = 0;
    free(ok);
    ok = NULL;
    if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
        goto cleanup;
    }
    if (mimg == NULL) {
        /* Without variants the image itself is completed (it changes only in the hole). */
        st = pc_algo_inpaint_complete_masked_inplace(w, h, ch, img, len, roles, n, &params, &inner, &filled, err);
        if (st == PC_OK && !filled) {
            st = membrane_into(w, h, ch, img, hole, ctl, err);
        }
    } else {
        st = pc_algo_inpaint_complete_masked(mw, mh, ch, mimg, mn * ch, roles, mn, &params, &inner, &f, err);
        if (st == PC_OK && f != NULL) {
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    if (hole[y * w + x]) {
                        memcpy(&img[(y * w + x) * ch], &f[(y * mw + x) * ch], ch * sizeof(float));
                    }
                }
            }
        } else if (st == PC_OK) {
            st = membrane_into(w, h, ch, img, hole, ctl, err);
        }
    }
    if (st == PC_OK && (st = pc_interrupt_check(ctl, err)) == PC_OK) {
        pc_interrupt_progress(ctl, 1.0f);
    }
cleanup:
    for (k = 0; k < nv; k++) {
        free(vars[k].img);
        free(vars[k].ok);
    }
    free(ok);
    free(roles);
    free(mimg);
    free(f);
    return st;
}

pc_status pc_algo_ca_fill(size_t w, size_t h, size_t ch, const float *img, size_t len, const bool *hole,
                          const bool *source, size_t mask_len, const pc_algo_ca_fill_options *opts,
                          const pc_interrupt *ctl, float **out, pc_error *err)
{
    size_t n;
    float *o;
    pc_status st = pc_algo_heal_dims(w, h, ch, len, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n > 0 && img == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing buffer");
    }
    o = pc_malloc_array(n * ch > 0 ? n * ch : 1, sizeof *o);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (n > 0) {
        memcpy(o, img, n * ch * sizeof *o);
    }
    st = pc_algo_ca_fill_inplace(w, h, ch, o, len, hole, source, mask_len, opts, ctl, err);
    if (st != PC_OK) {
        free(o);
        return st;
    }
    *out = o;
    return PC_OK;
}

/* ---- level transform ------------------------------------------------------------------------------ */

static void moments64(const float *v, size_t nv, size_t ch, size_t c, double *s, double *s2, double *n)
{
    size_t i;
    *s = 0.0, *s2 = 0.0, *n = 0.0;
    for (i = 0; i + ch <= nv; i += ch) {
        double x = (double)v[i + c];
        *s += x;
        *s2 += x * x;
        *n += 1.0;
    }
}

pc_status pc_algo_ca_level_transform(size_t ch, const float *from, size_t nfrom, const float *to, size_t nto,
                                     pc_algo_adapt a, float (*out)[2], pc_error *err)
{
    size_t c, step = ch > 1 ? ch : 1;
    pc_status st;
    if ((nfrom > 0 && from == NULL) || (nto > 0 && to == NULL) || (ch > 0 && out == NULL)) {
        return pc_fail(err, PC_ERR_INVALID, "missing samples");
    }
    if ((st = pc_algo_adapt_check(a, err)) != PC_OK) {
        return st;
    }
    for (c = 0; c < ch; c++) {
        double fs, fs2, fnn, ts, ts2, tn, k;
        moments64(from, nfrom, step, c, &fs, &fs2, &fnn);
        moments64(to, nto, step, c, &ts, &ts2, &tn);
        if (pc_algo_adapt_is_none(a) || fnn == 0.0 || tn == 0.0) {
            out[c][0] = 1.0f, out[c][1] = 0.0f;
            continue;
        }
        /* Both sets scaled to the same count for the fit. */
        k = fnn / tn;
        pc_algo_adapt_fit(a, (float)fnn, (float)(ts * k), (float)(ts2 * k), (float)fs, (float)fs2, &out[c][0],
                          &out[c][1]);
    }
    return PC_OK;
}
