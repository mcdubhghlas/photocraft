/* The sparse stroke buffer and the incremental stroke renderer (render.h; the second half of
 * crates/paint/src/render.rs).
 *
 * The coverage map is a hash table of 64 x 64 tiles keyed by tile coordinates. The reference
 * uses a HashMap and sorts keys wherever order matters (compositing, dirty lists), so any table
 * gives the same results; dirty tiles are a flag per tile. Tile rectangles are computed with the
 * reference's release-build wrapping (`(tx + 1) * 64` wraps for the last tile column near
 * INT32_MAX, which makes that column's rectangle empty there too). */
#include "paint/render.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "paint/pmath.h"
#include "color/blend.h"
#include "paint/rng.h"
#include "raster/pixel.h"

#define CT PC_PAINT_COV_TILE
#define TILE_PX ((size_t)CT * CT)

#define TRY(expr)                                                                                            \
    do {                                                                                                     \
        pc_status st_ = (expr);                                                                              \
        if (st_ != PC_OK) {                                                                                  \
            return st_;                                                                                      \
        }                                                                                                    \
    } while (0)

static int32_t div64(int32_t x)
{
    int32_t q = x / CT;
    return (x % CT < 0) ? q - 1 : q;
}

/* The reference's Rect::new(tx * 64, ty * 64, (tx + 1) * 64, (ty + 1) * 64), wrapping. */
static pc_rect tile_rect(int32_t tx, int32_t ty)
{
    return pc_rect_make(pc_i32_wrapping_mul(tx, CT), pc_i32_wrapping_mul(ty, CT),
                        pc_i32_wrapping_mul(pc_i32_wrapping_add(tx, 1), CT),
                        pc_i32_wrapping_mul(pc_i32_wrapping_add(ty, 1), CT));
}

/* ---- coverage map ---- */

typedef struct cov_tile {
    int32_t tx, ty;
    bool dirty;
    float *cov; /* TILE_PX */
    float *col; /* TILE_PX * nc premultiplied native colour, or NULL */
} cov_tile;

typedef struct cov_map {
    cov_tile *tiles;
    size_t count, cap;
    size_t *slots; /* index + 1 into tiles, 0 = empty; nslots is 0 or a power of two */
    size_t nslots;
    size_t nc;
    pc_rect bounds;
} cov_map;

static size_t key_hash(int32_t tx, int32_t ty)
{
    return (size_t)pc_paint_mix64((uint64_t)(uint32_t)tx << 32 | (uint32_t)ty);
}

static void map_init(cov_map *m, size_t nc)
{
    memset(m, 0, sizeof *m);
    m->nc = nc;
}

static void map_fini(cov_map *m)
{
    for (size_t i = 0; i < m->count; i++) {
        free(m->tiles[i].cov);
        free(m->tiles[i].col);
    }
    free(m->tiles);
    free(m->slots);
    memset(m, 0, sizeof *m);
}

static cov_tile *map_find(const cov_map *m, int32_t tx, int32_t ty)
{
    if (m->nslots == 0) {
        return NULL;
    }
    for (size_t i = key_hash(tx, ty) & (m->nslots - 1);; i = (i + 1) & (m->nslots - 1)) {
        size_t s = m->slots[i];
        if (s == 0) {
            return NULL;
        }
        if (m->tiles[s - 1].tx == tx && m->tiles[s - 1].ty == ty) {
            return &m->tiles[s - 1];
        }
    }
}

static void slot_put(size_t *slots, size_t nslots, const cov_tile *t, size_t idx)
{
    size_t i = key_hash(t->tx, t->ty) & (nslots - 1);
    while (slots[i] != 0) {
        i = (i + 1) & (nslots - 1);
    }
    slots[i] = idx + 1;
}

/* Appends tile t (taking its buffers) to the map; on failure t's buffers are not taken. */
static pc_status map_insert(cov_map *m, const cov_tile *t, pc_error *err)
{
    if (m->count == m->cap) {
        cov_tile *nt = pc_grow_array(m->tiles, &m->cap, m->count + 1, sizeof *nt);
        if (nt == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for the stroke buffer");
        }
        m->tiles = nt;
    }
    if ((m->count + 1) * 2 > m->nslots) {
        size_t ns = m->nslots == 0 ? 64 : m->nslots * 2;
        size_t *s = pc_calloc_array(ns, sizeof *s);
        if (s == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for the stroke buffer");
        }
        for (size_t i = 0; i < m->count; i++) {
            slot_put(s, ns, &m->tiles[i], i);
        }
        free(m->slots);
        m->slots = s;
        m->nslots = ns;
    }
    m->tiles[m->count] = *t;
    slot_put(m->slots, m->nslots, t, m->count);
    m->count++;
    return PC_OK;
}

static pc_status tile_clone(const cov_tile *src, size_t nc, cov_tile *out, pc_error *err)
{
    cov_tile t = *src;
    t.cov = pc_malloc_array(TILE_PX, sizeof(float));
    t.col = NULL;
    if (t.cov != NULL && src->col != NULL) {
        t.col = pc_malloc_array(TILE_PX * nc, sizeof(float));
    }
    if (t.cov == NULL || (src->col != NULL && t.col == NULL)) {
        free(t.cov);
        free(t.col);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the stroke buffer");
    }
    memcpy(t.cov, src->cov, TILE_PX * sizeof(float));
    if (src->col != NULL) {
        memcpy(t.col, src->col, TILE_PX * nc * sizeof(float));
    }
    *out = t;
    return PC_OK;
}

/* Inserts a copy of src (marked dirty as given). */
static pc_status map_insert_copy(cov_map *m, const cov_tile *src, bool dirty, pc_error *err)
{
    cov_tile t;
    pc_status st = tile_clone(src, m->nc, &t, err);
    if (st != PC_OK) {
        return st;
    }
    t.dirty = dirty;
    st = map_insert(m, &t, err);
    if (st != PC_OK) {
        free(t.cov);
        free(t.col);
    }
    return st;
}

static pc_status map_clone(const cov_map *src, cov_map *out, pc_error *err)
{
    cov_map m;
    map_init(&m, src->nc);
    m.bounds = src->bounds;
    for (size_t i = 0; i < src->count; i++) {
        pc_status st = map_insert_copy(&m, &src->tiles[i], src->tiles[i].dirty, err);
        if (st != PC_OK) {
            map_fini(&m);
            return st;
        }
    }
    *out = m;
    return PC_OK;
}

static float map_get(const cov_map *m, int32_t x, int32_t y)
{
    int32_t tx = div64(x), ty = div64(y);
    const cov_tile *t = map_find(m, tx, ty);
    if (t == NULL) {
        return 0.0f;
    }
    return t->cov[(size_t)(y - ty * CT) * CT + (size_t)(x - tx * CT)];
}

static int cmp_key(const void *a, const void *b)
{
    const cov_tile *const *p = a, *const *q = b;
    if ((*p)->tx != (*q)->tx) {
        return (*p)->tx < (*q)->tx ? -1 : 1;
    }
    return (*p)->ty < (*q)->ty ? -1 : ((*p)->ty > (*q)->ty);
}

/* The dirty tiles (all tiles when `all`) in (tx, ty) order, clearing their flags. *out is a
 * malloc'd array of pointers into m (valid until m changes), NULL when empty. */
static pc_status map_take_keys(cov_map *m, bool all, cov_tile ***out, size_t *n, pc_error *err)
{
    size_t k = 0;
    cov_tile **v = NULL;
    for (size_t i = 0; i < m->count; i++) {
        k += all || m->tiles[i].dirty;
    }
    if (k > 0) {
        v = pc_malloc_array(k, sizeof *v);
        if (v == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for the stroke buffer");
        }
        k = 0;
        for (size_t i = 0; i < m->count; i++) {
            if (all || m->tiles[i].dirty) {
                v[k++] = &m->tiles[i];
            }
        }
        qsort(v, k, sizeof *v, cmp_key);
    }
    for (size_t i = 0; i < m->count; i++) {
        m->tiles[i].dirty = false;
    }
    *out = v;
    *n = k;
    return PC_OK;
}

static void map_clear_dirty(cov_map *m)
{
    for (size_t i = 0; i < m->count; i++) {
        m->tiles[i].dirty = false;
    }
}

/* Accumulate dab values over rect: flow builds up towards the dab's opacity ceiling
 * (c <- c + v * (ceil - c)), or with max (wet edges). color = native colour for per-dab colour. */
static pc_status map_accumulate(cov_map *m, pc_rect rect, const float *vals, float ceil, bool use_max,
                                const float *color, pc_error *err)
{
    size_t w, nc = m->nc;
    bool want_col = color != NULL && nc > 0;
    int32_t t0x, t0y, t1x, t1y;
    if (pc_rect_is_empty(rect)) {
        return PC_OK;
    }
    m->bounds = pc_rect_union(m->bounds, rect);
    w = pc_rect_width(rect);
    t0x = div64(rect.x0);
    t0y = div64(rect.y0);
    t1x = div64(rect.x1 - 1);
    t1y = div64(rect.y1 - 1);
    for (int64_t ty = t0y; ty <= t1y; ty++) {
        for (int64_t tx = t0x; tx <= t1x; tx++) {
            pc_rect tr = pc_rect_intersect(tile_rect((int32_t)tx, (int32_t)ty), rect);
            bool any = false;
            cov_tile *tile;
            if (pc_rect_is_empty(tr)) {
                continue;
            }
            /* Skip tiles this dab doesn't touch. */
            for (int32_t y = tr.y0; y < tr.y1 && !any; y++) {
                const float *row = vals + (size_t)(y - rect.y0) * w;
                for (int32_t x = tr.x0; x < tr.x1; x++) {
                    if (row[x - rect.x0] > 0.0f) {
                        any = true;
                        break;
                    }
                }
            }
            if (!any) {
                continue;
            }
            tile = map_find(m, (int32_t)tx, (int32_t)ty);
            if (tile == NULL) {
                cov_tile t = {(int32_t)tx, (int32_t)ty, false, NULL, NULL};
                pc_status st;
                t.cov = pc_calloc_array(TILE_PX, sizeof(float));
                if (t.cov == NULL) {
                    return pc_fail(err, PC_ERR_NOMEM, "out of memory for the stroke buffer");
                }
                st = map_insert(m, &t, err);
                if (st != PC_OK) {
                    free(t.cov);
                    return st;
                }
                tile = &m->tiles[m->count - 1];
            }
            if (want_col && tile->col == NULL) {
                tile->col = pc_calloc_array(TILE_PX * nc, sizeof(float));
                if (tile->col == NULL) {
                    return pc_fail(err, PC_ERR_NOMEM, "out of memory for the stroke buffer");
                }
            }
            tile->dirty = true;
            for (int32_t y = tr.y0; y < tr.y1; y++) {
                const float *row = vals + (size_t)(y - rect.y0) * w;
                size_t trow = (size_t)(y - (int32_t)ty * CT) * CT;
                for (int32_t x = tr.x0; x < tr.x1; x++) {
                    float v = row[x - rect.x0], c, nv;
                    size_t ti;
                    if (v <= 0.0f) {
                        continue;
                    }
                    ti = trow + (size_t)(x - (int32_t)tx * CT);
                    c = tile->cov[ti];
                    if (use_max) {
                        nv = pc_rs_maxf(c, v * ceil);
                    } else if (ceil > c) {
                        nv = c + v * (ceil - c);
                    } else {
                        nv = c;
                    }
                    if (nv <= c) {
                        continue;
                    }
                    if (want_col) {
                        float k = c < 1.0f ? (nv - c) / (1.0f - c) : 0.0f;
                        float *p = &tile->col[ti * nc];
                        for (size_t i = 0; i < nc; i++) {
                            p[i] = color[i] * k + p[i] * (1.0f - k);
                        }
                    }
                    tile->cov[ti] = nv;
                }
            }
        }
    }
    return PC_OK;
}

/* Union two passes of the same stroke without applying opacity twice where they overlap: the
 * stronger coverage wins, with its per-dab colour when present. */
static pc_status map_union_max(cov_map *m, const cov_map *other, pc_error *err)
{
    m->bounds = pc_rect_union(m->bounds, other->bounds);
    for (size_t k = 0; k < other->count; k++) {
        const cov_tile *src = &other->tiles[k];
        cov_tile *dst = map_find(m, src->tx, src->ty);
        if (dst == NULL) {
            TRY(map_insert_copy(m, src, true, err));
            continue;
        }
        for (size_t i = 0; i < TILE_PX; i++) {
            if (src->cov[i] > dst->cov[i]) {
                dst->cov[i] = src->cov[i];
                if (dst->col != NULL && src->col != NULL && m->nc == other->nc) {
                    memcpy(&dst->col[i * m->nc], &src->col[i * m->nc], m->nc * sizeof(float));
                }
            }
        }
        dst->dirty = true;
    }
    return PC_OK;
}

/* ---- renderer ---- */

struct pc_paint_renderer {
    pc_paint_context ctx;
    pc_paint_dab_generator gen;
    cov_map cov;
    bool has_dual;
    cov_map dual;
    bool has_fmt;
    pc_pixel_format fmt;
    bool per_dab_color;
    size_t dabs_done;
    pc_paint_fbuf scratch;
    pc_paint_dab_vec dab_buf, dual_buf;
    bool recording;
    pc_paint_dab_vec all;
};

pc_status pc_paint_renderer_new(const pc_paint_brush *brush, const pc_pixel_format *fmt, float zoom,
                                pc_paint_renderer **out, pc_error *err)
{
    pc_paint_renderer *r = calloc(1, sizeof *r);
    size_t nc;
    pc_status st;
    if (r == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a stroke renderer");
    }
    st = pc_paint_context_init(&r->ctx, brush, err);
    if (st != PC_OK) {
        free(r);
        return st;
    }
    r->per_dab_color = brush->color_dynamics.enabled && !brush->erase && fmt != NULL;
    nc = r->per_dab_color ? pc_color_mode_channels(fmt->mode) : 0;
    pc_paint_dab_generator_init(&r->gen, brush, zoom);
    map_init(&r->cov, nc);
    r->has_dual = brush->dual_brush.enabled;
    map_init(&r->dual, 0);
    if (fmt != NULL) {
        r->has_fmt = true;
        r->fmt = *fmt;
    }
    *out = r;
    return PC_OK;
}

void pc_paint_renderer_free(pc_paint_renderer *r)
{
    if (r == NULL) {
        return;
    }
    pc_paint_context_fini(&r->ctx);
    pc_paint_dab_generator_fini(&r->gen);
    map_fini(&r->cov);
    map_fini(&r->dual);
    pc_paint_fbuf_fini(&r->scratch);
    pc_paint_dab_vec_fini(&r->dab_buf);
    pc_paint_dab_vec_fini(&r->dual_buf);
    pc_paint_dab_vec_fini(&r->all);
    free(r);
}

void pc_paint_renderer_record_dabs(pc_paint_renderer *r)
{
    r->recording = true;
    r->all.len = 0;
}

const pc_paint_dab *pc_paint_renderer_dabs(const pc_paint_renderer *r, size_t *n)
{
    *n = r->recording ? r->all.len : 0;
    return r->recording ? r->all.v : NULL;
}

size_t pc_paint_renderer_dab_count(const pc_paint_renderer *r)
{
    return r->dabs_done;
}

pc_rect pc_paint_renderer_bounds(const pc_paint_renderer *r)
{
    return r->cov.bounds;
}

const pc_paint_context *pc_paint_renderer_context(const pc_paint_renderer *r)
{
    return &r->ctx;
}

static pc_status raster_pending(pc_paint_renderer *r, pc_error *err)
{
    bool wet = r->ctx.brush.wet_edges;
    size_t nc = r->per_dab_color && r->has_fmt ? pc_color_mode_channels(r->fmt.mode) : 0;
    for (size_t i = 0; i < r->dab_buf.len; i++) {
        const pc_paint_dab *d = &r->dab_buf.v[i];
        pc_rect rect = pc_paint_context_dab_rect(&r->ctx, d, false);
        float native[PC_RASTER_MAX_CHANNELS] = {0};
        const float *col = NULL;
        TRY(pc_paint_context_rasterize(&r->ctx, d, false, rect, &r->scratch, err));
        if (nc > 0) {
            (void)pc_from_rgba(r->fmt, d->color, native);
            col = native;
        }
        TRY(map_accumulate(&r->cov, rect, r->scratch.v, d->opacity, wet, col, err));
        /* Bounds track the full dab rectangle (even fully transparent parts), like the damage. */
        r->cov.bounds = pc_rect_union(r->cov.bounds, rect);
    }
    if (r->has_dual) {
        for (size_t i = 0; i < r->dual_buf.len; i++) {
            const pc_paint_dab *d = &r->dual_buf.v[i];
            pc_rect rect = pc_paint_context_dab_rect(&r->ctx, d, true);
            TRY(pc_paint_context_rasterize(&r->ctx, d, true, rect, &r->scratch, err));
            TRY(map_accumulate(&r->dual, rect, r->scratch.v, 1.0f, false, NULL, err));
        }
    }
    r->dabs_done += r->dab_buf.len;
    if (r->recording) {
        for (size_t i = 0; i < r->dab_buf.len; i++) {
            TRY(pc_paint_dab_vec_push(&r->all, &r->dab_buf.v[i], err));
        }
    }
    r->dab_buf.len = 0;
    r->dual_buf.len = 0;
    return PC_OK;
}

pc_status pc_paint_renderer_push(pc_paint_renderer *r, const pc_paint_point *pts, size_t n, pc_error *err)
{
    TRY(pc_paint_dab_generator_push(&r->gen, pts, n, &r->dab_buf, &r->dual_buf, err));
    return raster_pending(r, err);
}

pc_status pc_paint_renderer_finish(pc_paint_renderer *r, pc_error *err)
{
    TRY(pc_paint_dab_generator_finish(&r->gen, &r->dab_buf, &r->dual_buf, err));
    return raster_pending(r, err);
}

/* Copies of m's tiles at the given keys (deduplicated), keeping m's bounds. */
static pc_status map_subset(const cov_map *m, const pc_tile *keys, size_t nkeys, cov_map *out, pc_error *err)
{
    cov_map s;
    map_init(&s, m->nc);
    s.bounds = m->bounds;
    for (size_t i = 0; i < nkeys; i++) {
        const cov_tile *t = map_find(m, keys[i].tx, keys[i].ty);
        pc_status st;
        if (t == NULL || map_find(&s, t->tx, t->ty) != NULL) {
            continue;
        }
        st = map_insert_copy(&s, t, false, err);
        if (st != PC_OK) {
            map_fini(&s);
            return st;
        }
    }
    *out = s;
    return PC_OK;
}

/* Appends the tile keys rect r touches. */
static pc_status push_keys(pc_rect r, pc_tile **keys, size_t *n, size_t *cap, pc_error *err)
{
    if (pc_rect_is_empty(r)) {
        return PC_OK;
    }
    for (int64_t ty = div64(r.y0); ty <= div64(r.y1 - 1); ty++) {
        for (int64_t tx = div64(r.x0); tx <= div64(r.x1 - 1); tx++) {
            if (*n == *cap) {
                pc_tile *nk = pc_grow_array(*keys, cap, *n + 1, sizeof *nk);
                if (nk == NULL) {
                    return pc_fail(err, PC_ERR_NOMEM, "out of memory for a tail preview");
                }
                *keys = nk;
            }
            (*keys)[(*n)++] = (pc_tile){(int32_t)tx, (int32_t)ty};
        }
    }
    return PC_OK;
}

pc_status pc_paint_renderer_tail_preview(const pc_paint_renderer *r, pc_paint_renderer **out, pc_error *err)
{
    pc_paint_renderer *t = calloc(1, sizeof *t);
    pc_tile *keys = NULL;
    size_t nkeys = 0, kcap = 0;
    pc_status st;
    if (t == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a tail preview");
    }
    pc_paint_dab_generator_copy(&t->gen, &r->gen);
    pc_paint_context_copy(&t->ctx, &r->ctx);
    map_init(&t->cov, r->cov.nc);
    map_init(&t->dual, 0);
    st = pc_paint_dab_generator_finish(&t->gen, &t->dab_buf, &t->dual_buf, err);
    if (st != PC_OK) {
        goto fail;
    }
    if (t->dab_buf.len == 0 && t->dual_buf.len == 0) {
        pc_paint_renderer_free(t);
        *out = NULL;
        return PC_OK;
    }
    for (size_t i = 0; i < t->dab_buf.len && st == PC_OK; i++) {
        st =
            push_keys(pc_paint_context_dab_rect(&r->ctx, &t->dab_buf.v[i], false), &keys, &nkeys, &kcap, err);
    }
    for (size_t i = 0; i < t->dual_buf.len && st == PC_OK; i++) {
        st =
            push_keys(pc_paint_context_dab_rect(&r->ctx, &t->dual_buf.v[i], true), &keys, &nkeys, &kcap, err);
    }
    if (st == PC_OK) {
        map_fini(&t->cov);
        st = map_subset(&r->cov, keys, nkeys, &t->cov, err);
    }
    if (st == PC_OK && r->has_dual) {
        map_fini(&t->dual);
        st = map_subset(&r->dual, keys, nkeys, &t->dual, err);
    }
    free(keys);
    if (st != PC_OK) {
        goto fail;
    }
    t->has_dual = r->has_dual;
    t->has_fmt = r->has_fmt;
    t->fmt = r->fmt;
    t->per_dab_color = r->per_dab_color;
    t->dabs_done = r->dabs_done;
    st = raster_pending(t, err);
    if (st != PC_OK) {
        goto fail;
    }
    /* Tiles whose coverage the tail doesn't change still composite the same: redraw them all. */
    for (size_t i = 0; i < t->cov.count; i++) {
        t->cov.tiles[i].dirty = true;
    }
    *out = t;
    return PC_OK;
fail:
    pc_paint_renderer_free(t);
    return st;
}

void pc_paint_renderer_mark_dirty(pc_paint_renderer *r, pc_rect rect)
{
    if (pc_rect_is_empty(rect)) {
        return;
    }
    /* Every existing tile whose key lies in rect's tile range (iterating the tiles, not the
     * range, which a huge rect makes enormous). */
    for (size_t i = 0; i < r->cov.count; i++) {
        cov_tile *t = &r->cov.tiles[i];
        if (t->tx >= div64(rect.x0) && t->tx <= div64(rect.x1 - 1) && t->ty >= div64(rect.y0) &&
            t->ty <= div64(rect.y1 - 1)) {
            t->dirty = true;
        }
    }
}

pc_rect pc_paint_renderer_take_dirty_rect(pc_paint_renderer *r)
{
    pc_rect acc = PC_RECT_EMPTY;
    for (size_t i = 0; i < r->cov.count; i++) {
        if (r->cov.tiles[i].dirty) {
            acc = pc_rect_union(acc, tile_rect(r->cov.tiles[i].tx, r->cov.tiles[i].ty));
            r->cov.tiles[i].dirty = false;
        }
    }
    map_clear_dirty(&r->dual);
    return pc_rect_intersect(acc, r->cov.bounds);
}

float pc_paint_renderer_coverage_at(const pc_paint_renderer *r, int32_t x, int32_t y)
{
    float c = map_get(&r->cov, x, y);
    if (c <= 0.0f) {
        return 0.0f;
    }
    return pc_paint_context_stroke_mask(&r->ctx, c, r->has_dual, r->has_dual ? map_get(&r->dual, x, y) : 0.0f,
                                        x, y);
}

static pc_status alloc_floats(pc_rect rect, float **out, size_t *n, pc_error *err)
{
    size_t len;
    if (!pc_size_mul(pc_rect_width(rect), pc_rect_height(rect), &len) || len > PC_MAX_ALLOC / sizeof(float)) {
        return pc_fail(err, PC_ERR_LIMIT, "coverage area too large");
    }
    *out = NULL;
    *n = len;
    if (len > 0) {
        *out = pc_calloc_array(len, sizeof(float));
        if (*out == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for stroke coverage");
        }
    }
    return PC_OK;
}

pc_status pc_paint_renderer_coverage_in(const pc_paint_renderer *r, pc_rect rect, float **out, size_t *n,
                                        pc_error *err)
{
    float *v = NULL;
    size_t len = 0, i = 0;
    TRY(alloc_floats(rect, &v, &len, err));
    for (int64_t y = rect.y0; len > 0 && y < rect.y1; y++) {
        for (int64_t x = rect.x0; x < rect.x1; x++) {
            v[i++] = pc_paint_renderer_coverage_at(r, (int32_t)x, (int32_t)y);
        }
    }
    *out = v;
    *n = len;
    return PC_OK;
}

pc_status pc_paint_renderer_dense_coverage(const pc_paint_renderer *r, pc_rect *rect, float **out, size_t *n,
                                           pc_error *err)
{
    pc_rect b = r->cov.bounds;
    size_t w = pc_rect_width(b), len = 0;
    float *v = NULL;
    TRY(alloc_floats(b, &v, &len, err));
    for (size_t k = 0; k < r->cov.count && len > 0; k++) {
        const cov_tile *t = &r->cov.tiles[k];
        pc_rect tr = pc_rect_intersect(tile_rect(t->tx, t->ty), b);
        for (int32_t y = tr.y0; y < tr.y1; y++) {
            for (int32_t x = tr.x0; x < tr.x1; x++) {
                float c = t->cov[(size_t)(y - t->ty * CT) * CT + (size_t)(x - t->tx * CT)];
                if (c > 0.0f) {
                    v[(size_t)(y - b.y0) * w + (size_t)(x - b.x0)] = pc_paint_context_stroke_mask(
                        &r->ctx, c, r->has_dual, r->has_dual ? map_get(&r->dual, x, y) : 0.0f, x, y);
                }
            }
        }
    }
    *rect = b;
    *out = v;
    *n = len;
    return PC_OK;
}

/* Reusable float buffer of exactly n values. */
static pc_status fbuf_exact(pc_paint_fbuf *b, size_t n, pc_error *err)
{
    return pc_paint_fbuf_zeroed(b, n, err);
}

/* The compositing core shared by composite and composite_union. */
static pc_status composite_maps(const pc_paint_context *ctx, cov_map *cov, bool has_dual, cov_map *dual,
                                bool per_dab_color, const pc_surface *pre, pc_surface *target,
                                const pc_surface *selection, bool lock, bool all, pc_rect *damage,
                                pc_error *err)
{
    const pc_paint_brush *b = &ctx->brush;
    pc_pixel_format fmt = pc_surface_format(target);
    size_t n = pc_pixel_format_channels(fmt), nc = pc_color_mode_channels(fmt.mode), a_idx = 0;
    bool has_a = pc_paint_alpha_index(fmt, &a_idx);
    float opacity = pc_clampf(b->opacity, 0.0f, 1.0f);
    float src[8] = {0};
    pc_rect bounds = cov->bounds, dmg = PC_RECT_EMPTY;
    cov_tile **keys = NULL;
    size_t nkeys = 0, sc = selection != NULL ? pc_surface_channels(selection) : 0;
    pc_paint_fbuf region = {0}, sel = {0};
    pc_status st;
    if (pc_surface_channels(pre) != n) {
        return pc_fail(err, PC_ERR_INVALID, "pre-stroke surface format differs from the target");
    }
    st = map_take_keys(cov, all, &keys, &nkeys, err);
    if (st != PC_OK) {
        return st;
    }
    if (has_dual) {
        map_clear_dirty(dual);
    }
    (void)pc_from_rgba(fmt, b->color, src);
    if (has_a) {
        src[a_idx] = b->color[3];
    }
    for (size_t k = 0; k < nkeys && st == PC_OK; k++) {
        const cov_tile *tile = keys[k];
        pc_rect tr = pc_rect_intersect(tile_rect(tile->tx, tile->ty), bounds);
        size_t w, area;
        if (pc_rect_is_empty(tr)) {
            continue;
        }
        w = pc_rect_width(tr);
        area = w * pc_rect_height(tr);
        st = fbuf_exact(&region, area * n, err);
        if (st == PC_OK) {
            st = pc_surface_read_region_into(pre, tr, region.v, region.len, err);
        }
        if (st == PC_OK && selection != NULL) {
            st = fbuf_exact(&sel, area * sc, err);
            if (st == PC_OK) {
                st = pc_surface_read_region_into(selection, tr, sel.v, sel.len, err);
            }
        }
        if (st != PC_OK) {
            break;
        }
        for (int32_t y = tr.y0; y < tr.y1; y++) {
            for (int32_t x = tr.x0; x < tr.x1; x++) {
                size_t ti = (size_t)(y - tile->ty * CT) * CT + (size_t)(x - tile->tx * CT);
                size_t i = (size_t)(y - tr.y0) * w + (size_t)(x - tr.x0);
                float c = tile->cov[ti], m, s, kk;
                float *px;
                if (c <= 0.0f) {
                    continue;
                }
                m = pc_paint_context_stroke_mask(ctx, c, has_dual, has_dual ? map_get(dual, x, y) : 0.0f, x,
                                                 y);
                s = selection != NULL ? (sc > 0 ? sel.v[i * sc] : 0.0f) : 1.0f;
                kk = pc_paint_minf_k(m * opacity * s, 1.0f);
                if (kk <= 0.0f) {
                    continue;
                }
                if (b->mode == PC_BLEND_DISSOLVE) {
                    kk = pc_paint_hash2(x, y, b->seed) < kk ? 1.0f : 0.0f;
                    if (kk == 0.0f) {
                        continue;
                    }
                }
                px = &region.v[i * n];
                if (b->erase) {
                    if (has_a && !lock) {
                        px[a_idx] *= 1.0f - kk;
                    }
                    continue;
                }
                if (lock && has_a && px[a_idx] <= 0.0f) {
                    continue;
                }
                if (per_dab_color && tile->col != NULL) {
                    const float *p = &tile->col[ti * nc];
                    for (size_t ch = 0; ch < nc; ch++) {
                        src[ch] = p[ch] / c;
                    }
                }
                if (b->mode == PC_BLEND_NORMAL || b->mode == PC_BLEND_DISSOLVE) {
                    pc_paint_over_native(fmt, px, src, kk, lock);
                } else {
                    float d4[4], s4[4], o[4], enc[PC_RASTER_MAX_CHANNELS] = {0};
                    pc_to_rgba(fmt, px, n, d4);
                    pc_to_rgba(fmt, src, n, s4);
                    pc_blend_composite(b->mode, d4, s4, kk, o);
                    if (lock) {
                        o[3] = d4[3];
                    }
                    (void)pc_from_rgba(fmt, o, enc);
                    memcpy(px, enc, n * sizeof(float));
                }
            }
        }
        st = pc_surface_write_region(target, tr, region.v, region.len, err);
        if (st == PC_OK) {
            dmg = pc_rect_union(dmg, tr);
        }
    }
    free(keys);
    pc_paint_fbuf_fini(&region);
    pc_paint_fbuf_fini(&sel);
    if (st == PC_OK) {
        *damage = all ? bounds : dmg;
    }
    return st;
}

pc_status pc_paint_renderer_composite(pc_paint_renderer *r, const pc_surface *pre, pc_surface *target,
                                      const pc_surface *selection, bool lock_transparency, bool all,
                                      pc_rect *damage, pc_error *err)
{
    return composite_maps(&r->ctx, &r->cov, r->has_dual, &r->dual, r->per_dab_color, pre, target, selection,
                          lock_transparency, all, damage, err);
}

pc_status pc_paint_renderer_composite_union(const pc_paint_renderer *r, const pc_paint_renderer *other,
                                            const pc_surface *pre, pc_surface *target,
                                            const pc_surface *selection, bool lock_transparency,
                                            pc_rect *damage, pc_error *err)
{
    cov_map cov, dual;
    pc_status st = map_clone(&r->cov, &cov, err);
    if (st != PC_OK) {
        return st;
    }
    map_init(&dual, 0);
    if (r->has_dual) {
        st = map_clone(&r->dual, &dual, err);
    }
    if (st == PC_OK) {
        st = map_union_max(&cov, &other->cov, err);
    }
    if (st == PC_OK && r->has_dual && other->has_dual) {
        st = map_union_max(&dual, &other->dual, err);
    }
    if (st == PC_OK) {
        st = composite_maps(&r->ctx, &cov, r->has_dual, &dual, r->per_dab_color, pre, target, selection,
                            lock_transparency, true, damage, err);
    }
    map_fini(&cov);
    map_fini(&dual);
    return st;
}

pc_status pc_paint_render_stroke(pc_surface *target, const pc_paint_brush *brush, const pc_paint_point *pts,
                                 size_t n, const pc_surface *selection, bool lock_transparency, float zoom,
                                 pc_rect *damage, pc_error *err)
{
    pc_surface pre;
    pc_paint_renderer *r = NULL;
    pc_pixel_format fmt = pc_surface_format(target);
    pc_status st;
    if (n == 0) {
        *damage = PC_RECT_EMPTY;
        return PC_OK;
    }
    st = pc_surface_clone(target, &pre, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_paint_renderer_new(brush, &fmt, zoom, &r, err);
    if (st == PC_OK) {
        st = pc_paint_renderer_push(r, pts, n, err);
    }
    if (st == PC_OK) {
        st = pc_paint_renderer_finish(r, err);
    }
    if (st == PC_OK) {
        st = pc_paint_renderer_composite(r, &pre, target, selection, lock_transparency, true, damage, err);
    }
    pc_paint_renderer_free(r);
    pc_surface_fini(&pre);
    return st;
}

pc_status pc_paint_apply_stroke(pc_surface *target, const pc_paint_stroke *stroke,
                                const pc_surface *selection, bool lock_transparency, pc_rect *damage,
                                pc_error *err)
{
    return pc_paint_render_stroke(target, &stroke->brush, stroke->points, stroke->count, selection,
                                  lock_transparency, 1.0f, damage, err);
}
