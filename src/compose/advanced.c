/* Layer Style > Blending Options > Advanced Blending in the CPU compositor (port of
 * crates/compose/src/advanced.rs):
 *
 * - Knockout. Inside the layer's shape the backdrop is replaced by the knockout target before the
 *   layer composites: for Shallow the bottom of the enclosing group (the backdrop the group's
 *   stack started from) or of the clipping group (the base layer's content); for Deep, and for
 *   Shallow at the top level, the Background layer (transparency without one). Isolated groups
 *   and grouped clipping layers stop a deep knockout at their bottom; pass-through groups let it
 *   through. Fill opacity sets how much of the layer shows over the knocked-out area, layer
 *   opacity how strong the whole knockout is.
 * - Blend Clipped Layers as Group off. The base composites alone; each clipped layer then
 *   composites onto the result in its own mode, kept inside the base's shape.
 * - Transparency Shapes Layer off. The layer's effects and knockout cover the whole layer.
 * - Layer / Vector Mask Hides Effects. The effects are built from the layer without that mask,
 *   and the mask is applied to the finished layer and effects.
 *
 * Every path here is skipped for layers at Photoshop's defaults. */
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "compose/internal.h"

static pc_status nomem(pc_error *err)
{
    return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
}

/* A knockout's shape and an individually blended clipping base render the layer's content once
 * more, on top of compositing it: nested inside each other (groups of such groups) the work
 * doubles per level, which a hostile file can make astronomical (the reference just runs on).
 * Real documents nest them a level or two deep; past MAX_EXTRA_NESTING it is PC_ERR_LIMIT. Each
 * tile renders on one thread, so the count is per thread. */
#define MAX_EXTRA_NESTING 8
static _Thread_local unsigned extra_nesting;

static pc_status extra_content(const pc_layer *layer, pc_rect rect, const pc_cmp_ctx *cx, bool *some,
                               pc_compose_buffer *out, pc_error *err)
{
    pc_status st;
    if (extra_nesting >= MAX_EXTRA_NESTING) {
        return pc_fail(err, PC_ERR_LIMIT,
                       "compose: knockouts or individually blended clipping groups nested "
                       "deeper than %d",
                       MAX_EXTRA_NESTING);
    }
    extra_nesting++;
    st = pc_cmp_render_content(layer, rect, cx, some, out, err);
    extra_nesting--;
    return st;
}

/* Premultiplied linear interpolation between two straight-alpha pixels (a and out may alias). */
static void mix(const float a[4], const float b[4], float k, float out[4])
{
    float alpha = a[3] + (b[3] - a[3]) * k;
    float r[3];
    int c;
    if (alpha <= 0.0f) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    for (c = 0; c < 3; c++) {
        r[c] = pc_clampf((a[c] * a[3] + (b[c] * b[3] - a[c] * a[3]) * k) / alpha, 0.0f, 1.0f);
    }
    out[0] = r[0];
    out[1] = r[1];
    out[2] = r[2];
    out[3] = pc_rs_minf_k(alpha, 1.0f);
}

/* ---- predicates ---- */

bool pc_cmp_hides_effects(const pc_layer *layer)
{
    const pc_advanced_blending *a = &layer->advanced;
    return pc_compose_has_effects(layer) &&
           ((a->layer_mask_hides_effects && layer->has_mask && layer->mask.enabled) ||
            (a->vector_mask_hides_effects && layer->has_vector_mask && layer->vector_mask.enabled));
}

static bool any_visible(const pc_layer *ls, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (ls[i].visible) {
            return true;
        }
    }
    return false;
}

bool pc_cmp_clips_individually(const pc_layer *layer, const pc_layer *clipped, size_t n)
{
    /* Adjustment layers keep the grouped path (they have no pixels of their own to clip to). */
    return !layer->advanced.blend_clipped && layer->content.kind != PC_LAYER_ADJUSTMENT &&
           any_visible(clipped, n);
}

bool pc_compose_advanced_active(const pc_layer *layer, const pc_layer *clipped, size_t nclipped)
{
    const pc_advanced_blending *a = &layer->advanced;
    bool fx;
    if (pc_advanced_blending_is_default(a)) {
        return false;
    }
    fx = pc_compose_has_effects(layer);
    return a->knockout != PC_KNOCKOUT_NONE || pc_cmp_clips_individually(layer, clipped, nclipped) ||
           (a->blend_interior && fx) || (!a->transparency_shapes && fx) || pc_cmp_hides_effects(layer);
}

bool pc_compose_shapeless(const pc_layer *layer)
{
    return !layer->advanced.transparency_shapes &&
           (layer->advanced.knockout != PC_KNOCKOUT_NONE || pc_compose_has_effects(layer));
}

bool pc_compose_is_background(const pc_layer *layer)
{
    /* By its locks and kind, never by its (localized) name. */
    return pc_layer_has_background_traits(layer);
}

bool pc_cmp_needs_floor(const pc_layer *layers, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (layers[i].visible && !layers[i].clipped && layers[i].advanced.knockout == PC_KNOCKOUT_SHALLOW) {
            return true;
        }
    }
    return false;
}

/* ---- knockout ---- */

/* What a deep knockout reveals over rect: the document's initial backdrop with its Background
 * layer (transparency when it has none, or when it is hidden). */
static pc_status deep_target(const pc_cmp_ctx *cx, pc_rect rect, pc_compose_buffer *out, pc_error *err)
{
    const pc_document *doc = cx->doc;
    pc_compose_buffer b;
    pc_status st;
    if (doc == NULL) {
        return pc_compose_buffer_transparent(rect, out, err);
    }
    st = pc_cmp_backdrop(doc, rect, &b, err);
    if (st != PC_OK) {
        return st;
    }
    if (doc->nlayers > 0 && doc->layers[0].visible && pc_compose_is_background(&doc->layers[0])) {
        /* No knockout target here, so a Background that knocks out can't recurse. */
        st = pc_cmp_composite_layer(&doc->layers[0], NULL, 0, &b, cx, NULL, PC_CMP_SCOPE_ROOT, err);
        if (st != PC_OK) {
            pc_compose_buffer_fini(&b);
            return st;
        }
    }
    *out = b;
    return PC_OK;
}

static pc_status deep(const pc_cmp_ctx *cx, pc_cmp_scope scope, pc_rect rect, pc_compose_buffer *out,
                      pc_error *err)
{
    if (scope.isolated) {
        return pc_compose_buffer_transparent(rect, out, err);
    }
    return deep_target(cx, rect, out, err);
}

pc_status pc_cmp_knockout_target(const pc_layer *layer, const pc_compose_buffer *floor, pc_rect rect,
                                 const pc_cmp_ctx *cx, pc_cmp_scope scope, pc_compose_buffer *owned,
                                 const pc_compose_buffer **target, pc_error *err)
{
    pc_status st;
    switch (layer->advanced.knockout) {
    case PC_KNOCKOUT_NONE:
        *target = NULL;
        return PC_OK;
    case PC_KNOCKOUT_SHALLOW:
        if (floor != NULL) {
            *target = floor;
            return PC_OK;
        }
        break;
    default:
        break;
    }
    st = deep(cx, scope, rect, owned, err);
    if (st == PC_OK) {
        *target = owned;
    }
    return st;
}

/* The layer's masks over rect, or all ones without any. */
static pc_status masks_or_ones(const pc_layer *layer, pc_rect rect, const pc_cmp_ctx *cx, size_t n,
                               float **out, pc_error *err)
{
    float *m = NULL;
    size_t i;
    pc_status st = pc_cmp_mask_vals(layer, rect, cx, &m, err);
    if (st != PC_OK) {
        return st;
    }
    if (m == NULL) {
        m = pc_malloc_array(n > 0 ? n : 1, sizeof *m);
        if (m == NULL) {
            return nomem(err);
        }
        for (i = 0; i < n; i++) {
            m[i] = 1.0f;
        }
    }
    *out = m;
    return PC_OK;
}

/* The shape a layer knocks out over rect (row-major, n values): its content's alpha with its
 * masks; with Transparency Shapes Layer off (and for adjustment layers) just the masks. */
static pc_status knockout_shape(const pc_layer *layer, pc_rect rect, const pc_cmp_ctx *cx, size_t n,
                                float **out, pc_error *err)
{
    pc_compose_buffer b;
    bool some = false;
    float *v;
    size_t i;
    pc_status st;
    if (!layer->advanced.transparency_shapes) {
        return masks_or_ones(layer, rect, cx, n, out, err);
    }
    st = extra_content(layer, rect, cx, &some, &b, err);
    if (st != PC_OK) {
        return st;
    }
    if (!some) {
        return masks_or_ones(layer, rect, cx, n, out, err);
    }
    v = pc_malloc_array(b.len > 0 ? b.len : 1, sizeof *v);
    if (v == NULL) {
        pc_compose_buffer_fini(&b);
        return nomem(err);
    }
    for (i = 0; i < b.len; i++) {
        v[i] = b.px[i][3];
    }
    if (b.len != n) {
        /* Never happens (content is rendered over rect); compare lengths like the reference. */
        free(v);
        v = NULL;
    }
    pc_compose_buffer_fini(&b);
    *out = v;
    return PC_OK;
}

pc_status pc_cmp_knockout(const pc_layer *layer, pc_compose_buffer *backdrop, const pc_compose_buffer *target,
                          bool clip_alpha, const pc_cmp_ctx *cx, pc_cmp_draw_fn draw, void *dctx,
                          pc_error *err)
{
    /* Coverage knocks out proportionally (Photoshop knocks out a little more at anti-aliased
     * shape edges; a binary shape matched worse). Compositing at opacity o is linear in o
     * (premultiplied), so with B the backdrop, B' the knocked-out one and R' what draw produced,
     * the result is R' + (1 - o)(B - B'). */
    float *shape = NULL;
    pc_compose_buffer before = {PC_RECT_EMPTY, NULL, 0};
    pc_compose_buffer knocked = {PC_RECT_EMPTY, NULL, 0};
    bool has_knocked = layer->opacity < 1.0f;
    float rest;
    size_t i;
    int c;
    pc_status st = knockout_shape(layer, backdrop->rect, cx, backdrop->len, &shape, err);
    if (st != PC_OK) {
        return st;
    }
    if (shape == NULL || !pc_rect_eq(target->rect, backdrop->rect) || target->len != backdrop->len) {
        /* Never reached (targets are built over the backdrop's rect); compositing without the
         * knockout beats a wrong pixel. */
        free(shape);
        return draw(dctx, backdrop, err);
    }
    if (has_knocked) {
        st = pc_compose_buffer_clone(backdrop, &before, err);
        if (st != PC_OK) {
            free(shape);
            return st;
        }
    }
    for (i = 0; i < backdrop->len; i++) {
        float k = shape[i];
        if (k > 0.0f) {
            float *p = backdrop->px[i];
            float a = p[3];
            mix(p, target->px[i], pc_rs_minf_k(k, 1.0f), p);
            if (clip_alpha) {
                p[3] = pc_rs_minf(p[3], a);
            }
        }
    }
    if (has_knocked) {
        st = pc_compose_buffer_clone(backdrop, &knocked, err);
    }
    if (st == PC_OK) {
        st = draw(dctx, backdrop, err);
    }
    rest = 1.0f - pc_clampf(layer->opacity, 0.0f, 1.0f);
    for (i = 0; st == PC_OK && has_knocked && i < backdrop->len; i++) {
        float *p = backdrop->px[i];
        const float *b = before.px[i];
        const float *k = knocked.px[i];
        float pm[4];
        float a;
        if (shape[i] <= 0.0f) {
            continue;
        }
        pm[0] = p[0] * p[3];
        pm[1] = p[1] * p[3];
        pm[2] = p[2] * p[3];
        pm[3] = p[3];
        for (c = 0; c < 3; c++) {
            pm[c] += rest * (b[c] * b[3] - k[c] * k[3]);
        }
        pm[3] += rest * (b[3] - k[3]);
        a = pc_clampf(pm[3], 0.0f, 1.0f);
        if (a > 0.0f) {
            p[0] = pc_clampf(pm[0] / a, 0.0f, 1.0f);
            p[1] = pc_clampf(pm[1] / a, 0.0f, 1.0f);
            p[2] = pc_clampf(pm[2] / a, 0.0f, 1.0f);
            p[3] = a;
        } else {
            p[0] = p[1] = p[2] = p[3] = 0.0f;
        }
    }
    free(shape);
    pc_compose_buffer_fini(&before);
    pc_compose_buffer_fini(&knocked);
    return st;
}

/* ---- clipping groups ---- */

typedef struct atop_draw {
    const pc_layer *layer;
    const pc_cmp_ctx *cx;
} atop_draw;

static pc_status draw_atop(void *ctx, pc_compose_buffer *b, pc_error *err)
{
    const atop_draw *d = ctx;
    return pc_cmp_composite_atop(d->layer, b, d->cx, err);
}

pc_status pc_cmp_composite_clipped(const pc_layer *clipped, size_t n, pc_compose_buffer *base,
                                   const pc_cmp_ctx *cx, pc_error *err)
{
    /* The grouped clipping layers composite in isolation, like an isolated group, so a clipped
     * layer's knockout (shallow or deep) stops at the bottom of the clipping group: the base's
     * content. */
    pc_compose_buffer floor = {PC_RECT_EMPTY, NULL, 0};
    bool has_floor = false;
    size_t i;
    pc_status st = PC_OK;
    for (i = 0; i < n; i++) {
        if (clipped[i].visible && clipped[i].advanced.knockout != PC_KNOCKOUT_NONE) {
            has_floor = true;
            break;
        }
    }
    if (has_floor) {
        st = pc_compose_buffer_clone(base, &floor, err);
    }
    for (i = 0; st == PC_OK && i < n; i++) {
        const pc_layer *c = &clipped[i];
        if (!c->visible) {
            continue;
        }
        if (has_floor && c->advanced.knockout != PC_KNOCKOUT_NONE) {
            atop_draw d = {c, cx};
            st = pc_cmp_knockout(c, base, &floor, true, cx, draw_atop, &d, err);
        } else {
            st = pc_cmp_composite_atop(c, base, cx, err);
        }
    }
    pc_compose_buffer_fini(&floor);
    return st;
}

pc_status pc_cmp_composite_clipped_individually(const pc_layer *layer, const pc_layer *clipped, size_t n,
                                                pc_compose_buffer *backdrop, const pc_cmp_ctx *cx,
                                                const pc_compose_buffer *ko, pc_cmp_scope scope,
                                                pc_error *err)
{
    /* The base alone (with its knockout ko), then each visible clipped layer in its own mode onto
     * the result, inside the base's shape (its alpha with masks, times its opacity). A clipped
     * layer's shallow knockout stops at the backdrop beneath the base. */
    pc_rect rect = backdrop->rect;
    size_t len = backdrop->len;
    pc_compose_buffer floor = {PC_RECT_EMPTY, NULL, 0};
    pc_compose_buffer content;
    bool has_floor = false;
    bool some = false;
    float *clip = NULL;
    float op;
    size_t i;
    size_t j;
    pc_cmp_scope sub = {false, scope.isolated};
    pc_status st = PC_OK;
    for (i = 0; i < n; i++) {
        if (clipped[i].visible && clipped[i].advanced.knockout == PC_KNOCKOUT_SHALLOW) {
            has_floor = true;
            break;
        }
    }
    if (has_floor) {
        st = pc_compose_buffer_clone(backdrop, &floor, err);
    }
    if (st == PC_OK) {
        st = pc_cmp_composite_layer(layer, NULL, 0, backdrop, cx, ko, scope, err);
    }
    op = pc_clampf(layer->opacity, 0.0f, 1.0f);
    if (st == PC_OK) {
        st = extra_content(layer, rect, cx, &some, &content, err);
    }
    if (st == PC_OK && some) {
        clip = pc_malloc_array(content.len > 0 ? content.len : 1, sizeof *clip);
        if (clip == NULL) {
            st = nomem(err);
        }
        for (i = 0; clip != NULL && i < content.len; i++) {
            clip[i] = content.px[i][3] * op;
        }
        if (clip != NULL && content.len != len) {
            free(clip);
            clip = NULL;
        }
        pc_compose_buffer_fini(&content);
    } else if (st == PC_OK) {
        st = masks_or_ones(layer, rect, cx, len, &clip, err);
        for (i = 0; st == PC_OK && i < len; i++) {
            clip[i] *= op;
        }
    }
    for (j = 0; st == PC_OK && clip != NULL && j < n; j++) {
        const pc_layer *c = &clipped[j];
        pc_compose_buffer before;
        pc_compose_buffer owned = {PC_RECT_EMPTY, NULL, 0};
        const pc_compose_buffer *t = NULL;
        if (!c->visible) {
            continue;
        }
        st = pc_compose_buffer_clone(backdrop, &before, err);
        if (st != PC_OK) {
            break;
        }
        st = pc_cmp_knockout_target(c, has_floor ? &floor : NULL, rect, cx, sub, &owned, &t, err);
        if (st == PC_OK) {
            st = pc_cmp_composite_layer(c, NULL, 0, backdrop, cx, t, scope, err);
        }
        for (i = 0; st == PC_OK && i < len; i++) {
            float k = clip[i];
            if (k < 1.0f) {
                mix(before.px[i], backdrop->px[i], pc_rs_maxf_k(k, 0.0f), backdrop->px[i]);
            }
        }
        pc_compose_buffer_fini(&owned);
        pc_compose_buffer_fini(&before);
    }
    free(clip);
    pc_compose_buffer_fini(&floor);
    return st;
}

/* ---- masks that hide effects ---- */

pc_status pc_cmp_effects_source(const pc_layer *layer, pc_layer *owned, bool *has_owned, pc_error *err)
{
    pc_layer l;
    pc_status st;
    if (!pc_cmp_hides_effects(layer)) {
        *has_owned = false;
        return PC_OK;
    }
    st = pc_layer_clone(layer, &l, err);
    if (st != PC_OK) {
        return st;
    }
    if (l.advanced.layer_mask_hides_effects && l.has_mask) {
        pc_layer_mask_fini(&l.mask);
        l.has_mask = false;
    }
    if (l.advanced.vector_mask_hides_effects && l.has_vector_mask) {
        pc_vector_mask_fini(&l.vector_mask);
        l.has_vector_mask = false;
    }
    *owned = l;
    *has_owned = true;
    return PC_OK;
}

pc_status pc_cmp_hiding_mask(const pc_layer *layer, pc_rect rect, const pc_cmp_ctx *cx, float **out,
                             pc_error *err)
{
    const pc_advanced_blending *a = &layer->advanced;
    pc_layer only;
    if (!pc_cmp_hides_effects(layer)) {
        *out = NULL;
        return PC_OK;
    }
    /* A layer carrying just the hiding masks (shallow copies, only read; the vector-mask cache
     * checks content, not just addresses). */
    memset(&only, 0, sizeof only);
    only.id = layer->id;
    only.visible = true;
    only.opacity = 1.0f;
    only.fill_opacity = 1.0f;
    if (a->layer_mask_hides_effects && layer->has_mask) {
        only.has_mask = true;
        only.mask = layer->mask;
    }
    if (a->vector_mask_hides_effects && layer->has_vector_mask) {
        only.has_vector_mask = true;
        only.vector_mask = layer->vector_mask;
    }
    return pc_cmp_mask_vals(&only, rect, cx, out, err);
}

void pc_cmp_apply_hiding_mask(pc_compose_buffer *out, const pc_compose_buffer *before, const float *mask)
{
    size_t i;
    size_t n = out->len < before->len ? out->len : before->len;
    for (i = 0; i < n; i++) {
        float k = mask[i];
        if (k < 1.0f) {
            mix(before->px[i], out->px[i], pc_rs_maxf_k(k, 0.0f), out->px[i]);
        }
    }
}
