/* The layer-effect map cache of crates/compose/src/lib.rs: effect maps are built once per layer
 * state (layer identity, region, global light) and shared by every tile and render through a
 * byte-bounded process-wide cache, plus a per-render table (Ctx::fx_maps) that keeps the maps a
 * render uses alive for its whole duration.
 *
 * Nothing ever waits for another thread's build (the reference's #276 deadlock rule): a miss
 * builds here, and the first finished build is kept. Each cache entry pins a clone of the layer
 * so the tile addresses in its key stay alive. The maps themselves come from C3
 * (pc_compose_fx_build_maps); here they are wrapped in a reference-counted holder. */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "compose/internal.h"
#include "compose/keyhash.h"

struct pc_cmp_fx_ref {
    atomic_size_t refs;
    pc_compose_fx_maps *maps;
};

static pc_cmp_fx_ref *ref_new(pc_compose_fx_maps *maps)
{
    pc_cmp_fx_ref *r = malloc(sizeof *r);
    if (r == NULL) {
        return NULL;
    }
    atomic_init(&r->refs, 1);
    r->maps = maps;
    return r;
}

static pc_cmp_fx_ref *ref_retain(pc_cmp_fx_ref *r)
{
    if (r != NULL) {
        atomic_fetch_add_explicit(&r->refs, 1, memory_order_relaxed);
    }
    return r;
}

void pc_cmp_fx_ref_release(pc_cmp_fx_ref *r)
{
    if (r != NULL && atomic_fetch_sub_explicit(&r->refs, 1, memory_order_acq_rel) == 1) {
        pc_compose_fx_maps_free(r->maps);
        free(r);
    }
}

const pc_compose_fx_maps *pc_cmp_fx_maps(const pc_cmp_fx_ref *r)
{
    return r != NULL ? r->maps : NULL;
}

/* ---- global cache ---- */

typedef struct fx_entry {
    uint64_t key;
    pc_cmp_fx_ref *ref; /* NULL while the first build runs */
    pc_layer pin;
    bool has_pin;
    size_t bytes;
} fx_entry;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static fx_entry *entries; /* insertion order = eviction order (oldest first) */
static size_t nentries;
static size_t centries;
static size_t cache_bytes;
static atomic_size_t budget = PC_COMPOSE_DEFAULT_FX_CACHE_BUDGET;

void pc_compose_set_effect_cache_budget(size_t bytes)
{
    atomic_store_explicit(&budget, bytes > ((size_t)1 << 20) ? bytes : (size_t)1 << 20, memory_order_relaxed);
}

size_t pc_compose_effect_cache_budget(void)
{
    return atomic_load_explicit(&budget, memory_order_relaxed);
}

size_t pc_compose_effect_cache_bytes(void)
{
    size_t b;
    pthread_mutex_lock(&lock);
    b = cache_bytes;
    pthread_mutex_unlock(&lock);
    return b;
}

static void entry_drop(fx_entry *e)
{
    pc_cmp_fx_ref_release(e->ref);
    if (e->has_pin) {
        pc_layer_fini(&e->pin);
    }
}

static void remove_at(size_t i)
{
    cache_bytes -= cache_bytes < entries[i].bytes ? cache_bytes : entries[i].bytes;
    entry_drop(&entries[i]);
    memmove(entries + i, entries + i + 1, (nentries - i - 1) * sizeof *entries);
    nentries--;
}

size_t pc_compose_purge_effect_cache(void)
{
    size_t freed;
    size_t i;
    pthread_mutex_lock(&lock);
    freed = cache_bytes;
    for (i = 0; i < nentries; i++) {
        entry_drop(&entries[i]);
    }
    nentries = 0;
    cache_bytes = 0;
    pthread_mutex_unlock(&lock);
    return freed;
}

static fx_entry *find(uint64_t key)
{
    size_t i;
    for (i = 0; i < nentries; i++) {
        if (entries[i].key == key) {
            return &entries[i];
        }
    }
    return NULL;
}

/* Order-independent identity of a layer's pixels, masks, effects and children. */
static pc_status layer_identity(const pc_layer *layer, uint64_t *h, unsigned depth, pc_error *err)
{
    pc_status st = PC_OK;
    pc_cmp_hash_u64(h, layer->id);
    /* fill_opacity shapes the maps; opacity and visibility don't. */
    pc_cmp_hash_f32(h, layer->fill_opacity);
    if (depth > PC_COMPOSE_MAX_DEPTH) {
        pc_cmp_hash_u64(h, 0xdee9u);
        return PC_OK;
    }
    switch (layer->content.kind) {
    case PC_LAYER_GROUP: {
        const pc_group *g = &layer->content.u.group;
        size_t i;
        for (i = 0; i < g->nchildren && st == PC_OK; i++) {
            const pc_layer *c = &g->children[i];
            /* How each child composites shapes a styled group's maps. */
            pc_cmp_hash_u64(h, (uint64_t)c->visible | (uint64_t)c->clipped << 1 | (uint64_t)c->blend << 8);
            pc_cmp_hash_f32(h, c->opacity);
            st = layer_identity(c, h, depth + 1, err);
        }
        if (st == PC_OK) {
            st = pc_cmp_hash_artboard(h, g, err);
        }
        break;
    }
    case PC_LAYER_FILL:
        st = pc_cmp_hash_fill(h, &layer->content.u.fill, err);
        /* The cached pixels shape the maps while they match the fill. */
        if (st == PC_OK && layer->has_fill_cache &&
            pc_fill_eq(&layer->fill_cache.fill, &layer->content.u.fill)) {
            pc_cmp_hash_u64(h, pc_cmp_surface_fp(&layer->fill_cache.surface));
        }
        break;
    case PC_LAYER_ADJUSTMENT:
        st = pc_cmp_hash_adjustment(h, &layer->content.u.adjustment, err);
        break;
    default: {
        const pc_surface *s = pc_layer_surface(layer);
        pc_cmp_hash_u64(h, s != NULL ? pc_cmp_surface_fp(s) : 0);
        break;
    }
    }
    if (st != PC_OK) {
        return st;
    }
    if (layer->has_mask) {
        pc_cmp_hash_u64(h, pc_cmp_surface_fp(&layer->mask.surface));
        pc_cmp_hash_u64(h, layer->mask.enabled);
        pc_cmp_hash_f32(h, layer->mask.density);
        pc_cmp_hash_f32(h, layer->mask.feather);
    }
    pc_cmp_hash_vector_mask(h, layer->has_vector_mask ? &layer->vector_mask : NULL);
    st = pc_cmp_hash_effects(h, &layer->effects, err);
    pc_cmp_hash_u64(h, 0xfe);
    return st;
}

static pc_compose_texture_ctx texture_ctx(const pc_layer *layer, pc_rect region, const pc_cmp_ctx *cx)
{
    pc_compose_texture_ctx t;
    pc_rect sb;
    if (!pc_compose_paint_bounds(layer, &sb)) {
        sb = pc_compose_layer_bounds(layer, cx->canvas);
    }
    t.rect = region;
    t.patterns = pc_compose_patterns_source(cx->patterns, &t.npatterns);
    if (layer->effects.has_reference) {
        t.anchor[0] = layer->effects.reference[0];
        t.anchor[1] = layer->effects.reference[1];
    } else {
        t.anchor[0] = (double)sb.x0;
        t.anchor[1] = (double)sb.y0;
    }
    return t;
}

static pc_status build(const pc_layer *layer, pc_rect region, const pc_cmp_ctx *cx, pc_cmp_fx_ref **out,
                       size_t *bytes, pc_error *err)
{
    float *shape = NULL;
    size_t len = 0;
    pc_compose_fx_maps *maps = NULL;
    pc_compose_texture_ctx tex = texture_ctx(layer, region, cx);
    pc_status st = PC_OK;
    pc_cmp_fx_ref *r;
    if (!pc_rect_is_empty(region)) {
        st = pc_cmp_effect_shape(layer, region, cx, &shape, &len, err);
    }
    if (st == PC_OK) {
        st = pc_compose_fx_build_maps(layer, shape, len, region, &cx->light, &tex, cx->patterns, &maps, err);
    } else {
        free(shape);
    }
    if (st != PC_OK) {
        return st;
    }
    r = ref_new(maps);
    if (r == NULL) {
        pc_compose_fx_maps_free(maps);
        return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
    }
    *bytes = pc_compose_fx_maps_bytes(maps);
    *out = r;
    return PC_OK;
}

/* cached_effect_maps: from the global cache, built on a miss. */
static pc_status cached_effect_maps(const pc_layer *layer, pc_rect region, uint64_t key, const pc_cmp_ctx *cx,
                                    pc_cmp_fx_ref **out, pc_error *err)
{
    fx_entry *e;
    pc_cmp_fx_ref *maps = NULL;
    size_t bytes = 0;
    size_t limit;
    size_t rounds;
    pc_status st;
    pthread_mutex_lock(&lock);
    e = find(key);
    if (e == NULL) {
        fx_entry *grown = pc_grow_array(entries, &centries, nentries + 1, sizeof *entries);
        if (grown != NULL) {
            entries = grown;
            e = &entries[nentries++];
            memset(e, 0, sizeof *e);
            e->key = key;
        }
    } else if (e->ref != NULL) {
        maps = ref_retain(e->ref);
    }
    pthread_mutex_unlock(&lock);
    if (maps == NULL) {
        pc_layer pin;
        bool has_pin;
        st = build(layer, region, cx, &maps, &bytes, err);
        if (st != PC_OK) {
            return st;
        }
        has_pin = pc_layer_clone(layer, &pin, NULL) == PC_OK;
        pthread_mutex_lock(&lock);
        e = find(key);
        if (e != NULL && e->ref == NULL && has_pin) {
            /* Counted once, by the build that filled the slot, while the slot is cached. */
            e->ref = ref_retain(maps);
            e->pin = pin;
            e->has_pin = true;
            e->bytes = bytes;
            cache_bytes += bytes;
            has_pin = false;
        } else if (e != NULL && e->ref != NULL) {
            /* A concurrent build finished first: share it, so every tile uses one map. */
            pc_cmp_fx_ref_release(maps);
            maps = ref_retain(e->ref);
        }
        pthread_mutex_unlock(&lock);
        if (has_pin) {
            pc_layer_fini(&pin);
        }
    }
    /* Evict the oldest entries over budget (never the one just used). */
    pthread_mutex_lock(&lock);
    limit = pc_compose_effect_cache_budget();
    rounds = nentries;
    while (cache_bytes > limit && nentries > 1 && rounds-- > 0) {
        if (entries[0].key == key) {
            fx_entry keep = entries[0];
            memmove(entries, entries + 1, (nentries - 1) * sizeof *entries);
            entries[nentries - 1] = keep;
            continue;
        }
        remove_at(0);
    }
    pthread_mutex_unlock(&lock);
    *out = maps;
    return PC_OK;
}

/* ---- per-render table ---- */

void pc_cmp_fx_local_init(pc_cmp_fx_local *l)
{
    memset(l, 0, sizeof *l);
    pthread_mutex_init(&l->lock, NULL);
}

void pc_cmp_fx_local_fini(pc_cmp_fx_local *l)
{
    size_t i;
    for (i = 0; i < l->len; i++) {
        pc_cmp_fx_ref_release(l->items[i].ref);
    }
    free(l->items);
    pthread_mutex_destroy(&l->lock);
    memset(l, 0, sizeof *l);
}

static pc_cmp_fx_ref *local_get(pc_cmp_fx_local *l, uint64_t key)
{
    size_t i;
    for (i = 0; i < l->len; i++) {
        if (l->items[i].key == key) {
            return ref_retain(l->items[i].ref);
        }
    }
    return NULL;
}

/* Inserts maps unless the key is there already; returns the table's entry (a new reference).
 * When the table can't grow the maps are still returned, just not kept for the render. */
static pc_cmp_fx_ref *local_put(pc_cmp_fx_local *l, uint64_t key, pc_cmp_fx_ref *maps, bool clipped)
{
    pc_cmp_fx_ref *r;
    pthread_mutex_lock(&l->lock);
    r = local_get(l, key);
    if (r == NULL) {
        pc_cmp_fx_pair *grown = pc_grow_array(l->items, &l->cap, l->len + 1, sizeof *l->items);
        if (grown != NULL) {
            l->items = grown;
            l->items[l->len].key = key;
            l->items[l->len].ref = ref_retain(maps);
            l->items[l->len].clipped = clipped;
            l->len++;
        }
        r = ref_retain(maps);
    }
    pthread_mutex_unlock(&l->lock);
    return r;
}

void pc_cmp_set_fx_clip(const pc_cmp_ctx *cx, pc_rect rect)
{
    pc_cmp_fx_local *l = cx->fx;
    size_t i;
    size_t kept = 0;
    if (l == NULL) {
        return;
    }
    pthread_mutex_lock(&l->lock);
    if (!l->has_clip || !pc_rect_eq(l->clip, rect)) {
        l->has_clip = true;
        l->clip = rect;
        for (i = 0; i < l->len; i++) {
            if (l->items[i].clipped) {
                pc_cmp_fx_ref_release(l->items[i].ref);
            } else {
                l->items[kept++] = l->items[i];
            }
        }
        l->len = kept;
    }
    pthread_mutex_unlock(&l->lock);
}

pc_rect pc_cmp_effect_region(const pc_layer *layer, pc_rect rect, const pc_cmp_ctx *cx, bool *clipped)
{
    int32_t m = pc_compose_effects_margin(layer);
    pc_rect region = pc_rect_intersect(pc_rect_inflate(pc_compose_effect_source_bounds(layer, cx->canvas), m),
                                       pc_rect_inflate(cx->canvas, m));
    bool has_clip = false;
    pc_rect clip = PC_RECT_EMPTY;
    if (cx->fx != NULL) {
        pthread_mutex_lock(&cx->fx->lock);
        has_clip = cx->fx->has_clip;
        clip = cx->fx->clip;
        pthread_mutex_unlock(&cx->fx->lock);
    }
    if (has_clip && pc_rect_contains_rect(clip, rect) && pc_compose_shapeless(layer) &&
        pc_compose_fx_maps_are_local(layer)) {
        pc_rect part = pc_rect_intersect(region, pc_rect_inflate(clip, pc_i32_sat_add(m, m)));
        *clipped = !pc_rect_eq(part, region);
        return part;
    }
    *clipped = false;
    return region;
}

pc_status pc_cmp_effect_maps(const pc_layer *layer, pc_rect rect, const pc_cmp_ctx *cx, pc_cmp_fx_ref **out,
                             pc_error *err)
{
    bool clipped = false;
    pc_rect region = pc_cmp_effect_region(layer, rect, cx, &clipped);
    uint64_t key = PC_CMP_HASH0;
    pc_cmp_fx_ref *maps = NULL;
    pc_cmp_fx_ref *r;
    size_t bytes;
    pc_status st;
    if (getenv("PHOTOCRAFT_FX_NOCACHE") != NULL) {
        return build(layer, region, cx, out, &bytes, err);
    }
    st = layer_identity(layer, &key, 0, err);
    if (st != PC_OK) {
        return st;
    }
    pc_cmp_hash_rect(&key, region);
    pc_cmp_hash_f32(&key, cx->light.angle);
    pc_cmp_hash_f32(&key, cx->light.altitude);
    /* Prepared for this render: no global lookup, no build. */
    if (cx->fx != NULL) {
        pthread_mutex_lock(&cx->fx->lock);
        r = local_get(cx->fx, key);
        pthread_mutex_unlock(&cx->fx->lock);
        if (r != NULL) {
            *out = r;
            return PC_OK;
        }
    }
    st = cached_effect_maps(layer, region, key, cx, &maps, err);
    if (st != PC_OK) {
        return st;
    }
    if (cx->fx == NULL) {
        *out = maps;
        return PC_OK;
    }
    r = local_put(cx->fx, key, maps, clipped);
    pc_cmp_fx_ref_release(maps);
    *out = r;
    return PC_OK;
}

/* ---- preparing before parallel tiles ---- */

/* Deepest group nesting prepare walks (deeper layers build their maps on demand). */
#define PREPARE_DEPTH 64

typedef struct todo_item {
    const pc_layer *layer;
    pc_layer owned;
    bool has_owned;
} todo_item;

typedef struct todo_list {
    todo_item *items;
    size_t len;
    size_t cap;
} todo_list;

static void effect_layers(const pc_layer *layers, size_t n, pc_rect rect, unsigned depth, todo_list *out)
{
    bool base_visible = true;
    size_t i;
    for (i = 0; i < n; i++) {
        const pc_layer *l = &layers[i];
        if (!l->clipped) {
            base_visible = l->visible;
        }
        /* A clipping group is drawn only when its base is. */
        if (!l->visible || !base_visible) {
            continue;
        }
        if (pc_compose_has_effects(l) && !pc_cmp_empty_in(l, rect)) {
            todo_item *grown = pc_grow_array(out->items, &out->cap, out->len + 1, sizeof *out->items);
            if (grown != NULL) {
                todo_item *t;
                out->items = grown;
                t = &out->items[out->len];
                memset(t, 0, sizeof *t);
                /* A mask that hides the effects is left out of the shape they are built from. */
                if (pc_cmp_effects_source(l, &t->owned, &t->has_owned, NULL) == PC_OK) {
                    t->layer = l;
                    out->len++;
                }
            }
        }
        if (l->content.kind == PC_LAYER_GROUP && depth < PREPARE_DEPTH) {
            effect_layers(l->content.u.group.children, l->content.u.group.nchildren, rect, depth + 1, out);
        }
    }
}

typedef struct prep_job {
    todo_list *todo;
    pc_rect rect;
    const pc_cmp_ctx *cx;
    pc_cmyk_space *cmyk;
    bool lab;
    bool hdr;
} prep_job;

static pc_status prep_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    prep_job *j = ctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        todo_item *t = &j->todo->items[i];
        pc_cmyk_space *prev = pc_cmyk_space_enter(j->cmyk);
        pc_cmp_fx_ref *r = NULL;
        bool lab = pc_psblend_set_lab_mix(j->lab);
        bool hdr = pc_psblend_set_hdr(j->hdr);
        /* Failures surface again when the tile that needs the maps builds them. */
        if (pc_cmp_effect_maps(t->has_owned ? &t->owned : t->layer, j->rect, j->cx, &r, NULL) == PC_OK) {
            pc_cmp_fx_ref_release(r);
        }
        (void)pc_psblend_set_lab_mix(lab);
        (void)pc_psblend_set_hdr(hdr);
        pc_cmyk_space_leave(prev);
    }
    return PC_OK;
}

pc_status pc_cmp_prepare_effects(const pc_layer *layers, size_t n, pc_rect rect, const pc_cmp_ctx *cx,
                                 pc_cmyk_space *cmyk, bool lab, bool hdr, pc_error *err)
{
    todo_list todo = {NULL, 0, 0};
    prep_job j = {&todo, rect, cx, cmyk, lab, hdr};
    size_t i;
    (void)err;
    effect_layers(layers, n, rect, 0, &todo);
    if (todo.len > 1) {
        (void)pc_pool_for(pc_pool_global(), todo.len, 1, prep_chunk, &j);
    } else if (todo.len == 1) {
        (void)prep_chunk(&j, 0, 0, 1);
    }
    for (i = 0; i < todo.len; i++) {
        if (todo.items[i].has_owned) {
            pc_layer_fini(&todo.items[i].owned);
        }
    }
    free(todo.items);
    return PC_OK;
}
