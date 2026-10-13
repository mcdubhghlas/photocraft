/* Tile references owned by history snapshots (port of crates/ops/src/tiles.rs), plus the
 * allocation wrappers and the surface walker shared by src/ops. */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "ops/ops_internal.h"

/* ---- allocation with fault injection ---- */

static atomic_long fail_countdown = -1;

void pc_ops_test_fail_alloc(long n)
{
    atomic_store(&fail_countdown, n < 0 ? -1 : n);
}

bool pc_ops_test_fail_pending(void)
{
    return atomic_load(&fail_countdown) >= 0;
}

static bool injected_failure(void)
{
    if (atomic_load(&fail_countdown) < 0) {
        return false;
    }
    return atomic_fetch_sub(&fail_countdown, 1) == 0;
}

void *pc_ops_malloc(size_t n)
{
    return injected_failure() ? NULL : pc_malloc(n);
}

void *pc_ops_calloc_array(size_t count, size_t size)
{
    return injected_failure() ? NULL : pc_calloc_array(count, size);
}

void *pc_ops_grow_array(void *ptr, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap && ptr != NULL) {
        return ptr; /* no allocation, nothing to fail */
    }
    return injected_failure() ? NULL : pc_grow_array(ptr, cap, need, elem);
}

pc_status pc_ops_str_copy(const char *p, size_t len, pc_str *out, pc_error *err)
{
    char *s;
    if (p == NULL && len > 0) {
        return pc_fail(err, PC_ERR_INVALID, "label is NULL");
    }
    if (len >= PC_MAX_ALLOC) {
        return pc_fail(err, PC_ERR_LIMIT, "label too long");
    }
    s = pc_ops_malloc(len + 1);
    if (s == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (len > 0) {
        memcpy(s, p, len);
    }
    s[len] = '\0';
    *out = pc_str_adopt(s, len);
    return PC_OK;
}

pc_status pc_ops_layer_target_clone(const pc_layer_target *t, pc_layer_target *out, pc_error *err)
{
    pc_layer_target r = {t->has_active, t->active, NULL, 0};
    if (t->nselected > 0) {
        if (t->selected == NULL) {
            return pc_fail(err, PC_ERR_INVALID, "layer target has a count but no ids");
        }
        r.selected = pc_ops_calloc_array(t->nselected, sizeof *r.selected);
        if (r.selected == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        memcpy(r.selected, t->selected, t->nselected * sizeof *r.selected);
        r.nselected = t->nselected;
    }
    *out = r;
    return PC_OK;
}

/* ---- surface walk ---- */

#define WALK_INLINE 64

typedef struct frame {
    const pc_layer *arr;
    size_t n;
    size_t i;
} frame;

pc_status pc_ops_surfaces(const pc_document *d, pc_ops_surface_fn fn, void *ctx)
{
    frame inline_frames[WALK_INLINE];
    frame *frames = inline_frames;
    size_t len = 0, cap = WALK_INLINE;
    pc_status st = PC_OK;

    /* Iterative pre-order walk, so no nesting depth can overflow the C stack. */
    if (d->nlayers > 0) {
        frames[len++] = (frame){d->layers, d->nlayers, 0};
    }
    while (len > 0 && st == PC_OK) {
        frame *f = &frames[len - 1];
        const pc_layer *l;
        const pc_surface *s;
        if (f->i >= f->n) {
            len--;
            continue;
        }
        l = &f->arr[f->i++];
        s = pc_layer_surface(l);
        if (s != NULL) {
            st = fn(s, ctx);
        }
        if (st == PC_OK && l->has_mask) {
            st = fn(&l->mask.surface, ctx);
        }
        if (st == PC_OK && l->has_video) {
            /* Undo retains the whole clip, not only the displayed frame. Visit aliases too: the
             * tile set tracks every owner and counts their shared allocations once. */
            const pc_video_data *v = &l->video;
            for (size_t k = 0; k < v->nframes && st == PC_OK; k++) {
                st = fn(&v->frames[k], ctx);
            }
            for (size_t k = 0; v->has_original_frames && k < v->noriginal_frames && st == PC_OK; k++) {
                st = fn(&v->original_frames[k], ctx);
            }
        }
        if (st == PC_OK && l->content.kind == PC_LAYER_GROUP && l->content.u.group.nchildren > 0) {
            if (len == cap) {
                size_t ncap = cap;
                frame *grown;
                if (frames == inline_frames) {
                    grown = pc_ops_grow_array(NULL, &ncap, cap + 1, sizeof *grown);
                    if (grown != NULL) {
                        memcpy(grown, inline_frames, sizeof inline_frames);
                    }
                } else {
                    grown = pc_ops_grow_array(frames, &ncap, cap + 1, sizeof *grown);
                }
                if (grown == NULL) {
                    st = PC_ERR_NOMEM;
                    break;
                }
                frames = grown;
                cap = ncap;
            }
            frames[len++] = (frame){l->content.u.group.children, l->content.u.group.nchildren, 0};
        }
    }
    if (frames != inline_frames) {
        free(frames);
    }
    for (size_t i = 0; i < d->nchannels && st == PC_OK; i++) {
        st = fn(&d->channels[i].surface, ctx);
    }
    if (st == PC_OK && d->has_quick_mask) {
        st = fn(&d->quick_mask.surface, ctx);
    }
    return st;
}

/* ---- hashing ---- */

static size_t slot_of(uintptr_t key, size_t cap)
{
    /* Fibonacci hashing of the address (low bits are alignment zeros). */
    uint64_t h = (uint64_t)key * 0x9E3779B97F4A7C15ull;
    return (size_t)(h ^ (h >> 29)) & (cap - 1);
}

/* Capacity for at least n entries at a load factor of at most 3/4 (power of two). */
static bool table_cap(size_t n, size_t *out)
{
    size_t cap = 16;
    while (cap / 4 * 3 < n) {
        if (cap > SIZE_MAX / 2) {
            return false;
        }
        cap *= 2;
    }
    *out = cap;
    return true;
}

/* ---- pointer set ---- */

void pc_ops_ptrset_fini(pc_ops_ptrset *s)
{
    free(s->slots);
    s->slots = NULL;
    s->cap = 0;
    s->count = 0;
}

static pc_status ptrset_resize(pc_ops_ptrset *s, size_t ncap)
{
    uintptr_t *slots = pc_ops_calloc_array(ncap, sizeof *slots);
    if (slots == NULL) {
        return PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < s->cap; i++) {
        uintptr_t k = s->slots[i];
        if (k != 0) {
            size_t j = slot_of(k, ncap);
            while (slots[j] != 0) {
                j = (j + 1) & (ncap - 1);
            }
            slots[j] = k;
        }
    }
    free(s->slots);
    s->slots = slots;
    s->cap = ncap;
    return PC_OK;
}

pc_status pc_ops_ptrset_insert(pc_ops_ptrset *s, uintptr_t key, bool *inserted)
{
    size_t i;
    if (s->cap > 0) {
        for (i = slot_of(key, s->cap); s->slots[i] != 0; i = (i + 1) & (s->cap - 1)) {
            if (s->slots[i] == key) {
                *inserted = false;
                return PC_OK;
            }
        }
    }
    if (s->cap == 0 || s->count + 1 > s->cap / 4 * 3) {
        size_t ncap;
        pc_status st;
        if (s->count == SIZE_MAX || !table_cap(s->count + 1, &ncap)) {
            return PC_ERR_NOMEM;
        }
        st = ptrset_resize(s, ncap);
        if (st != PC_OK) {
            return st;
        }
    }
    for (i = slot_of(key, s->cap); s->slots[i] != 0; i = (i + 1) & (s->cap - 1)) {
    }
    s->slots[i] = key;
    s->count++;
    *inserted = true;
    return PC_OK;
}

static size_t sat_add(size_t a, size_t b)
{
    return a > SIZE_MAX - b ? SIZE_MAX : a + b;
}

static uint64_t sat_add64(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

typedef struct tile_bytes_ctx {
    pc_ops_ptrset *seen;
    size_t n;
} tile_bytes_ctx;

static pc_status tile_bytes_visit(const pc_surface *s, void *p)
{
    tile_bytes_ctx *c = p;
    size_t cursor = 0;
    pc_tile coord;
    const pc_raster_tile *tile;
    while (pc_surface_next_tile(s, &cursor, &coord, &tile)) {
        bool inserted;
        pc_status st = pc_ops_ptrset_insert(c->seen, (uintptr_t)tile, &inserted);
        if (st != PC_OK) {
            return st;
        }
        if (inserted) {
            c->n = sat_add(c->n, pc_raster_tile_len(tile));
        }
    }
    return PC_OK;
}

pc_status pc_ops_tile_bytes(const pc_document *doc, pc_ops_ptrset *seen, size_t *out)
{
    tile_bytes_ctx c = {seen, 0};
    pc_status st = pc_ops_surfaces(doc, tile_bytes_visit, &c);
    if (st == PC_OK) {
        *out = c.n;
    }
    return st;
}

/* ---- the counter ---- */

void pc_ops_tiles_fini(pc_ops_tiles *t)
{
    free(t->slots);
    memset(t, 0, sizeof *t);
}

pc_status pc_ops_tiles_clone(const pc_ops_tiles *t, pc_ops_tiles *out)
{
    pc_ops_tiles r = {NULL, t->cap, t->count, t->bytes};
    if (t->cap > 0) {
        r.slots = pc_ops_calloc_array(t->cap, sizeof *r.slots);
        if (r.slots == NULL) {
            return PC_ERR_NOMEM;
        }
        memcpy(r.slots, t->slots, t->cap * sizeof *r.slots);
    }
    *out = r;
    return PC_OK;
}

static pc_ops_tile_ref *tiles_find(const pc_ops_tiles *t, uintptr_t key, size_t *pos)
{
    if (t->cap == 0) {
        return NULL;
    }
    for (size_t i = slot_of(key, t->cap); t->slots[i].key != 0; i = (i + 1) & (t->cap - 1)) {
        if (t->slots[i].key == key) {
            *pos = i;
            return &t->slots[i];
        }
    }
    return NULL;
}

static pc_status tiles_resize(pc_ops_tiles *t, size_t ncap)
{
    pc_ops_tile_ref *slots = pc_ops_calloc_array(ncap, sizeof *slots);
    if (slots == NULL) {
        return PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < t->cap; i++) {
        if (t->slots[i].key != 0) {
            size_t j = slot_of(t->slots[i].key, ncap);
            while (slots[j].key != 0) {
                j = (j + 1) & (ncap - 1);
            }
            slots[j] = t->slots[i];
        }
    }
    free(t->slots);
    t->slots = slots;
    t->cap = ncap;
    return PC_OK;
}

/* Backward-shift deletion keeps linear probing free of tombstones. */
static void tiles_erase(pc_ops_tiles *t, size_t i)
{
    size_t mask = t->cap - 1;
    size_t j = i;
    t->slots[i].key = 0;
    for (;;) {
        size_t k;
        j = (j + 1) & mask;
        if (t->slots[j].key == 0) {
            break;
        }
        k = slot_of(t->slots[j].key, t->cap);
        /* Move j into the hole at i unless its home slot k lies cyclically in (i, j]. */
        if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) {
            t->slots[i] = t->slots[j];
            t->slots[j].key = 0;
            i = j;
        }
    }
    t->count--;
}

static pc_status tiles_add_visit(const pc_surface *s, void *p)
{
    pc_ops_tiles *t = p;
    size_t cursor = 0;
    pc_tile coord;
    const pc_raster_tile *tile;
    while (pc_surface_next_tile(s, &cursor, &coord, &tile)) {
        uintptr_t key = (uintptr_t)tile;
        size_t pos, i;
        pc_ops_tile_ref *r = tiles_find(t, key, &pos);
        if (r != NULL) {
            r->count++;
            continue;
        }
        if (t->cap == 0 || t->count + 1 > t->cap / 4 * 3) {
            size_t ncap;
            pc_status st;
            if (t->count == SIZE_MAX || !table_cap(t->count + 1, &ncap)) {
                return PC_ERR_NOMEM;
            }
            st = tiles_resize(t, ncap);
            if (st != PC_OK) {
                return st;
            }
        }
        for (i = slot_of(key, t->cap); t->slots[i].key != 0; i = (i + 1) & (t->cap - 1)) {
        }
        t->slots[i] = (pc_ops_tile_ref){key, 1, pc_raster_tile_len(tile)};
        t->count++;
        t->bytes = sat_add64(t->bytes, (uint64_t)pc_raster_tile_len(tile));
    }
    return PC_OK;
}

pc_status pc_ops_tiles_add(pc_ops_tiles *t, const pc_document *doc)
{
    return pc_ops_surfaces(doc, tiles_add_visit, t);
}

static pc_status tiles_remove_visit(const pc_surface *s, void *p)
{
    pc_ops_tiles *t = p;
    size_t cursor = 0;
    pc_tile coord;
    const pc_raster_tile *tile;
    while (pc_surface_next_tile(s, &cursor, &coord, &tile)) {
        size_t pos;
        pc_ops_tile_ref *r = tiles_find(t, (uintptr_t)tile, &pos);
        if (r != NULL) {
            r->count--;
            if (r->count == 0) {
                uint64_t len = (uint64_t)r->len;
                t->bytes = t->bytes >= len ? t->bytes - len : 0;
                tiles_erase(t, pos);
            }
        }
    }
    return PC_OK;
}

pc_status pc_ops_tiles_remove(pc_ops_tiles *t, const pc_document *doc)
{
    return pc_ops_surfaces(doc, tiles_remove_visit, t);
}

typedef struct bytes_with_ctx {
    const pc_ops_tiles *t;
    pc_ops_ptrset seen;
    uint64_t total;
} bytes_with_ctx;

static pc_status bytes_with_visit(const pc_surface *s, void *p)
{
    bytes_with_ctx *c = p;
    size_t cursor = 0;
    pc_tile coord;
    const pc_raster_tile *tile;
    while (pc_surface_next_tile(s, &cursor, &coord, &tile)) {
        size_t pos;
        bool inserted;
        pc_status st;
        if (tiles_find(c->t, (uintptr_t)tile, &pos) != NULL) {
            continue;
        }
        st = pc_ops_ptrset_insert(&c->seen, (uintptr_t)tile, &inserted);
        if (st != PC_OK) {
            return st;
        }
        if (inserted) {
            c->total = sat_add64(c->total, (uint64_t)pc_raster_tile_len(tile));
        }
    }
    return PC_OK;
}

pc_status pc_ops_tiles_bytes_with(const pc_ops_tiles *t, const pc_document *current, uint64_t *out)
{
    bytes_with_ctx c = {t, {NULL, 0, 0}, t->bytes};
    pc_status st = pc_ops_surfaces(current, bytes_with_visit, &c);
    pc_ops_ptrset_fini(&c.seen);
    if (st == PC_OK) {
        *out = c.total;
    }
    return st;
}
