/* Layer queries of crates/compose/src/lib.rs: bounds, frames, channel restrictions, Blend If
 * activity, occlusion. Walks over group children are iterative (an explicit stack) where the
 * answer doesn't depend on the nesting, and depth-bounded otherwise, giving the conservative
 * answer ("anywhere") past PC_COMPOSE_MAX_DEPTH or when memory runs out. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "compose/internal.h"

/* ---- shapes ---- */

const pc_path *pc_compose_effect_outline(const pc_layer *layer)
{
    const pc_shape_layer *sh;
    bool stroke_inside;
    if (layer->content.kind != PC_LAYER_SHAPE) {
        return NULL;
    }
    sh = &layer->content.u.shape;
    stroke_inside = !sh->has_stroke || sh->stroke.width <= 0.0f || sh->stroke.align == PC_STROKE_ALIGN_INSIDE;
    if (sh->has_fill && stroke_inside && !pc_path_is_empty(&sh->path) && !sh->path.inverted) {
        return &sh->path;
    }
    return NULL;
}

bool pc_compose_paint_bounds(const pc_layer *layer, pc_rect *out)
{
    double b[4];
    pc_rect r;
    if (layer->content.kind != PC_LAYER_SHAPE || !pc_path_control_bounds(&layer->content.u.shape.path, b)) {
        return false;
    }
    r = pc_rect_make(pc_f64_to_i32(floor(b[0])), pc_f64_to_i32(floor(b[1])), pc_f64_to_i32(floor(b[2])),
                     pc_f64_to_i32(floor(b[3])));
    if (pc_rect_is_empty(r)) {
        return false;
    }
    *out = r;
    return true;
}

/* ---- bounds ---- */

static bool surface_transparent_outside(const pc_surface *s)
{
    float dp[PC_RASTER_MAX_CHANNELS];
    size_t n = pc_surface_channels(s);
    pc_surface_default_pixel(s, dp);
    return pc_surface_format(s).alpha && n > 0 && dp[n - 1] <= 0.0f;
}

/* A leaf's own bounds (not a group, not a fill, not shapeless). */
static pc_rect leaf_bounds(const pc_layer *layer)
{
    const pc_surface *s = pc_layer_surface(layer);
    pc_rect b = s != NULL ? pc_compose_content_bounds(s) : PC_RECT_EMPTY;
    pc_rect p;
    /* Effects follow a filled shape's outline, also where its fill is transparent. */
    if (pc_compose_effect_outline(layer) != NULL && pc_compose_paint_bounds(layer, &p) &&
        pc_compose_has_effects(layer)) {
        return pc_rect_is_empty(b) ? p : pc_rect_union(b, p);
    }
    return b;
}

pc_rect pc_compose_layer_bounds(const pc_layer *layer, pc_rect canvas)
{
    /* Union over every visible descendant leaf; union is order-independent, so an explicit
     * stack of pending layers gives the reference's recursive fold. */
    const pc_layer **stack = NULL;
    size_t sp = 0;
    size_t cap = 0;
    pc_rect acc = PC_RECT_EMPTY;
    const pc_layer *l = layer;
    for (;;) {
        pc_rect b = PC_RECT_EMPTY;
        if (pc_compose_shapeless(l)) {
            /* Transparency Shapes Layer off: effects and knockout cover the whole canvas. */
            b = canvas;
        } else if (l->content.kind == PC_LAYER_GROUP && l->content.u.group.has_artboard) {
            b = l->content.u.group.artboard.rect;
        } else if (l->content.kind == PC_LAYER_GROUP) {
            const pc_group *g = &l->content.u.group;
            size_t i;
            for (i = 0; i < g->nchildren; i++) {
                const pc_layer **ns;
                if (!g->children[i].visible) {
                    continue;
                }
                ns = pc_grow_array(stack, &cap, sp + 1, sizeof *stack);
                if (ns == NULL) {
                    free(stack);
                    return canvas; /* out of memory: the conservative answer */
                }
                stack = ns;
                stack[sp++] = &g->children[i];
            }
        } else if (l->content.kind == PC_LAYER_FILL) {
            b = canvas;
        } else {
            b = leaf_bounds(l);
        }
        if (!pc_rect_is_empty(b)) {
            acc = pc_rect_is_empty(acc) ? b : pc_rect_union(acc, b);
        }
        if (sp == 0) {
            break;
        }
        l = stack[--sp];
    }
    free(stack);
    return acc;
}

bool pc_compose_transparent_outside(const pc_layer *layer)
{
    const pc_layer **stack = NULL;
    size_t sp = 0;
    size_t cap = 0;
    const pc_layer *l = layer;
    bool ok = true;
    for (;;) {
        if (l->content.kind == PC_LAYER_GROUP) {
            const pc_group *g = &l->content.u.group;
            size_t i;
            for (i = 0; i < g->nchildren; i++) {
                const pc_layer **ns;
                if (!g->children[i].visible) {
                    continue;
                }
                ns = pc_grow_array(stack, &cap, sp + 1, sizeof *stack);
                if (ns == NULL) {
                    free(stack);
                    return false; /* conservative */
                }
                stack = ns;
                stack[sp++] = &g->children[i];
            }
        } else if (l->content.kind != PC_LAYER_FILL && l->content.kind != PC_LAYER_ADJUSTMENT) {
            const pc_surface *s = pc_layer_surface(l);
            if (s != NULL && !surface_transparent_outside(s)) {
                ok = false;
                break;
            }
        }
        if (sp == 0) {
            break;
        }
        l = stack[--sp];
    }
    free(stack);
    return ok;
}

static bool composite_bounds_at(const pc_layer *layer, pc_rect canvas, unsigned depth, pc_rect *out)
{
    pc_rect own = PC_RECT_EMPTY;
    int32_t m;
    if (depth > PC_COMPOSE_MAX_DEPTH || !pc_compose_transparent_outside(layer)) {
        return false;
    }
    if (layer->content.kind == PC_LAYER_GROUP && !layer->content.u.group.has_artboard) {
        const pc_group *g = &layer->content.u.group;
        size_t i;
        for (i = 0; i < g->nchildren; i++) {
            pc_rect b;
            if (!g->children[i].visible) {
                continue;
            }
            if (!composite_bounds_at(&g->children[i], canvas, depth + 1, &b)) {
                return false;
            }
            if (!pc_rect_is_empty(b)) {
                own = pc_rect_is_empty(own) ? b : pc_rect_union(own, b);
            }
        }
    } else if (layer->content.kind != PC_LAYER_ADJUSTMENT) {
        own = pc_compose_layer_bounds(layer, canvas);
    }
    if (pc_rect_is_empty(own)) {
        *out = PC_RECT_EMPTY;
        return true;
    }
    m = pc_compose_has_effects(layer) ? pc_compose_effects_margin(layer) : 0;
    *out = pc_rect_intersect(pc_rect_inflate(own, m), canvas);
    return true;
}

bool pc_compose_composite_bounds(const pc_layer *layer, pc_rect canvas, pc_rect *out)
{
    return composite_bounds_at(layer, canvas, 0, out);
}

static bool reaches_below(const pc_layer *l, unsigned depth)
{
    size_t i;
    if (depth > PC_COMPOSE_MAX_DEPTH) {
        return true; /* conservative */
    }
    if (l->content.kind == PC_LAYER_ADJUSTMENT) {
        /* Clipped, it changes only its base's pixels. */
        return !l->clipped;
    }
    if (l->content.kind == PC_LAYER_GROUP && l->blend == PC_BLEND_PASS_THROUGH) {
        for (i = 0; i < l->content.u.group.nchildren; i++) {
            if (reaches_below(&l->content.u.group.children[i], depth + 1)) {
                return true;
            }
        }
    }
    return false;
}

bool pc_compose_change_bounds(const pc_layer *layer, pc_rect canvas, pc_rect *out)
{
    if (layer->content.kind == PC_LAYER_ADJUSTMENT || reaches_below(layer, 0)) {
        return false;
    }
    return pc_compose_composite_bounds(layer, canvas, out);
}

/* ---- frames ---- */

pc_rect pc_compose_fill_frame(const pc_layer *layer, pc_rect canvas)
{
    pc_rect frame = canvas;
    double cb[4];
    /* "Align with layer" off: the canvas (the Gradient tool's live gradients). */
    if (layer->content.kind == PC_LAYER_FILL && layer->content.u.fill.kind == PC_FILL_GRADIENT &&
        !layer->content.u.fill.u.gradient.align) {
        return frame;
    }
    if (layer->has_mask && layer->mask.enabled && pc_surface_channels(&layer->mask.surface) > 0) {
        float dp[PC_RASTER_MAX_CHANNELS];
        pc_surface_default_pixel(&layer->mask.surface, dp);
        if (dp[0] <= 0.0f) {
            pc_rect b = pc_rect_intersect(pc_compose_content_bounds(&layer->mask.surface), canvas);
            if (!pc_rect_is_empty(b)) {
                frame = b;
            }
        }
    }
    if (layer->has_vector_mask && layer->vector_mask.enabled && !layer->vector_mask.path.inverted &&
        pc_path_control_bounds(&layer->vector_mask.path, cb)) {
        pc_rect b = pc_rect_intersect(pc_rect_make(pc_f64_to_i32(floor(cb[0])), pc_f64_to_i32(floor(cb[1])),
                                                   pc_f64_to_i32(ceil(cb[2])), pc_f64_to_i32(ceil(cb[3]))),
                                      frame);
        if (!pc_rect_is_empty(b)) {
            frame = b;
        }
    }
    return frame;
}

/* ---- blending options ---- */

bool pc_compose_channel_weights(const pc_layer *layer, pc_color_mode mode, float out[3])
{
    uint32_t x = layer->excluded_channels;
    float w[3];
    int i;
    if (x == 0) {
        return false;
    }
    switch (mode) {
    case PC_COLOR_MODE_RGB:
        for (i = 0; i < 3; i++) {
            w[i] = (x & (1u << i)) != 0 ? 0.0f : 1.0f;
        }
        break;
    case PC_COLOR_MODE_GRAYSCALE:
    case PC_COLOR_MODE_DUOTONE:
        w[0] = (x & 1u) != 0 ? 0.0f : 1.0f;
        w[1] = w[0];
        w[2] = w[0];
        break;
    default:
        return false;
    }
    if (w[0] == 1.0f && w[1] == 1.0f && w[2] == 1.0f) {
        return false;
    }
    memcpy(out, w, sizeof w);
    return true;
}

bool pc_compose_blend_if_active(const pc_layer *layer, pc_color_mode mode)
{
    return (mode == PC_COLOR_MODE_RGB || mode == PC_COLOR_MODE_GRAYSCALE || mode == PC_COLOR_MODE_DUOTONE) &&
           !pc_blend_if_is_default(&layer->blend_if);
}

bool pc_compose_occludes_below(const pc_layer *layer, pc_color_mode mode)
{
    const pc_fill *f;
    bool opaque = false;
    size_t i;
    if (layer->content.kind != PC_LAYER_FILL) {
        return false;
    }
    f = &layer->content.u.fill;
    switch (f->kind) {
    case PC_FILL_SOLID:
        opaque = f->u.solid.alpha >= 1.0f;
        break;
    case PC_FILL_GRADIENT:
        opaque = f->u.gradient.nstops > 0;
        for (i = 0; opaque && i < f->u.gradient.nstops; i++) {
            opaque = f->u.gradient.stops[i].color.alpha >= 1.0f;
        }
        for (i = 0; opaque && i < f->u.gradient.nopacity_stops; i++) {
            opaque = f->u.gradient.opacity_stops[i].opacity >= 1.0f;
        }
        break;
    default:
        opaque = false;
        break;
    }
    /* A cached render in use (a PSD's stored fill pixels) may not be opaque. */
    return opaque && !(layer->has_fill_cache && pc_fill_eq(&layer->fill_cache.fill, f)) && layer->visible &&
           !layer->clipped && layer->blend == PC_BLEND_NORMAL && layer->opacity >= 1.0f &&
           layer->fill_opacity >= 1.0f && !(layer->has_mask && layer->mask.enabled) &&
           !(layer->has_vector_mask && layer->vector_mask.enabled) && !pc_compose_has_effects(layer) &&
           layer->excluded_channels == 0 && !pc_compose_blend_if_active(layer, mode);
}

float pc_compose_text_gamma(const pc_layer *layer)
{
    return layer->content.kind == PC_LAYER_TEXT ? pc_psblend_text_gamma() : 1.0f;
}

/* ---- buffers ---- */

pc_status pc_compose_surface_to_buffer(const pc_surface *s, pc_rect rect, pc_compose_buffer *out,
                                       pc_error *err)
{
    pc_compose_buffer b;
    pc_status st = pc_compose_buffer_transparent(rect, &b, err);
    if (st != PC_OK) {
        return st;
    }
    if (b.len > 0) {
        st = pc_surface_read_rgba_into(s, rect, b.px, b.len, err);
        if (st != PC_OK) {
            pc_compose_buffer_fini(&b);
            return st;
        }
    }
    *out = b;
    return PC_OK;
}

pc_status pc_compose_layer_shape(const pc_document *doc, const pc_layer *layer, pc_rect rect, float **out,
                                 size_t *len, pc_error *err)
{
    pc_cmp_ctx_store s;
    pc_status st = pc_cmp_ctx_init_doc(&s, doc, NULL, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_cmp_effect_shape(layer, rect, &s.cx, out, len, err);
    pc_cmp_ctx_fini(&s);
    return st;
}

pc_status pc_compose_render_fill_content(const pc_layer *layer, const pc_fill *f, pc_rect canvas,
                                         const pc_pattern *patterns, size_t npatterns, pc_compose_buffer *out,
                                         pc_error *err)
{
    pc_compose_patterns *prepared = NULL;
    pc_status st =
        pc_compose_patterns_new(patterns, npatterns, PC_COMPOSE_PREPARED_PATTERN_BYTES, &prepared, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_cmp_fill_content(f, canvas, pc_compose_fill_frame(layer, canvas), prepared, out, err);
    pc_compose_patterns_free(prepared);
    return st;
}

pc_status pc_compose_stroke_frame(const pc_document *doc, const pc_layer *layer, const pc_stroke_fx *st_fx,
                                  bool *found, pc_rect *out, pc_error *err)
{
    pc_cmp_ctx_store s;
    pc_cmp_fx_ref *maps = NULL;
    pc_rect r;
    pc_status st;
    if (st_fx->paint.kind != PC_FX_PAINT_GRADIENT || !pc_compose_has_effects(layer)) {
        *found = false;
        return PC_OK;
    }
    st = pc_cmp_ctx_init_doc(&s, doc, NULL, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_cmp_effect_maps(layer, &s.cx, &maps, err);
    if (st == PC_OK) {
        *found = pc_compose_fx_maps_stroke_frame(pc_cmp_fx_maps(maps), st_fx, &r);
        if (*found) {
            *out = r;
        }
    }
    pc_cmp_fx_ref_release(maps);
    pc_cmp_ctx_fini(&s);
    return st;
}
