/* The compositor core: layer stacks, clipping groups, pass-through and isolated groups,
 * artboards, adjustment mixing, Dissolve, channel restrictions and Blend If (port of the
 * compositing half of crates/compose/src/lib.rs).
 *
 * The tree is walked recursively like the reference, but every stack entered counts against a
 * per-thread depth (PC_COMPOSE_MAX_DEPTH), so hostile nesting is PC_ERR_LIMIT instead of a stack
 * overflow. Every function returns pc_status (allocation failures, limits, cancellation). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "compose/internal.h"
#include "compose/keyhash.h"

static _Thread_local unsigned stack_depth;

static pc_status nomem(pc_error *err)
{
    return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
}

pc_status pc_cmp_check(const pc_cmp_ctx *cx, pc_error *err)
{
    return pc_interrupt_check(cx->intr, err);
}

static float mask_k(const float *m, size_t i)
{
    return m != NULL ? m[i] : 1.0f;
}

/* ---- masks ---- */

/* RenderVectorMasks::values: the compiled mask for vm (compiled once per render). */
static pc_status vmask_values(const pc_cmp_ctx *cx, const pc_vector_mask *vm, pc_rect rect, float **out,
                              pc_error *err)
{
    pc_cmp_vmasks *v = cx->vmasks;
    pc_cmp_vmask_slot *slot = NULL;
    size_t i;
    size_t len = 0;
    pc_status st = PC_OK;
    float *vals = NULL;
    size_t n;
    uint64_t hash = PC_CMP_HASH0;
    if (v == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "compose: no render context");
    }
    pc_cmp_hash_vector_mask(&hash, vm);
    pthread_mutex_lock(&v->lock);
    for (i = 0; i < v->len; i++) {
        if (v->slots[i]->key == vm && v->slots[i]->hash == hash) {
            slot = v->slots[i];
            break;
        }
    }
    if (slot == NULL) {
        pc_cmp_vmask_slot **grown = pc_grow_array(v->slots, &v->cap, v->len + 1, sizeof *v->slots);
        if (grown == NULL) {
            st = nomem(err);
        } else {
            v->slots = grown;
            slot = calloc(1, sizeof *slot);
            if (slot == NULL) {
                st = nomem(err);
            } else {
                st = pc_vec_compiled_mask_new(vm, &slot->mask, err);
                if (st != PC_OK) {
                    free(slot);
                    slot = NULL;
                } else {
                    slot->key = vm;
                    slot->hash = hash;
                    v->slots[v->len++] = slot;
                }
            }
        }
    }
    pthread_mutex_unlock(&v->lock);
    if (st != PC_OK) {
        return st;
    }
    st = pc_cmp_rect_pixels(rect, &n, err);
    if (st == PC_OK) {
        st = pc_vec_compiled_mask_render(&slot->mask, rect, &vals, &len, err);
    }
    if (st == PC_OK && len != n) {
        free(vals);
        st = pc_fail(err, PC_ERR_INVALID, "compose: vector mask size mismatch");
    }
    if (st == PC_OK) {
        *out = vals;
    }
    return st;
}

pc_status pc_cmp_mask_vals(const pc_layer *layer, pc_rect rect, const pc_cmp_ctx *cx, float **out,
                           pc_error *err)
{
    size_t n;
    float *vector = NULL;
    float *v;
    size_t i;
    pc_status st = pc_cmp_rect_pixels(rect, &n, err);
    if (st != PC_OK) {
        return st;
    }
    /* Feathers blur across tiles: read the cached, canvas-wide combined mask. */
    if (pc_compose_has_feather(layer)) {
        bool found = false;
        pc_surface s;
        st = pc_compose_combined_mask(layer, cx->canvas, &found, &s, err);
        if (st != PC_OK) {
            return st;
        }
        if (found) {
            v = pc_malloc_array(n > 0 ? n : 1, sizeof *v);
            if (v == NULL) {
                pc_surface_fini(&s);
                return nomem(err);
            }
            st = pc_surface_read_region_into(&s, rect, v, n, err);
            pc_surface_fini(&s);
            if (st != PC_OK) {
                free(v);
                return st;
            }
            *out = v;
            return PC_OK;
        }
    }
    if (layer->has_vector_mask) {
        st = vmask_values(cx, &layer->vector_mask, rect, &vector, err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (!layer->has_mask) {
        *out = vector;
        return PC_OK;
    }
    v = pc_malloc_array(n > 0 ? n : 1, sizeof *v);
    if (v == NULL) {
        free(vector);
        return nomem(err);
    }
    st = pc_layer_mask_values_into(&layer->mask, rect, v, n, err);
    if (st != PC_OK) {
        free(v);
        free(vector);
        return st;
    }
    if (vector != NULL) {
        for (i = 0; i < n; i++) {
            v[i] *= vector[i];
        }
        free(vector);
    }
    *out = v;
    return PC_OK;
}

/* ---- content ---- */

/* quantum (has_q): the document depth's levels, which a dithered gradient is rounded to
 * (gradient_fill::render_quantized); has_q false keeps it in float. */
static pc_status render_fill(const pc_fill *f, pc_rect rect, pc_rect frame,
                             const pc_compose_patterns *patterns, bool has_q, float q, pc_compose_buffer *out,
                             pc_error *err)
{
    switch (f->kind) {
    case PC_FILL_SOLID: {
        float rgb[3];
        float c[4];
        pc_color_to_rgb(&f->u.solid, rgb);
        c[0] = rgb[0];
        c[1] = rgb[1];
        c[2] = rgb[2];
        c[3] = f->u.solid.alpha;
        return pc_compose_buffer_filled(rect, c, out, err);
    }
    case PC_FILL_GRADIENT:
        /* Gradient geometry relative to the layer's frame, independent of the render rect. */
        return pc_compose_gradient_fill_render_quantized(f, rect, frame, has_q, q, out, err);
    default:
        /* Laid out from the layer's frame when linked; transparent if the pattern is missing. */
        return pc_compose_pattern_fill_render(patterns, f, frame, rect, out, err);
    }
}

pc_status pc_cmp_fill_content(const pc_fill *f, pc_rect rect, pc_rect frame,
                              const pc_compose_patterns *patterns, pc_compose_buffer *out, pc_error *err)
{
    return render_fill(f, rect, frame, patterns, false, 0.0f, out, err);
}

/* A fill layer's content in a CMYK document: solid and gradient fills worked out in ink values at
 * the document's depth (gradient_fill::render_cmyk_fill), patterns through RGB. */
static pc_status render_fill_cmyk(const pc_fill *f, pc_rect rect, pc_rect frame, const pc_cmp_ctx *cx,
                                  bool has_q, float q, pc_compose_buffer *out, pc_error *err)
{
    bool solid = f->kind == PC_FILL_SOLID;
    bool found = false;
    pc_surface s;
    pc_status st = pc_compose_render_cmyk_fill(f, solid ? pc_rect_make(0, 0, 1, 1) : rect, frame, cx->depth,
                                               &found, &s, err);
    if (st != PC_OK) {
        return st;
    }
    if (!found) {
        return render_fill(f, rect, frame, cx->patterns, has_q, q, out, err);
    }
    if (solid) {
        float c[4];
        pc_surface_rgba(&s, 0, 0, c);
        st = pc_compose_buffer_filled(rect, c, out, err);
    } else {
        st = pc_compose_surface_to_buffer(&s, rect, out, err);
    }
    pc_surface_fini(&s);
    return st;
}

pc_status pc_cmp_render_content(const pc_layer *layer, pc_rect rect, const pc_cmp_ctx *cx, bool *some,
                                pc_compose_buffer *out, pc_error *err)
{
    pc_compose_buffer buf;
    float *m = NULL;
    size_t i;
    pc_status st;
    switch (layer->content.kind) {
    case PC_LAYER_GROUP:
        st = pc_compose_buffer_transparent(rect, &buf, err);
        if (st == PC_OK) {
            st = pc_cmp_composite_stack_at(layer->content.u.group.children, layer->content.u.group.nchildren,
                                           &buf, cx, PC_CMP_SCOPE_ISOLATED, err);
            if (st != PC_OK) {
                pc_compose_buffer_fini(&buf);
            }
        }
        break;
    case PC_LAYER_FILL:
        if (layer->has_fill_cache && pc_fill_eq(&layer->fill_cache.fill, &layer->content.u.fill)) {
            /* Photoshop's own rendering, valid while the fill is unchanged. */
            st = pc_compose_surface_to_buffer(&layer->fill_cache.surface, rect, &buf, err);
        } else {
            float q = 0.0f;
            bool has_q = pc_compose_adjustment_quantum(cx->depth, &q);
            pc_rect frame = pc_compose_fill_frame(layer, cx->canvas);
            if (cx->mode == PC_COLOR_MODE_CMYK) {
                st = render_fill_cmyk(&layer->content.u.fill, rect, frame, cx, has_q, q, &buf, err);
            } else {
                st = render_fill(&layer->content.u.fill, rect, frame, cx->patterns, has_q, q, &buf, err);
            }
        }
        break;
    case PC_LAYER_ADJUSTMENT:
        *some = false;
        return PC_OK;
    default: {
        const pc_surface *s = pc_layer_surface(layer);
        st = s != NULL ? pc_compose_surface_to_buffer(s, rect, &buf, err)
                       : pc_compose_buffer_transparent(rect, &buf, err);
        break;
    }
    }
    if (st != PC_OK) {
        return st;
    }
    st = pc_cmp_mask_vals(layer, rect, cx, &m, err);
    if (st != PC_OK) {
        pc_compose_buffer_fini(&buf);
        return st;
    }
    if (m != NULL) {
        for (i = 0; i < buf.len; i++) {
            buf.px[i][3] *= m[i];
        }
        free(m);
    }
    *some = true;
    *out = buf;
    return PC_OK;
}

/* ---- blending ---- */

/* Deterministic hash for Dissolve (document coordinates, so it is stable under tiling). */
static float dissolve_noise(int32_t x, int32_t y)
{
    uint32_t h = ((uint32_t)x * 0x8da6b343u) ^ ((uint32_t)y * 0xd8163841u) ^ 0x9e3779b9u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return (float)(h & 0xffffu) / 65536.0f;
}

void pc_cmp_blend_into(pc_compose_buffer *backdrop, const pc_compose_buffer *src, pc_blend_mode mode,
                       float opacity, float gamma)
{
    pc_rect rect = backdrop->rect;
    size_t w = pc_cmp_w(rect);
    size_t i;
    if (src->len != backdrop->len) {
        return; /* never: both cover the same rect */
    }
    for (i = 0; i < backdrop->len; i++) {
        float *b = backdrop->px[i];
        float s[4];
        memcpy(s, src->px[i], sizeof s);
        if (s[3] <= 0.0f) {
            continue;
        }
        if (mode == PC_BLEND_DISSOLVE) {
            /* i < 2^28, so the i32 arithmetic of the reference is exact. */
            int32_t x = pc_i32_wrapping_add(rect.x0, (int32_t)(i % w));
            int32_t y = pc_i32_wrapping_add(rect.y0, (int32_t)(i / w));
            s[3] = dissolve_noise(x, y) < s[3] * opacity ? 1.0f : 0.0f;
            pc_psblend_composite(PC_BLEND_NORMAL, b, s, 1.0f, b);
            continue;
        }
        pc_psblend_composite_gamma(mode, b, s, opacity, gamma, b);
    }
}

/* blend_into_fill: blend_into for a layer's own opacity and fill: Fill is part of the blend for
 * Photoshop's special eight (pc_psblend_composite_fill), plain coverage otherwise. */
static void blend_into_fill(pc_compose_buffer *backdrop, const pc_compose_buffer *src, pc_blend_mode mode,
                            float opacity, float fill, float gamma)
{
    size_t i;
    if (fill >= 1.0f || !pc_psblend_fill_is_special(mode)) {
        pc_cmp_blend_into(backdrop, src, mode, opacity * fill, gamma);
        return;
    }
    for (i = 0; i < backdrop->len && i < src->len; i++) {
        if (src->px[i][3] > 0.0f) {
            pc_psblend_composite_fill(mode, backdrop->px[i], src->px[i], opacity, fill, gamma,
                                      backdrop->px[i]);
        }
    }
}

/* Blend If: how much of a pixel shows, from the layer's own colour (NULL = no content there) and
 * the colour beneath (NULL = nothing beneath). */
static float blend_if_weight(const pc_layer *layer, pc_color_mode mode, const float *this_px,
                             const float *under)
{
    const pc_blend_if *bi = &layer->blend_if;
    float k = 1.0f;
    int side;
    for (side = 0; side < 2; side++) {
        const float *p = side == 0 ? this_px : under;
        float v[3];
        pc_blend_range r0[2];
        pc_blend_range r1[2];
        int c;
        if (p == NULL) {
            continue;
        }
        for (c = 0; c < 3; c++) {
            v[c] = pc_clampf(p[c], 0.0f, 1.0f) * 255.0f;
        }
        pc_blend_if_get(bi, 0, r0);
        if (mode == PC_COLOR_MODE_RGB) {
            /* Gray is the colour channels' luma (Rec. 601 weights), then R, G, B. */
            float gray = 0.299f * v[0] + 0.587f * v[1] + 0.114f * v[2];
            k *= pc_blend_range_weight(r0[side], gray);
            for (c = 0; c < 3; c++) {
                pc_blend_if_get(bi, (size_t)c + 1, r1);
                k *= pc_blend_range_weight(r1[side], v[c]);
            }
        } else {
            /* One channel: both the composite-gray and the channel's own entry are honoured. */
            pc_blend_if_get(bi, 1, r1);
            k *= pc_blend_range_weight(r0[side], v[0]) * pc_blend_range_weight(r1[side], v[0]);
        }
        if (k <= 0.0f) {
            return 0.0f;
        }
    }
    return k;
}

/* Mixes out (the backdrop with the layer drawn) back towards before where the ranges hide it. */
static pc_status apply_blend_if(const pc_layer *layer, const pc_compose_buffer *before,
                                pc_compose_buffer *out, const pc_cmp_ctx *cx, pc_error *err)
{
    pc_compose_buffer own = {PC_RECT_EMPTY, NULL, 0};
    bool has_own = false;
    size_t i;
    if (layer->content.kind != PC_LAYER_ADJUSTMENT) {
        pc_status st = pc_cmp_render_content(layer, out->rect, cx, &has_own, &own, err);
        if (st != PC_OK) {
            return st;
        }
    }
    for (i = 0; i < out->len && i < before->len; i++) {
        float *p = out->px[i];
        const float *b = before->px[i];
        const float *this_px;
        const float *under = b[3] > 0.0f ? b : NULL;
        float k;
        float a;
        if (has_own) {
            this_px = own.px[i][3] > 0.0f ? own.px[i] : NULL;
        } else {
            this_px = p;
        }
        k = blend_if_weight(layer, cx->mode, this_px, under);
        if (k >= 1.0f) {
            continue;
        }
        a = b[3] + (p[3] - b[3]) * k;
        if (a > 0.0f) {
            float r[3];
            int c;
            for (c = 0; c < 3; c++) {
                r[c] = pc_clampf((b[c] * b[3] + (p[c] * p[3] - b[c] * b[3]) * k) / a, 0.0f, 1.0f);
            }
            p[0] = r[0];
            p[1] = r[1];
            p[2] = r[2];
            p[3] = a;
        } else {
            memset(p, 0, 4 * sizeof(float));
        }
    }
    pc_compose_buffer_fini(&own);
    return PC_OK;
}

/* Put back the backdrop's values in the channels a layer leaves out; alpha stays the result. */
static void restore_channels(pc_compose_buffer *out, const pc_compose_buffer *before, const float w[3])
{
    size_t i;
    int c;
    for (i = 0; i < out->len && i < before->len; i++) {
        for (c = 0; c < 3; c++) {
            const float b = before->px[i][c];
            out->px[i][c] = b + (out->px[i][c] - b) * w[c];
        }
    }
}

static void quantize(pc_compose_buffer *b, float q)
{
    size_t i;
    int c;
    for (i = 0; i < b->len; i++) {
        for (c = 0; c < 4; c++) {
            b->px[i][c] = floorf(b->px[i][c] * q + 0.5f) / q;
        }
    }
}

bool pc_compose_adjustment_quantum(pc_sample_type depth, float *q)
{
    switch (depth) {
    case PC_SAMPLE_TYPE_U8:
        *q = 255.0f;
        return true;
    case PC_SAMPLE_TYPE_U16:
        *q = 32768.0f;
        return true;
    default:
        return false;
    }
}

/* ---- emptiness ---- */

/* shapeless_stroke_bounds: for a Transparency Shapes Layer off layer whose only enabled effects
 * are local strokes, the effects can't reach past the stroke from the layer's pixels; false in
 * every other case (other effects can cover the whole layer, and advanced blending can change the
 * backdrop independently of those pixels). */
bool pc_cmp_shapeless_stroke_bounds(const pc_layer *layer, pc_rect *out)
{
    size_t i;
    bool any = false;
    const pc_surface *s;
    pc_rect b;
    pc_rect p;
    if (layer->advanced.transparency_shapes || layer->advanced.knockout != PC_KNOCKOUT_NONE ||
        layer->advanced.blend_interior || layer->clipped || !pc_blend_if_is_default(&layer->blend_if) ||
        (layer->has_mask && layer->mask.enabled) || (layer->has_vector_mask && layer->vector_mask.enabled) ||
        !pc_compose_transparent_outside(layer) || !pc_compose_fx_maps_are_local(layer)) {
        return false;
    }
    if (!layer->effects.enabled) {
        return false;
    }
    for (i = 0; i < layer->effects.nitems; i++) {
        const pc_effect *e = &layer->effects.items[i];
        if (!pc_effect_enabled(e)) {
            continue;
        }
        if (e->kind != PC_FX_STROKE) {
            return false;
        }
        any = true;
    }
    if (!any) {
        return false;
    }
    switch (layer->content.kind) {
    case PC_LAYER_RASTER:
    case PC_LAYER_TEXT:
    case PC_LAYER_SHAPE:
    case PC_LAYER_SMART:
        s = pc_layer_surface(layer);
        b = s != NULL ? pc_compose_content_bounds(s) : PC_RECT_EMPTY;
        /* A filled shape's effects follow its outline, also where its fill is transparent (as in
         * layer_bounds). */
        if (pc_compose_effect_outline(layer) != NULL && pc_compose_paint_bounds(layer, &p)) {
            *out = pc_rect_is_empty(b) ? p : pc_rect_union(b, p);
        } else {
            *out = b;
        }
        return true;
    default:
        return false;
    }
}

bool pc_cmp_empty_in(const pc_layer *layer, pc_rect rect)
{
    if (pc_compose_has_effects(layer)) {
        /* Effects reach at most `margin` beyond the layer's pixels (when it is transparent
         * outside them; a group's from its children with their effects): tiles away from a small
         * layer skip it entirely. */
        pc_rect canvas = pc_rect_make(INT32_MIN / 4, INT32_MIN / 4, INT32_MAX / 4, INT32_MAX / 4);
        pc_rect bounds;
        if (!pc_cmp_shapeless_stroke_bounds(layer, &bounds)) {
            bounds = pc_compose_effect_source_bounds(layer, canvas);
        }
        return pc_compose_transparent_outside(layer) &&
               pc_rect_is_empty(
                   pc_rect_intersect(pc_rect_inflate(bounds, pc_compose_effects_margin(layer)), rect));
    }
    switch (layer->content.kind) {
    case PC_LAYER_RASTER:
    case PC_LAYER_TEXT:
    case PC_LAYER_SHAPE:
    case PC_LAYER_SMART: {
        const pc_surface *s = pc_layer_surface(layer);
        float dp[PC_RASTER_MAX_CHANNELS];
        size_t n;
        if (s == NULL) {
            return true;
        }
        n = pc_surface_channels(s);
        pc_surface_default_pixel(s, dp);
        return !pc_surface_has_tiles_in(s, rect) && n > 0 && dp[n - 1] <= 0.0f && pc_surface_format(s).alpha;
    }
    default:
        return false;
    }
}

/* ---- shapes split into fill and stroke (C2) ---- */

typedef struct parts {
    pc_compose_buffer fill;
    pc_compose_buffer stroke;
    float *mask;
} parts;

static void parts_fini(parts *p)
{
    pc_compose_buffer_fini(&p->fill);
    pc_compose_buffer_fini(&p->stroke);
    free(p->mask);
    p->mask = NULL;
}

/* split_parts: a stroked shape layer's (fill, vector stroke, mask) over rect; *found false for
 * other layers. */
static pc_status split_parts(const pc_layer *layer, pc_rect rect, const pc_cmp_ctx *cx, bool *found,
                             parts *out, pc_error *err)
{
    pc_surface fs;
    pc_surface ss;
    bool split = false;
    parts p;
    pc_status st;
    *found = false;
    if (layer->content.kind != PC_LAYER_SHAPE || !layer->content.u.shape.has_stroke) {
        return PC_OK;
    }
    st = pc_compose_shape_split(&layer->content.u.shape, cx->canvas, cx->depth, &split, &fs, &ss, err);
    if (st != PC_OK || !split) {
        return st;
    }
    memset(&p, 0, sizeof p);
    st = pc_compose_surface_to_buffer(&fs, rect, &p.fill, err);
    if (st == PC_OK) {
        st = pc_compose_surface_to_buffer(&ss, rect, &p.stroke, err);
    }
    if (st == PC_OK) {
        st = pc_cmp_mask_vals(layer, rect, cx, &p.mask, err);
    }
    pc_surface_fini(&fs);
    pc_surface_fini(&ss);
    if (st != PC_OK) {
        parts_fini(&p);
        return st;
    }
    *out = p;
    *found = true;
    return PC_OK;
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

/* ---- effects (C3) ---- */

/* The layer bounds effects are laid out in: paint bounds, else layer bounds. */
static pc_rect fx_layout_bounds(const pc_layer *layer, const pc_cmp_ctx *cx)
{
    pc_rect r;
    if (pc_compose_paint_bounds(layer, &r)) {
        return r;
    }
    return pc_compose_layer_bounds(layer, cx->canvas);
}

/* Content (or split fill) plus the optional vector stroke of an effects layer. *found false when
 * the layer has no content of its own (adjustments). */
static pc_status fx_content(const pc_layer *layer, pc_rect rect, const pc_cmp_ctx *cx, bool *found,
                            parts *out, bool *has_vstroke, pc_error *err)
{
    pc_status st = split_parts(layer, rect, cx, has_vstroke, out, err);
    if (st != PC_OK) {
        return st;
    }
    if (*has_vstroke) {
        *found = true;
        return PC_OK;
    }
    memset(out, 0, sizeof *out);
    return pc_cmp_render_content(layer, rect, cx, found, &out->fill, err);
}

/* composite_layer_effects: a layer with effects (and its clipping group) onto backdrop. */
static pc_status composite_layer_effects(const pc_layer *layer, const pc_layer *clipped, size_t nclipped,
                                         pc_compose_buffer *backdrop, const pc_cmp_ctx *cx, pc_error *err)
{
    parts p;
    bool found = false;
    bool has_vs = false;
    pc_cmp_fx_ref *maps = NULL;
    pc_compose_vector_stroke vs;
    pc_status st = fx_content(layer, backdrop->rect, cx, &found, &p, &has_vs, err);
    if (st != PC_OK || !found) {
        return st;
    }
    st = pc_cmp_composite_clipped(clipped, nclipped, &p.fill, cx, err);
    if (st == PC_OK) {
        st = pc_cmp_effect_maps(layer, backdrop->rect, cx, &maps, err);
    }
    if (st == PC_OK) {
        vs.stroke = &p.stroke;
        vs.mask = p.mask;
        st = pc_compose_fx_composite(layer, &p.fill, backdrop, pc_cmp_fx_maps(maps),
                                     fx_layout_bounds(layer, cx), cx->patterns, has_vs ? &vs : NULL, err);
    }
    pc_cmp_fx_ref_release(maps);
    parts_fini(&p);
    return st;
}

/* ---- layers ---- */

/* Pass-through groups at full fill composite their children straight into the backdrop. */
static pc_status composite_pass_through(const pc_layer *layer, const pc_layer *clipped, size_t nclipped,
                                        pc_compose_buffer *backdrop, const pc_cmp_ctx *cx, pc_cmp_scope scope,
                                        float opacity, pc_error *err)
{
    pc_rect rect = backdrop->rect;
    const pc_group *g = &layer->content.u.group;
    bool needs_mix = opacity < 1.0f || layer->has_mask || layer->has_vector_mask;
    bool has_clipped = any_visible(clipped, nclipped);
    pc_compose_buffer before = {PC_RECT_EMPTY, NULL, 0};
    bool has_before = false;
    size_t i;
    int c;
    pc_status st = PC_OK;
    /* Children can draw directly unless mixing or clipping needs the original backdrop. */
    if (needs_mix || has_clipped) {
        st = pc_compose_buffer_clone(backdrop, &before, err);
        if (st != PC_OK) {
            return st;
        }
        has_before = true;
    }
    st = pc_cmp_composite_stack_at(g->children, g->nchildren, backdrop, cx, pc_cmp_scope_pass_through(scope),
                                   err);
    if (st == PC_OK && needs_mix && has_before) {
        float *mv = NULL;
        st = pc_cmp_mask_vals(layer, rect, cx, &mv, err);
        for (i = 0; st == PC_OK && i < backdrop->len; i++) {
            float *p = backdrop->px[i];
            const float *a = before.px[i];
            float k = opacity * mask_k(mv, i);
            float b[4];
            float wa;
            float wb;
            float alpha;
            memcpy(b, p, sizeof b);
            /* Coverage mixes premultiplied colour; the stored buffer remains straight alpha. */
            wa = a[3] * (1.0f - k);
            wb = b[3] * k;
            alpha = wa + wb;
            if (alpha > 0.0f) {
                for (c = 0; c < 3; c++) {
                    p[c] = (a[c] * wa + b[c] * wb) / alpha;
                }
                p[3] = alpha;
            } else {
                memset(p, 0, 4 * sizeof(float));
            }
        }
        free(mv);
    }
    /* Layers clipped to a pass-through group sit atop the group's isolated rendering; their
     * effect (isolated result with vs. without them, each over the original backdrop) is added
     * to the pass-through result. Exact when the children blend Normal, close otherwise. */
    if (st == PC_OK && has_clipped && has_before) {
        pc_compose_buffer iso;
        bool some = false;
        st = pc_cmp_render_content(layer, rect, cx, &some, &iso, err);
        if (st == PC_OK && some) {
            pc_compose_buffer clipped_iso;
            pc_compose_buffer without;
            st = pc_compose_buffer_clone(&iso, &clipped_iso, err);
            if (st == PC_OK) {
                st = pc_cmp_composite_clipped(clipped, nclipped, &clipped_iso, cx, err);
                if (st == PC_OK) {
                    st = pc_compose_buffer_clone(&before, &without, err);
                }
                if (st == PC_OK) {
                    pc_cmp_blend_into(&without, &iso, PC_BLEND_NORMAL, opacity, 1.0f);
                    pc_cmp_blend_into(&before, &clipped_iso, PC_BLEND_NORMAL, opacity, 1.0f); /* `with` */
                    for (i = 0; i < backdrop->len; i++) {
                        float *p = backdrop->px[i];
                        const float *w = before.px[i];
                        const float *wo = without.px[i];
                        float pa = p[3];
                        float pm[4] = {p[0] * pa, p[1] * pa, p[2] * pa, pa};
                        float a;
                        /* Work premultiplied so transparent areas stay consistent. */
                        for (c = 0; c < 3; c++) {
                            pm[c] += w[c] * w[3] - wo[c] * wo[3];
                        }
                        pm[3] += w[3] - wo[3];
                        a = pc_clampf(pm[3], 0.0f, 1.0f);
                        if (a > 0.0f) {
                            p[0] = pc_clampf(pm[0] / a, 0.0f, 1.0f);
                            p[1] = pc_clampf(pm[1] / a, 0.0f, 1.0f);
                            p[2] = pc_clampf(pm[2] / a, 0.0f, 1.0f);
                            p[3] = a;
                        } else {
                            memset(p, 0, 4 * sizeof(float));
                        }
                    }
                    pc_compose_buffer_fini(&without);
                }
                pc_compose_buffer_fini(&clipped_iso);
            }
            pc_compose_buffer_fini(&iso);
        }
    }
    pc_compose_buffer_fini(&before);
    return st;
}

/* Adjustment layers transform the backdrop, then blend the result back in. */
static pc_status composite_adjustment(const pc_layer *layer, const pc_layer *clipped, size_t nclipped,
                                      pc_compose_buffer *backdrop, const pc_cmp_ctx *cx, float opacity,
                                      pc_error *err)
{
    pc_compose_buffer adjusted;
    float *mv = NULL;
    size_t i;
    int c;
    pc_status st = pc_compose_buffer_clone(backdrop, &adjusted, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_compose_adjust_apply_in(&layer->content.u.adjustment, &adjusted, cx->transfer, true, cx->depth,
                                    cx->mode, err);
    /* Clipped layers onto an adjustment are uncommon; they composite atop the adjusted result. */
    if (st == PC_OK) {
        st = pc_cmp_composite_clipped(clipped, nclipped, &adjusted, cx, err);
    }
    if (st == PC_OK) {
        st = pc_cmp_mask_vals(layer, backdrop->rect, cx, &mv, err);
    }
    for (i = 0; st == PC_OK && i < backdrop->len; i++) {
        float k = opacity * mask_k(mv, i);
        float *b = backdrop->px[i];
        const float *a = adjusted.px[i];
        float bl[3];
        if (k <= 0.0f) {
            continue;
        }
        /* Only `adjusted` has changed; this pixel's original backdrop is still in place. */
        pc_psblend_rgb(layer->blend, b, a, bl);
        for (c = 0; c < 3; c++) {
            b[c] = b[c] + (bl[c] - b[c]) * k;
        }
    }
    free(mv);
    pc_compose_buffer_fini(&adjusted);
    return st;
}

static pc_status composite_layer_plain(const pc_layer *layer, const pc_layer *clipped, size_t nclipped,
                                       pc_compose_buffer *backdrop, const pc_cmp_ctx *cx, pc_cmp_scope scope,
                                       pc_error *err)
{
    pc_rect rect = backdrop->rect;
    float opacity;
    pc_compose_buffer content;
    bool some = false;
    parts p;
    bool found = false;
    pc_status st;
    pc_rect unused;
    /* A TSL-off stroke base that is transparent here can still have visible clipped siblings: it
     * keeps the full clipping path instead of the stroke-bounds shortcut. Other bases keep the
     * usual skip (their clipped layers vanish with them). */
    if ((nclipped == 0 || !pc_cmp_shapeless_stroke_bounds(layer, &unused)) && pc_cmp_empty_in(layer, rect)) {
        return PC_OK;
    }
    opacity = layer->opacity * layer->fill_opacity;
    /* Below 100 % fill Photoshop renders a pass-through group isolated, like Normal. */
    if (layer->content.kind == PC_LAYER_GROUP && layer->blend == PC_BLEND_PASS_THROUGH &&
        layer->fill_opacity >= 1.0f && !pc_compose_has_effects(layer)) {
        return composite_pass_through(layer, clipped, nclipped, backdrop, cx, scope, opacity, err);
    }
    if (layer->content.kind == PC_LAYER_ADJUSTMENT) {
        return composite_adjustment(layer, clipped, nclipped, backdrop, cx, opacity, err);
    }
    if (pc_compose_has_effects(layer)) {
        /* Layer / Vector Mask Hides Effects: the effects are built without that mask, which then
         * hides the finished layer and effects. */
        pc_layer owned;
        bool has_owned = false;
        float *hide = NULL;
        pc_compose_buffer before = {PC_RECT_EMPTY, NULL, 0};
        st = pc_cmp_effects_source(layer, &owned, &has_owned, err);
        if (st != PC_OK) {
            return st;
        }
        st = pc_cmp_hiding_mask(layer, rect, cx, &hide, err);
        if (st == PC_OK && hide != NULL) {
            st = pc_compose_buffer_clone(backdrop, &before, err);
        }
        if (st == PC_OK) {
            st = composite_layer_effects(has_owned ? &owned : layer, clipped, nclipped, backdrop, cx, err);
        }
        if (st == PC_OK && hide != NULL) {
            pc_cmp_apply_hiding_mask(backdrop, &before, hide);
        }
        free(hide);
        pc_compose_buffer_fini(&before);
        if (has_owned) {
            pc_layer_fini(&owned);
        }
        return st;
    }
    /* A stroked shape with visible clipped layers: the vector stroke sits above them. */
    if (any_visible(clipped, nclipped)) {
        st = split_parts(layer, rect, cx, &found, &p, err);
        if (st != PC_OK) {
            return st;
        }
        if (found) {
            size_t i;
            st = pc_cmp_composite_clipped(clipped, nclipped, &p.fill, cx, err);
            for (i = 0; st == PC_OK && i < p.fill.len; i++) {
                pc_psblend_composite(PC_BLEND_NORMAL, p.fill.px[i], p.stroke.px[i], 1.0f, p.fill.px[i]);
                p.fill.px[i][3] *= mask_k(p.mask, i);
            }
            if (st == PC_OK) {
                blend_into_fill(backdrop, &p.fill, layer->blend, layer->opacity, layer->fill_opacity, 1.0f);
            }
            parts_fini(&p);
            return st;
        }
    }
    st = pc_cmp_render_content(layer, rect, cx, &some, &content, err);
    if (st != PC_OK || !some) {
        return st;
    }
    st = pc_cmp_composite_clipped(clipped, nclipped, &content, cx, err);
    if (st == PC_OK) {
        blend_into_fill(backdrop, &content, layer->blend, layer->opacity, layer->fill_opacity,
                        pc_compose_text_gamma(layer));
    }
    pc_compose_buffer_fini(&content);
    return st;
}

/* An artboard: its background and the group, composited only inside the board. */
static pc_status composite_artboard(const pc_layer *layer, const pc_artboard *ab, const pc_layer *clipped,
                                    size_t nclipped, pc_compose_buffer *backdrop, const pc_cmp_ctx *cx,
                                    pc_cmp_scope scope, pc_error *err)
{
    pc_rect board = pc_rect_intersect(ab->rect, backdrop->rect);
    pc_compose_buffer sub;
    size_t bw;
    size_t w;
    int32_t y;
    float bg[4];
    pc_status st;
    if (pc_rect_is_empty(board)) {
        return PC_OK;
    }
    bw = pc_cmp_w(board);
    w = pc_cmp_w(backdrop->rect);
    st = pc_compose_buffer_transparent(board, &sub, err);
    if (st != PC_OK) {
        return st;
    }
    for (y = board.y0; y < board.y1; y++) {
        size_t o = (size_t)(y - backdrop->rect.y0) * w + (size_t)(board.x0 - backdrop->rect.x0);
        size_t so = (size_t)(y - board.y0) * bw;
        memcpy(sub.px + so, backdrop->px + o, bw * sizeof *sub.px);
    }
    if (pc_artboard_background_rgba(&ab->background, bg) && bg[3] > 0.0f) {
        size_t i;
        for (i = 0; i < sub.len; i++) {
            pc_psblend_composite(PC_BLEND_NORMAL, sub.px[i], bg, 1.0f, sub.px[i]);
        }
    }
    st = composite_layer_plain(layer, clipped, nclipped, &sub, cx, scope, err);
    if (st == PC_OK) {
        for (y = board.y0; y < board.y1; y++) {
            size_t o = (size_t)(y - backdrop->rect.y0) * w + (size_t)(board.x0 - backdrop->rect.x0);
            size_t so = (size_t)(y - board.y0) * bw;
            memcpy(backdrop->px + o, sub.px + so, bw * sizeof *sub.px);
        }
    }
    pc_compose_buffer_fini(&sub);
    return st;
}

pc_status pc_cmp_composite_layer_any(const pc_layer *layer, const pc_layer *clipped, size_t nclipped,
                                     pc_compose_buffer *backdrop, const pc_cmp_ctx *cx, pc_cmp_scope scope,
                                     pc_error *err)
{
    if (layer->content.kind == PC_LAYER_GROUP && layer->content.u.group.has_artboard) {
        return composite_artboard(layer, &layer->content.u.group.artboard, clipped, nclipped, backdrop, cx,
                                  scope, err);
    }
    return composite_layer_plain(layer, clipped, nclipped, backdrop, cx, scope, err);
}

typedef struct any_draw {
    const pc_layer *layer;
    const pc_layer *clipped;
    size_t nclipped;
    const pc_cmp_ctx *cx;
    pc_cmp_scope scope;
} any_draw;

static pc_status draw_any(void *ctx, pc_compose_buffer *b, pc_error *err)
{
    const any_draw *d = ctx;
    return pc_cmp_composite_layer_any(d->layer, d->clipped, d->nclipped, b, d->cx, d->scope, err);
}

pc_status pc_cmp_composite_layer(const pc_layer *layer, const pc_layer *clipped, size_t nclipped,
                                 pc_compose_buffer *backdrop, const pc_cmp_ctx *cx,
                                 const pc_compose_buffer *ko, pc_cmp_scope scope, pc_error *err)
{
    float w[3];
    bool has_w;
    bool blend_if;
    pc_compose_buffer before = {PC_RECT_EMPTY, NULL, 0};
    pc_status st;
    /* Blend Clipped Layers as Group off: the base alone, then each clipped layer in its mode. */
    if (pc_cmp_clips_individually(layer, clipped, nclipped)) {
        return pc_cmp_composite_clipped_individually(layer, clipped, nclipped, backdrop, cx, ko, scope, err);
    }
    has_w = pc_compose_channel_weights(layer, cx->mode, w);
    blend_if = pc_compose_blend_if_active(layer, cx->mode);
    if (has_w || blend_if) {
        st = pc_compose_buffer_clone(backdrop, &before, err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (ko != NULL) {
        any_draw d = {layer, clipped, nclipped, cx, scope};
        st = pc_cmp_knockout(layer, backdrop, ko, false, cx, draw_any, &d, err);
    } else {
        st = pc_cmp_composite_layer_any(layer, clipped, nclipped, backdrop, cx, scope, err);
    }
    if (st == PC_OK && (has_w || blend_if)) {
        if (has_w) {
            restore_channels(backdrop, &before, w);
        }
        if (blend_if) {
            st = apply_blend_if(layer, &before, backdrop, cx, err);
        }
    }
    pc_compose_buffer_fini(&before);
    return st;
}

pc_status pc_cmp_composite_stack_at(const pc_layer *layers, size_t n, pc_compose_buffer *backdrop,
                                    const pc_cmp_ctx *cx, pc_cmp_scope scope, pc_error *err)
{
    pc_compose_buffer floor = {PC_RECT_EMPTY, NULL, 0};
    bool has_floor = false;
    size_t i = 0;
    float q;
    bool has_q = pc_compose_adjustment_quantum(cx->depth, &q);
    pc_status st = pc_cmp_check(cx, err);
    if (st != PC_OK) {
        return st;
    }
    if (stack_depth >= PC_COMPOSE_MAX_DEPTH) {
        return pc_fail(err, PC_ERR_LIMIT, "compose: layer groups nested deeper than %d",
                       PC_COMPOSE_MAX_DEPTH);
    }
    stack_depth++;
    /* Shallow knockouts stop at the bottom of this stack: the backdrop it starts from. */
    if (!scope.root && pc_cmp_needs_floor(layers, n)) {
        st = pc_compose_buffer_clone(backdrop, &floor, err);
        has_floor = st == PC_OK;
    }
    while (st == PC_OK && i < n) {
        const pc_layer *base = &layers[i];
        size_t j = i + 1;
        /* The clipping group: following layers with clipped set. */
        while (j < n && layers[j].clipped && !base->clipped) {
            j++;
        }
        if (base->visible) {
            pc_compose_buffer owned = {PC_RECT_EMPTY, NULL, 0};
            const pc_compose_buffer *ko = NULL;
            st = pc_cmp_knockout_target(base, has_floor ? &floor : NULL, backdrop->rect, cx, scope, &owned,
                                        &ko, err);
            if (st == PC_OK) {
                st = pc_cmp_composite_layer(base, &layers[i + 1], j - i - 1, backdrop, cx, ko, scope, err);
            }
            pc_compose_buffer_fini(&owned);
            if (st == PC_OK && base->content.kind == PC_LAYER_ADJUSTMENT && has_q) {
                quantize(backdrop, q);
            }
        }
        i = j > i + 1 ? j : i + 1;
    }
    pc_compose_buffer_fini(&floor);
    stack_depth--;
    return st;
}

/* ---- clipping (atop) ---- */

static pc_status composite_atop_effects(const pc_layer *layer, pc_compose_buffer *base, const pc_cmp_ctx *cx,
                                        pc_error *err)
{
    /* Effects of a clipped layer are clipped to the base too: render them over the base (treated
     * as opaque) and keep the base's alpha. A mask that hides the effects applies afterwards. */
    pc_rect rect = base->rect;
    float *hide = NULL;
    pc_layer owned;
    bool has_owned = false;
    const pc_layer *src;
    parts p;
    bool found = false;
    bool has_vs = false;
    pc_compose_buffer opaque = {PC_RECT_EMPTY, NULL, 0};
    pc_compose_buffer plain = {PC_RECT_EMPTY, NULL, 0};
    pc_cmp_fx_ref *maps = NULL;
    pc_compose_vector_stroke vs;
    size_t i;
    pc_status st = pc_cmp_hiding_mask(layer, rect, cx, &hide, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_cmp_effects_source(layer, &owned, &has_owned, err);
    if (st != PC_OK) {
        free(hide);
        return st;
    }
    src = has_owned ? &owned : layer;
    st = fx_content(src, rect, cx, &found, &p, &has_vs, err);
    if (st == PC_OK && found) {
        st = pc_compose_buffer_clone(base, &opaque, err);
        for (i = 0; st == PC_OK && i < opaque.len; i++) {
            opaque.px[i][3] = 1.0f;
        }
        if (st == PC_OK) {
            st = pc_cmp_effect_maps(src, rect, cx, &maps, err);
        }
        if (st == PC_OK) {
            vs.stroke = &p.stroke;
            vs.mask = p.mask;
            st = pc_compose_fx_composite(src, &p.fill, &opaque, pc_cmp_fx_maps(maps),
                                         fx_layout_bounds(src, cx), cx->patterns, has_vs ? &vs : NULL, err);
        }
        if (st == PC_OK && hide != NULL) {
            st = pc_compose_buffer_clone(base, &plain, err);
            for (i = 0; st == PC_OK && i < plain.len; i++) {
                plain.px[i][3] = 1.0f;
            }
            if (st == PC_OK) {
                pc_cmp_apply_hiding_mask(&opaque, &plain, hide);
            }
        }
        for (i = 0; st == PC_OK && i < base->len; i++) {
            float *q = base->px[i];
            if (q[3] > 0.0f) {
                q[0] = opaque.px[i][0];
                q[1] = opaque.px[i][1];
                q[2] = opaque.px[i][2];
            }
        }
        pc_cmp_fx_ref_release(maps);
        parts_fini(&p);
    }
    pc_compose_buffer_fini(&opaque);
    pc_compose_buffer_fini(&plain);
    if (has_owned) {
        pc_layer_fini(&owned);
    }
    free(hide);
    return st;
}

static pc_status composite_atop_any(const pc_layer *layer, pc_compose_buffer *base, const pc_cmp_ctx *cx,
                                    pc_error *err)
{
    pc_rect rect = base->rect;
    pc_compose_buffer content;
    bool some = false;
    float opacity;
    float gamma;
    size_t i;
    pc_status st;
    if (layer->content.kind == PC_LAYER_ADJUSTMENT) {
        pc_compose_buffer adjusted;
        float *mv = NULL;
        st = pc_compose_buffer_clone(base, &adjusted, err);
        if (st != PC_OK) {
            return st;
        }
        st = pc_compose_adjust_apply_in(&layer->content.u.adjustment, &adjusted, cx->transfer, true,
                                        cx->depth, cx->mode, err);
        if (st == PC_OK) {
            st = pc_cmp_mask_vals(layer, rect, cx, &mv, err);
        }
        for (i = 0; st == PC_OK && i < base->len; i++) {
            float *p = base->px[i];
            float k = layer->opacity * layer->fill_opacity * mask_k(mv, i);
            float bl[3];
            int c;
            pc_psblend_rgb(layer->blend, p, adjusted.px[i], bl);
            for (c = 0; c < 3; c++) {
                p[c] += (bl[c] - p[c]) * k;
            }
        }
        free(mv);
        pc_compose_buffer_fini(&adjusted);
        return st;
    }
    if (pc_compose_has_effects(layer)) {
        return composite_atop_effects(layer, base, cx, err);
    }
    st = pc_cmp_render_content(layer, rect, cx, &some, &content, err);
    if (st != PC_OK || !some) {
        return st;
    }
    opacity = layer->opacity * layer->fill_opacity;
    gamma = pc_compose_text_gamma(layer);
    for (i = 0; i < base->len; i++) {
        float *p = base->px[i];
        float alpha = p[3];
        float b[4];
        float r[4];
        if (alpha <= 0.0f) {
            continue;
        }
        /* Blend as if the base were opaque, then keep the base's alpha. */
        b[0] = p[0];
        b[1] = p[1];
        b[2] = p[2];
        b[3] = 1.0f;
        pc_psblend_composite_gamma(layer->blend, b, content.px[i], opacity, gamma, r);
        p[0] = r[0];
        p[1] = r[1];
        p[2] = r[2];
    }
    pc_compose_buffer_fini(&content);
    return PC_OK;
}

pc_status pc_cmp_composite_atop(const pc_layer *layer, pc_compose_buffer *base, const pc_cmp_ctx *cx,
                                pc_error *err)
{
    float w[3];
    bool has_w = pc_compose_channel_weights(layer, cx->mode, w);
    bool blend_if = pc_compose_blend_if_active(layer, cx->mode);
    pc_compose_buffer before = {PC_RECT_EMPTY, NULL, 0};
    pc_status st;
    if (has_w || blend_if) {
        st = pc_compose_buffer_clone(base, &before, err);
        if (st != PC_OK) {
            return st;
        }
    }
    st = composite_atop_any(layer, base, cx, err);
    if (st == PC_OK && (has_w || blend_if)) {
        if (has_w) {
            restore_channels(base, &before, w);
        }
        if (blend_if) {
            st = apply_blend_if(layer, &before, base, cx, err);
        }
    }
    pc_compose_buffer_fini(&before);
    return st;
}

/* ---- effect shapes ---- */

pc_status pc_cmp_effect_shape(const pc_layer *layer, pc_rect rect, const pc_cmp_ctx *cx, float **out,
                              size_t *len, pc_error *err)
{
    size_t n;
    size_t w;
    size_t h;
    size_t x;
    size_t y;
    float *res = NULL;
    float *a = NULL;
    float *cov = NULL;
    float *mv = NULL;
    size_t cov_len = 0;
    const pc_path *path;
    pc_status st = pc_cmp_rect_pixels(rect, &n, err);
    if (st != PC_OK) {
        return st;
    }
    /* Transparency Shapes Layer off: the effects take the whole layer as their shape. */
    if (!layer->advanced.transparency_shapes) {
        st = pc_cmp_mask_vals(layer, rect, cx, &res, err);
        if (st == PC_OK && res == NULL && n > 0) {
            res = pc_malloc_array(n, sizeof *res);
            if (res == NULL) {
                return nomem(err);
            }
            for (x = 0; x < n; x++) {
                res[x] = 1.0f;
            }
        }
        if (st == PC_OK) {
            *out = res;
            *len = n;
        }
        return st;
    }
    path = n > 0 ? pc_compose_effect_outline(layer) : NULL;
    if (n > 0) {
        res = pc_calloc_array(n, sizeof *res);
        if (res == NULL) {
            return nomem(err);
        }
    }
    if (path == NULL) {
        pc_compose_buffer b;
        bool some = false;
        if (n > 0) {
            st = pc_cmp_render_content(layer, rect, cx, &some, &b, err);
            if (st == PC_OK && some) {
                for (x = 0; x < n; x++) {
                    res[x] = b.px[x][3];
                }
                pc_compose_buffer_fini(&b);
            }
        }
        if (st != PC_OK) {
            free(res);
            return st;
        }
        *out = res;
        *len = n;
        return PC_OK;
    }
    /* Unmasked: the masks scale the shape, they aren't the fill's transparency. */
    a = pc_calloc_array(n, sizeof *a);
    if (a == NULL) {
        free(res);
        return nomem(err);
    }
    {
        const pc_surface *s = pc_layer_surface(layer);
        if (s != NULL) {
            pc_compose_buffer sb;
            st = pc_compose_surface_to_buffer(s, rect, &sb, err);
            if (st == PC_OK) {
                for (x = 0; x < n; x++) {
                    a[x] = sb.px[x][3];
                }
                pc_compose_buffer_fini(&sb);
            }
        }
    }
    if (st == PC_OK) {
        st = pc_vec_path_coverage(path, rect, &cov, &cov_len, err);
        if (st == PC_OK && cov_len != n) {
            st = pc_fail(err, PC_ERR_INVALID, "compose: coverage size mismatch");
        }
    }
    if (st == PC_OK) {
        st = pc_cmp_mask_vals(layer, rect, cx, &mv, err);
    }
    w = pc_cmp_w(rect);
    h = (size_t)pc_rect_height(rect);
    for (y = 0; st == PC_OK && y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t i = y * w + x;
            float v;
            /* A pixel the outline covers is inside, whatever the fill's opacity. */
            if (cov[i] >= 1.0f - 1e-3f) {
                v = 1.0f;
            } else {
                float f = 0.0f;
                size_t yy;
                size_t xx;
                for (yy = y > 0 ? y - 1 : 0; yy < (y + 2 < h ? y + 2 : h); yy++) {
                    for (xx = x > 0 ? x - 1 : 0; xx < (x + 2 < w ? x + 2 : w); xx++) {
                        f = pc_rs_maxf(f, a[yy * w + xx]);
                    }
                }
                v = pc_clampf(a[i] + (1.0f - pc_rs_minf_k(f, 1.0f)) * cov[i], 0.0f, 1.0f);
            }
            res[i] = v * mask_k(mv, i);
        }
    }
    free(a);
    free(cov);
    free(mv);
    if (st != PC_OK) {
        free(res);
        return st;
    }
    *out = res;
    *len = n;
    return PC_OK;
}
