/* Lab-native compositing of plain Lab layer stacks (port of crates/compose/src/native_lab.rs).
 *
 * The compositor works in display RGB, so a Lab document's layers are read through lab_to_srgb,
 * which clips colours outside the display gamut, and adjustments Photoshop evaluates on L, a and b
 * can then only see those clipped colours. When every visible layer is a plain Lab raster layer in
 * Normal mode or such an adjustment layer, the stack is worked out on the stored values instead:
 * Normal mixes L, a and b linearly, as Photoshop does in Lab, and an opaque layer keeps them
 * exactly. A layer mask (unfeathered) scales the mix. Stacks with such adjustment layers render
 * for display this way (pc_compose_render_lab). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "color/convert.h"
#include "compose/adjust.h"
#include "compose/adjust_internal.h"
#include "compose/internal.h"
#include "compose/masks.h"

/* Colour channels of a Lab pixel. */
#define LAB 3
#define LUT PC_COMPOSE_LUT_SIZE
/* Neutral a or b as Lab surfaces store it ((0 + 128) / 255). */
#define LAB_NEUTRAL (128.0f / 255.0f)
#define LAB_MAX_DEPTH 64

static pc_status nomem(pc_error *err)
{
    return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
}

/* The per-pixel Lab adjustment an item applies (adjust.rs lab_stored_op). */
typedef enum { LABOP_RULES, LABOP_THRESH, LABOP_BC, LABOP_TONE } lab_kind;

typedef enum { LITEM_LAYER, LITEM_ADJUST } litem_kind;

typedef struct lab_item {
    litem_kind kind;
    float opacity;
    const pc_layer_mask *mask; /* NULL: none */
    /* LITEM_LAYER */
    const pc_surface *surf;
    /* LITEM_ADJUST */
    lab_kind lkind;
    const pc_adjustment *adj; /* LABOP_RULES */
    bool has_depth;
    pc_sample_type depth;
    float thresh;                 /* LABOP_THRESH */
    bool bc_legacy;               /* LABOP_BC */
    float bc_b;
    float bc_c;
    pc_cmp_legacy_bc bc_curve;
    float *tone; /* LABOP_TONE: LAB rows of LUT, owned */
} lab_item;

typedef struct lab_stack {
    lab_item *items;
    size_t n;
    size_t cap;
    bool has_quantum;
    float quantum;
} lab_stack;

static void lab_stack_fini(lab_stack *st)
{
    size_t i;
    for (i = 0; i < st->n; i++) {
        if (st->items[i].kind == LITEM_ADJUST && st->items[i].lkind == LABOP_TONE) {
            free(st->items[i].tone);
        }
    }
    free(st->items);
    memset(st, 0, sizeof *st);
}

static bool stack_push(lab_stack *st, lab_item it)
{
    if (st->n == st->cap) {
        lab_item *grown = pc_grow_array(st->items, &st->cap, st->n + 1, sizeof *grown);
        if (grown == NULL) {
            return false;
        }
        st->items = grown;
    }
    st->items[st->n++] = it;
    return true;
}

static bool simple(const pc_layer *layer)
{
    return layer->blend == PC_BLEND_NORMAL && !layer->clipped && !pc_compose_has_feather(layer) &&
           !(layer->has_vector_mask && layer->vector_mask.enabled) && !pc_compose_has_effects(layer) &&
           layer->excluded_channels == 0 && pc_blend_if_is_default(&layer->blend_if) &&
           pc_advanced_blending_is_default(&layer->advanced) && !layer->has_video;
}

/* The Lab evaluation of an adjustment layer the native path can composite: fills *out, returns
 * true, or false when the adjustment needs the RGB compositor. *fail on OOM. */
static bool lab_op(const pc_layer *layer, pc_sample_type depth, lab_item *out, bool *fail)
{
    const pc_adjustment *adj;
    if (layer->content.kind != PC_LAYER_ADJUSTMENT || !simple(layer)) {
        return false;
    }
    adj = &layer->content.u.adjustment;
    out->adj = adj;
    out->has_depth = true;
    out->depth = depth;
    out->tone = NULL;
    switch (adj->kind) {
    case PC_ADJ_INVERT:
    case PC_ADJ_POSTERIZE:
        out->lkind = LABOP_RULES;
        return true;
    case PC_ADJ_THRESHOLD:
        out->lkind = LABOP_THRESH;
        out->thresh = roundf(adj->u.threshold.level * 255.0f);
        return true;
    case PC_ADJ_BRIGHTNESS_CONTRAST:
        out->lkind = LABOP_BC;
        out->bc_legacy = adj->u.brightness_contrast.legacy;
        out->bc_b = adj->u.brightness_contrast.brightness;
        out->bc_c = adj->u.brightness_contrast.contrast;
        out->bc_curve = pc_cmp_legacy_bc_new(out->bc_b, out->bc_c);
        return true;
    case PC_ADJ_LEVELS:
        if (adj->u.levels.space != PC_TONE_LAB) {
            return false;
        }
        break;
    case PC_ADJ_CURVES:
        if (adj->u.curves.space != PC_TONE_LAB) {
            return false;
        }
        break;
    default:
        return false;
    }
    {
        /* Levels or Curves with a Lab record: the per-channel LUTs applied to L, a and b. */
        float *t = pc_malloc_array((size_t)4 * LUT, sizeof *t);
        if (t == NULL) {
            *fail = true;
            return false;
        }
        if (pc_compose_tone_luts_depth(adj, true, depth, t, NULL) != PC_OK) {
            free(t);
            *fail = true;
            return false;
        }
        out->lkind = LABOP_TONE;
        out->tone = t;
        return true;
    }
}

/* One pixel's stored L, a, b through a Lab adjustment op, in place. */
static void lab_apply(const lab_item *op, float ch[3])
{
    int c;
    switch (op->lkind) {
    case LABOP_RULES:
        pc_cmp_stored_channels(op->adj, PC_COLOR_MODE_LAB, op->has_depth, op->depth, ch, LAB, 0.0f);
        break;
    case LABOP_THRESH: {
        float white = roundf(pc_clampf(ch[0], 0.0f, 1.0f) * 255.0f) >= op->thresh ? 1.0f : 0.0f;
        ch[0] = white;
        ch[1] = LAB_NEUTRAL;
        ch[2] = LAB_NEUTRAL;
        break;
    }
    case LABOP_BC:
        ch[0] = op->bc_legacy
                    ? pc_cmp_legacy_bc_apply(&op->bc_curve, ch[0])
                    : pc_clampf(pc_compose_modern_contrast(pc_compose_modern_brightness(ch[0], op->bc_b),
                                                           op->bc_c),
                                0.0f, 1.0f);
        break;
    case LABOP_TONE:
        for (c = 0; c < LAB; c++) {
            ch[c] = pc_cmp_lut(op->tone + (size_t)c * LUT, LUT, ch[c]);
        }
        break;
    }
}

/* `layers`' visible steps, appended to `st`; false when one needs the RGB compositor. */
static bool collect(const pc_layer *layers, size_t nlayers, pc_sample_type depth, bool adjustments,
                    lab_stack *st, size_t level, bool *fail)
{
    size_t i;
    if (level > LAB_MAX_DEPTH) {
        return false;
    }
    for (i = 0; i < nlayers; i++) {
        const pc_layer *layer = &layers[i];
        float alpha;
        const pc_layer_mask *mask;
        lab_item it;
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
        if (layer->content.kind == PC_LAYER_RASTER && simple(layer) &&
            pc_surface_format(&layer->content.u.raster).mode == PC_COLOR_MODE_LAB) {
            it.kind = LITEM_LAYER;
            it.surf = &layer->content.u.raster;
        } else if (adjustments && lab_op(layer, depth, &it, fail)) {
            it.kind = LITEM_ADJUST;
        } else {
            return false;
        }
        if (!stack_push(st, it)) {
            if (it.kind == LITEM_ADJUST && it.lkind == LABOP_TONE) {
                free(it.tone);
            }
            *fail = true;
            return false;
        }
    }
    return true;
}

static bool lab_stack_new(const pc_document *doc, lab_stack *st, bool *fail)
{
    memset(st, 0, sizeof *st);
    *fail = false;
    if (doc->mode != PC_COLOR_MODE_LAB) {
        return false;
    }
    if (!collect(doc->layers, doc->nlayers, doc->depth, true, st, 0, fail)) {
        lab_stack_fini(st);
        return false;
    }
    st->has_quantum = pc_compose_adjustment_quantum(doc->depth, &st->quantum);
    return true;
}

static bool stack_adjusts(const lab_stack *st)
{
    size_t i;
    for (i = 0; i < st->n; i++) {
        if (st->items[i].kind == LITEM_ADJUST) {
            return true;
        }
    }
    return false;
}

static bool default_alpha_zero(const pc_surface *s)
{
    float dp[PC_RASTER_MAX_CHANNELS];
    size_t n = pc_surface_channels(s);
    pc_surface_default_pixel(s, dp);
    return n <= LAB || dp[LAB] <= 0.0f;
}

/* One band of the stack: straight L, a, b, alpha per pixel in acc (reset by the caller to the
 * neutral paper), over `band`. src and mv are scratch. */
static pc_status mix_band(const lab_stack *st, pc_rect band, float (*acc)[LAB + 1], size_t npx,
                          float *src, float *mv, pc_error *err)
{
    size_t l;
    size_t i;
    pc_status status;
    for (l = 0; l < st->n; l++) {
        const lab_item *item = &st->items[l];
        if (item->opacity <= 0.0f) {
            continue;
        }
        if (item->mask != NULL) {
            status = pc_layer_mask_values_into(item->mask, band, mv, npx, err);
            if (status != PC_OK) {
                return status;
            }
        }
        if (item->kind == LITEM_LAYER) {
            const pc_surface *surf = item->surf;
            pc_pixel_format sf = pc_surface_format(surf);
            size_t sn = pc_pixel_format_channels(sf);
            if (pc_rect_is_empty(pc_rect_intersect(pc_surface_content_bounds(surf), band)) &&
                default_alpha_zero(surf)) {
                continue;
            }
            status = pc_surface_read_region_into(surf, band, src, npx * sn, err);
            if (status != PC_OK) {
                return status;
            }
            for (i = 0; i < npx; i++) {
                const float *s = src + i * sn;
                float *d = acc[i];
                float k = item->mask != NULL ? pc_clampf(mv[i], 0.0f, 1.0f) : 1.0f;
                float sa = (sf.alpha && sn > LAB ? s[LAB] : 1.0f) * item->opacity * k;
                float oa;
                float keep;
                int c;
                if (sa <= 0.0f) {
                    continue;
                }
                oa = sa + d[LAB] * (1.0f - sa);
                keep = d[LAB] * (1.0f - sa);
                for (c = 0; c < LAB; c++) {
                    d[c] = sa >= 1.0f ? s[c] : (s[c] * sa + d[c] * keep) / oa;
                }
                d[LAB] = oa;
            }
        } else {
            for (i = 0; i < npx; i++) {
                float *d = acc[i];
                float k = item->mask != NULL ? pc_clampf(mv[i], 0.0f, 1.0f) : 1.0f;
                float mix = item->opacity * k;
                float lab[3];
                int c;
                if (d[LAB] <= 0.0f || mix <= 0.0f) {
                    continue;
                }
                lab[0] = d[0];
                lab[1] = d[1];
                lab[2] = d[2];
                lab_apply(item, lab);
                for (c = 0; c < LAB; c++) {
                    d[c] += (lab[c] - d[c]) * mix;
                    if (st->has_quantum) {
                        d[c] = floorf(d[c] * st->quantum + 0.5f) / st->quantum;
                    }
                }
            }
        }
    }
    return PC_OK;
}

pc_status pc_compose_render_lab(const pc_document *doc, pc_rect rect, int32_t tile, bool *used,
                                pc_compose_buffer *out, pc_error *err)
{
    lab_stack st;
    bool fail = false;
    size_t w;
    int32_t rows;
    int32_t y;
    float (*acc)[LAB + 1] = NULL;
    float *src = NULL;
    float *mv = NULL;
    size_t band_px;
    pc_compose_buffer buf;
    pc_status status;
    *used = false;
    if (!lab_stack_new(doc, &st, &fail)) {
        return fail ? nomem(err) : PC_OK;
    }
    if (!stack_adjusts(&st)) {
        lab_stack_fini(&st);
        return PC_OK;
    }
    status = pc_compose_buffer_transparent(rect, &buf, err);
    if (status != PC_OK) {
        lab_stack_fini(&st);
        return status;
    }
    if (pc_rect_is_empty(rect) || buf.len == 0) {
        lab_stack_fini(&st);
        *out = buf;
        *used = true;
        return PC_OK;
    }
    w = pc_cmp_w(rect);
    rows = tile > 0 ? tile : 1;
    y = pc_i32_sat_add(rect.y0, rows);
    status =
        pc_cmp_rect_pixels(pc_rect_make(rect.x0, rect.y0, rect.x1, y < rect.y1 ? y : rect.y1), &band_px, err);
    if (status == PC_OK) {
        acc = pc_malloc_array(band_px, sizeof *acc);
        src = pc_malloc_array(band_px, PC_RASTER_MAX_CHANNELS * sizeof *src);
        mv = pc_malloc_array(band_px, sizeof *mv);
        if (acc == NULL || src == NULL || mv == NULL) {
            status = nomem(err);
        }
    }
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
        for (i = 0; i < npx; i++) {
            acc[i][0] = 0.0f;
            acc[i][1] = LAB_NEUTRAL;
            acc[i][2] = LAB_NEUTRAL;
            acc[i][3] = 0.0f;
        }
        status = mix_band(&st, band, acc, npx, src, mv, err);
        for (i = 0; status == PC_OK && i < npx && base + i < buf.len; i++) {
            const float *p = acc[i];
            if (p[LAB] > 0.0f) {
                float lab[3] = {p[0] * 100.0f, p[1] * 255.0f - 128.0f, p[2] * 255.0f - 128.0f};
                float rgb[3];
                pc_lab_to_srgb(lab, rgb);
                buf.px[base + i][0] = rgb[0];
                buf.px[base + i][1] = rgb[1];
                buf.px[base + i][2] = rgb[2];
                buf.px[base + i][3] = p[LAB];
            }
        }
        y = y1;
    }
    lab_stack_fini(&st);
    free(acc);
    free(src);
    free(mv);
    if (status != PC_OK) {
        pc_compose_buffer_fini(&buf);
        return status;
    }
    *out = buf;
    *used = true;
    return PC_OK;
}
