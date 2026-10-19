#include "surface.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "color/convert.h"

#define TS PC_TILE_SIZE
#define TILE_PIXELS ((size_t)TS * (size_t)TS)

struct pc_raster_tile {
    atomic_size_t refs;
    size_t len;
    uint8_t data[];
};

/* ---- tiles ---- */

pc_raster_tile *pc_raster_tile_retain(pc_raster_tile *t)
{
    if (t != NULL) {
        atomic_fetch_add(&t->refs, 1);
    }
    return t;
}

void pc_raster_tile_release(pc_raster_tile *t)
{
    if (t != NULL && atomic_fetch_sub(&t->refs, 1) == 1) {
        free(t);
    }
}

const uint8_t *pc_raster_tile_bytes(const pc_raster_tile *t)
{
    return t->data;
}

size_t pc_raster_tile_len(const pc_raster_tile *t)
{
    return t->len;
}

bool pc_raster_tile_eq(const pc_raster_tile *a, const pc_raster_tile *b)
{
    return a == b || (a->len == b->len && memcmp(a->data, b->data, a->len) == 0);
}

void pc_tile_entries_free(pc_tile_entry *entries, size_t n)
{
    size_t i;
    for (i = 0; entries != NULL && i < n; i++) {
        pc_raster_tile_release(entries[i].tile);
    }
    free(entries);
}

static pc_raster_tile *tile_alloc(size_t len)
{
    pc_raster_tile *t = malloc(sizeof *t + len);
    if (t != NULL) {
        atomic_init(&t->refs, 1);
        t->len = len;
    }
    return t;
}

/* A tile of `bpp`-byte pixels all equal to `pixel`. */
static pc_raster_tile *tile_filled(size_t bpp, const uint8_t *pixel)
{
    size_t len = TILE_PIXELS * bpp;
    pc_raster_tile *t = tile_alloc(len);
    size_t done;
    size_t i;
    bool zero = true;
    if (t == NULL) {
        return NULL;
    }
    for (i = 0; i < bpp; i++) {
        zero = zero && pixel[i] == 0;
    }
    if (zero) {
        memset(t->data, 0, len);
        return t;
    }
    memcpy(t->data, pixel, bpp);
    for (done = bpp; done < len;) { /* doubling copy */
        size_t n = done <= len - done ? done : len - done;
        memcpy(t->data + done, t->data, n);
        done += n;
    }
    return t;
}

uint8_t *pc_raster_tile_make_mut(pc_raster_tile **t)
{
    pc_raster_tile *c;
    if (*t == NULL) {
        return NULL;
    }
    if (atomic_load(&(*t)->refs) == 1) {
        return (*t)->data;
    }
    c = tile_alloc((*t)->len);
    if (c == NULL) {
        return NULL;
    }
    memcpy(c->data, (*t)->data, c->len);
    pc_raster_tile_release(*t);
    *t = c;
    return c->data;
}

/* ---- sample access (the same formulas as color's pc_read_sample / pc_write_sample, without
 * bounds checks: every index below is inside the tile by construction) ---- */

static inline float rd(const uint8_t *d, pc_sample_type s, size_t i)
{
    switch (s) {
    case PC_SAMPLE_TYPE_U8:
        return (float)d[i] / 255.0f;
    case PC_SAMPLE_TYPE_U16: {
        uint16_t v;
        memcpy(&v, d + 2 * i, 2);
        return (float)v / 65535.0f;
    }
    case PC_SAMPLE_TYPE_F32: {
        float v;
        memcpy(&v, d + 4 * i, 4);
        return v;
    }
    }
    return 0.0f;
}

static inline void wr(uint8_t *d, pc_sample_type s, size_t i, float v)
{
    switch (s) {
    case PC_SAMPLE_TYPE_U8:
        d[i] = pc_f32_to_u8(pc_clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        return;
    case PC_SAMPLE_TYPE_U16: {
        uint16_t q = pc_f32_to_u16(pc_clampf(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
        memcpy(d + 2 * i, &q, 2);
        return;
    }
    case PC_SAMPLE_TYPE_F32:
        memcpy(d + 4 * i, &v, 4);
        return;
    }
}

/* ---- the tile table: open addressing, linear probing, backward-shift deletion ---- */

static size_t slot_of(const pc_surface *s, pc_tile c)
{
    uint64_t h =
        (uint64_t)(uint32_t)c.tx * 0x9E3779B97F4A7C15u ^ (uint64_t)(uint32_t)c.ty * 0xC2B2AE3D27D4EB4Fu;
    h ^= h >> 29;
    return (size_t)h & (s->cap - 1);
}

static bool same(pc_tile a, pc_tile b)
{
    return a.tx == b.tx && a.ty == b.ty;
}

/* Index of c's slot, or SIZE_MAX. */
static size_t find(const pc_surface *s, pc_tile c)
{
    size_t i;
    if (s->count == 0) {
        return SIZE_MAX;
    }
    for (i = slot_of(s, c); s->slots[i].tile != NULL; i = (i + 1) & (s->cap - 1)) {
        if (same(s->slots[i].coord, c)) {
            return i;
        }
    }
    return SIZE_MAX;
}

static pc_raster_tile *get(const pc_surface *s, pc_tile c)
{
    size_t i = find(s, c);
    return i == SIZE_MAX ? NULL : s->slots[i].tile;
}

static void place(pc_surface *s, pc_tile c, pc_raster_tile *t)
{
    size_t i = slot_of(s, c);
    while (s->slots[i].tile != NULL) {
        i = (i + 1) & (s->cap - 1);
    }
    s->slots[i].coord = c;
    s->slots[i].tile = t;
    s->count++;
}

static pc_status reserve(pc_surface *s, size_t need, pc_error *err)
{
    size_t cap = s->cap == 0 ? 64 : s->cap;
    pc_tile_entry *old = s->slots;
    size_t old_cap = s->cap;
    size_t i;
    while (need > cap / 2) {
        if (cap > PC_MAX_ALLOC / sizeof(pc_tile_entry) / 2) {
            return pc_fail(err, PC_ERR_LIMIT, "too many tiles (%zu)", need);
        }
        cap *= 2;
    }
    if (cap == s->cap) {
        return PC_OK;
    }
    s->slots = pc_calloc_array(cap, sizeof *s->slots);
    if (s->slots == NULL) {
        s->slots = old;
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for %zu tiles", need);
    }
    s->cap = cap;
    s->count = 0;
    for (i = 0; i < old_cap; i++) {
        if (old[i].tile != NULL) {
            place(s, old[i].coord, old[i].tile);
        }
    }
    free(old);
    return PC_OK;
}

/* Sets tile c to t, taking over t's reference (released on failure too). */
static pc_status put(pc_surface *s, pc_tile c, pc_raster_tile *t, pc_error *err)
{
    size_t i = find(s, c);
    pc_status st;
    if (i != SIZE_MAX) {
        pc_raster_tile_release(s->slots[i].tile);
        s->slots[i].tile = t;
        return PC_OK;
    }
    st = reserve(s, s->count + 1, err);
    if (st != PC_OK) {
        pc_raster_tile_release(t);
        return st;
    }
    place(s, c, t);
    return PC_OK;
}

/* Removes tile c and returns its reference (or NULL). */
static pc_raster_tile *take(pc_surface *s, pc_tile c)
{
    size_t i = find(s, c);
    size_t j;
    pc_raster_tile *t;
    if (i == SIZE_MAX) {
        return NULL;
    }
    t = s->slots[i].tile;
    s->slots[i].tile = NULL;
    s->count--;
    /* Backward shift: move later members of the probe run into the hole when allowed. */
    for (j = (i + 1) & (s->cap - 1); s->slots[j].tile != NULL; j = (j + 1) & (s->cap - 1)) {
        size_t home = slot_of(s, s->slots[j].coord);
        /* j may move to i if home is not cyclically within (i, j]. */
        bool stays = i <= j ? (home > i && home <= j) : (home > i || home <= j);
        if (!stays) {
            s->slots[i] = s->slots[j];
            s->slots[j].tile = NULL;
            i = j;
        }
    }
    return t;
}

/* ---- tile ranges ---- */

typedef struct span {
    pc_tile a;
    pc_tile b;
    uint64_t n; /* 0 for an empty rect */
} span;

static span span_of(pc_rect r)
{
    span sp;
    memset(&sp, 0, sizeof sp);
    if (pc_rect_tile_range(r, &sp.a, &sp.b)) {
        sp.n = (uint64_t)((int64_t)sp.b.tx - sp.a.tx + 1) * (uint64_t)((int64_t)sp.b.ty - sp.a.ty + 1);
    }
    return sp;
}

static pc_status span_check(span sp, pc_error *err)
{
    if (sp.n > PC_SURFACE_MAX_TILE_SPAN) {
        return pc_fail(err, PC_ERR_LIMIT, "operation spans %llu tiles (limit %llu)", (unsigned long long)sp.n,
                       (unsigned long long)PC_SURFACE_MAX_TILE_SPAN);
    }
    return PC_OK;
}

/* Iterates the tiles of a span in the reference's order (ty outer, tx inner). */
#define FOR_SPAN(sp, c)                                                                                      \
    for ((c).ty = (sp).a.ty; (sp).n != 0 && (c).ty <= (sp).b.ty; (c).ty++)                                   \
        for ((c).tx = (sp).a.tx; (c).tx <= (sp).b.tx; (c).tx++)

/* Offset (in pixels) of (x, y) from tile c's origin; both in [0, TS) for pixels of c. */
static size_t local_index(pc_tile c, int32_t x, int32_t y)
{
    pc_rect o = pc_tile_rect(c);
    uint32_t lx = (uint32_t)pc_i32_wrapping_sub(x, o.x0) & (TS - 1);
    uint32_t ly = (uint32_t)pc_i32_wrapping_sub(y, o.y0) & (TS - 1);
    return (size_t)ly * TS + lx;
}

/* ---- the surface ---- */

void pc_surface_init(pc_surface *s, pc_pixel_format f)
{
    memset(s, 0, sizeof *s);
    s->format = f;
    s->bpp = pc_pixel_format_bytes_per_pixel(f);
}

void pc_surface_init_default(pc_surface *s, pc_pixel_format f, const float *px, size_t n)
{
    pc_surface_init(s, f);
    pc_encode_pixel(f, px, n, s->default_pixel);
}

void pc_surface_fini(pc_surface *s)
{
    size_t i;
    if (s == NULL) {
        return;
    }
    for (i = 0; i < s->cap; i++) {
        pc_raster_tile_release(s->slots[i].tile);
    }
    free(s->slots);
    s->slots = NULL;
    s->cap = 0;
    s->count = 0;
}

pc_status pc_surface_clone(const pc_surface *s, pc_surface *out, pc_error *err)
{
    pc_surface c = *s;
    size_t i;
    if (s->cap > 0) {
        c.slots = pc_malloc_array(s->cap, sizeof *c.slots);
        if (c.slots == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory cloning a surface");
        }
        memcpy(c.slots, s->slots, s->cap * sizeof *c.slots);
        for (i = 0; i < c.cap; i++) {
            pc_raster_tile_retain(c.slots[i].tile);
        }
    }
    *out = c;
    return PC_OK;
}

bool pc_surface_eq(const pc_surface *a, const pc_surface *b)
{
    size_t i;
    if (!pc_pixel_format_eq(a->format, b->format) ||
        memcmp(a->default_pixel, b->default_pixel, a->bpp) != 0 || a->count != b->count) {
        return false;
    }
    for (i = 0; i < a->cap; i++) {
        if (a->slots[i].tile != NULL) {
            const pc_raster_tile *t = get(b, a->slots[i].coord);
            if (t == NULL || !pc_raster_tile_eq(a->slots[i].tile, t)) {
                return false;
            }
        }
    }
    return true;
}

pc_pixel_format pc_surface_format(const pc_surface *s)
{
    return s->format;
}

size_t pc_surface_channels(const pc_surface *s)
{
    return pc_pixel_format_channels(s->format);
}

size_t pc_surface_tile_count(const pc_surface *s)
{
    return s->count;
}

const pc_raster_tile *pc_surface_tile(const pc_surface *s, pc_tile c)
{
    return get(s, c);
}

pc_raster_tile *pc_surface_retain_tile(const pc_surface *s, pc_tile c)
{
    return pc_raster_tile_retain(get(s, c));
}

bool pc_surface_next_tile(const pc_surface *s, size_t *cursor, pc_tile *coord, const pc_raster_tile **tile)
{
    size_t i;
    for (i = *cursor; i < s->cap; i++) {
        if (s->slots[i].tile != NULL) {
            *coord = s->slots[i].coord;
            *tile = s->slots[i].tile;
            *cursor = i + 1;
            return true;
        }
    }
    *cursor = s->cap;
    return false;
}

static int entry_cmp(const void *a, const void *b)
{
    return pc_tile_cmp(((const pc_tile_entry *)a)->coord, ((const pc_tile_entry *)b)->coord);
}

pc_status pc_surface_tiles_sorted(const pc_surface *s, pc_tile_entry **out, size_t *n, pc_error *err)
{
    pc_tile_entry *e = NULL;
    size_t k = 0;
    size_t i;
    if (s->count > 0) {
        e = pc_malloc_array(s->count, sizeof *e);
        if (e == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory listing %zu tiles", s->count);
        }
        for (i = 0; i < s->cap; i++) {
            if (s->slots[i].tile != NULL) {
                e[k].coord = s->slots[i].coord;
                e[k].tile = pc_raster_tile_retain(s->slots[i].tile);
                k++;
            }
        }
        qsort(e, k, sizeof *e, entry_cmp);
    }
    *out = e;
    *n = k;
    return PC_OK;
}

void pc_surface_default_pixel(const pc_surface *s, float out[PC_RASTER_MAX_CHANNELS])
{
    pc_decode_pixel(s->format, s->default_pixel, out);
}

const uint8_t *pc_surface_default_bytes(const pc_surface *s)
{
    return s->default_pixel;
}

/* The slot index of tile c, allocated (default-filled) or un-shared. */
static pc_status tile_mut_slot(pc_surface *s, pc_tile c, size_t *slot, pc_error *err)
{
    size_t i = find(s, c);
    pc_status st;
    if (i == SIZE_MAX) {
        pc_raster_tile *t = tile_filled(s->bpp, s->default_pixel);
        if (t == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for a tile");
        }
        st = put(s, c, t, err);
        if (st != PC_OK) {
            return st;
        }
        i = find(s, c);
    } else if (pc_raster_tile_make_mut(&s->slots[i].tile) == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory copying a shared tile");
    }
    *slot = i;
    return PC_OK;
}

pc_status pc_surface_tile_mut(pc_surface *s, pc_tile c, uint8_t **data, pc_error *err)
{
    size_t i;
    pc_status st = tile_mut_slot(s, c, &i, err);
    if (st == PC_OK) {
        *data = s->slots[i].tile->data;
    }
    return st;
}

pc_rect pc_surface_tile_bounds(const pc_surface *s)
{
    pc_rect r = PC_RECT_EMPTY;
    size_t i;
    for (i = 0; i < s->cap; i++) {
        if (s->slots[i].tile != NULL) {
            r = pc_rect_union(r, pc_tile_rect(s->slots[i].coord));
        }
    }
    return r;
}

typedef struct ring_entry {
    int64_t key;
    size_t slot;
} ring_entry;

static int ring_cmp(const void *a, const void *b)
{
    int64_t x = ((const ring_entry *)a)->key;
    int64_t y = ((const ring_entry *)b)->key;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int64_t min64(int64_t a, int64_t b)
{
    return a < b ? a : b;
}

/* Content rect of one tile in local coordinates; false when all default. */
static bool tile_content(const pc_surface *s, const uint8_t *data, pc_rect *out)
{
    size_t bpp = s->bpp;
    const uint8_t *dp = s->default_pixel;
    size_t x0 = SIZE_MAX;
    size_t y0 = SIZE_MAX;
    size_t x1 = 0;
    size_t y1 = 0;
    size_t y;
    for (y = 0; y < TS; y++) {
        const uint8_t *row = data + y * TS * bpp;
        size_t first;
        size_t last;
        for (first = 0; first < TS && memcmp(row + first * bpp, dp, bpp) == 0; first++) {
        }
        if (first == TS) {
            continue;
        }
        for (last = TS - 1; last > first && memcmp(row + last * bpp, dp, bpp) == 0; last--) {
        }
        x0 = first < x0 ? first : x0;
        x1 = last + 1 > x1 ? last + 1 : x1;
        y0 = y < y0 ? y : y0;
        y1 = y + 1;
    }
    if (x0 == SIZE_MAX) {
        return false;
    }
    *out = pc_rect_make((int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1);
    return true;
}

pc_rect pc_surface_content_bounds(const pc_surface *s)
{
    int64_t gx0 = INT32_MAX;
    int64_t gy0 = INT32_MAX;
    int64_t gx1 = INT32_MIN;
    int64_t gy1 = INT32_MIN;
    ring_entry *order;
    pc_rect out = PC_RECT_EMPTY;
    size_t k = 0;
    size_t i;
    if (s->count == 0) {
        return PC_RECT_EMPTY;
    }
    for (i = 0; i < s->cap; i++) {
        if (s->slots[i].tile != NULL) {
            pc_tile c = s->slots[i].coord;
            gx0 = min64(gx0, c.tx);
            gy0 = min64(gy0, c.ty);
            gx1 = c.tx > gx1 ? c.tx : gx1;
            gy1 = c.ty > gy1 ? c.ty : gy1;
        }
    }
    /* Scan tiles from the outside of the tile grid inwards: a tile lying entirely inside the
     * bounds found so far cannot extend them and is skipped (the result doesn't depend on the
     * order; it only saves work). Without memory for the order, scan in table order. */
    order = pc_malloc_array(s->count, sizeof *order);
    for (i = 0; i < s->cap; i++) {
        if (s->slots[i].tile != NULL) {
            pc_tile c = s->slots[i].coord;
            if (order != NULL) {
                order[k].key = min64(min64(c.tx - gx0, gx1 - c.tx), min64(c.ty - gy0, gy1 - c.ty));
                order[k].slot = i;
            }
            k++;
        }
    }
    if (order != NULL) {
        qsort(order, k, sizeof *order, ring_cmp);
    }
    for (i = 0; i < k || (order == NULL && i < s->cap); i++) {
        size_t slot = order != NULL ? order[i].slot : i;
        pc_rect origin;
        pc_rect local;
        if (s->slots[slot].tile == NULL) {
            continue;
        }
        origin = pc_tile_rect(s->slots[slot].coord);
        if (!pc_rect_is_empty(out) && pc_rect_contains_rect(out, origin)) {
            continue;
        }
        if (tile_content(s, s->slots[slot].tile->data, &local)) {
            out = pc_rect_union(out, pc_rect_translate(local, origin.x0, origin.y0));
        }
    }
    free(order);
    return out;
}

static bool all_default(const pc_surface *s, const pc_raster_tile *t)
{
    size_t i;
    for (i = 0; i < t->len; i += s->bpp) {
        if (memcmp(t->data + i, s->default_pixel, s->bpp) != 0) {
            return false;
        }
    }
    return true;
}

void pc_surface_prune(pc_surface *s)
{
    size_t i = 0;
    /* A removal shifts later entries of the probe run back into the emptied slot, so rescan it.
     * Entries not yet visited only ever move to slots at or after the cursor. */
    while (i < s->cap) {
        if (s->slots[i].tile != NULL && all_default(s, s->slots[i].tile)) {
            pc_raster_tile_release(take(s, s->slots[i].coord));
            continue;
        }
        i++;
    }
}

void pc_surface_read_pixel(const pc_surface *s, int32_t x, int32_t y, float *out)
{
    pc_tile c = pc_tile_containing(x, y);
    const pc_raster_tile *t = get(s, c);
    size_t n = pc_pixel_format_channels(s->format);
    size_t i;
    if (t != NULL) {
        size_t base = local_index(c, x, y) * n;
        for (i = 0; i < n; i++) {
            out[i] = rd(t->data, s->format.sample, base + i);
        }
    } else {
        for (i = 0; i < n; i++) {
            out[i] = rd(s->default_pixel, s->format.sample, i);
        }
    }
}

pc_status pc_surface_write_pixel(pc_surface *s, int32_t x, int32_t y, const float *px, size_t n,
                                 pc_error *err)
{
    pc_tile c = pc_tile_containing(x, y);
    size_t ch = pc_pixel_format_channels(s->format);
    size_t slot;
    size_t base;
    size_t i;
    pc_status st = tile_mut_slot(s, c, &slot, err);
    if (st != PC_OK) {
        return st;
    }
    base = local_index(c, x, y) * ch;
    for (i = 0; i < n && i < ch; i++) {
        wr(s->slots[slot].tile->data, s->format.sample, base + i, px[i]);
    }
    return PC_OK;
}

float pc_surface_sample_channel(const pc_surface *s, int32_t x, int32_t y, size_t c)
{
    pc_tile tc = pc_tile_containing(x, y);
    const pc_raster_tile *t;
    size_t n = pc_pixel_format_channels(s->format);
    if (c >= n) {
        return 0.0f;
    }
    t = get(s, tc);
    if (t == NULL) {
        return rd(s->default_pixel, s->format.sample, c);
    }
    return rd(t->data, s->format.sample, local_index(tc, x, y) * n + c);
}

static int32_t floor_div_ts(int32_t v)
{
    return v >= 0 ? v / TS : -(int32_t)(((uint32_t)-(int64_t)v + (TS - 1)) / TS);
}

void pc_surface_sample_row_strided(const pc_surface *s, int32_t y, int32_t x0, int32_t step, size_t c,
                                   float *out, size_t n)
{
    size_t ch = pc_pixel_format_channels(s->format);
    int32_t ty = floor_div_ts(y);
    size_t ly = (uint32_t)y & (TS - 1);
    float dp;
    size_t i = 0;
    if (c >= ch) {
        for (i = 0; i < n; i++) {
            out[i] = 0.0f;
        }
        return;
    }
    step = step < 1 ? 1 : step;
    dp = rd(s->default_pixel, s->format.sample, c);
    while (i < n) {
        /* The reference's release-build i32 arithmetic (wrapping). */
        int32_t x = pc_i32_wrapping_add(x0, pc_i32_wrapping_mul(pc_u32_as_i32((uint32_t)i), step));
        int32_t tx = floor_div_ts(x);
        int32_t tile_end = pc_i32_wrapping_mul(pc_i32_wrapping_add(tx, 1), TS);
        int32_t cnt =
            pc_i32_wrapping_sub(pc_i32_wrapping_add(pc_i32_wrapping_sub(tile_end, x), step), 1) / step;
        size_t count = cnt < 1 ? 1 : (size_t)cnt;
        size_t end = count < n - i ? i + count : n;
        pc_tile key;
        const pc_raster_tile *t;
        key.tx = tx;
        key.ty = ty;
        t = get(s, key);
        if (t == NULL) {
            for (; i < end; i++) {
                out[i] = dp;
            }
            continue;
        }
        {
            size_t k;
            int32_t origin = pc_i32_wrapping_mul(tx, TS);
            for (k = 0; i + k < end; k++) {
                int32_t xk = pc_i32_wrapping_add(x, pc_i32_wrapping_mul(pc_u32_as_i32((uint32_t)k), step));
                size_t lx = (uint32_t)pc_i32_wrapping_sub(xk, origin) & (TS - 1);
                out[i + k] = rd(t->data, s->format.sample, (ly * TS + lx) * ch + c);
            }
        }
        i = end;
    }
}

void pc_surface_rgba(const pc_surface *s, int32_t x, int32_t y, float out[4])
{
    float p[PC_RASTER_MAX_CHANNELS];
    pc_surface_read_pixel(s, x, y, p);
    pc_to_rgba(s->format, p, pc_pixel_format_channels(s->format), out);
}

bool pc_surface_has_tiles_in(const pc_surface *s, pc_rect r)
{
    span sp = span_of(r);
    pc_tile c;
    size_t i;
    if (sp.n == 0 || s->count == 0) {
        return false;
    }
    if (sp.n > s->count) { /* cheaper to test every allocated tile against the range */
        for (i = 0; i < s->cap; i++) {
            pc_tile t = s->slots[i].coord;
            if (s->slots[i].tile != NULL && t.tx >= sp.a.tx && t.tx <= sp.b.tx && t.ty >= sp.a.ty &&
                t.ty <= sp.b.ty) {
                return true;
            }
        }
        return false;
    }
    FOR_SPAN(sp, c)
    {
        if (get(s, c) != NULL) {
            return true;
        }
    }
    return false;
}

/* w * h * per, checked against PC_MAX_ALLOC (as bytes of `elem`). */
static pc_status region_len(pc_rect r, size_t per, size_t elem, size_t *out, pc_error *err)
{
    size_t n;
    size_t bytes;
    if (!pc_size_mul3(pc_rect_width(r), pc_rect_height(r), per, &n) || !pc_size_mul(n, elem, &bytes) ||
        bytes > PC_MAX_ALLOC) {
        return pc_fail(err, PC_ERR_LIMIT, "region of %ux%u is too large", (unsigned)pc_rect_width(r),
                       (unsigned)pc_rect_height(r));
    }
    *out = n;
    return PC_OK;
}

/* Offset of (x, y) from r's origin in pixels (both inside r). */
static size_t region_index(pc_rect r, int32_t x, int32_t y)
{
    return (size_t)((int64_t)y - r.y0) * pc_rect_width(r) + (size_t)((int64_t)x - r.x0);
}

pc_status pc_surface_read_region_into(const pc_surface *s, pc_rect r, float *out, size_t len, pc_error *err)
{
    size_t n = pc_pixel_format_channels(s->format);
    size_t want;
    float dp[PC_RASTER_MAX_CHANNELS];
    span sp;
    pc_tile tc;
    pc_status st = region_len(r, n, sizeof(float), &want, err);
    if (st != PC_OK) {
        return st;
    }
    if (len != want) {
        return pc_fail(err, PC_ERR_INVALID, "region buffer holds %zu floats, need %zu", len, want);
    }
    sp = span_of(r);
    pc_surface_default_pixel(s, dp);
    FOR_SPAN(sp, tc)
    {
        pc_rect tr = pc_rect_intersect(pc_tile_rect(tc), r);
        const pc_raster_tile *t = get(s, tc);
        int32_t y;
        size_t span_n = (size_t)pc_rect_width(tr) * n;
        for (y = tr.y0; y < tr.y1; y++) {
            float *dst = out + region_index(r, tr.x0, y) * n;
            size_t i;
            if (t != NULL) {
                size_t base = local_index(tc, tr.x0, y) * n;
                for (i = 0; i < span_n; i++) {
                    dst[i] = rd(t->data, s->format.sample, base + i);
                }
            } else {
                for (i = 0; i < span_n; i += n) {
                    memcpy(dst + i, dp, n * sizeof *dp);
                }
            }
        }
    }
    return PC_OK;
}

pc_status pc_surface_read_region(const pc_surface *s, pc_rect r, float **out, size_t *len, pc_error *err)
{
    size_t n;
    float *buf = NULL;
    pc_status st = region_len(r, pc_pixel_format_channels(s->format), sizeof(float), &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n > 0) {
        buf = pc_malloc_array(n, sizeof *buf);
        if (buf == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory reading a region");
        }
        st = pc_surface_read_region_into(s, r, buf, n, err);
        if (st != PC_OK) {
            free(buf);
            return st;
        }
    }
    *out = buf;
    *len = n;
    return PC_OK;
}

pc_status pc_surface_read_rgba_into(const pc_surface *s, pc_rect r, float (*out)[4], size_t len,
                                    pc_error *err)
{
    pc_pixel_format f = s->format;
    size_t n = pc_pixel_format_channels(f);
    size_t want;
    float dpx[PC_RASTER_MAX_CHANNELS];
    float dp[4];
    span sp;
    pc_tile tc;
    pc_status st = region_len(r, 1, 4 * sizeof(float), &want, err);
    if (st != PC_OK) {
        return st;
    }
    if (len != want) {
        return pc_fail(err, PC_ERR_INVALID, "RGBA buffer holds %zu pixels, need %zu", len, want);
    }
    pc_surface_default_pixel(s, dpx);
    pc_to_rgba(f, dpx, n, dp);
    sp = span_of(r);
    FOR_SPAN(sp, tc)
    {
        pc_rect tr = pc_rect_intersect(pc_tile_rect(tc), r);
        const pc_raster_tile *t = get(s, tc);
        size_t w = pc_rect_width(tr);
        int32_t y;
        for (y = tr.y0; y < tr.y1; y++) {
            float (*dst)[4] = out + region_index(r, tr.x0, y);
            size_t i;
            size_t base;
            if (t == NULL) {
                for (i = 0; i < w; i++) {
                    memcpy(dst[i], dp, sizeof dp);
                }
                continue;
            }
            base = local_index(tc, tr.x0, y) * n;
            if (f.mode == PC_COLOR_MODE_RGB && f.sample == PC_SAMPLE_TYPE_U8 && f.alpha) {
                const uint8_t *src = t->data + base;
                for (i = 0; i < w; i++) {
                    dst[i][0] = (float)src[4 * i] / 255.0f;
                    dst[i][1] = (float)src[4 * i + 1] / 255.0f;
                    dst[i][2] = (float)src[4 * i + 2] / 255.0f;
                    dst[i][3] = (float)src[4 * i + 3] / 255.0f;
                }
            } else if (f.mode == PC_COLOR_MODE_GRAYSCALE && f.sample == PC_SAMPLE_TYPE_U8 && f.alpha) {
                const uint8_t *src = t->data + base;
                for (i = 0; i < w; i++) {
                    float g = (float)src[2 * i] / 255.0f;
                    dst[i][0] = g;
                    dst[i][1] = g;
                    dst[i][2] = g;
                    dst[i][3] = (float)src[2 * i + 1] / 255.0f;
                }
            } else if (f.mode == PC_COLOR_MODE_CMYK && f.sample == PC_SAMPLE_TYPE_U8 && n >= 4) {
                /* Identical ink bytes share the exact ICC result; alpha is independent. */
                const uint8_t *src = t->data + base;
                uint8_t key[4] = {0, 0, 0, 0};
                float rgb[3] = {0.0f, 0.0f, 0.0f};
                bool have = false;
                for (i = 0; i < w; i++) {
                    const uint8_t *ink = src + i * n;
                    if (!have || memcmp(ink, key, 4) != 0) {
                        float c4[4];
                        size_t c;
                        for (c = 0; c < 4; c++) {
                            c4[c] = (float)ink[c] / 255.0f;
                        }
                        pc_cmyk_to_rgb(c4, rgb);
                        memcpy(key, ink, 4);
                        have = true;
                    }
                    dst[i][0] = rgb[0];
                    dst[i][1] = rgb[1];
                    dst[i][2] = rgb[2];
                    dst[i][3] = n > 4 ? (float)ink[4] / 255.0f : 1.0f;
                }
            } else {
                for (i = 0; i < w; i++) {
                    float px[PC_RASTER_MAX_CHANNELS];
                    size_t c;
                    for (c = 0; c < n; c++) {
                        px[c] = rd(t->data, f.sample, base + i * n + c);
                    }
                    pc_to_rgba(f, px, n, dst[i]);
                }
            }
        }
    }
    return PC_OK;
}

static uint8_t q8(float v)
{
    return pc_f32_to_u8(pc_clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

pc_status pc_surface_read_rgba8_into(const pc_surface *s, pc_rect r, uint8_t (*out)[4], size_t len,
                                     pc_error *err)
{
    pc_pixel_format f = s->format;
    bool rgba8 = f.mode == PC_COLOR_MODE_RGB && f.sample == PC_SAMPLE_TYPE_U8 && f.alpha;
    size_t want;
    float dpx[PC_RASTER_MAX_CHANNELS];
    float dp[4];
    uint8_t dp8[4];
    float row[TS][4];
    span sp;
    pc_tile tc;
    pc_status st = region_len(r, 1, 4, &want, err);
    if (st != PC_OK) {
        return st;
    }
    if (len != want) {
        return pc_fail(err, PC_ERR_INVALID, "RGBA8 buffer holds %zu pixels, need %zu", len, want);
    }
    memset(row, 0, sizeof row);
    pc_surface_default_pixel(s, dpx);
    pc_to_rgba(f, dpx, pc_pixel_format_channels(f), dp);
    dp8[0] = q8(dp[0]);
    dp8[1] = q8(dp[1]);
    dp8[2] = q8(dp[2]);
    dp8[3] = q8(dp[3]);
    sp = span_of(r);
    FOR_SPAN(sp, tc)
    {
        pc_rect tr = pc_rect_intersect(pc_tile_rect(tc), r);
        const pc_raster_tile *t = get(s, tc);
        size_t w = pc_rect_width(tr);
        int32_t y;
        for (y = tr.y0; y < tr.y1; y++) {
            uint8_t (*dst)[4] = out + region_index(r, tr.x0, y);
            size_t i;
            if (t == NULL) {
                for (i = 0; i < w; i++) {
                    memcpy(dst[i], dp8, 4);
                }
            } else if (rgba8) {
                memcpy(dst, t->data + local_index(tc, tr.x0, y) * 4, w * 4);
            } else {
                st = pc_surface_read_rgba_into(s, pc_rect_make(tr.x0, y, tr.x1, y + 1), row, w, err);
                if (st != PC_OK) {
                    return st;
                }
                for (i = 0; i < w; i++) {
                    dst[i][0] = q8(row[i][0]);
                    dst[i][1] = q8(row[i][1]);
                    dst[i][2] = q8(row[i][2]);
                    dst[i][3] = q8(row[i][3]);
                }
            }
        }
    }
    return PC_OK;
}

pc_status pc_surface_write_region(pc_surface *s, pc_rect r, const float *data, size_t len, pc_error *err)
{
    size_t n = pc_pixel_format_channels(s->format);
    size_t want;
    span sp = span_of(r);
    pc_tile tc;
    pc_status st = region_len(r, n, sizeof(float), &want, err);
    if (st != PC_OK) {
        return st;
    }
    if (len != want) {
        return pc_fail(err, PC_ERR_INVALID, "region data length mismatch");
    }
    st = span_check(sp, err);
    if (st != PC_OK) {
        return st;
    }
    FOR_SPAN(sp, tc)
    {
        pc_rect tr = pc_rect_intersect(pc_tile_rect(tc), r);
        size_t slot;
        int32_t y;
        size_t span_n = (size_t)pc_rect_width(tr) * n;
        st = tile_mut_slot(s, tc, &slot, err);
        if (st != PC_OK) {
            return st;
        }
        for (y = tr.y0; y < tr.y1; y++) {
            const float *src = data + region_index(r, tr.x0, y) * n;
            size_t base = local_index(tc, tr.x0, y) * n;
            size_t i;
            for (i = 0; i < span_n; i++) {
                wr(s->slots[slot].tile->data, s->format.sample, base + i, src[i]);
            }
        }
    }
    return PC_OK;
}

pc_status pc_surface_take_tiles(pc_surface *s, pc_rect r, pc_tile_entry **out, size_t *n, pc_error *err)
{
    span sp = span_of(r);
    pc_tile_entry *e = NULL;
    pc_raster_tile *blank = NULL;
    size_t k = 0;
    pc_tile c;
    pc_status st = span_check(sp, err);
    if (st != PC_OK) {
        return st;
    }
    if (sp.n > 0) {
        e = pc_malloc_array((size_t)sp.n, sizeof *e);
        if (e == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory taking %llu tiles", (unsigned long long)sp.n);
        }
    }
    FOR_SPAN(sp, c)
    {
        pc_raster_tile *t = take(s, c);
        if (t == NULL) {
            if (blank == NULL) {
                blank = tile_filled(s->bpp, s->default_pixel);
                if (blank == NULL) {
                    /* Put back what was taken: removals never shrink the table, so this can't fail. */
                    (void)pc_surface_put_tiles(s, e, k, NULL);
                    free(e);
                    return pc_fail(err, PC_ERR_NOMEM, "out of memory for a tile");
                }
            }
            t = pc_raster_tile_retain(blank);
        }
        e[k].coord = c;
        e[k].tile = t;
        k++;
    }
    pc_raster_tile_release(blank);
    *out = e;
    *n = k;
    return PC_OK;
}

pc_status pc_surface_put_tiles(pc_surface *s, pc_tile_entry *entries, size_t n, pc_error *err)
{
    size_t i;
    pc_status st = PC_OK;
    for (i = 0; i < n; i++) {
        pc_raster_tile *t = entries[i].tile;
        entries[i].tile = NULL;
        if (st == PC_OK) {
            st = put(s, entries[i].coord, t, err); /* consumes t */
        } else {
            pc_raster_tile_release(t);
        }
    }
    return st;
}

pc_status pc_surface_solid_tile(const pc_surface *s, const float *px, size_t n, pc_raster_tile **out,
                                pc_error *err)
{
    uint8_t enc[PC_RASTER_MAX_PIXEL_BYTES];
    pc_raster_tile *t;
    memset(enc, 0, sizeof enc);
    pc_encode_pixel(s->format, px, n, enc);
    t = tile_filled(s->bpp, enc);
    if (t == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a tile");
    }
    *out = t;
    return PC_OK;
}

pc_status pc_surface_fill_rect(pc_surface *s, pc_rect r, const float *px, size_t n, pc_error *err)
{
    size_t ch = pc_pixel_format_channels(s->format);
    size_t bpp = s->bpp;
    uint8_t enc[PC_RASTER_MAX_PIXEL_BYTES];
    pc_raster_tile *full = NULL;
    span sp = span_of(r);
    pc_tile tc;
    pc_status st;
    if (n < ch) {
        return pc_fail(err, PC_ERR_INVALID, "fill pixel has %zu values, the format needs %zu", n, ch);
    }
    st = span_check(sp, err);
    if (st != PC_OK) {
        return st;
    }
    memset(enc, 0, sizeof enc);
    pc_encode_pixel(s->format, px, ch, enc);
    FOR_SPAN(sp, tc)
    {
        pc_rect tile_r = pc_tile_rect(tc);
        pc_rect tr = pc_rect_intersect(tile_r, r);
        size_t slot;
        int32_t y;
        if (pc_rect_eq(tr, tile_r)) {
            /* Fully covered tiles share one tile; copy-on-write splits it off when edited. */
            if (full == NULL) {
                full = tile_filled(bpp, enc);
                if (full == NULL) {
                    return pc_fail(err, PC_ERR_NOMEM, "out of memory for a tile");
                }
            }
            st = put(s, tc, pc_raster_tile_retain(full), err);
            if (st != PC_OK) {
                break;
            }
            continue;
        }
        st = tile_mut_slot(s, tc, &slot, err);
        if (st != PC_OK) {
            break;
        }
        for (y = tr.y0; y < tr.y1; y++) {
            uint8_t *row = s->slots[slot].tile->data + local_index(tc, tr.x0, y) * bpp;
            size_t w = pc_rect_width(tr);
            size_t i;
            for (i = 0; i < w; i++) {
                memcpy(row + i * bpp, enc, bpp);
            }
        }
    }
    pc_raster_tile_release(full);
    return st;
}

pc_status pc_surface_write_interleaved(pc_surface *s, pc_rect r, const uint8_t *bytes, size_t len,
                                       pc_error *err)
{
    size_t bpp = s->bpp;
    size_t want;
    span sp = span_of(r);
    pc_tile tc;
    pc_status st = region_len(r, bpp, 1, &want, err);
    if (st != PC_OK) {
        return st;
    }
    if (len != want) {
        return pc_fail(err, PC_ERR_INVALID, "interleaved length mismatch");
    }
    st = span_check(sp, err);
    if (st != PC_OK) {
        return st;
    }
    FOR_SPAN(sp, tc)
    {
        pc_rect tr = pc_rect_intersect(pc_tile_rect(tc), r);
        size_t span_b = (size_t)pc_rect_width(tr) * bpp;
        size_t slot;
        int32_t y;
        st = tile_mut_slot(s, tc, &slot, err);
        if (st != PC_OK) {
            return st;
        }
        for (y = tr.y0; y < tr.y1; y++) {
            memcpy(s->slots[slot].tile->data + local_index(tc, tr.x0, y) * bpp,
                   bytes + region_index(r, tr.x0, y) * bpp, span_b);
        }
    }
    return PC_OK;
}

pc_status pc_surface_from_interleaved(pc_pixel_format f, pc_rect r, const uint8_t *bytes, size_t len,
                                      pc_surface *out, pc_error *err)
{
    pc_surface s;
    pc_status st;
    pc_surface_init(&s, f);
    st = pc_surface_write_interleaved(&s, r, bytes, len, err);
    if (st != PC_OK) {
        pc_surface_fini(&s);
        return st;
    }
    *out = s;
    return PC_OK;
}

pc_status pc_surface_to_interleaved(const pc_surface *s, pc_rect r, uint8_t **out, size_t *len, pc_error *err)
{
    size_t bpp = s->bpp;
    size_t n;
    uint8_t *buf = NULL;
    span sp = span_of(r);
    pc_tile tc;
    pc_status st = region_len(r, bpp, 1, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n == 0) { /* empty r: nothing to read */
        *out = NULL;
        *len = 0;
        return PC_OK;
    }
    buf = malloc(n);
    if (buf == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for %zu bytes", n);
    }
    FOR_SPAN(sp, tc)
    {
        pc_rect tr = pc_rect_intersect(pc_tile_rect(tc), r);
        const pc_raster_tile *t = get(s, tc);
        size_t w = pc_rect_width(tr);
        int32_t y;
        for (y = tr.y0; y < tr.y1; y++) {
            uint8_t *dst = buf + region_index(r, tr.x0, y) * bpp;
            if (t != NULL) {
                memcpy(dst, t->data + local_index(tc, tr.x0, y) * bpp, w * bpp);
            } else {
                size_t i;
                for (i = 0; i < w; i++) {
                    memcpy(dst + i * bpp, s->default_pixel, bpp);
                }
            }
        }
    }
    *out = buf;
    *len = n;
    return PC_OK;
}

static int tile_cmp_v(const void *a, const void *b)
{
    return pc_tile_cmp(*(const pc_tile *)a, *(const pc_tile *)b);
}

/* Copies the part of source tile `st` (at sc) that lands in target tile `dst` (at dc). */
static void copy_moved(const pc_surface *s, const pc_raster_tile *st, pc_tile sc, pc_rect sr, pc_rect dr,
                       int32_t dx, int32_t dy, uint8_t *dst)
{
    size_t bpp = s->bpp;
    pc_rect src_r = pc_tile_rect(sc);
    pc_rect part = pc_rect_intersect(src_r, sr);
    size_t span_b = (size_t)pc_rect_width(part) * bpp;
    int32_t y;
    for (y = part.y0; y < part.y1; y++) {
        /* Offsets inside the tiles, computed wide like the reference: an offset near the i32
         * limits saturates `translate`, and such rows are skipped rather than misplaced. */
        int64_t sx = pc_i32_wrapping_sub(part.x0, pc_i32_wrapping_mul(sc.tx, TS));
        int64_t sy = pc_i32_wrapping_sub(y, pc_i32_wrapping_mul(sc.ty, TS));
        int64_t tx = (int64_t)part.x0 + dx - dr.x0;
        int64_t ty = (int64_t)y + dy - dr.y0;
        int64_t si = sy * TS + sx;
        int64_t di = ty * TS + tx;
        size_t so;
        size_t doff;
        if (si < 0 || di < 0) {
            continue;
        }
        so = (size_t)si * bpp;
        doff = (size_t)di * bpp;
        if (so <= st->len && span_b <= st->len - so && doff <= TILE_PIXELS * bpp &&
            span_b <= TILE_PIXELS * bpp - doff) {
            memcpy(dst + doff, st->data + so, span_b);
        }
    }
}

pc_status pc_surface_translated(const pc_surface *s, int32_t dx, int32_t dy, pc_rect content, pc_surface *out,
                                pc_error *err)
{
    pc_surface o;
    pc_tile_entry *src = NULL;
    size_t nsrc = 0;
    pc_tile *targets = NULL;
    size_t nt = 0;
    size_t i;
    pc_status st;
    pc_surface_init(&o, s->format);
    memcpy(o.default_pixel, s->default_pixel, sizeof o.default_pixel);
    /* Sources in (tx, ty) order: on coordinate collisions (saturation) the last one wins, as
     * when the reference collects into its BTreeMap. */
    st = pc_surface_tiles_sorted(s, &src, &nsrc, err);
    if (st != PC_OK) {
        return st;
    }
    if (dx % TS == 0 && dy % TS == 0) {
        int32_t tx = dx / TS;
        int32_t ty = dy / TS;
        for (i = 0; i < nsrc && st == PC_OK; i++) {
            pc_tile c;
            if (pc_rect_is_empty(pc_rect_intersect(pc_tile_rect(src[i].coord), content))) {
                continue;
            }
            c.tx = pc_i32_sat_add(src[i].coord.tx, tx);
            c.ty = pc_i32_sat_add(src[i].coord.ty, ty);
            st = put(&o, c, pc_raster_tile_retain(src[i].tile), err);
        }
        goto done;
    }
    {
        pc_rect moved = pc_rect_translate(content, dx, dy);
        size_t cap = 0;
        for (i = 0; i < nsrc; i++) {
            pc_rect sr = pc_tile_rect(src[i].coord);
            span sp;
            pc_tile c;
            if (pc_rect_is_empty(pc_rect_intersect(sr, content))) {
                continue;
            }
            sp = span_of(pc_rect_intersect(pc_rect_translate(sr, dx, dy), moved));
            if (sp.n > PC_SURFACE_MAX_TILE_SPAN) {
                st = span_check(sp, err);
                goto done;
            }
            FOR_SPAN(sp, c)
            {
                pc_tile *grown = pc_grow_array(targets, &cap, nt + 1, sizeof *targets);
                if (grown == NULL) {
                    st = pc_fail(err, PC_ERR_NOMEM, "out of memory moving a surface");
                    goto done;
                }
                targets = grown;
                targets[nt++] = c;
            }
        }
    }
    if (nt > 0) {
        qsort(targets, nt, sizeof *targets, tile_cmp_v);
    }
    for (i = 0; i < nt; i++) {
        pc_rect dr;
        pc_rect sr;
        span sp;
        pc_tile sc;
        pc_raster_tile *tile;
        if (i > 0 && same(targets[i], targets[i - 1])) {
            continue;
        }
        tile = tile_filled(s->bpp, s->default_pixel);
        if (tile == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a tile");
            goto done;
        }
        dr = pc_tile_rect(targets[i]);
        sr = pc_rect_translate(dr, pc_i32_sat_sub(0, dx), pc_i32_sat_sub(0, dy));
        sp = span_of(pc_rect_intersect(sr, content));
        if (sp.n <= PC_SURFACE_MAX_TILE_SPAN) {
            FOR_SPAN(sp, sc)
            {
                const pc_raster_tile *stile = get(s, sc);
                if (stile != NULL) {
                    copy_moved(s, stile, sc, sr, dr, dx, dy, tile->data);
                }
            }
        }
        st = put(&o, targets[i], tile, err);
        if (st != PC_OK) {
            goto done;
        }
    }
done:
    pc_tile_entries_free(src, nsrc);
    free(targets);
    if (st != PC_OK) {
        pc_surface_fini(&o);
        return st;
    }
    *out = o;
    return PC_OK;
}

pc_status pc_surface_convert(const pc_surface *s, pc_pixel_format to, pc_surface *out, pc_error *err)
{
    pc_surface o;
    float dp[PC_RASTER_MAX_CHANNELS];
    float cdp[PC_RASTER_MAX_CHANNELS];
    size_t from_n = pc_pixel_format_channels(s->format);
    size_t to_n = pc_pixel_format_channels(to);
    size_t i;
    pc_status st = PC_OK;
    size_t cn;
    pc_surface_default_pixel(s, dp);
    cn = pc_convert_pixel(s->format, to, dp, from_n, cdp);
    pc_surface_init_default(&o, to, cdp, cn);
    for (i = 0; i < s->cap && st == PC_OK; i++) {
        const pc_raster_tile *t = s->slots[i].tile;
        uint8_t *dst;
        size_t p;
        if (t == NULL) {
            continue;
        }
        st = pc_surface_tile_mut(&o, s->slots[i].coord, &dst, err);
        for (p = 0; st == PC_OK && p < TILE_PIXELS; p++) {
            float src[PC_RASTER_MAX_CHANNELS];
            float px[PC_RASTER_MAX_CHANNELS];
            size_t k;
            size_t m;
            for (k = 0; k < from_n; k++) {
                src[k] = rd(t->data, s->format.sample, p * from_n + k);
            }
            m = pc_convert_pixel(s->format, to, src, from_n, px);
            for (k = 0; k < m && k < to_n; k++) {
                wr(dst, to.sample, p * to_n + k, px[k]);
            }
        }
    }
    if (st != PC_OK) {
        pc_surface_fini(&o);
        return st;
    }
    *out = o;
    return PC_OK;
}

pc_status pc_surface_default_tile(const pc_surface *s, pc_raster_tile **out, pc_error *err)
{
    pc_raster_tile *t = tile_filled(s->bpp, s->default_pixel);
    if (t == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a tile");
    }
    *out = t;
    return PC_OK;
}

pc_status pc_surface_map_color(pc_surface *s, float (*f)(void *ctx, float v), void *ctx, pc_error *err)
{
    pc_pixel_format fmt = s->format;
    size_t color = pc_color_mode_channels(fmt.mode);
    size_t n = pc_pixel_format_channels(fmt);
    float dp[PC_RASTER_MAX_CHANNELS];
    pc_surface o;
    size_t i;
    size_t k;
    /* Work on a copy (tiles shared, copied on write) so a failure leaves s as it was. */
    pc_status st = pc_surface_clone(s, &o, err);
    if (st != PC_OK) {
        return st;
    }
    pc_decode_pixel(fmt, o.default_pixel, dp);
    for (k = 0; k < color && k < n; k++) {
        dp[k] = f(ctx, dp[k]);
    }
    pc_encode_pixel(fmt, dp, n, o.default_pixel);
    for (i = 0; i < o.cap; i++) {
        size_t slot;
        uint8_t *data;
        size_t p;
        if (o.slots[i].tile == NULL) {
            continue;
        }
        st = tile_mut_slot(&o, o.slots[i].coord, &slot, err);
        if (st != PC_OK) {
            pc_surface_fini(&o);
            return st;
        }
        data = o.slots[slot].tile->data;
        for (p = 0; p < TILE_PIXELS; p++) {
            for (k = 0; k < color && k < n; k++) {
                size_t at = p * n + k;
                wr(data, fmt.sample, at, f(ctx, rd(data, fmt.sample, at)));
            }
        }
    }
    pc_surface_fini(s);
    *s = o;
    return PC_OK;
}
