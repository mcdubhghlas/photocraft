/* Color Replacement (replace.h). */
#include "paint/replace.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "color/blend.h"
#include "paint/retouch.h"
#include "raster/pixel.h"

static const char *const MODE_NAMES[] = {"hue", "saturation", "color", "luminosity"};
static const char *const SAMPLING_NAMES[] = {"continuous", "once", "backgroundSwatch"};
static const char *const LIMITS_NAMES[] = {"discontiguous", "contiguous", "findEdges"};

const char *pc_paint_replace_mode_name(pc_paint_replace_mode m)
{
    return (unsigned)m < 4 ? MODE_NAMES[m] : NULL;
}

const char *pc_paint_sampling_name(pc_paint_sampling s)
{
    return (unsigned)s < 3 ? SAMPLING_NAMES[s] : NULL;
}

const char *pc_paint_limits_name(pc_paint_limits l)
{
    return (unsigned)l < 3 ? LIMITS_NAMES[l] : NULL;
}

static bool find_name(const char *const *names, unsigned count, const char *s, size_t len, unsigned *out)
{
    for (unsigned i = 0; s != NULL && i < count; i++) {
        if (strlen(names[i]) == len && memcmp(names[i], s, len) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

bool pc_paint_replace_mode_parse(const char *s, size_t len, pc_paint_replace_mode *out)
{
    unsigned i;
    if (!find_name(MODE_NAMES, 4, s, len, &i)) {
        return false;
    }
    *out = (pc_paint_replace_mode)i;
    return true;
}

bool pc_paint_sampling_parse(const char *s, size_t len, pc_paint_sampling *out)
{
    unsigned i;
    if (!find_name(SAMPLING_NAMES, 3, s, len, &i)) {
        return false;
    }
    *out = (pc_paint_sampling)i;
    return true;
}

bool pc_paint_limits_parse(const char *s, size_t len, pc_paint_limits *out)
{
    unsigned i;
    if (!find_name(LIMITS_NAMES, 3, s, len, &i)) {
        return false;
    }
    *out = (pc_paint_limits)i;
    return true;
}

void pc_paint_replace_settings_default(pc_paint_replace_settings *s)
{
    static const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f}, white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    memset(s, 0, sizeof *s);
    s->mode = PC_PAINT_REPLACE_COLOR;
    s->sampling = PC_PAINT_SAMPLING_CONTINUOUS;
    s->limits = PC_PAINT_LIMITS_CONTIGUOUS;
    s->tolerance = 0.3f;
    s->anti_alias = true;
    memcpy(s->color, red, sizeof s->color);
    memcpy(s->background, white, sizeof s->background);
}

bool pc_paint_replace_settings_eq(const pc_paint_replace_settings *a, const pc_paint_replace_settings *b)
{
    if (a->mode != b->mode || a->sampling != b->sampling || a->limits != b->limits ||
        !(a->tolerance == b->tolerance) || a->anti_alias != b->anti_alias) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (!(a->color[i] == b->color[i]) || !(a->background[i] == b->background[i])) {
            return false;
        }
    }
    return true;
}

pc_status pc_paint_keep_connected(float *weight, size_t w, size_t h, size_t sx, size_t sy, pc_error *err)
{
    size_t total = w * h, si = sy * w + sx, top = 0;
    bool *keep;
    size_t *stack = NULL;
    if (total == 0) {
        return PC_OK;
    }
    keep = pc_calloc_array(total, sizeof *keep);
    if (keep == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a contiguous fill");
    }
    if (sx < w && sy < h && weight[si] > 0.0f) {
        /* Each pixel is pushed at most once, so total entries suffice. */
        stack = pc_malloc_array(total, sizeof *stack);
        if (stack == NULL) {
            free(keep);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for a contiguous fill");
        }
        stack[top++] = si;
        keep[si] = true;
        while (top > 0) {
            size_t i = stack[--top], x = i % w, y = i / w, nb[4], nn = 0;
            if (x > 0) {
                nb[nn++] = i - 1;
            }
            if (x + 1 < w) {
                nb[nn++] = i + 1;
            }
            if (y > 0) {
                nb[nn++] = i - w;
            }
            if (y + 1 < h) {
                nb[nn++] = i + w;
            }
            for (size_t k = 0; k < nn; k++) {
                if (!keep[nb[k]] && weight[nb[k]] > 0.0f) {
                    keep[nb[k]] = true;
                    stack[top++] = nb[k];
                }
            }
        }
    }
    for (size_t i = 0; i < total; i++) {
        if (!keep[i]) {
            weight[i] = 0.0f;
        }
    }
    free(stack);
    free(keep);
    return PC_OK;
}

struct pc_paint_replace_effect {
    pc_paint_replace_settings rs_own;
    pc_surface pre_own;
    const pc_paint_replace_settings *rs;
    const pc_surface *pre;
    pc_pixel_format fmt;
    size_t n;
    pc_blend_mode mode;
    float fg[3];
    float tol;
    bool soft;
    bool has_once;
    float once[3];
};
typedef struct pc_paint_replace_effect rep_ctx;

static float rgb_dist(const float a[4], const float b[3])
{
    return pc_rs_maxf(pc_rs_maxf(fabsf(a[0] - b[0]), fabsf(a[1] - b[1])), fabsf(a[2] - b[2]));
}

static float match_w(const rep_ctx *c, float d)
{
    if (d > c->tol) {
        return 0.0f;
    }
    if (c->soft && c->tol > 0.0f) {
        /* Soft falloff over the last quarter of the tolerance. */
        return pc_clampf((c->tol - d) / (c->tol * 0.25f), 0.0f, 1.0f);
    }
    return 1.0f;
}

static pc_status replace_dab(void *vctx, pc_paint_region *work, const pc_paint_footprint *fp, pc_error *err)
{
    rep_ctx *c = vctx;
    int32_t cx = pc_f64_to_i32(floor(fp->dab.center.x)), cy = pc_f64_to_i32(floor(fp->dab.center.y));
    float sampled[3], px4[4];
    pc_rect r;
    size_t w, h;
    float *weight;
    pc_status st = PC_OK;
    switch (c->rs->sampling) {
    case PC_PAINT_SAMPLING_ONCE:
        if (!c->has_once) {
            pc_surface_rgba(c->pre, cx, cy, px4);
            memcpy(c->once, px4, sizeof c->once);
            c->has_once = true;
        }
        memcpy(sampled, c->once, sizeof sampled);
        break;
    case PC_PAINT_SAMPLING_BACKGROUND_SWATCH:
        memcpy(sampled, c->rs->background, sizeof sampled);
        break;
    default:
        pc_surface_rgba(c->pre, cx, cy, px4);
        memcpy(sampled, px4, sizeof sampled);
        break;
    }
    r = pc_rect_intersect(fp->rect, work->rect);
    if (pc_rect_is_empty(r)) {
        return PC_OK;
    }
    w = pc_rect_width(r);
    h = pc_rect_height(r);
    /* r lies inside the footprint, which is capped at PC_PAINT_MAX_DAB_PIXELS. */
    weight = pc_calloc_array(w * h, sizeof *weight);
    if (weight == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for colour replacement");
    }
    for (int32_t y = r.y0; y < r.y1; y++) {
        for (int32_t x = r.x0; x < r.x1; x++) {
            float col[4];
            if (pc_paint_footprint_at(fp, x, y) <= 0.0f) {
                continue;
            }
            /* Match against the pre-stroke colours, so pixels recoloured by earlier dabs of this
             * stroke still count as the sampled colour. */
            pc_surface_rgba(c->pre, x, y, col);
            if (col[3] <= 0.0f) {
                continue;
            }
            weight[(size_t)((int64_t)y - r.y0) * w + (size_t)((int64_t)x - r.x0)] =
                match_w(c, rgb_dist(col, sampled));
        }
    }
    if (c->rs->limits != PC_PAINT_LIMITS_DISCONTIGUOUS) {
        int32_t sx = cx < r.x0 ? r.x0 : (cx > r.x1 - 1 ? r.x1 - 1 : cx);
        int32_t sy = cy < r.y0 ? r.y0 : (cy > r.y1 - 1 ? r.y1 - 1 : cy);
        st = pc_paint_keep_connected(weight, w, h, (size_t)((int64_t)sx - r.x0), (size_t)((int64_t)sy - r.y0),
                                     err);
    }
    for (int32_t y = r.y0; st == PC_OK && y < r.y1; y++) {
        for (int32_t x = r.x0; x < r.x1; x++) {
            float k = pc_paint_footprint_at(fp, x, y) *
                      weight[(size_t)((int64_t)y - r.y0) * w + (size_t)((int64_t)x - r.x0)];
            float col[4], b[3], o[4], enc[PC_RASTER_MAX_CHANNELS];
            float *px;
            if (k <= 0.0f || (px = pc_paint_region_px(work, x, y)) == NULL) {
                continue;
            }
            pc_to_rgba(c->fmt, px, c->n, col);
            pc_blend_rgb(c->mode, col, c->fg, b);
            o[0] = col[0] + (b[0] - col[0]) * k;
            o[1] = col[1] + (b[1] - col[1]) * k;
            o[2] = col[2] + (b[2] - col[2]) * k;
            o[3] = col[3];
            (void)pc_from_rgba(c->fmt, o, enc);
            memcpy(px, enc, c->n * sizeof *px);
        }
    }
    free(weight);
    return st;
}

static const pc_blend_mode MODES[4] = {PC_BLEND_HUE, PC_BLEND_SATURATION, PC_BLEND_COLOR,
                                       PC_BLEND_LUMINOSITY};

/* Fills c's derived settings from *rs and *pre (both borrowed). */
static void rep_setup(rep_ctx *c, const pc_paint_replace_settings *rs, const pc_surface *pre)
{
    c->rs = rs;
    c->pre = pre;
    c->fmt = pc_surface_format(pre);
    c->n = pc_pixel_format_channels(c->fmt);
    c->mode = (unsigned)rs->mode < 4 ? MODES[rs->mode] : PC_BLEND_COLOR;
    memcpy(c->fg, rs->color, sizeof c->fg);
    c->tol = pc_clampf(rs->tolerance, 0.0f, 1.0f);
    c->soft = rs->anti_alias && rs->limits != PC_PAINT_LIMITS_FIND_EDGES;
}

pc_status pc_paint_replace_effect_new(const pc_surface *pre, const pc_paint_replace_settings *rs,
                                      pc_paint_replace_effect **out, pc_error *err)
{
    rep_ctx *c = calloc(1, sizeof *c);
    pc_status st;
    if (c == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for colour replacement");
    }
    st = pc_surface_clone(pre, &c->pre_own, err);
    if (st != PC_OK) {
        free(c);
        return st;
    }
    c->rs_own = *rs;
    rep_setup(c, &c->rs_own, &c->pre_own);
    *out = c;
    return PC_OK;
}

void pc_paint_replace_effect_free(pc_paint_replace_effect *e)
{
    if (e == NULL) {
        return;
    }
    pc_surface_fini(&e->pre_own);
    free(e);
}

pc_status pc_paint_replace_effect_dab(void *ctx, pc_paint_region *work, const pc_paint_footprint *fp,
                                      pc_error *err)
{
    return replace_dab(ctx, work, fp, err);
}

pc_status pc_paint_apply_color_replacement(pc_surface *target, const pc_paint_stroke *stroke,
                                           const pc_paint_replace_settings *rs, const pc_surface *selection,
                                           bool lock_transparency, pc_rect *damage, pc_error *err)
{
    rep_ctx c;
    pc_surface pre;
    pc_status st;
    memset(&c, 0, sizeof c);
    st = pc_surface_clone(target, &pre, err);
    if (st != PC_OK) {
        return st;
    }
    rep_setup(&c, rs, &pre);
    st = pc_paint_apply_dab_stroke(target, stroke, selection, lock_transparency, 0, replace_dab, &c, damage,
                                   err);
    pc_surface_fini(&pre);
    return st;
}
