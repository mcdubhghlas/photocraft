/* Mode-native compositing of plain CMYK layer stacks (port of crates/compose/src/native.rs, #3778).
 *
 * The compositor works in display RGB, so flattening a CMYK document through it reads every ink
 * value through the profile and separates it again: even a lone Background comes back with
 * different ink. When every visible layer is a plain raster layer in Normal mode (no masks,
 * effects, clipping or advanced blending), the composite needs no colour conversion at all:
 * Normal mixes ink values linearly, as Photoshop does in CMYK, so it is worked out on the stored
 * values and an opaque layer keeps them exactly. A layer mask (unfeathered) scales the mix.
 *
 * Plain adjustment layers that Photoshop evaluates on the inks (Invert, Posterize, Threshold,
 * Brightness/Contrast, Hue/Saturation, Color Balance, and Levels and Curves on CMYK channels)
 * join such a stack too: on the stored inks their rules are exact, where the RGB compositor can
 * only apply them to a separation of its RGB composite, whose black differs from the file's.
 * Such stacks are also rendered for display this way (pc_compose_render_cmyk). */
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "color/convert.h"
#include "compose/adjust.h"
#include "compose/adjust_internal.h"
#include "compose/internal.h"
#include "compose/masks.h"

/* Process channels of a CMYK pixel. */
#define INKS 4
#define LUT PC_COMPOSE_LUT_SIZE

static pc_status nomem(pc_error *err)
{
    return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
}

/* Groups nested deeper than this keep the RGB compositor. */
#define NATIVE_MAX_DEPTH 64

/* Whether the compositor would only blend `layer` Normally at its opacity through its layer mask:
 * no clipping, vector mask, feathered mask, effects, excluded channels, Blend If or advanced
 * blending. */
static bool simple(const pc_layer *layer)
{
    return layer->blend == PC_BLEND_NORMAL && !layer->clipped && !pc_compose_has_feather(layer) &&
           !(layer->has_vector_mask && layer->vector_mask.enabled) && !pc_compose_has_effects(layer) &&
           layer->excluded_channels == 0 && pc_blend_if_is_default(&layer->blend_if) &&
           pc_advanced_blending_is_default(&layer->advanced) && !layer->has_video;
}

/* A layer the native path can composite: a visible raster layer the compositor would only blend
 * Normally at its opacity. NULL otherwise. */
static const pc_surface *plain(const pc_layer *layer)
{
    const pc_surface *s;
    if (layer->content.kind != PC_LAYER_RASTER) {
        return NULL;
    }
    s = &layer->content.u.raster;
    if (!simple(layer) || pc_surface_format(s).mode != PC_COLOR_MODE_CMYK) {
        return NULL;
    }
    return s;
}

/* An adjustment on straight inks, prepared once per composite. */
typedef enum { INK_TABLES, INK_RULES, INK_CMY } ink_kind;

typedef enum { ITEM_LAYER, ITEM_ADJUST } item_kind;

typedef struct ink_item {
    item_kind kind;
    float opacity;
    const pc_layer_mask *mask; /* NULL: none */
    /* ITEM_LAYER */
    const pc_surface *surf;
    /* ITEM_ADJUST */
    ink_kind ikind;
    const pc_adjustment *adj; /* INK_RULES, INK_CMY */
    float *tables;            /* INK_TABLES: INKS rows of LUT, owned */
} ink_item;

typedef struct ink_stack {
    ink_item *items;
    size_t n;
    size_t cap;
    pc_sample_type depth;
} ink_stack;

static void ink_stack_fini(ink_stack *st)
{
    size_t i;
    for (i = 0; i < st->n; i++) {
        if (st->items[i].kind == ITEM_ADJUST && st->items[i].ikind == INK_TABLES) {
            free(st->items[i].tables);
        }
    }
    free(st->items);
    st->items = NULL;
    st->n = st->cap = 0;
}

static bool stack_push(ink_stack *st, ink_item it)
{
    if (st->n == st->cap) {
        ink_item *grown = pc_grow_array(st->items, &st->cap, st->n + 1, sizeof *grown);
        if (grown == NULL) {
            return false;
        }
        st->items = grown;
    }
    st->items[st->n++] = it;
    return true;
}

/* The ink evaluation of an adjustment layer the native path can composite: fills *out, returns
 * true, or returns false when the adjustment needs the RGB compositor. *fail stays false unless
 * out of memory. */
static bool ink_op(const pc_layer *layer, pc_sample_type depth, ink_item *out, bool *fail)
{
    const pc_adjustment *adj;
    if (layer->content.kind != PC_LAYER_ADJUSTMENT || !simple(layer)) {
        return false;
    }
    adj = &layer->content.u.adjustment;
    out->adj = adj;
    out->tables = NULL;
    switch (adj->kind) {
    case PC_ADJ_INVERT:
    case PC_ADJ_POSTERIZE:
    case PC_ADJ_THRESHOLD:
        out->ikind = INK_RULES;
        return true;
    case PC_ADJ_BRIGHTNESS_CONTRAST: {
        float *t = pc_malloc_array((size_t)INKS * LUT, sizeof *t);
        int c;
        if (t == NULL) {
            *fail = true;
            return false;
        }
        if (pc_cmp_ink_bc_lut(adj->u.brightness_contrast.brightness, adj->u.brightness_contrast.contrast,
                              adj->u.brightness_contrast.legacy, t, NULL) != PC_OK) {
            free(t);
            *fail = true;
            return false;
        }
        for (c = 1; c < INKS; c++) {
            memcpy(t + (size_t)c * LUT, t, LUT * sizeof *t);
        }
        out->ikind = INK_TABLES;
        out->tables = t;
        return true;
    }
    default:
        break;
    }
    if (pc_cmp_cmy_adjustment(adj)) {
        out->ikind = INK_CMY;
        return true;
    }
    if ((adj->kind == PC_ADJ_LEVELS && adj->u.levels.space == PC_TONE_CMYK) ||
        (adj->kind == PC_ADJ_CURVES && adj->u.curves.space == PC_TONE_CMYK)) {
        float *t = pc_malloc_array((size_t)INKS * LUT, sizeof *t);
        if (t == NULL) {
            *fail = true;
            return false;
        }
        if (pc_compose_tone_luts_depth(adj, true, depth, t, NULL) != PC_OK) {
            free(t);
            *fail = true;
            return false;
        }
        out->ikind = INK_TABLES;
        out->tables = t;
        return true;
    }
    return false;
}

/* `layers`' visible steps, appended to `st`; false when one needs the RGB compositor. A group at
 * full opacity without a mask is inlined. *fail is set on OOM. */
static bool collect(const pc_layer *layers, size_t nlayers, pc_sample_type depth, bool adjustments,
                    ink_stack *st, size_t level, bool *fail)
{
    size_t i;
    if (level > NATIVE_MAX_DEPTH) {
        return false;
    }
    for (i = 0; i < nlayers; i++) {
        const pc_layer *layer = &layers[i];
        float alpha;
        const pc_layer_mask *mask;
        const pc_surface *ps;
        ink_item it;
        if (!layer->visible) {
            continue;
        }
        alpha = pc_clampf(layer->opacity * layer->fill_opacity, 0.0f, 1.0f);
        mask = (layer->has_mask && layer->mask.enabled) ? &layer->mask : NULL;
        if (layer->content.kind == PC_LAYER_GROUP) {
            const pc_group *g = &layer->content.u.group;
            bool open = !g->has_artboard && alpha >= 1.0f && mask == NULL && !layer->clipped &&
                        !(layer->has_vector_mask && layer->vector_mask.enabled) &&
                        !pc_compose_has_effects(layer) && layer->excluded_channels == 0 &&
                        pc_blend_if_is_default(&layer->blend_if) &&
                        pc_advanced_blending_is_default(&layer->advanced);
            if (layer->blend == PC_BLEND_PASS_THROUGH && open) {
                if (!collect(g->children, g->nchildren, depth, adjustments, st, level + 1, fail)) {
                    return false;
                }
            } else if (layer->blend == PC_BLEND_NORMAL && open) {
                if (!collect(g->children, g->nchildren, depth, false, st, level + 1, fail)) {
                    return false;
                }
            } else {
                return false;
            }
            continue;
        }
        memset(&it, 0, sizeof it);
        it.opacity = alpha;
        it.mask = mask;
        ps = plain(layer);
        if (ps != NULL) {
            it.kind = ITEM_LAYER;
            it.surf = ps;
        } else if (adjustments && ink_op(layer, depth, &it, fail)) {
            it.kind = ITEM_ADJUST;
        } else {
            return false;
        }
        if (!stack_push(st, it)) {
            if (it.kind == ITEM_ADJUST && it.ikind == INK_TABLES) {
                free(it.tables);
            }
            *fail = true;
            return false;
        }
    }
    return true;
}

/* None unless `doc` is CMYK and every visible layer is a plain raster layer, a plain adjustment
 * layer the inks carry exactly, or a group that can be inlined. *fail on OOM. */
static bool ink_stack_new(const pc_document *doc, ink_stack *st, bool *fail)
{
    memset(st, 0, sizeof *st);
    st->depth = doc->depth;
    *fail = false;
    if (doc->mode != PC_COLOR_MODE_CMYK) {
        return false;
    }
    if (!collect(doc->layers, doc->nlayers, doc->depth, true, st, 0, fail)) {
        ink_stack_fini(st);
        return false;
    }
    return true;
}

static bool stack_adjusts(const ink_stack *st)
{
    size_t i;
    for (i = 0; i < st->n; i++) {
        if (st->items[i].kind == ITEM_ADJUST) {
            return true;
        }
    }
    return false;
}

static bool default_all_zero(const pc_surface *s)
{
    float dp[PC_RASTER_MAX_CHANNELS];
    size_t n = pc_surface_channels(s);
    size_t i;
    pc_surface_default_pixel(s, dp);
    for (i = 0; i < n; i++) {
        if (dp[i] != 0.0f) {
            return false;
        }
    }
    return true;
}

/* The adjusted inks of each covered pixel of `acc` (its fifth value is the coverage) into out. */
static pc_status ink_apply(const ink_item *op, float (*acc)[INKS + 1], size_t npx,
                           pc_sample_type depth, float (*out)[INKS], pc_error *err)
{
    size_t i;
    int c;
    for (i = 0; i < npx; i++) {
        for (c = 0; c < INKS; c++) {
            out[i][c] = acc[i][c];
        }
    }
    switch (op->ikind) {
    case INK_TABLES:
        for (i = 0; i < npx; i++) {
            for (c = 0; c < INKS; c++) {
                out[i][c] = pc_cmp_ink_through(op->tables + (size_t)c * LUT, out[i][c]);
            }
        }
        return PC_OK;
    case INK_RULES:
        for (i = 0; i < npx; i++) {
            pc_cmp_stored_channels(op->adj, PC_COLOR_MODE_CMYK, true, depth, out[i], INKS,
                                   pc_cmp_ink_luma(out[i]));
        }
        return PC_OK;
    case INK_CMY:
        return pc_cmp_apply_cmy(op->adj, out, npx, depth, err);
    }
    return PC_OK;
}

/* One band of the stack: straight C, M, Y, K, alpha per pixel in acc (zeroed by the caller), over
 * `band`. src (npx*PC_RASTER_MAX_CHANNELS), mv (npx) and adj (npx) are scratch. */
static pc_status mix_band(const ink_stack *st, pc_rect band, float (*acc)[INKS + 1], size_t npx,
                          float *src, float *mv, float (*adj)[INKS], pc_error *err)
{
    size_t l;
    size_t i;
    pc_status status;
    for (l = 0; l < st->n; l++) {
        const ink_item *item = &st->items[l];
        if (item->opacity <= 0.0f) {
            continue;
        }
        if (item->kind == ITEM_LAYER) {
            const pc_surface *surf = item->surf;
            pc_pixel_format sf = pc_surface_format(surf);
            size_t sn = pc_pixel_format_channels(sf);
            if (pc_rect_is_empty(pc_rect_intersect(pc_surface_content_bounds(surf), band)) &&
                default_all_zero(surf)) {
                continue;
            }
            status = pc_surface_read_region_into(surf, band, src, npx * sn, err);
            if (status != PC_OK) {
                return status;
            }
            if (item->mask != NULL) {
                status = pc_layer_mask_values_into(item->mask, band, mv, npx, err);
                if (status != PC_OK) {
                    return status;
                }
            }
            for (i = 0; i < npx; i++) {
                const float *s = src + i * sn;
                float *d = acc[i];
                float k = item->mask != NULL ? pc_clampf(mv[i], 0.0f, 1.0f) : 1.0f;
                float sa = (sf.alpha && sn > INKS ? s[INKS] : 1.0f) * item->opacity * k;
                float oa;
                float keep;
                int c;
                if (sa <= 0.0f) {
                    continue;
                }
                oa = sa + d[INKS] * (1.0f - sa);
                keep = d[INKS] * (1.0f - sa);
                for (c = 0; c < INKS; c++) {
                    /* An opaque pixel replaces what is below exactly (no rounding through the mix). */
                    d[c] = sa >= 1.0f ? s[c] : (s[c] * sa + d[c] * keep) / oa;
                }
                d[INKS] = oa;
            }
        } else {
            if (item->mask != NULL) {
                status = pc_layer_mask_values_into(item->mask, band, mv, npx, err);
                if (status != PC_OK) {
                    return status;
                }
            }
            status = ink_apply(item, acc, npx, st->depth, adj, err);
            if (status != PC_OK) {
                return status;
            }
            for (i = 0; i < npx; i++) {
                float *d = acc[i];
                float k = item->mask != NULL ? pc_clampf(mv[i], 0.0f, 1.0f) : 1.0f;
                float mix = item->opacity * k;
                int c;
                if (d[INKS] <= 0.0f || mix <= 0.0f) {
                    continue;
                }
                for (c = 0; c < INKS; c++) {
                    d[c] += (adj[i][c] - d[c]) * mix;
                }
            }
        }
    }
    return PC_OK;
}

/* Scratch for one band of the native CMYK path. */
typedef struct native_scratch {
    float (*acc)[INKS + 1];
    float *src;
    float *mv;
    float (*adj)[INKS];
} native_scratch;

static void scratch_fini(native_scratch *s)
{
    free(s->acc);
    free(s->src);
    free(s->mv);
    free(s->adj);
    memset(s, 0, sizeof *s);
}

static pc_status scratch_init(native_scratch *s, size_t band_px, pc_error *err)
{
    memset(s, 0, sizeof *s);
    s->acc = pc_malloc_array(band_px, sizeof *s->acc);
    s->src = pc_malloc_array(band_px, PC_RASTER_MAX_CHANNELS * sizeof *s->src);
    s->mv = pc_malloc_array(band_px, sizeof *s->mv);
    s->adj = pc_malloc_array(band_px, sizeof *s->adj);
    if (s->acc == NULL || s->src == NULL || s->mv == NULL || s->adj == NULL) {
        scratch_fini(s);
        return nomem(err);
    }
    return PC_OK;
}

pc_status pc_compose_flatten_cmyk(const pc_document *doc, pc_rect rect, pc_pixel_format fmt, bool paper,
                                  bool *done, pc_surface *out, pc_error *err)
{
    ink_stack st;
    bool fail = false;
    size_t n;
    size_t w;
    int32_t rows;
    int32_t y;
    native_scratch sc;
    float *data = NULL;
    size_t band_px;
    pc_surface s;
    pc_status status = PC_OK;
    *done = false;
    if (fmt.mode != PC_COLOR_MODE_CMYK) {
        return PC_OK;
    }
    if (!ink_stack_new(doc, &st, &fail)) {
        return fail ? nomem(err) : PC_OK;
    }
    pc_surface_init(&s, fmt);
    if (pc_rect_is_empty(rect)) {
        ink_stack_fini(&st);
        *out = s;
        *done = true;
        return PC_OK;
    }
    n = pc_pixel_format_channels(fmt);
    w = pc_cmp_w(rect);
    rows = pc_compose_band_rows_for(pc_rect_width(rect), 0);
    y = pc_i32_sat_add(rect.y0, rows);
    status =
        pc_cmp_rect_pixels(pc_rect_make(rect.x0, rect.y0, rect.x1, y < rect.y1 ? y : rect.y1), &band_px, err);
    memset(&sc, 0, sizeof sc);
    if (status == PC_OK) {
        status = scratch_init(&sc, band_px, err);
    }
    if (status == PC_OK) {
        data = pc_malloc_array(band_px, PC_RASTER_MAX_CHANNELS * sizeof *data);
        if (data == NULL) {
            status = nomem(err);
        }
    }
    y = rect.y0;
    while (status == PC_OK && y < rect.y1) {
        int32_t y1 = pc_i32_sat_add(y, rows);
        pc_rect band;
        size_t npx;
        size_t i;
        if (y1 > rect.y1) {
            y1 = rect.y1;
        }
        band = pc_rect_make(rect.x0, y, rect.x1, y1);
        npx = w * (size_t)pc_rect_height(band);
        memset(sc.acc, 0, npx * sizeof *sc.acc);
        status = mix_band(&st, band, sc.acc, npx, sc.src, sc.mv, sc.adj, err);
        for (i = 0; status == PC_OK && i < npx; i++) {
            /* White paper carries no ink, so flattening onto it scales the ink by coverage. */
            const float *p = sc.acc[i];
            float *o = data + i * n;
            float a = p[INKS];
            float k = paper && a < 1.0f ? a : 1.0f;
            int c;
            for (c = 0; c < INKS; c++) {
                o[c] = p[c] * k;
            }
            if (fmt.alpha) {
                o[INKS] = paper ? 1.0f : a;
            }
        }
        if (status == PC_OK) {
            status = pc_surface_write_region(&s, band, data, npx * n, err);
        }
        y = y1;
    }
    ink_stack_fini(&st);
    scratch_fini(&sc);
    free(data);
    if (status != PC_OK) {
        pc_surface_fini(&s);
        return status;
    }
    pc_surface_prune(&s);
    *out = s;
    *done = true;
    return PC_OK;
}

pc_status pc_compose_render_cmyk(const pc_document *doc, pc_rect rect, int32_t tile, bool *used,
                                 pc_compose_buffer *out, pc_error *err)
{
    ink_stack st;
    bool fail = false;
    pc_cmyk_space *cmyk;
    pc_cmyk_space *prev;
    size_t w;
    int32_t rows;
    int32_t y;
    native_scratch sc;
    size_t band_px;
    pc_compose_buffer buf;
    pc_status status;
    *used = false;
    if (!ink_stack_new(doc, &st, &fail)) {
        return fail ? nomem(err) : PC_OK;
    }
    if (!stack_adjusts(&st)) {
        /* A stack of raster layers alone keeps the RGB compositor. */
        ink_stack_fini(&st);
        return PC_OK;
    }
    status = pc_compose_buffer_transparent(rect, &buf, err);
    if (status != PC_OK) {
        ink_stack_fini(&st);
        return status;
    }
    if (pc_rect_is_empty(rect) || buf.len == 0) {
        ink_stack_fini(&st);
        *out = buf;
        *used = true;
        return PC_OK;
    }
    w = pc_cmp_w(rect);
    rows = tile > 0 ? tile : 1;
    y = pc_i32_sat_add(rect.y0, rows);
    status =
        pc_cmp_rect_pixels(pc_rect_make(rect.x0, rect.y0, rect.x1, y < rect.y1 ? y : rect.y1), &band_px, err);
    memset(&sc, 0, sizeof sc);
    if (status == PC_OK) {
        status = scratch_init(&sc, band_px, err);
    }
    cmyk = pc_compose_cmyk_space(doc);
    prev = pc_cmyk_space_enter(cmyk);
    y = rect.y0;
    while (status == PC_OK && y < rect.y1) {
        int32_t y1 = pc_i32_sat_add(y, rows);
        pc_rect band;
        size_t npx;
        size_t i;
        size_t base;
        if (y1 > rect.y1) {
            y1 = rect.y1;
        }
        band = pc_rect_make(rect.x0, y, rect.x1, y1);
        npx = w * (size_t)pc_rect_height(band);
        base = (size_t)(y - rect.y0) * w;
        memset(sc.acc, 0, npx * sizeof *sc.acc);
        status = mix_band(&st, band, sc.acc, npx, sc.src, sc.mv, sc.adj, err);
        for (i = 0; status == PC_OK && i < npx && base + i < buf.len; i++) {
            const float *p = sc.acc[i];
            if (p[INKS] > 0.0f) {
                float ink[4] = {p[0], p[1], p[2], p[3]};
                float rgb[3];
                pc_cmyk_to_rgb(ink, rgb);
                buf.px[base + i][0] = rgb[0];
                buf.px[base + i][1] = rgb[1];
                buf.px[base + i][2] = rgb[2];
                buf.px[base + i][3] = p[INKS];
            }
        }
        y = y1;
    }
    pc_cmyk_space_leave(prev);
    pc_cmyk_space_release(cmyk);
    ink_stack_fini(&st);
    scratch_fini(&sc);
    if (status != PC_OK) {
        pc_compose_buffer_fini(&buf);
        return status;
    }
    *out = buf;
    *used = true;
    return PC_OK;
}
