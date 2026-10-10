/* Port of crates/doc/src/lib.rs, part 2: Document (construction, copy, equality, tree queries
 * and edits). Every walk is iterative (doc/tree.h). */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "doc/doc.h"
#include "doc/tree.h"
#include "raster/pixel.h"

/* ---- construction ------------------------------------------------------------------------- */

pc_status pc_document_init(pc_document *out, const char *name, pc_size size, pc_color_mode mode,
                           pc_sample_type depth, pc_error *err)
{
    pc_document d;
    pc_status s;
    memset(&d, 0, sizeof d);
    d.id = pc_doc_id_fresh();
    s = pc_str_set_c(&d.name, name != NULL ? name : "", err);
    if (s != PC_OK) {
        goto fail;
    }
    d.size = size;
    d.resolution_dpi = 72.0f;
    d.mode = mode;
    d.depth = depth;
    d.global_light = pc_global_light_default();
    s = pc_measurement_default(&d.measurement, err);
    if (s != PC_OK) {
        goto fail;
    }
    s = pc_text_styles_default(&d.text_styles, err);
    if (s != PC_OK) {
        goto fail;
    }
    *out = d;
    return PC_OK;
fail:
    pc_document_fini(&d);
    return s;
}

pc_status pc_document_with_background(pc_document *out, const char *name, pc_size size, pc_color_mode mode,
                                      pc_sample_type depth, pc_color fill, pc_error *err)
{
    pc_document d;
    pc_layer bg;
    pc_pixel_format f;
    float rgb[3];
    float rgba[4];
    float px[PC_RASTER_MAX_CHANNELS];
    size_t n;
    pc_status s = pc_document_init(&d, name, size, mode, depth, err);
    if (s != PC_OK) {
        return s;
    }
    f = pc_document_pixel_format(&d);
    s = pc_layer_raster(&bg, "Background", f, err);
    if (s != PC_OK) {
        pc_document_fini(&d);
        return s;
    }
    bg.locks.transparency = true;
    bg.locks.position = true;
    pc_color_to_rgb(&fill, rgb);
    rgba[0] = rgb[0];
    rgba[1] = rgb[1];
    rgba[2] = rgb[2];
    rgba[3] = fill.alpha;
    n = pc_from_rgba(pc_surface_format(&bg.content.u.raster), rgba, px);
    s = pc_surface_fill_rect(&bg.content.u.raster, pc_document_bounds(&d), px, n, err);
    if (s == PC_OK) {
        d.layers = pc_malloc_array(1, sizeof *d.layers);
        if (d.layers == NULL) {
            s = pc_fail(err, PC_ERR_NOMEM, "out of memory adding the background layer");
        }
    }
    if (s != PC_OK) {
        pc_layer_fini(&bg);
        pc_document_fini(&d);
        return s;
    }
    d.layers[0] = bg;
    d.nlayers = 1;
    *out = d;
    return PC_OK;
}

/* ---- fini / clone / eq -------------------------------------------------------------------- */

void pc_document_fini(pc_document *d)
{
    if (d == NULL) {
        return;
    }
    pc_str_fini(&d->name);
    pc_blob_release(d->icc_profile);
    pc_doc_fini_layers(d->layers, d->nlayers);
    PC_DOC_FINI_VEC(d->channels, d->nchannels, pc_alpha_channel_fini);
    pc_guides_fini(&d->guides);
    if (d->has_selection) {
        pc_surface_fini(&d->selection);
    }
    pc_metadata_fini(&d->metadata);
    PC_DOC_FINI_VEC(d->paths, d->npaths, pc_named_path_fini);
    if (d->has_work_path) {
        pc_path_fini(&d->work_path);
    }
    if (d->has_clipping_path) {
        pc_clipping_path_fini(&d->clipping_path);
    }
    if (d->has_quick_mask) {
        pc_alpha_channel_fini(&d->quick_mask);
    }
    PC_DOC_FINI_VEC(d->patterns, d->npatterns, pc_pattern_fini);
    if (d->has_color_table) {
        pc_color_table_fini(&d->color_table);
    }
    if (d->has_duotone) {
        pc_duotone_fini(&d->duotone);
    }
    PC_DOC_FINI_VEC(d->layer_comps, d->nlayer_comps, pc_layer_comp_fini);
    if (d->has_last_document_state) {
        pc_layer_comp_fini(&d->last_document_state);
    }
    pc_measurement_fini(&d->measurement);
    PC_DOC_FINI_VEC(d->notes, d->nnotes, pc_note_fini);
    pc_text_styles_fini(&d->text_styles);
    pc_slices_fini(&d->slices);
    pc_variables_fini(&d->variables);
    memset(d, 0, sizeof *d);
}

pc_status pc_document_clone(const pc_document *d, pc_document *out, pc_error *err)
{
    pc_document c;
    pc_status s;
    memset(&c, 0, sizeof c);
    c.id = d->id;
    c.size = d->size;
    c.resolution_dpi = d->resolution_dpi;
    c.mode = d->mode;
    c.depth = d->depth;
    c.global_light = d->global_light;
    c.has_last_applied_comp = d->has_last_applied_comp;
    c.last_applied_comp = d->last_applied_comp;
    c.has_timeline = d->has_timeline;
    c.timeline = d->timeline;
    s = pc_str_clone(&d->name, &c.name, err);
    if (s != PC_OK) {
        goto fail;
    }
    s = pc_doc_clone_layers(d->layers, d->nlayers, false, &c.layers, err);
    if (s != PC_OK) {
        goto fail;
    }
    c.nlayers = c.layers != NULL ? d->nlayers : 0;
    PC_DOC_CLONE_VEC(s, d->channels, d->nchannels, c.channels, pc_alpha_channel_clone, pc_alpha_channel_fini,
                     err);
    if (s != PC_OK) {
        goto fail;
    }
    c.nchannels = d->nchannels;
    s = pc_guides_clone(&d->guides, &c.guides, err);
    if (s != PC_OK) {
        goto fail;
    }
    if (d->has_selection) {
        s = pc_surface_clone(&d->selection, &c.selection, err);
        if (s != PC_OK) {
            goto fail;
        }
        c.has_selection = true;
    }
    s = pc_metadata_clone(&d->metadata, &c.metadata, err);
    if (s != PC_OK) {
        goto fail;
    }
    PC_DOC_CLONE_VEC(s, d->paths, d->npaths, c.paths, pc_named_path_clone, pc_named_path_fini, err);
    if (s != PC_OK) {
        goto fail;
    }
    c.npaths = d->npaths;
    if (d->has_work_path) {
        s = pc_path_clone(&d->work_path, &c.work_path, err);
        if (s != PC_OK) {
            goto fail;
        }
        c.has_work_path = true;
    }
    if (d->has_clipping_path) {
        s = pc_clipping_path_clone(&d->clipping_path, &c.clipping_path, err);
        if (s != PC_OK) {
            goto fail;
        }
        c.has_clipping_path = true;
    }
    if (d->has_quick_mask) {
        s = pc_alpha_channel_clone(&d->quick_mask, &c.quick_mask, err);
        if (s != PC_OK) {
            goto fail;
        }
        c.has_quick_mask = true;
    }
    PC_DOC_CLONE_VEC(s, d->patterns, d->npatterns, c.patterns, pc_pattern_clone, pc_pattern_fini, err);
    if (s != PC_OK) {
        goto fail;
    }
    c.npatterns = d->npatterns;
    if (d->has_color_table) {
        s = pc_color_table_clone(&d->color_table, &c.color_table, err);
        if (s != PC_OK) {
            goto fail;
        }
        c.has_color_table = true;
    }
    if (d->has_duotone) {
        s = pc_duotone_clone(&d->duotone, &c.duotone, err);
        if (s != PC_OK) {
            goto fail;
        }
        c.has_duotone = true;
    }
    PC_DOC_CLONE_VEC(s, d->layer_comps, d->nlayer_comps, c.layer_comps, pc_layer_comp_clone,
                     pc_layer_comp_fini, err);
    if (s != PC_OK) {
        goto fail;
    }
    c.nlayer_comps = d->nlayer_comps;
    if (d->has_last_document_state) {
        s = pc_layer_comp_clone(&d->last_document_state, &c.last_document_state, err);
        if (s != PC_OK) {
            goto fail;
        }
        c.has_last_document_state = true;
    }
    s = pc_measurement_clone(&d->measurement, &c.measurement, err);
    if (s != PC_OK) {
        goto fail;
    }
    PC_DOC_CLONE_VEC(s, d->notes, d->nnotes, c.notes, pc_note_clone, pc_note_fini, err);
    if (s != PC_OK) {
        goto fail;
    }
    c.nnotes = d->nnotes;
    s = pc_text_styles_clone(&d->text_styles, &c.text_styles, err);
    if (s != PC_OK) {
        goto fail;
    }
    s = pc_slices_clone(&d->slices, &c.slices, err);
    if (s != PC_OK) {
        goto fail;
    }
    s = pc_variables_clone(&d->variables, &c.variables, err);
    if (s != PC_OK) {
        goto fail;
    }
    c.icc_profile = pc_blob_retain(d->icc_profile);
    *out = c;
    return PC_OK;
fail:
    pc_document_fini(&c);
    return s;
}

bool pc_document_eq(const pc_document *a, const pc_document *b)
{
    bool same;
    if (a->id != b->id || !pc_str_eq(&a->name, &b->name) || a->size.width != b->size.width ||
        a->size.height != b->size.height || !(a->resolution_dpi == b->resolution_dpi) || a->mode != b->mode ||
        a->depth != b->depth || !pc_blob_eq(a->icc_profile, b->icc_profile)) {
        return false;
    }
    PC_DOC_EQ_VEC(same, a->channels, a->nchannels, b->channels, b->nchannels, pc_alpha_channel_eq);
    if (!same || !pc_guides_eq(&a->guides, &b->guides) || a->has_selection != b->has_selection ||
        (a->has_selection && !pc_surface_eq(&a->selection, &b->selection)) ||
        !pc_metadata_eq(&a->metadata, &b->metadata) || !(a->global_light.angle == b->global_light.angle) ||
        !(a->global_light.altitude == b->global_light.altitude)) {
        return false;
    }
    PC_DOC_EQ_VEC(same, a->paths, a->npaths, b->paths, b->npaths, pc_named_path_eq);
    if (!same || a->has_work_path != b->has_work_path ||
        (a->has_work_path && !pc_path_eq(&a->work_path, &b->work_path)) ||
        a->has_clipping_path != b->has_clipping_path ||
        (a->has_clipping_path && !pc_clipping_path_eq(&a->clipping_path, &b->clipping_path)) ||
        a->has_quick_mask != b->has_quick_mask ||
        (a->has_quick_mask && !pc_alpha_channel_eq(&a->quick_mask, &b->quick_mask))) {
        return false;
    }
    PC_DOC_EQ_VEC(same, a->patterns, a->npatterns, b->patterns, b->npatterns, pc_pattern_eq);
    if (!same || a->has_color_table != b->has_color_table ||
        (a->has_color_table && !pc_color_table_eq(&a->color_table, &b->color_table)) ||
        a->has_duotone != b->has_duotone || (a->has_duotone && !pc_duotone_eq(&a->duotone, &b->duotone))) {
        return false;
    }
    PC_DOC_EQ_VEC(same, a->layer_comps, a->nlayer_comps, b->layer_comps, b->nlayer_comps, pc_layer_comp_eq);
    if (!same || a->has_last_applied_comp != b->has_last_applied_comp ||
        (a->has_last_applied_comp && a->last_applied_comp != b->last_applied_comp) ||
        a->has_last_document_state != b->has_last_document_state ||
        (a->has_last_document_state && !pc_layer_comp_eq(&a->last_document_state, &b->last_document_state)) ||
        !pc_measurement_eq(&a->measurement, &b->measurement)) {
        return false;
    }
    PC_DOC_EQ_VEC(same, a->notes, a->nnotes, b->notes, b->nnotes, pc_note_eq);
    if (!same || !pc_text_styles_eq(&a->text_styles, &b->text_styles) ||
        !pc_slices_eq(&a->slices, &b->slices) || !pc_variables_eq(&a->variables, &b->variables) ||
        a->has_timeline != b->has_timeline ||
        (a->has_timeline && !pc_timeline_eq(&a->timeline, &b->timeline))) {
        return false;
    }
    /* Layers last: the deepest comparison. */
    return pc_doc_layers_eq(a->layers, a->nlayers, b->layers, b->nlayers);
}

/* ---- simple queries ----------------------------------------------------------------------- */

pc_pixel_format pc_document_pixel_format(const pc_document *d)
{
    pc_pixel_format f;
    pc_color_mode m = d->mode;
    if (m == PC_COLOR_MODE_INDEXED || m == PC_COLOR_MODE_MULTICHANNEL) {
        m = PC_COLOR_MODE_RGB;
    } else if (m == PC_COLOR_MODE_BITMAP || m == PC_COLOR_MODE_DUOTONE) {
        m = PC_COLOR_MODE_GRAYSCALE;
    }
    f.mode = m;
    f.sample = d->depth;
    f.alpha = true;
    return f;
}

pc_rect pc_document_bounds(const pc_document *d)
{
    return pc_rect_from_size(d->size);
}

pc_status pc_document_max_group_depth(const pc_document *d, size_t *out, pc_error *err)
{
    /* The deepest layer's depth equals the reference's "deepest non-empty level". */
    pc_doc_iter it;
    const pc_layer *l;
    size_t max = 0;
    int r;
    pc_doc_iter_init(&it, d->layers, d->nlayers);
    while ((r = pc_doc_iter_next(&it, &l)) == 1) {
        size_t depth = pc_doc_iter_depth(&it);
        if (depth > max) {
            max = depth;
        }
    }
    pc_doc_iter_fini(&it);
    if (r < 0) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory measuring group depth");
    }
    *out = max;
    return PC_OK;
}

void pc_layer_path_fini(pc_layer_path *p)
{
    if (p == NULL) {
        return;
    }
    free(p->idx);
    p->idx = NULL;
    p->len = 0;
}

void pc_walk_fini(pc_walk *w)
{
    if (w == NULL) {
        return;
    }
    free(w->items);
    free(w->paths);
    memset(w, 0, sizeof *w);
}

pc_status pc_document_walk(const pc_document *d, pc_walk *out, pc_error *err)
{
    pc_doc_iter it;
    const pc_layer *l;
    pc_walk w;
    size_t cap_items = 0;
    size_t cap_paths = 0;
    size_t npaths = 0;
    int r;
    memset(&w, 0, sizeof w);
    pc_doc_iter_init(&it, d->layers, d->nlayers);
    while ((r = pc_doc_iter_next(&it, &l)) == 1) {
        size_t depth = pc_doc_iter_depth(&it);
        size_t start;
        size_t from;
        size_t need;
        size_t k;
        pc_walk_item *last = w.len > 0 ? &w.items[w.len - 1] : NULL;
        if (w.len == cap_items) {
            pc_walk_item *v = pc_grow_array(w.items, &cap_items, w.len + 1, sizeof *w.items);
            if (v == NULL) {
                r = -1;
                break;
            }
            w.items = v;
            last = w.len > 0 ? &w.items[w.len - 1] : NULL;
        }
        /* A first child directly follows its parent, whose path is the tail of `paths`: share
         * that prefix instead of copying it (keeps a deep chain linear in size). */
        if (last != NULL && depth == last->depth + 1 && last->path_start + depth == npaths) {
            start = last->path_start;
            from = depth;
        } else {
            start = npaths;
            from = 0;
        }
        if (!pc_size_add(npaths, depth + 1 - from, &need)) {
            r = -1;
            break;
        }
        if (need > cap_paths) {
            size_t *v = pc_grow_array(w.paths, &cap_paths, need, sizeof *w.paths);
            if (v == NULL) {
                r = -1;
                break;
            }
            w.paths = v;
        }
        for (k = from; k <= depth; k++) {
            w.paths[npaths++] = pc_doc_iter_index(&it, k);
        }
        w.items[w.len].layer = l;
        w.items[w.len].depth = depth;
        w.items[w.len].path_start = start;
        w.len++;
    }
    pc_doc_iter_fini(&it);
    if (r < 0) {
        pc_walk_fini(&w);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory walking the layer tree");
    }
    *out = w;
    return PC_OK;
}

pc_status pc_document_layer_count(const pc_document *d, size_t *out, pc_error *err)
{
    pc_doc_iter it;
    const pc_layer *l;
    size_t n = 0;
    int r;
    pc_doc_iter_init(&it, d->layers, d->nlayers);
    while ((r = pc_doc_iter_next(&it, &l)) == 1) {
        n++;
    }
    pc_doc_iter_fini(&it);
    if (r < 0) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory counting layers");
    }
    *out = n;
    return PC_OK;
}

pc_status pc_document_path_of(const pc_document *d, pc_layer_id id, pc_layer_path *out, pc_error *err)
{
    pc_doc_iter it;
    const pc_layer *l;
    pc_layer_path p;
    size_t k;
    int r = pc_doc_iter_find(&it, d->layers, d->nlayers, id, &l);
    if (r <= 0) {
        pc_doc_iter_fini(&it);
        if (r < 0) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory finding layer %" PRIu64, id);
        }
        return pc_fail(err, PC_ERR_NOT_FOUND, "no layer with id %" PRIu64, id);
    }
    p.len = pc_doc_iter_depth(&it) + 1;
    p.idx = pc_malloc_array(p.len, sizeof *p.idx);
    if (p.idx == NULL) {
        pc_doc_iter_fini(&it);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory finding layer %" PRIu64, id);
    }
    for (k = 0; k < p.len; k++) {
        p.idx[k] = pc_doc_iter_index(&it, k);
    }
    pc_doc_iter_fini(&it);
    *out = p;
    return PC_OK;
}

pc_locks pc_document_locks_at(const pc_document *d, const size_t *path, size_t n)
{
    pc_locks acc = {false, false, false, false, false};
    const pc_layer *arr = d->layers;
    size_t na = d->nlayers;
    size_t k;
    for (k = 0; k < n; k++) {
        const pc_layer *l;
        if (arr == NULL || path[k] >= na) {
            break;
        }
        l = &arr[path[k]];
        acc = pc_locks_union(acc, l->locks);
        arr = pc_layer_children(l, &na);
    }
    return acc;
}

pc_locks pc_document_effective_locks(const pc_document *d, pc_layer_id id)
{
    pc_locks acc = {false, false, false, false, false};
    pc_doc_iter it;
    const pc_layer *l;
    if (pc_doc_iter_find(&it, d->layers, d->nlayers, id, &l) == 1) {
        size_t depth = pc_doc_iter_depth(&it);
        size_t k;
        for (k = 0; k <= depth; k++) {
            acc = pc_locks_union(acc, it.frames[k].arr[it.frames[k].i - 1].locks);
        }
    }
    pc_doc_iter_fini(&it);
    return acc;
}

const pc_layer *pc_document_layer(const pc_document *d, pc_layer_id id)
{
    pc_doc_iter it;
    const pc_layer *l = NULL;
    if (pc_doc_iter_find(&it, d->layers, d->nlayers, id, &l) != 1) {
        l = NULL;
    }
    pc_doc_iter_fini(&it);
    return l;
}

/* The sibling array (as owner fields) and index of layer id: 1 found, 0 not, -1 no memory. */
static int locate_mut(pc_document *d, pc_layer_id id, pc_layer ***arr, size_t **n, size_t *idx)
{
    pc_doc_iter it;
    const pc_layer *l;
    int r = pc_doc_iter_find(&it, d->layers, d->nlayers, id, &l);
    if (r == 1) {
        size_t depth = pc_doc_iter_depth(&it);
        pc_layer **a = &d->layers;
        size_t *na = &d->nlayers;
        size_t k;
        for (k = 0; k < depth; k++) {
            pc_layer *g = &(*a)[pc_doc_iter_index(&it, k)];
            a = &g->content.u.group.children;
            na = &g->content.u.group.nchildren;
        }
        *arr = a;
        *n = na;
        *idx = pc_doc_iter_index(&it, depth);
    }
    pc_doc_iter_fini(&it);
    return r;
}

pc_layer *pc_document_layer_mut(pc_document *d, pc_layer_id id)
{
    pc_layer **arr;
    size_t *n;
    size_t idx;
    if (locate_mut(d, id, &arr, &n, &idx) != 1) {
        return NULL;
    }
    return &(*arr)[idx];
}

const pc_layer *pc_document_layer_at(const pc_document *d, const size_t *path, size_t n)
{
    const pc_layer *arr = d->layers;
    size_t na = d->nlayers;
    const pc_layer *cur = NULL;
    size_t k;
    if (n == 0) {
        return NULL;
    }
    for (k = 0; k < n; k++) {
        if (arr == NULL || path[k] >= na) {
            return NULL;
        }
        cur = &arr[path[k]];
        arr = pc_layer_children(cur, &na);
    }
    return cur;
}

pc_layer *pc_document_layer_at_mut(pc_document *d, const size_t *path, size_t n)
{
    pc_layer *arr = d->layers;
    size_t na = d->nlayers;
    pc_layer *cur = NULL;
    size_t k;
    if (n == 0) {
        return NULL;
    }
    for (k = 0; k < n; k++) {
        if (arr == NULL || path[k] >= na) {
            return NULL;
        }
        cur = &arr[path[k]];
        if (cur->content.kind == PC_LAYER_GROUP) {
            arr = cur->content.u.group.children;
            na = cur->content.u.group.nchildren;
        } else {
            arr = NULL;
            na = 0;
        }
    }
    return cur;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

pc_status pc_document_unused_link_group(const pc_document *d, bool *found, uint64_t *out, pc_error *err)
{
    pc_doc_iter it;
    const pc_layer *l;
    uint64_t *used = NULL;
    size_t len = 0;
    size_t cap = 0;
    size_t i;
    uint64_t free_id;
    int r;
    pc_doc_iter_init(&it, d->layers, d->nlayers);
    while ((r = pc_doc_iter_next(&it, &l)) == 1) {
        if (!l->has_link_group) {
            continue;
        }
        if (len == cap) {
            uint64_t *v = pc_grow_array(used, &cap, len + 1, sizeof *used);
            if (v == NULL) {
                r = -1;
                break;
            }
            used = v;
        }
        used[len++] = l->link_group;
    }
    pc_doc_iter_fini(&it);
    if (r < 0) {
        free(used);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory listing link groups");
    }
    if (len == 0) {
        *found = true;
        *out = 1;
        return PC_OK;
    }
    qsort(used, len, sizeof *used, cmp_u64);
    if (used[len - 1] != UINT64_MAX) {
        *found = true;
        *out = used[len - 1] + 1;
        free(used);
        return PC_OK;
    }
    /* A stored u64::MAX (#961): the smallest free id from 1 (sorted, so duplicates are
     * harmless here). */
    free_id = 1;
    for (i = 0; i < len; i++) {
        if (used[i] > free_id) {
            break;
        }
        if (used[i] == free_id) {
            if (free_id == UINT64_MAX) {
                free(used);
                *found = false;
                *out = 0;
                return PC_OK;
            }
            free_id++;
        }
    }
    free(used);
    *found = true;
    *out = free_id;
    return PC_OK;
}

/* ---- edits -------------------------------------------------------------------------------- */

static pc_status array_insert(pc_layer **arr, size_t *n, size_t at, pc_layer *layer, pc_error *err)
{
    size_t cap = *n;
    size_t need;
    pc_layer *v;
    if (!pc_size_add(*n, 1, &need)) {
        return pc_fail(err, PC_ERR_LIMIT, "too many layers");
    }
    v = pc_grow_array(*arr, &cap, need, sizeof *v);
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory inserting a layer");
    }
    if (at < *n) {
        memmove(&v[at + 1], &v[at], (*n - at) * sizeof *v);
    }
    v[at] = *layer;
    memset(layer, 0, sizeof *layer);
    *arr = v;
    *n = need;
    return PC_OK;
}

pc_status pc_document_insert_above(pc_document *d, const pc_layer_id *above, pc_layer *layer,
                                   pc_layer_id *out_id, pc_error *err)
{
    pc_layer_id id = layer->id;
    pc_status s;
    if (above != NULL) {
        pc_layer **arr;
        size_t *n;
        size_t idx;
        int r = locate_mut(d, *above, &arr, &n, &idx);
        if (r < 0) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory finding layer %" PRIu64, *above);
        }
        if (r == 1) {
            s = array_insert(arr, n, idx + 1, layer, err);
            if (s == PC_OK && out_id != NULL) {
                *out_id = id;
            }
            return s;
        }
    }
    s = array_insert(&d->layers, &d->nlayers, d->nlayers, layer, err);
    if (s == PC_OK && out_id != NULL) {
        *out_id = id;
    }
    return s;
}

pc_status pc_document_remove(pc_document *d, pc_layer_id id, pc_layer *out, pc_error *err)
{
    pc_layer **arr;
    size_t *n;
    size_t idx;
    pc_layer *a;
    int r = locate_mut(d, id, &arr, &n, &idx);
    if (r < 0) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory finding layer %" PRIu64, id);
    }
    if (r == 0) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "no layer with id %" PRIu64, id);
    }
    a = *arr;
    *out = a[idx];
    if (idx + 1 < *n) {
        memmove(&a[idx], &a[idx + 1], (*n - idx - 1) * sizeof *a);
    }
    (*n)--;
    if (*n == 0) {
        free(a);
        *arr = NULL;
    }
    return PC_OK;
}

bool pc_document_shift(pc_document *d, pc_layer_id id, int32_t delta)
{
    pc_layer **arr;
    size_t *n;
    size_t idx;
    size_t dst;
    pc_layer tmp;
    pc_layer *a;
    int64_t to;
    if (locate_mut(d, id, &arr, &n, &idx) != 1 || idx > (size_t)INT64_MAX - (size_t)INT32_MAX) {
        return false;
    }
    to = (int64_t)idx + (int64_t)delta;
    if (to < 0 || (uint64_t)to >= (uint64_t)*n) {
        return false;
    }
    dst = (size_t)to;
    a = *arr;
    tmp = a[idx];
    if (dst > idx) {
        memmove(&a[idx], &a[idx + 1], (dst - idx) * sizeof *a);
    } else if (dst < idx) {
        memmove(&a[dst + 1], &a[dst], (idx - dst) * sizeof *a);
    }
    a[dst] = tmp;
    return true;
}

/* ---- names -------------------------------------------------------------------------------- */

static int cmp_str_ptr(const void *a, const void *b)
{
    return pc_str_cmp(*(const pc_str *const *)a, *(const pc_str *const *)b);
}

/* Every layer name, sorted for binary search. */
static pc_status sorted_names(const pc_document *d, const pc_str ***out, size_t *n, pc_error *err)
{
    pc_doc_iter it;
    const pc_layer *l;
    const pc_str **names = NULL;
    size_t len = 0;
    size_t cap = 0;
    int r;
    pc_doc_iter_init(&it, d->layers, d->nlayers);
    while ((r = pc_doc_iter_next(&it, &l)) == 1) {
        if (len == cap) {
            const pc_str **v = pc_grow_array(names, &cap, len + 1, sizeof *names);
            if (v == NULL) {
                r = -1;
                break;
            }
            names = v;
        }
        names[len++] = &l->name;
    }
    pc_doc_iter_fini(&it);
    if (r < 0) {
        free(names);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory listing layer names");
    }
    if (len > 1) {
        qsort(names, len, sizeof *names, cmp_str_ptr);
    }
    *out = names;
    *n = len;
    return PC_OK;
}

static bool has_name(const pc_str **names, size_t n, char *p, size_t len)
{
    pc_str key;
    const pc_str *kp = &key;
    key.ptr = p;
    key.len = len;
    return n > 0 && bsearch(&kp, names, n, sizeof *names, cmp_str_ptr) != NULL;
}

/* buf (cap bytes) = prefix + suffix + optional " N"; returns the length. cap must fit. */
static size_t compose(char *buf, const char *prefix, size_t plen, const char *suffix, size_t slen,
                      bool numbered, size_t num)
{
    size_t len = 0;
    if (plen > 0) {
        memcpy(buf, prefix, plen);
    }
    len += plen;
    if (slen > 0) {
        memcpy(buf + len, suffix, slen);
    }
    len += slen;
    if (numbered) {
        char digits[32];
        int k = snprintf(digits, sizeof digits, " %zu", num);
        if (k > 0) {
            memcpy(buf + len, digits, (size_t)k);
            len += (size_t)k;
        }
    }
    buf[len] = '\0';
    return len;
}

pc_status pc_document_next_layer_name(const pc_document *d, const char *base, pc_str *out, pc_error *err)
{
    const pc_str **names;
    size_t n;
    size_t blen = base != NULL ? strlen(base) : 0;
    size_t cap;
    size_t k;
    char *buf;
    pc_status s = sorted_names(d, &names, &n, err);
    if (s != PC_OK) {
        return s;
    }
    if (!pc_size_add(blen, 40, &cap) || (buf = pc_malloc(cap)) == NULL) {
        free(names);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory naming a layer");
    }
    /* At most n + 1 candidates are needed, so the search always succeeds. */
    for (k = 1; k <= n + 1; k++) {
        size_t len = compose(buf, base, blen, NULL, 0, true, k);
        if (!has_name(names, n, buf, len)) {
            free(names);
            *out = pc_str_adopt(buf, len);
            return PC_OK;
        }
    }
    free(names);
    free(buf);
    return pc_str_set(out, base, blen, err);
}

pc_status pc_document_copy_name(const pc_document *d, const char *name, size_t len, pc_str *out,
                                pc_error *err)
{
    static const char copy[] = " copy";
    const size_t clen = sizeof copy - 1;
    const pc_str **names;
    size_t n;
    size_t root = len;
    size_t cap;
    size_t k;
    size_t blen;
    char *buf;
    pc_status s;
    if (name == NULL) {
        len = 0;
        root = 0;
    }
    /* rsplit_once(" copy"): the last occurrence, kept as the root only when what follows is
     * nothing or " <digits>". */
    if (len >= clen) {
        size_t pos = len - clen + 1;
        while (pos > 0) {
            pos--;
            if (memcmp(name + pos, copy, clen) == 0) {
                const char *rest = name + pos + clen;
                size_t rlen = len - pos - clen;
                bool ok = rlen == 0;
                if (rlen >= 2 && rest[0] == ' ') {
                    size_t j;
                    ok = true;
                    for (j = 1; j < rlen; j++) {
                        if (rest[j] < '0' || rest[j] > '9') {
                            ok = false;
                            break;
                        }
                    }
                }
                if (ok) {
                    root = pos;
                }
                break;
            }
        }
    }
    s = sorted_names(d, &names, &n, err);
    if (s != PC_OK) {
        return s;
    }
    if (!pc_size_add(root, clen + 40, &cap) || (buf = pc_malloc(cap)) == NULL) {
        free(names);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory naming a copy");
    }
    blen = compose(buf, name, root, copy, clen, false, 0);
    if (!has_name(names, n, buf, blen)) {
        free(names);
        *out = pc_str_adopt(buf, blen);
        return PC_OK;
    }
    /* At most n + 1 candidates are needed, so the search always succeeds. */
    for (k = 2; k <= n + 2; k++) {
        size_t l2 = compose(buf, name, root, copy, clen, true, k);
        if (!has_name(names, n, buf, l2)) {
            free(names);
            *out = pc_str_adopt(buf, l2);
            return PC_OK;
        }
    }
    free(names);
    blen = compose(buf, name, root, copy, clen, false, 0);
    *out = pc_str_adopt(buf, blen);
    return PC_OK;
}

bool pc_document_top_layer(const pc_document *d, pc_layer_id *out)
{
    if (d->nlayers == 0 || d->layers == NULL) {
        return false;
    }
    *out = d->layers[d->nlayers - 1].id;
    return true;
}
