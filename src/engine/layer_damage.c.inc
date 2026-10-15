/* Bounded per-step damage (port of layer_multi_cmds::{same_pixels, step_damage} and helpers).
 * Included by layer_multi_cmds.c. Provides the E5 step_damage hook relocated out of hooks.c.
 *
 * Deviation (REWRITE_PLAN.md §7): the reference compares pixels by copy-on-write tile pointer
 * identity; this port compares them by content (pc_surface_eq and the pc_*_eq helpers). Content
 * equality is tighter (no damage when bytes match even if the tiles differ) and never
 * under-reports a real pixel change, so recomposition stays correct. */

static bool masks_eq(bool ha, const pc_layer_mask *a, bool hb, const pc_layer_mask *b)
{
    if (!ha && !hb) {
        return true;
    }
    if (ha && hb) {
        return pc_layer_mask_eq(a, b);
    }
    return false;
}

/* Whether two layers render to identical pixels in place (layer_pixels_equal). Group children are
 * compared by the caller's walk, not here. */
static bool layer_pixels_equal(const pc_layer *a, const pc_layer *b)
{
    if (a->id != b->id || a->visible != b->visible || a->blend != b->blend || a->opacity != b->opacity ||
        a->fill_opacity != b->fill_opacity || a->clipped != b->clipped) {
        return false;
    }
    if (!masks_eq(a->has_mask, &a->mask, b->has_mask, &b->mask)) {
        return false;
    }
    if (a->has_vector_mask != b->has_vector_mask) {
        return false;
    }
    if (a->has_vector_mask && !pc_vector_mask_eq(&a->vector_mask, &b->vector_mask)) {
        return false;
    }
    if (!pc_effects_eq(&a->effects, &b->effects) || a->excluded_channels != b->excluded_channels ||
        !pc_blend_if_eq(&a->blend_if, &b->blend_if) || !pc_advanced_blending_eq(&a->advanced, &b->advanced)) {
        return false;
    }
    if (a->content.kind != b->content.kind) {
        return false;
    }
    switch (a->content.kind) {
    case PC_LAYER_RASTER:
        if (!pc_surface_eq(&a->content.u.raster, &b->content.u.raster)) {
            return false;
        }
        break;
    case PC_LAYER_GROUP: {
        /* Children are walked separately; only the group's own clipping frame counts. */
        const pc_group *x = &a->content.u.group;
        const pc_group *y = &b->content.u.group;
        if (x->has_artboard != y->has_artboard) {
            return false;
        }
        if (x->has_artboard && !pc_artboard_eq(&x->artboard, &y->artboard)) {
            return false;
        }
        break;
    }
    case PC_LAYER_FILL:
        if (!pc_fill_eq(&a->content.u.fill, &b->content.u.fill)) {
            return false;
        }
        break;
    case PC_LAYER_ADJUSTMENT:
        if (!pc_adjustment_eq(&a->content.u.adjustment, &b->content.u.adjustment)) {
            return false;
        }
        break;
    case PC_LAYER_TEXT:
        if (!pc_text_layer_eq(&a->content.u.text, &b->content.u.text)) {
            return false;
        }
        break;
    case PC_LAYER_SHAPE:
        if (!pc_shape_layer_eq(&a->content.u.shape, &b->content.u.shape)) {
            return false;
        }
        break;
    case PC_LAYER_SMART:
        if (!pc_smart_object_eq(&a->content.u.smart, &b->content.u.smart)) {
            return false;
        }
        break;
    default:
        return false;
    }
    if (a->has_fill_cache != b->has_fill_cache) {
        return false;
    }
    if (a->has_fill_cache && (!pc_fill_eq(&a->fill_cache.fill, &b->fill_cache.fill) ||
                              !pc_surface_eq(&a->fill_cache.surface, &b->fill_cache.surface))) {
        return false;
    }
    return true;
}

/* Document-level fields the compositor reads (doc_pixels_equal). */
static bool doc_pixels_equal(const pc_document *a, const pc_document *b)
{
    if (a->size.width != b->size.width || a->size.height != b->size.height) {
        return false;
    }
    if (memcmp(&a->resolution_dpi, &b->resolution_dpi, sizeof(float)) != 0) {
        return false;
    }
    if (a->mode != b->mode || a->depth != b->depth) {
        return false;
    }
    if (!pc_doc_bytes_eq(a->icc_profile, b->icc_profile)) {
        return false;
    }
    if (a->nchannels != b->nchannels) {
        return false;
    }
    for (size_t i = 0; i < a->nchannels; i++) {
        if (!pc_alpha_channel_eq(&a->channels[i], &b->channels[i])) {
            return false;
        }
    }
    if (a->global_light.angle != b->global_light.angle ||
        a->global_light.altitude != b->global_light.altitude) {
        return false;
    }
    if (a->has_quick_mask != b->has_quick_mask) {
        return false;
    }
    if (a->has_quick_mask && !pc_alpha_channel_eq(&a->quick_mask, &b->quick_mask)) {
        return false;
    }
    if (a->npatterns != b->npatterns) {
        return false;
    }
    for (size_t i = 0; i < a->npatterns; i++) {
        if (!pc_pattern_eq(&a->patterns[i], &b->patterns[i])) {
            return false;
        }
    }
    if (a->has_color_table != b->has_color_table) {
        return false;
    }
    if (a->has_color_table && !pc_color_table_eq(&a->color_table, &b->color_table)) {
        return false;
    }
    if (a->has_duotone != b->has_duotone) {
        return false;
    }
    if (a->has_duotone && !pc_duotone_eq(&a->duotone, &b->duotone)) {
        return false;
    }
    return true;
}

static bool same_pixels_walk(const pc_layer *xs, size_t nx, const pc_layer *ys, size_t ny, unsigned depth)
{
    if (depth >= 256 || nx != ny) {
        return false;
    }
    for (size_t i = 0; i < nx; i++) {
        const pc_layer *x = &xs[i];
        const pc_layer *y = &ys[i];
        if (x->has_video || y->has_video || !layer_pixels_equal(x, y)) {
            return false;
        }
        bool xg = x->content.kind == PC_LAYER_GROUP;
        bool yg = y->content.kind == PC_LAYER_GROUP;
        if (xg != yg) {
            return false;
        }
        if (xg && !same_pixels_walk(x->content.u.group.children, x->content.u.group.nchildren,
                                    y->content.u.group.children, y->content.u.group.nchildren, depth + 1)) {
            return false;
        }
    }
    return true;
}

bool pc_engine_same_pixels(const pc_document *a, const pc_document *b)
{
    return doc_pixels_equal(a, b) && same_pixels_walk(a->layers, a->nlayers, b->layers, b->nlayers, 0);
}

/* Whether layers[k] draws nothing wherever it sits (inert_at). */
static bool inert_at(const pc_layer *layers, size_t n, size_t k)
{
    if (k >= n) {
        return false;
    }
    const pc_layer *l = &layers[k];
    bool empty = false;
    if (l->content.kind == PC_LAYER_RASTER) {
        const pc_surface *s = &l->content.u.raster;
        if (pc_surface_tile_count(s) == 0 && pc_surface_format(s).alpha) {
            float def[PC_RASTER_MAX_CHANNELS];
            pc_surface_default_pixel(s, def);
            size_t ch = pc_surface_channels(s);
            empty = true;
            for (size_t i = 0; i < ch; i++) {
                if (def[i] != 0.0f) {
                    empty = false;
                    break;
                }
            }
        }
    } else if (l->content.kind == PC_LAYER_GROUP) {
        empty = l->content.u.group.nchildren == 0 && !l->content.u.group.has_artboard;
    }
    if (!empty) {
        return false;
    }
    if (l->blend != PC_BLEND_NORMAL && l->blend != PC_BLEND_PASS_THROUGH) {
        return false;
    }
    pc_advanced_blending def_adv = pc_advanced_blending_default();
    if (l->has_mask || l->has_vector_mask || pc_compose_has_effects(l) || l->has_fill_cache ||
        l->has_video || !pc_advanced_blending_eq(&l->advanced, &def_adv)) {
        return false;
    }
    return l->clipped || k + 1 >= n || !layers[k + 1].clipped;
}

/* Whether a layer and everything inside it is bounded (contained): no video, no channel exclusion
 * (both reach outside the layer's bounds). */
static bool contained(const pc_layer *l, unsigned depth)
{
    if (depth >= 256 || l->has_video || l->excluded_channels != 0) {
        return false;
    }
    if (l->content.kind == PC_LAYER_GROUP) {
        const pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            if (!contained(&g->children[i], depth + 1)) {
                return false;
            }
        }
    }
    return true;
}

/* The pixels adding/removing layers[k] can change, when bounded (one_sided_bounds); false = None. */
static bool one_sided_bounds(const pc_layer *layers, size_t n, size_t k, pc_rect canvas, pc_rect *out)
{
    if (k >= n) {
        return false;
    }
    const pc_layer *l = &layers[k];
    bool new_base = !l->clipped && k + 1 < n && layers[k + 1].clipped;
    if (new_base || !contained(l, 0)) {
        return false;
    }
    return pc_compose_change_bounds(l, canvas, out);
}

/* The id-paired walk of step_damage. Returns false (== Rust None, unbounded) or true with *out
 * extended by the damage of this layer list. */
static bool step_walk(const pc_layer *xs, size_t nx, const pc_layer *ys, size_t ny, const pc_document *a,
                      const pc_document *b, pc_rect *out)
{
    pc_rect ca = pc_document_bounds(a);
    pc_rect cb = pc_document_bounds(b);
    size_t i = 0;
    size_t j = 0;
    for (;;) {
        bool xa = i < nx;
        bool yb = j < ny;
        if (!xa && !yb) {
            return true;
        }
        const pc_layer *x = xa ? &xs[i] : NULL;
        const pc_layer *y = yb ? &ys[j] : NULL;
        if (x != NULL && y != NULL && x->id == y->id) {
            /* fall through to the paired case below */
        } else if (yb && inert_at(ys, ny, j)) {
            j++;
            continue;
        } else if (xa && inert_at(xs, nx, i)) {
            i++;
            continue;
        } else if (y != NULL) {
            bool in_x = false;
            for (size_t t = 0; t < nx; t++) {
                if (xs[t].id == y->id) {
                    in_x = true;
                    break;
                }
            }
            if (!in_x) {
                pc_rect r;
                if (!one_sided_bounds(ys, ny, j, cb, &r)) {
                    return false;
                }
                if (!pc_rect_is_empty(r)) {
                    *out = pc_rect_is_empty(*out) ? r : pc_rect_union(*out, r);
                }
                j++;
                continue;
            }
            if (x != NULL) {
                bool in_y = false;
                for (size_t t = 0; t < ny; t++) {
                    if (ys[t].id == x->id) {
                        in_y = true;
                        break;
                    }
                }
                if (!in_y) {
                    pc_rect r;
                    if (!one_sided_bounds(xs, nx, i, ca, &r)) {
                        return false;
                    }
                    if (!pc_rect_is_empty(r)) {
                        *out = pc_rect_is_empty(*out) ? r : pc_rect_union(*out, r);
                    }
                    i++;
                    continue;
                }
            }
            /* Both sides have the layer, in another order. */
            return false;
        } else {
            /* x present, y absent: x only on this side. */
            bool in_y = false;
            for (size_t t = 0; t < ny; t++) {
                if (x != NULL && ys[t].id == x->id) {
                    in_y = true;
                    break;
                }
            }
            if (!in_y) {
                pc_rect r;
                if (!one_sided_bounds(xs, nx, i, ca, &r)) {
                    return false;
                }
                if (!pc_rect_is_empty(r)) {
                    *out = pc_rect_is_empty(*out) ? r : pc_rect_union(*out, r);
                }
                i++;
                continue;
            }
            return false;
        }
        /* paired: both sides have the same layer id */
        i++;
        j++;
        if (x->has_video || y->has_video) {
            return false;
        }
        if (x->clipped != y->clipped || x->excluded_channels != y->excluded_channels) {
            return false;
        }
        if (!layer_pixels_equal(x, y)) {
            pc_rect bx;
            pc_rect by;
            if (!pc_compose_change_bounds(x, ca, &bx) || !pc_compose_change_bounds(y, cb, &by)) {
                return false;
            }
            pc_rect u = pc_rect_union(bx, by);
            *out = pc_rect_is_empty(*out) ? u : pc_rect_union(*out, u);
        }
        bool xg = x->content.kind == PC_LAYER_GROUP;
        bool yg = y->content.kind == PC_LAYER_GROUP;
        if (xg != yg) {
            return false;
        }
        if (xg && !step_walk(x->content.u.group.children, x->content.u.group.nchildren,
                             y->content.u.group.children, y->content.u.group.nchildren, a, b, out)) {
            return false;
        }
    }
}

/* layer_multi_cmds::step_damage (relocated E5 hook): the pixels a single history step can have
 * changed between `a` (before) and `b` (after). *present=false means "composite everything". */
void pc_engine_hook_step_damage(const pc_document *before, const pc_document *after, bool *present,
                                pc_rect *out)
{
    if (!doc_pixels_equal(before, after)) {
        *present = false;
        return;
    }
    pc_rect dmg = PC_RECT_EMPTY;
    if (step_walk(before->layers, before->nlayers, after->layers, after->nlayers, before, after, &dmg)) {
        *present = true;
        *out = dmg;
    } else {
        *present = false;
    }
}
