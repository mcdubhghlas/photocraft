/* Port of crates/doc/src/lib.rs, part 1: ids, locks, labels, masks, effects, smart objects and
 * layers (layer trees are handled without recursion, see fini_layer_array and clone_children).
 * The document itself is in document.c. */
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "doc/doc.h"
#include "doc/tree.h"

#ifdef __STDC_NO_ATOMICS__
#error "PhotoCraft needs C11 atomics"
#endif

/* fini_layer_array parks a parent pointer in a uint64_t field. */
_Static_assert(UINTPTR_MAX <= UINT64_MAX, "pointers must fit in 64 bits");

/* ---- ids ---------------------------------------------------------------------------------- */

/* One counter for layers and documents, like the reference's NEXT_ID. */
static _Atomic uint64_t next_id = 1;

static uint64_t fresh_id(void)
{
    return atomic_fetch_add_explicit(&next_id, 1, memory_order_relaxed);
}

pc_layer_id pc_layer_id_fresh(void)
{
    return fresh_id();
}

pc_doc_id pc_doc_id_fresh(void)
{
    return fresh_id();
}

void pc_ensure_ids_above(uint64_t max)
{
    /* AtomicU64::fetch_max(max.saturating_add(1)) as a CAS loop. */
    uint64_t want = max == UINT64_MAX ? UINT64_MAX : max + 1;
    uint64_t cur = atomic_load_explicit(&next_id, memory_order_relaxed);
    while (cur < want && !atomic_compare_exchange_weak_explicit(&next_id, &cur, want, memory_order_relaxed,
                                                                memory_order_relaxed)) {
        /* cur was reloaded by the failed exchange */
    }
}

/* ---- locks and labels --------------------------------------------------------------------- */

pc_locks pc_locks_union(pc_locks a, pc_locks b)
{
    pc_locks r;
    r.transparency = a.transparency || b.transparency;
    r.pixels = a.pixels || b.pixels;
    r.position = a.position || b.position;
    r.artboard = a.artboard || b.artboard;
    r.all = a.all || b.all;
    return r;
}

bool pc_locks_eq(pc_locks a, pc_locks b)
{
    return a.transparency == b.transparency && a.pixels == b.pixels && a.position == b.position &&
           a.artboard == b.artboard && a.all == b.all;
}

const pc_label_color PC_LABEL_COLOR_ALL[PC_LABEL_COLOR_COUNT] = {
    PC_LABEL_NONE, PC_LABEL_RED,    PC_LABEL_ORANGE,  PC_LABEL_YELLOW,  PC_LABEL_GREEN,  PC_LABEL_SEAFOAM,
    PC_LABEL_BLUE, PC_LABEL_INDIGO, PC_LABEL_MAGENTA, PC_LABEL_FUCHSIA, PC_LABEL_VIOLET, PC_LABEL_GRAY,
};

/* Indexed by enum (declaration) order. */
static const char *const label_ids[PC_LABEL_COLOR_COUNT] = {
    "none",   "red",  "orange",  "yellow", "green",   "blue",
    "violet", "gray", "seafoam", "indigo", "magenta", "fuchsia",
};
static const char *const label_labels[PC_LABEL_COLOR_COUNT] = {
    "No Color", "Red",  "Orange",  "Yellow", "Green",   "Blue",
    "Violet",   "Gray", "Seafoam", "Indigo", "Magenta", "Fuchsia",
};
static const char *const label_names[PC_LABEL_COLOR_COUNT] = {
    "None",   "Red",  "Orange",  "Yellow", "Green",   "Blue",
    "Violet", "Gray", "Seafoam", "Indigo", "Magenta", "Fuchsia",
};

static size_t label_index(pc_label_color c)
{
    size_t i = (size_t)c;
    return i < PC_LABEL_COLOR_COUNT ? i : 0;
}

const char *pc_label_color_id(pc_label_color c)
{
    return label_ids[label_index(c)];
}

const char *pc_label_color_label(pc_label_color c)
{
    return label_labels[label_index(c)];
}

const char *pc_label_color_name(pc_label_color c)
{
    return label_names[label_index(c)];
}

bool pc_label_color_from_id(const char *id, size_t len, pc_label_color *out)
{
    size_t i;
    if (id == NULL && len > 0) {
        return false;
    }
    for (i = 0; i < PC_LABEL_COLOR_COUNT; i++) {
        pc_label_color c = PC_LABEL_COLOR_ALL[i];
        const char *s = label_ids[(size_t)c];
        if (strlen(s) == len && (len == 0 || memcmp(s, id, len) == 0)) {
            *out = c;
            return true;
        }
    }
    return false;
}

/* ---- layer masks -------------------------------------------------------------------------- */

static void mask_with_default(pc_layer_mask *out, float v)
{
    memset(out, 0, sizeof *out);
    pc_surface_init_default(&out->surface, PC_PIXEL_FORMAT_GRAY8, &v, 1);
    out->enabled = true;
    out->linked = true;
    out->density = 1.0f;
    out->feather = 0.0f;
}

void pc_layer_mask_reveal_all(pc_layer_mask *out)
{
    mask_with_default(out, 1.0f);
}

void pc_layer_mask_hide_all(pc_layer_mask *out)
{
    mask_with_default(out, 0.0f);
}

void pc_layer_mask_fini(pc_layer_mask *m)
{
    if (m == NULL) {
        return;
    }
    pc_surface_fini(&m->surface);
    memset(m, 0, sizeof *m);
}

pc_status pc_layer_mask_clone(const pc_layer_mask *m, pc_layer_mask *out, pc_error *err)
{
    pc_layer_mask c = *m;
    pc_status s = pc_surface_clone(&m->surface, &c.surface, err);
    if (s != PC_OK) {
        return s;
    }
    *out = c;
    return PC_OK;
}

bool pc_layer_mask_eq(const pc_layer_mask *a, const pc_layer_mask *b)
{
    return pc_surface_eq(&a->surface, &b->surface) && a->enabled == b->enabled && a->linked == b->linked &&
           a->density == b->density && a->feather == b->feather;
}

float pc_layer_mask_value(const pc_layer_mask *m, int32_t x, int32_t y)
{
    float v;
    if (!m->enabled) {
        return 1.0f;
    }
    v = pc_surface_sample_channel(&m->surface, x, y, 0);
    return 1.0f - m->density * (1.0f - v);
}

pc_status pc_layer_mask_values_into(const pc_layer_mask *m, pc_rect r, float *out, size_t n, pc_error *err)
{
    size_t want;
    size_t ch;
    size_t i;
    if (!pc_size_mul(pc_rect_width(r), pc_rect_height(r), &want) || want != n || (out == NULL && n > 0)) {
        return pc_fail(err, PC_ERR_INVALID, "mask values: buffer of %zu values for a %ux%u region", n,
                       pc_rect_width(r), pc_rect_height(r));
    }
    if (!m->enabled) {
        for (i = 0; i < n; i++) {
            out[i] = 1.0f;
        }
        return PC_OK;
    }
    ch = pc_surface_channels(&m->surface);
    if (ch <= 1) {
        pc_status s = pc_surface_read_region_into(&m->surface, r, out, n, err);
        if (s != PC_OK) {
            return s;
        }
    } else {
        /* A mask surface with more than one channel: keep channel 0 of each pixel. */
        size_t len;
        float *tmp;
        pc_status s;
        if (!pc_size_mul(n, ch, &len)) {
            return pc_fail(err, PC_ERR_LIMIT, "mask values: region too large");
        }
        tmp = pc_malloc_array(len, sizeof *tmp);
        if (tmp == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory reading mask values");
        }
        s = pc_surface_read_region_into(&m->surface, r, tmp, len, err);
        if (s != PC_OK) {
            free(tmp);
            return s;
        }
        for (i = 0; i < n; i++) {
            out[i] = tmp[i * ch];
        }
        free(tmp);
    }
    if (m->density < 1.0f) {
        for (i = 0; i < n; i++) {
            out[i] = 1.0f - m->density * (1.0f - out[i]);
        }
    }
    return PC_OK;
}

/* ---- effects (the list; single effects live in effects.c) --------------------------------- */

void pc_effects_fini(pc_effects *e)
{
    if (e == NULL) {
        return;
    }
    PC_DOC_FINI_VEC(e->items, e->nitems, pc_effect_fini);
    pc_blob_release(e->psd_raw);
    memset(e, 0, sizeof *e);
}

pc_status pc_effects_clone(const pc_effects *e, pc_effects *out, pc_error *err)
{
    pc_effects c = *e;
    pc_status s;
    PC_DOC_CLONE_VEC(s, e->items, e->nitems, c.items, pc_effect_clone, pc_effect_fini, err);
    if (s != PC_OK) {
        return s;
    }
    c.psd_raw = pc_blob_retain(e->psd_raw);
    *out = c;
    return PC_OK;
}

bool pc_effects_eq(const pc_effects *a, const pc_effects *b)
{
    bool same;
    if (a->enabled != b->enabled || !pc_blob_eq(a->psd_raw, b->psd_raw) ||
        a->has_reference != b->has_reference) {
        return false;
    }
    if (a->has_reference && !(a->reference[0] == b->reference[0] && a->reference[1] == b->reference[1])) {
        return false;
    }
    PC_DOC_EQ_VEC(same, a->items, a->nitems, b->items, b->nitems, pc_effect_eq);
    return same;
}

/* ---- smart objects ------------------------------------------------------------------------ */

static void smart_source_fini(pc_smart_source *s)
{
    pc_str_fini(&s->file_name);
    pc_str_fini(&s->path);
    pc_blob_release(s->bytes);
    memset(s, 0, sizeof *s);
}

static pc_status smart_source_clone(const pc_smart_source *s, pc_smart_source *out, pc_error *err)
{
    pc_smart_source c;
    pc_status st;
    memset(&c, 0, sizeof c);
    c.linked = s->linked;
    st = pc_str_clone(&s->file_name, &c.file_name, err);
    if (st == PC_OK) {
        st = pc_str_clone(&s->path, &c.path, err);
    }
    if (st != PC_OK) {
        smart_source_fini(&c);
        return st;
    }
    c.bytes = pc_blob_retain(s->bytes);
    *out = c;
    return PC_OK;
}

static bool smart_source_eq(const pc_smart_source *a, const pc_smart_source *b)
{
    if (a->linked != b->linked) {
        return false;
    }
    if (a->linked) {
        return pc_str_eq(&a->path, &b->path);
    }
    return pc_str_eq(&a->file_name, &b->file_name) && pc_doc_bytes_eq(a->bytes, b->bytes);
}

void pc_smart_filter_fini(pc_smart_filter *f)
{
    if (f == NULL) {
        return;
    }
    pc_str_fini(&f->command);
    pc_json_value_fini(&f->params);
    memset(f, 0, sizeof *f);
}

pc_status pc_smart_filter_clone(const pc_smart_filter *f, pc_smart_filter *out, pc_error *err)
{
    pc_smart_filter c = *f;
    pc_status s = pc_str_clone(&f->command, &c.command, err);
    if (s != PC_OK) {
        return s;
    }
    s = pc_json_value_clone(&f->params, &c.params, err);
    if (s != PC_OK) {
        pc_str_fini(&c.command);
        return s;
    }
    *out = c;
    return PC_OK;
}

bool pc_smart_filter_eq(const pc_smart_filter *a, const pc_smart_filter *b)
{
    return pc_str_eq(&a->command, &b->command) && pc_json_value_eq(&a->params, &b->params) &&
           a->blend == b->blend && a->opacity == b->opacity && a->visible == b->visible;
}

void pc_smart_object_init(pc_smart_object *out, pc_smart_source *source, pc_affine transform,
                          pc_surface *cache)
{
    memset(out, 0, sizeof *out);
    out->source = *source;
    memset(source, 0, sizeof *source);
    out->transform = transform;
    if (cache != NULL) {
        out->has_cache = true;
        out->cache = *cache;
        memset(cache, 0, sizeof *cache);
    }
    out->filters_enabled = true;
}

void pc_smart_object_fini(pc_smart_object *s)
{
    if (s == NULL) {
        return;
    }
    smart_source_fini(&s->source);
    PC_DOC_FINI_VEC(s->smart_filters, s->nsmart_filters, pc_smart_filter_fini);
    if (s->has_cache) {
        pc_surface_fini(&s->cache);
    }
    pc_blob_release(s->psd_raw);
    if (s->has_filter_mask) {
        pc_layer_mask_fini(&s->filter_mask);
    }
    if (s->has_warp) {
        pc_warp_fini(&s->warp);
    }
    memset(s, 0, sizeof *s);
}

pc_status pc_smart_object_clone(const pc_smart_object *s, pc_smart_object *out, pc_error *err)
{
    pc_smart_object c;
    pc_status st;
    memset(&c, 0, sizeof c);
    c.transform = s->transform;
    c.filters_enabled = s->filters_enabled;
    c.has_stack_mode = s->has_stack_mode;
    c.stack_mode = s->stack_mode;
    c.has_perspective = s->has_perspective;
    memcpy(c.perspective, s->perspective, sizeof c.perspective);
    st = smart_source_clone(&s->source, &c.source, err);
    if (st != PC_OK) {
        goto fail;
    }
    PC_DOC_CLONE_VEC(st, s->smart_filters, s->nsmart_filters, c.smart_filters, pc_smart_filter_clone,
                     pc_smart_filter_fini, err);
    if (st != PC_OK) {
        goto fail;
    }
    c.nsmart_filters = s->nsmart_filters;
    if (s->has_cache) {
        st = pc_surface_clone(&s->cache, &c.cache, err);
        if (st != PC_OK) {
            goto fail;
        }
        c.has_cache = true;
    }
    if (s->has_filter_mask) {
        st = pc_layer_mask_clone(&s->filter_mask, &c.filter_mask, err);
        if (st != PC_OK) {
            goto fail;
        }
        c.has_filter_mask = true;
    }
    if (s->has_warp) {
        st = pc_warp_clone(&s->warp, &c.warp, err);
        if (st != PC_OK) {
            goto fail;
        }
        c.has_warp = true;
    }
    c.psd_raw = pc_blob_retain(s->psd_raw);
    *out = c;
    return PC_OK;
fail:
    pc_smart_object_fini(&c);
    return st;
}

bool pc_smart_object_eq(const pc_smart_object *a, const pc_smart_object *b)
{
    bool same;
    size_t i;
    if (!smart_source_eq(&a->source, &b->source) || !pc_doc_affine_eq(&a->transform, &b->transform) ||
        a->has_cache != b->has_cache || (a->has_cache && !pc_surface_eq(&a->cache, &b->cache)) ||
        !pc_blob_eq(a->psd_raw, b->psd_raw) || a->filters_enabled != b->filters_enabled ||
        a->has_filter_mask != b->has_filter_mask ||
        (a->has_filter_mask && !pc_layer_mask_eq(&a->filter_mask, &b->filter_mask)) ||
        a->has_warp != b->has_warp || (a->has_warp && !pc_warp_eq(&a->warp, &b->warp)) ||
        a->has_stack_mode != b->has_stack_mode || (a->has_stack_mode && a->stack_mode != b->stack_mode) ||
        a->has_perspective != b->has_perspective) {
        return false;
    }
    if (a->has_perspective) {
        for (i = 0; i < 9; i++) {
            if (!(a->perspective[i] == b->perspective[i])) {
                return false;
            }
        }
    }
    PC_DOC_EQ_VEC(same, a->smart_filters, a->nsmart_filters, b->smart_filters, b->nsmart_filters,
                  pc_smart_filter_eq);
    return same;
}

/* ---- layer content and layers, one level at a time ---------------------------------------- */

static const pc_layer *group_children(const pc_layer_content *c, size_t *n)
{
    if (c->kind != PC_LAYER_GROUP || c->u.group.children == NULL) {
        *n = 0;
        return NULL;
    }
    *n = c->u.group.nchildren;
    return c->u.group.children;
}

/* Everything but a group's children (artboard included). */
static void content_fini_shallow(pc_layer_content *c)
{
    switch (c->kind) {
    case PC_LAYER_RASTER:
        pc_surface_fini(&c->u.raster);
        break;
    case PC_LAYER_GROUP:
        if (c->u.group.has_artboard) {
            pc_artboard_fini(&c->u.group.artboard);
            c->u.group.has_artboard = false;
        }
        break;
    case PC_LAYER_ADJUSTMENT:
        pc_adjustment_fini(&c->u.adjustment);
        break;
    case PC_LAYER_FILL:
        pc_fill_fini(&c->u.fill);
        break;
    case PC_LAYER_TEXT:
        pc_text_layer_fini(&c->u.text);
        break;
    case PC_LAYER_SHAPE:
        pc_shape_layer_fini(&c->u.shape);
        break;
    case PC_LAYER_SMART:
        pc_smart_object_fini(&c->u.smart);
        break;
    default:
        break;
    }
}

/* A copy whose group children (if any) are a zeroed array of the same length, to be filled by
 * clone_children. */
static pc_status content_clone_shallow(const pc_layer_content *c, pc_layer_content *out, pc_error *err)
{
    pc_layer_content r;
    pc_status s = PC_OK;
    memset(&r, 0, sizeof r);
    r.kind = c->kind;
    switch (c->kind) {
    case PC_LAYER_RASTER:
        s = pc_surface_clone(&c->u.raster, &r.u.raster, err);
        break;
    case PC_LAYER_GROUP: {
        size_t n;
        (void)group_children(c, &n);
        r.u.group.expanded = c->u.group.expanded;
        if (c->u.group.has_artboard) {
            s = pc_artboard_clone(&c->u.group.artboard, &r.u.group.artboard, err);
            if (s != PC_OK) {
                break;
            }
            r.u.group.has_artboard = true;
        }
        if (n > 0) {
            r.u.group.children = pc_calloc_array(n, sizeof *r.u.group.children);
            if (r.u.group.children == NULL) {
                if (r.u.group.has_artboard) {
                    pc_artboard_fini(&r.u.group.artboard);
                }
                s = pc_fail(err, PC_ERR_NOMEM, "out of memory copying a layer group");
                break;
            }
            r.u.group.nchildren = n;
        }
        break;
    }
    case PC_LAYER_ADJUSTMENT:
        s = pc_adjustment_clone(&c->u.adjustment, &r.u.adjustment, err);
        break;
    case PC_LAYER_FILL:
        s = pc_fill_clone(&c->u.fill, &r.u.fill, err);
        break;
    case PC_LAYER_TEXT:
        s = pc_text_layer_clone(&c->u.text, &r.u.text, err);
        break;
    case PC_LAYER_SHAPE:
        s = pc_shape_layer_clone(&c->u.shape, &r.u.shape, err);
        break;
    case PC_LAYER_SMART:
        s = pc_smart_object_clone(&c->u.smart, &r.u.smart, err);
        break;
    default:
        s = pc_fail(err, PC_ERR_INVALID, "unknown layer kind %d", (int)c->kind);
        break;
    }
    if (s != PC_OK) {
        return s;
    }
    *out = r;
    return PC_OK;
}

/* Equality of everything but the children themselves (their count included). */
static bool content_eq_shallow(const pc_layer_content *a, const pc_layer_content *b)
{
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case PC_LAYER_RASTER:
        return pc_surface_eq(&a->u.raster, &b->u.raster);
    case PC_LAYER_GROUP: {
        size_t na;
        size_t nb;
        (void)group_children(a, &na);
        (void)group_children(b, &nb);
        return na == nb && a->u.group.expanded == b->u.group.expanded &&
               a->u.group.has_artboard == b->u.group.has_artboard &&
               (!a->u.group.has_artboard || pc_artboard_eq(&a->u.group.artboard, &b->u.group.artboard));
    }
    case PC_LAYER_ADJUSTMENT:
        return pc_adjustment_eq(&a->u.adjustment, &b->u.adjustment);
    case PC_LAYER_FILL:
        return pc_fill_eq(&a->u.fill, &b->u.fill);
    case PC_LAYER_TEXT:
        return pc_text_layer_eq(&a->u.text, &b->u.text);
    case PC_LAYER_SHAPE:
        return pc_shape_layer_eq(&a->u.shape, &b->u.shape);
    case PC_LAYER_SMART:
        return pc_smart_object_eq(&a->u.smart, &b->u.smart);
    default:
        return false;
    }
}

static void psd_blocks_free(pc_psd_block *b, size_t n)
{
    size_t i;
    if (b == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        pc_blob_release(b[i].data);
    }
    free(b);
}

static void fill_cache_fini(pc_fill_cache *f)
{
    pc_fill_fini(&f->fill);
    pc_surface_fini(&f->surface);
}

/* Finalises every field of l except a group's children array, which stays in place. */
static void layer_fini_shallow(pc_layer *l)
{
    pc_str_fini(&l->name);
    if (l->has_mask) {
        pc_layer_mask_fini(&l->mask);
        l->has_mask = false;
    }
    if (l->has_vector_mask) {
        pc_vector_mask_fini(&l->vector_mask);
        l->has_vector_mask = false;
    }
    pc_effects_fini(&l->effects);
    content_fini_shallow(&l->content);
    psd_blocks_free(l->psd_blocks, l->npsd_blocks);
    l->psd_blocks = NULL;
    l->npsd_blocks = 0;
    if (l->has_fill_cache) {
        fill_cache_fini(&l->fill_cache);
        l->has_fill_cache = false;
    }
    pc_blend_if_fini(&l->blend_if);
    if (l->has_video) {
        pc_video_data_fini(&l->video);
        l->has_video = false;
    }
}

/* Finalises layers arr[0 .. n) and their subtrees, then frees arr, without recursion and
 * without allocating: while a group's children are being finalised, the group (already
 * finalised itself, so its fields are free) keeps a pointer to its own parent group in
 * link_group. Climbing back up follows that chain; the index to resume at is the group's
 * position in its sibling array. */
static void fini_layer_array(pc_layer *arr, size_t n)
{
    pc_layer *parent = NULL;
    pc_layer *a = arr;
    size_t na = n;
    size_t i = 0;
    if (arr == NULL) {
        return;
    }
    for (;;) {
        if (i < na) {
            pc_layer *c = &a[i];
            layer_fini_shallow(c);
            if (c->content.kind == PC_LAYER_GROUP && c->content.u.group.children != NULL) {
                if (c->content.u.group.nchildren > 0) {
                    c->link_group = (uint64_t)(uintptr_t)parent;
                    parent = c;
                    a = c->content.u.group.children;
                    na = c->content.u.group.nchildren;
                    i = 0;
                    continue;
                }
                free(c->content.u.group.children);
            }
            memset(c, 0, sizeof *c);
            i++;
            continue;
        }
        free(a);
        if (parent == NULL) {
            return;
        }
        {
            pc_layer *done = parent;
            parent = (pc_layer *)(uintptr_t)done->link_group;
            a = parent != NULL ? parent->content.u.group.children : arr;
            na = parent != NULL ? parent->content.u.group.nchildren : n;
            i = (size_t)(done - a) + 1;
            memset(done, 0, sizeof *done);
        }
    }
}

/* A copy of l whose group children are a zeroed array (see content_clone_shallow). With
 * fresh, the copy gets a new id and no PSD id (Duplicate Layer). */
static pc_status layer_clone_shallow(const pc_layer *l, pc_layer *out, bool fresh, pc_error *err)
{
    pc_layer c;
    pc_status s;
    size_t i;
    memset(&c, 0, sizeof c);
    c.id = l->id;
    c.visible = l->visible;
    c.locks = l->locks;
    c.blend = l->blend;
    c.opacity = l->opacity;
    c.fill_opacity = l->fill_opacity;
    c.clipped = l->clipped;
    c.label = l->label;
    c.has_psd_id = l->has_psd_id;
    c.psd_id = l->psd_id;
    c.has_link_group = l->has_link_group;
    c.link_group = l->link_group;
    c.excluded_channels = l->excluded_channels;
    c.advanced = l->advanced;
    c.content.kind = PC_LAYER_RASTER; /* a zeroed raster until the content is copied */
    s = pc_str_clone(&l->name, &c.name, err);
    if (s != PC_OK) {
        goto fail;
    }
    if (l->has_mask) {
        s = pc_layer_mask_clone(&l->mask, &c.mask, err);
        if (s != PC_OK) {
            goto fail;
        }
        c.has_mask = true;
    }
    if (l->has_vector_mask) {
        s = pc_vector_mask_clone(&l->vector_mask, &c.vector_mask, err);
        if (s != PC_OK) {
            goto fail;
        }
        c.has_vector_mask = true;
    }
    s = pc_effects_clone(&l->effects, &c.effects, err);
    if (s != PC_OK) {
        goto fail;
    }
    s = content_clone_shallow(&l->content, &c.content, err);
    if (s != PC_OK) {
        goto fail;
    }
    if (l->npsd_blocks > 0 && l->psd_blocks != NULL) {
        c.psd_blocks = pc_calloc_array(l->npsd_blocks, sizeof *c.psd_blocks);
        if (c.psd_blocks == NULL) {
            s = pc_fail(err, PC_ERR_NOMEM, "out of memory copying PSD blocks");
            goto fail;
        }
        for (i = 0; i < l->npsd_blocks; i++) {
            memcpy(c.psd_blocks[i].key, l->psd_blocks[i].key, 4);
            c.psd_blocks[i].data = pc_blob_retain(l->psd_blocks[i].data);
        }
        c.npsd_blocks = l->npsd_blocks;
    }
    if (l->has_fill_cache) {
        s = pc_fill_clone(&l->fill_cache.fill, &c.fill_cache.fill, err);
        if (s != PC_OK) {
            goto fail;
        }
        s = pc_surface_clone(&l->fill_cache.surface, &c.fill_cache.surface, err);
        if (s != PC_OK) {
            pc_fill_fini(&c.fill_cache.fill);
            goto fail;
        }
        c.has_fill_cache = true;
    }
    s = pc_blend_if_clone(&l->blend_if, &c.blend_if, err);
    if (s != PC_OK) {
        goto fail;
    }
    if (l->has_video) {
        s = pc_video_data_clone(&l->video, &c.video, err);
        if (s != PC_OK) {
            goto fail;
        }
        c.has_video = true;
    }
    if (fresh) {
        c.id = pc_layer_id_fresh();
        c.has_psd_id = false;
        c.psd_id = 0;
    }
    *out = c;
    return PC_OK;
fail:
    pc_layer_fini(&c);
    return s;
}

static bool psd_blocks_eq(const pc_layer *a, const pc_layer *b)
{
    size_t i;
    size_t n = a->psd_blocks != NULL ? a->npsd_blocks : 0;
    size_t m = b->psd_blocks != NULL ? b->npsd_blocks : 0;
    if (n != m) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (memcmp(a->psd_blocks[i].key, b->psd_blocks[i].key, 4) != 0 ||
            !pc_blob_eq(a->psd_blocks[i].data, b->psd_blocks[i].data)) {
            return false;
        }
    }
    return true;
}

static bool layer_eq_shallow(const pc_layer *a, const pc_layer *b)
{
    return a->id == b->id && pc_str_eq(&a->name, &b->name) && a->visible == b->visible &&
           pc_locks_eq(a->locks, b->locks) && a->blend == b->blend && a->opacity == b->opacity &&
           a->fill_opacity == b->fill_opacity && a->clipped == b->clipped && a->has_mask == b->has_mask &&
           (!a->has_mask || pc_layer_mask_eq(&a->mask, &b->mask)) &&
           a->has_vector_mask == b->has_vector_mask &&
           (!a->has_vector_mask || pc_vector_mask_eq(&a->vector_mask, &b->vector_mask)) &&
           pc_effects_eq(&a->effects, &b->effects) && a->label == b->label &&
           content_eq_shallow(&a->content, &b->content) && psd_blocks_eq(a, b) &&
           a->has_psd_id == b->has_psd_id && (!a->has_psd_id || a->psd_id == b->psd_id) &&
           a->has_fill_cache == b->has_fill_cache &&
           (!a->has_fill_cache || (pc_fill_eq(&a->fill_cache.fill, &b->fill_cache.fill) &&
                                   pc_surface_eq(&a->fill_cache.surface, &b->fill_cache.surface))) &&
           a->has_link_group == b->has_link_group && (!a->has_link_group || a->link_group == b->link_group) &&
           a->excluded_channels == b->excluded_channels && pc_blend_if_eq(&a->blend_if, &b->blend_if) &&
           pc_advanced_blending_eq(&a->advanced, &b->advanced) && a->has_video == b->has_video &&
           (!a->has_video || pc_video_data_eq(&a->video, &b->video));
}

/* ---- whole subtrees (explicit stacks) ----------------------------------------------------- */

typedef struct pair_frame {
    const pc_layer *a;
    const pc_layer *b; /* eq */
    pc_layer *out;     /* clone */
    size_t n;
    size_t i;
} pair_frame;

typedef struct pair_stack {
    pair_frame *v;
    size_t len;
    size_t cap;
    pair_frame buf[PC_DOC_ITER_INLINE];
} pair_stack;

static void ps_init(pair_stack *s)
{
    s->v = s->buf;
    s->len = 0;
    s->cap = PC_DOC_ITER_INLINE;
}

static bool ps_push(pair_stack *s, pair_frame f)
{
    if (s->len == s->cap) {
        size_t cap;
        pair_frame *v;
        if (!pc_size_mul(s->cap, 2, &cap)) {
            return false;
        }
        v = pc_malloc_array(cap, sizeof *v);
        if (v == NULL) {
            return false;
        }
        memcpy(v, s->v, s->len * sizeof *v);
        if (s->v != s->buf) {
            free(s->v);
        }
        s->v = v;
        s->cap = cap;
    }
    s->v[s->len++] = f;
    return true;
}

static void ps_fini(pair_stack *s)
{
    if (s->v != s->buf) {
        free(s->v);
    }
    s->v = s->buf;
    s->len = 0;
}

/* Fills dst[0 .. n) (zeroed) with copies of src's subtrees in pre-order (so fresh ids come out
 * in the reference's order). On failure dst holds a partial tree for the caller to finalise. */
static pc_status clone_children(const pc_layer *src, pc_layer *dst, size_t n, bool fresh, pc_error *err)
{
    pair_stack st;
    pc_status s = PC_OK;
    pair_frame f0;
    if (n == 0) {
        return PC_OK;
    }
    ps_init(&st);
    memset(&f0, 0, sizeof f0);
    f0.a = src;
    f0.out = dst;
    f0.n = n;
    (void)ps_push(&st, f0); /* fits the inline buffer */
    while (st.len > 0) {
        pair_frame *f = &st.v[st.len - 1];
        const pc_layer *l;
        pc_layer *o;
        if (f->i >= f->n) {
            st.len--;
            continue;
        }
        l = &f->a[f->i];
        o = &f->out[f->i];
        f->i++;
        s = layer_clone_shallow(l, o, fresh, err);
        if (s != PC_OK) {
            break;
        }
        if (o->content.kind == PC_LAYER_GROUP && o->content.u.group.nchildren > 0) {
            pair_frame g;
            memset(&g, 0, sizeof g);
            g.a = l->content.u.group.children;
            g.out = o->content.u.group.children;
            g.n = o->content.u.group.nchildren;
            if (!ps_push(&st, g)) {
                s = pc_fail(err, PC_ERR_NOMEM, "out of memory copying nested layers");
                break;
            }
        }
    }
    ps_fini(&st);
    return s;
}

/* Subtree equality; false also when the stack can't grow past PC_DOC_ITER_INLINE levels. */
static bool eq_children(const pc_layer *a, const pc_layer *b, size_t n)
{
    pair_stack st;
    bool same = true;
    pair_frame f0;
    if (n == 0) {
        return true;
    }
    ps_init(&st);
    memset(&f0, 0, sizeof f0);
    f0.a = a;
    f0.b = b;
    f0.n = n;
    (void)ps_push(&st, f0);
    while (st.len > 0) {
        pair_frame *f = &st.v[st.len - 1];
        const pc_layer *x;
        const pc_layer *y;
        size_t nc;
        if (f->i >= f->n) {
            st.len--;
            continue;
        }
        x = &f->a[f->i];
        y = &f->b[f->i];
        f->i++;
        if (!layer_eq_shallow(x, y)) {
            same = false;
            break;
        }
        if (group_children(&x->content, &nc) != NULL && nc > 0) {
            pair_frame g;
            memset(&g, 0, sizeof g);
            g.a = x->content.u.group.children;
            g.b = y->content.u.group.children;
            g.n = nc;
            if (!ps_push(&st, g)) {
                same = false;
                break;
            }
        }
    }
    ps_fini(&st);
    return same;
}

pc_status pc_doc_clone_layers(const pc_layer *src, size_t n, bool fresh, pc_layer **out, pc_error *err)
{
    pc_layer *dst;
    pc_status s;
    if (n == 0 || src == NULL) {
        *out = NULL;
        return PC_OK;
    }
    dst = pc_calloc_array(n, sizeof *dst);
    if (dst == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory copying layers");
    }
    s = clone_children(src, dst, n, fresh, err);
    if (s != PC_OK) {
        fini_layer_array(dst, n);
        return s;
    }
    *out = dst;
    return PC_OK;
}

void pc_doc_fini_layers(pc_layer *arr, size_t n)
{
    fini_layer_array(arr, n);
}

bool pc_doc_layers_eq(const pc_layer *a, size_t na, const pc_layer *b, size_t nb)
{
    if (a == NULL) {
        na = 0;
    }
    if (b == NULL) {
        nb = 0;
    }
    return na == nb && eq_children(a, b, na);
}

/* ---- public layer content API ------------------------------------------------------------- */

void pc_layer_content_fini(pc_layer_content *c)
{
    pc_layer *children = NULL;
    size_t n = 0;
    if (c == NULL) {
        return;
    }
    if (c->kind == PC_LAYER_GROUP) {
        children = c->u.group.children;
        n = c->u.group.nchildren;
    }
    content_fini_shallow(c);
    fini_layer_array(children, n);
    memset(c, 0, sizeof *c);
}

pc_status pc_layer_content_clone(const pc_layer_content *c, pc_layer_content *out, pc_error *err)
{
    pc_layer_content r;
    pc_status s = content_clone_shallow(c, &r, err);
    if (s != PC_OK) {
        return s;
    }
    if (r.kind == PC_LAYER_GROUP && r.u.group.nchildren > 0) {
        s = clone_children(c->u.group.children, r.u.group.children, r.u.group.nchildren, false, err);
        if (s != PC_OK) {
            pc_layer_content_fini(&r);
            return s;
        }
    }
    *out = r;
    return PC_OK;
}

bool pc_layer_content_eq(const pc_layer_content *a, const pc_layer_content *b)
{
    size_t n;
    if (!content_eq_shallow(a, b)) {
        return false;
    }
    if (group_children(a, &n) == NULL || n == 0) {
        return true;
    }
    return eq_children(a->u.group.children, b->u.group.children, n);
}

const char *pc_layer_content_kind_name(const pc_layer_content *c)
{
    switch (c->kind) {
    case PC_LAYER_RASTER:
        return "Pixel";
    case PC_LAYER_GROUP:
        return "Group";
    case PC_LAYER_ADJUSTMENT:
        return "Adjustment";
    case PC_LAYER_FILL:
        return "Fill";
    case PC_LAYER_TEXT:
        return "Type";
    case PC_LAYER_SHAPE:
        return "Shape";
    case PC_LAYER_SMART:
        return "Smart Object";
    default:
        return "Pixel";
    }
}

const char *pc_layer_content_article(const pc_layer_content *c)
{
    return c->kind == PC_LAYER_ADJUSTMENT ? "an" : "a";
}

/* ---- layers ------------------------------------------------------------------------------- */

pc_status pc_layer_init(pc_layer *out, const char *name, pc_layer_content *content, pc_error *err)
{
    pc_layer l;
    pc_status s;
    memset(&l, 0, sizeof l);
    s = pc_str_set_c(&l.name, name != NULL ? name : "", err);
    if (s != PC_OK) {
        return s;
    }
    l.id = pc_layer_id_fresh();
    l.visible = true;
    l.blend = PC_BLEND_NORMAL;
    l.opacity = 1.0f;
    l.fill_opacity = 1.0f;
    l.effects.enabled = true;
    l.label = PC_LABEL_NONE;
    l.content = *content;
    memset(content, 0, sizeof *content);
    l.advanced = pc_advanced_blending_default();
    *out = l;
    return PC_OK;
}

pc_status pc_layer_raster(pc_layer *out, const char *name, pc_pixel_format format, pc_error *err)
{
    pc_layer_content c;
    pc_status s;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_RASTER;
    pc_surface_init(&c.u.raster, format);
    s = pc_layer_init(out, name, &c, err);
    if (s != PC_OK) {
        pc_surface_fini(&c.u.raster);
    }
    return s;
}

pc_status pc_layer_group(pc_layer *out, const char *name, pc_layer *children, size_t n, pc_error *err)
{
    pc_layer_content c;
    pc_status s;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_GROUP;
    c.u.group.children = n > 0 ? children : NULL;
    c.u.group.nchildren = n > 0 ? n : 0;
    c.u.group.expanded = true;
    s = pc_layer_init(out, name, &c, err);
    if (s != PC_OK) {
        return s; /* children still belong to the caller */
    }
    if (n == 0) {
        free(children); /* taken over: an empty array is dropped, the group holds NULL */
    }
    out->blend = PC_BLEND_PASS_THROUGH;
    return PC_OK;
}

void pc_layer_fini(pc_layer *l)
{
    pc_layer *children = NULL;
    size_t n = 0;
    if (l == NULL) {
        return;
    }
    if (l->content.kind == PC_LAYER_GROUP) {
        children = l->content.u.group.children;
        n = l->content.u.group.nchildren;
    }
    layer_fini_shallow(l);
    fini_layer_array(children, n);
    memset(l, 0, sizeof *l);
}

static pc_status layer_clone(const pc_layer *l, pc_layer *out, bool fresh, pc_error *err)
{
    pc_layer c;
    pc_status s = layer_clone_shallow(l, &c, fresh, err);
    if (s != PC_OK) {
        return s;
    }
    if (c.content.kind == PC_LAYER_GROUP && c.content.u.group.nchildren > 0) {
        s = clone_children(l->content.u.group.children, c.content.u.group.children,
                           c.content.u.group.nchildren, fresh, err);
        if (s != PC_OK) {
            pc_layer_fini(&c);
            return s;
        }
    }
    *out = c;
    return PC_OK;
}

pc_status pc_layer_clone(const pc_layer *l, pc_layer *out, pc_error *err)
{
    return layer_clone(l, out, false, err);
}

pc_status pc_layer_duplicate(const pc_layer *l, pc_layer *out, pc_error *err)
{
    return layer_clone(l, out, true, err);
}

bool pc_layer_eq(const pc_layer *a, const pc_layer *b)
{
    size_t n;
    if (!layer_eq_shallow(a, b)) {
        return false;
    }
    if (group_children(&a->content, &n) == NULL || n == 0) {
        return true;
    }
    return eq_children(a->content.u.group.children, b->content.u.group.children, n);
}

bool pc_layer_is_group(const pc_layer *l)
{
    return l->content.kind == PC_LAYER_GROUP;
}

const pc_layer *pc_layer_children(const pc_layer *l, size_t *n)
{
    return group_children(&l->content, n);
}

const pc_surface *pc_layer_surface(const pc_layer *l)
{
    switch (l->content.kind) {
    case PC_LAYER_RASTER:
        return &l->content.u.raster;
    case PC_LAYER_TEXT:
        return l->content.u.text.has_cache ? &l->content.u.text.cache : NULL;
    case PC_LAYER_SHAPE:
        return l->content.u.shape.has_cache ? &l->content.u.shape.cache : NULL;
    case PC_LAYER_SMART:
        return l->content.u.smart.has_cache ? &l->content.u.smart.cache : NULL;
    default:
        return NULL;
    }
}

pc_surface *pc_layer_surface_mut(pc_layer *l)
{
    return l->content.kind == PC_LAYER_RASTER ? &l->content.u.raster : NULL;
}

/* ---- the pre-order iterator (tree.h) ------------------------------------------------------ */

void pc_doc_iter_init(pc_doc_iter *it, const pc_layer *layers, size_t n)
{
    it->frames = it->inline_frames;
    it->cap = PC_DOC_ITER_INLINE;
    it->len = 1;
    it->pending = NULL;
    it->frames[0].arr = layers;
    it->frames[0].n = layers != NULL ? n : 0;
    it->frames[0].i = 0;
}

static bool iter_push(pc_doc_iter *it, const pc_layer *arr, size_t n)
{
    if (it->len == it->cap) {
        size_t cap;
        pc_doc_iter_frame *v;
        if (!pc_size_mul(it->cap, 2, &cap)) {
            return false;
        }
        v = pc_malloc_array(cap, sizeof *v);
        if (v == NULL) {
            return false;
        }
        memcpy(v, it->frames, it->len * sizeof *v);
        if (it->frames != it->inline_frames) {
            free(it->frames);
        }
        it->frames = v;
        it->cap = cap;
    }
    it->frames[it->len].arr = arr;
    it->frames[it->len].n = n;
    it->frames[it->len].i = 0;
    it->len++;
    return true;
}

int pc_doc_iter_next(pc_doc_iter *it, const pc_layer **out)
{
    if (it->pending != NULL) {
        size_t n;
        const pc_layer *ch = group_children(&it->pending->content, &n);
        it->pending = NULL;
        if (!iter_push(it, ch, n)) {
            it->len = 0;
            return -1;
        }
    }
    while (it->len > 0) {
        pc_doc_iter_frame *f = &it->frames[it->len - 1];
        const pc_layer *l;
        size_t n;
        if (f->i >= f->n) {
            it->len--;
            continue;
        }
        l = &f->arr[f->i];
        f->i++;
        if (group_children(&l->content, &n) != NULL && n > 0) {
            it->pending = l;
        }
        *out = l;
        return 1;
    }
    return 0;
}

void pc_doc_iter_fini(pc_doc_iter *it)
{
    if (it->frames != it->inline_frames) {
        free(it->frames);
    }
    it->frames = it->inline_frames;
    it->len = 0;
    it->pending = NULL;
}

int pc_doc_iter_find(pc_doc_iter *it, const pc_layer *layers, size_t n, pc_layer_id id, const pc_layer **out)
{
    const pc_layer *l;
    int r;
    pc_doc_iter_init(it, layers, n);
    while ((r = pc_doc_iter_next(it, &l)) == 1) {
        if (l->id == id) {
            *out = l;
            return 1;
        }
    }
    return r;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

pc_status pc_doc_sorted_ids(const pc_layer *layers, size_t n, uint64_t **out, size_t *count, pc_error *err)
{
    pc_doc_iter it;
    const pc_layer *l;
    uint64_t *ids = NULL;
    size_t len = 0;
    size_t cap = 0;
    int r;
    pc_doc_iter_init(&it, layers, n);
    while ((r = pc_doc_iter_next(&it, &l)) == 1) {
        if (len == cap) {
            uint64_t *v = pc_grow_array(ids, &cap, len + 1, sizeof *ids);
            if (v == NULL) {
                r = -1;
                break;
            }
            ids = v;
        }
        ids[len++] = l->id;
    }
    pc_doc_iter_fini(&it);
    if (r < 0) {
        free(ids);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory listing layer ids");
    }
    if (len > 1) {
        qsort(ids, len, sizeof *ids, cmp_u64);
    }
    *out = ids;
    *count = len;
    return PC_OK;
}
