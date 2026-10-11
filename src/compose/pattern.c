/* Pattern paint (port of crates/compose/src/pattern.rs): prepared patterns, tiles, placements
 * and pattern fills. */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "compose/internal.h"

/* At most this many patterns are kept per render call (MAX_PREPARED_PATTERNS). */
#define MAX_PREPARED 64

struct pc_compose_pattern_tile {
    atomic_size_t refs;
    size_t w;
    size_t h;
    float (*px)[4]; /* premultiplied RGBA, w * h */
};

/* One prepared pattern (the reference's Arc<OnceLock<Option<Arc<Tile>>>>): converted once under
 * its own lock, so threads asking for different patterns don't wait on each other. */
typedef struct slot {
    const pc_pattern *key;
    pthread_mutex_t lock;
    bool done;
    pc_compose_pattern_tile *tile; /* NULL for an empty pattern */
} slot;

/* The mutable part, behind a pointer: the object is shared through const pointers by the tiles
 * of one render, and this cache is internally synchronised. */
typedef struct slots {
    pthread_mutex_t lock; /* guards everything below */
    slot *slots[MAX_PREPARED];
    size_t nslots;
    size_t reserved;
} slots;

struct pc_compose_patterns {
    const pc_pattern *patterns;
    size_t n;
    size_t budget;
    slots *cache;
};

static pc_status nomem(pc_error *err)
{
    return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
}

/* ---- tiles ---- */

pc_status pc_compose_pattern_tile_new(const pc_pattern *p, pc_compose_pattern_tile **out, pc_error *err)
{
    pc_compose_pattern_tile *t;
    uint64_t n64;
    size_t n;
    size_t i;
    pc_status st;
    if (pc_pattern_is_empty(p)) {
        *out = NULL;
        return PC_OK;
    }
    n64 = (uint64_t)p->width * (uint64_t)p->height;
    if (n64 > PC_COMPOSE_MAX_PIXELS) {
        return pc_fail(err, PC_ERR_LIMIT, "compose: a %ux%u pattern is too large", p->width, p->height);
    }
    n = (size_t)n64;
    t = malloc(sizeof *t);
    if (t == NULL) {
        return nomem(err);
    }
    t->px = pc_calloc_array(n, sizeof *t->px);
    if (t->px == NULL) {
        free(t);
        return nomem(err);
    }
    st = pc_surface_read_rgba_into(&p->surface, pc_pattern_rect(p), t->px, n, err);
    if (st != PC_OK) {
        free(t->px);
        free(t);
        return st;
    }
    for (i = 0; i < n; i++) {
        float *q = t->px[i];
        q[0] *= q[3];
        q[1] *= q[3];
        q[2] *= q[3];
    }
    t->w = p->width;
    t->h = p->height;
    atomic_init(&t->refs, 1);
    *out = t;
    return PC_OK;
}

pc_compose_pattern_tile *pc_compose_pattern_tile_retain(pc_compose_pattern_tile *t)
{
    if (t != NULL) {
        atomic_fetch_add_explicit(&t->refs, 1, memory_order_relaxed);
    }
    return t;
}

void pc_compose_pattern_tile_release(pc_compose_pattern_tile *t)
{
    if (t != NULL && atomic_fetch_sub_explicit(&t->refs, 1, memory_order_acq_rel) == 1) {
        free(t->px);
        free(t);
    }
}

static const float *tile_at(const pc_compose_pattern_tile *t, int64_t x, int64_t y)
{
    /* rem_euclid by a positive modulus (w, h <= 2^28). */
    int64_t w = (int64_t)t->w;
    int64_t h = (int64_t)t->h;
    int64_t xi = x % w;
    int64_t yi = y % h;
    if (xi < 0) {
        xi += w;
    }
    if (yi < 0) {
        yi += h;
    }
    return t->px[(size_t)yi * t->w + (size_t)xi];
}

/* Rust's i64 `+` on values that may be saturated casts: wrapping (release semantics). */
static int64_t add_wrap(int64_t a, int64_t b)
{
    return (int64_t)((uint64_t)a + (uint64_t)b);
}

void pc_compose_pattern_tile_sample(const pc_compose_pattern_tile *t, double u, double v, float out[4])
{
    double uu = u - 0.5;
    double vv = v - 0.5;
    double fx0 = floor(uu);
    double fy0 = floor(vv);
    float fx = (float)(uu - fx0);
    float fy = (float)(vv - fy0);
    int64_t x0 = pc_f64_to_i64(fx0);
    int64_t y0 = pc_f64_to_i64(fy0);
    const float wys[2] = {1.0f - fy, fy};
    const float wxs[2] = {1.0f - fx, fx};
    float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int dy;
    int dx;
    int c;
    for (dy = 0; dy < 2; dy++) {
        float wy = wys[dy];
        if (wy == 0.0f) {
            continue;
        }
        for (dx = 0; dx < 2; dx++) {
            float wx = wxs[dx];
            const float *p;
            float k;
            if (wx == 0.0f) {
                continue;
            }
            p = tile_at(t, add_wrap(x0, dx), add_wrap(y0, dy));
            k = wx * wy;
            for (c = 0; c < 4; c++) {
                acc[c] += p[c] * k;
            }
        }
    }
    if (acc[3] <= 0.0f) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    out[0] = acc[0] / acc[3];
    out[1] = acc[1] / acc[3];
    out[2] = acc[2] / acc[3];
    out[3] = pc_rs_minf_k(acc[3], 1.0f);
}

/* ---- placement ---- */

pc_compose_placement pc_compose_placement_anchored(const double anchor[2], bool link, const float phase[2],
                                                   float scale, float angle)
{
    const double rads_per_deg = 3.14159265358979323846 / 180.0;
    pc_compose_placement p;
    double s = (double)scale;
    double a = (double)angle * rads_per_deg;
    double bx = link ? anchor[0] : 0.0;
    double by = link ? anchor[1] : 0.0;
    p.origin[0] = bx + (double)phase[0];
    p.origin[1] = by + (double)phase[1];
    p.inv_scale = isfinite(s) && s > 1e-3 ? 1.0 / s : 1.0;
    p.cs[0] = cos(a);
    p.cs[1] = sin(a);
    return p;
}

pc_compose_placement pc_compose_placement_new(pc_rect frame, bool link, const float phase[2], float scale,
                                              float angle)
{
    double anchor[2] = {0.0, 0.0};
    if (!pc_rect_is_empty(frame)) {
        anchor[0] = (double)frame.x0;
        anchor[1] = (double)frame.y0;
    }
    return pc_compose_placement_anchored(anchor, link, phase, scale, angle);
}

void pc_compose_placement_map(const pc_compose_placement *p, double x, double y, double *u, double *v)
{
    double dx = x - p->origin[0];
    double dy = y - p->origin[1];
    /* Undo a counter-clockwise rotation (y points down on screen). */
    double c = p->cs[0];
    double s = p->cs[1];
    double rx = dx * c - dy * s;
    double ry = dx * s + dy * c;
    *u = rx * p->inv_scale;
    *v = ry * p->inv_scale;
}

pc_status pc_compose_pattern_render(const pc_compose_pattern_tile *t, const pc_compose_placement *place,
                                    pc_rect rect, pc_compose_buffer *out, pc_error *err)
{
    pc_compose_buffer b;
    size_t i = 0;
    int32_t x;
    int32_t y;
    pc_status st = pc_compose_buffer_transparent(rect, &b, err);
    if (st != PC_OK) {
        return st;
    }
    for (y = rect.y0; y < rect.y1 && b.len > 0; y++) {
        for (x = rect.x0; x < rect.x1; x++) {
            double u;
            double v;
            pc_compose_placement_map(place, (double)x + 0.5, (double)y + 0.5, &u, &v);
            pc_compose_pattern_tile_sample(t, u, v, b.px[i++]);
        }
    }
    *out = b;
    return PC_OK;
}

/* ---- prepared patterns ---- */

pc_status pc_compose_patterns_new(const pc_pattern *patterns, size_t n, size_t budget,
                                  pc_compose_patterns **out, pc_error *err)
{
    pc_compose_patterns *p = calloc(1, sizeof *p);
    slots *c = calloc(1, sizeof *c);
    if (p == NULL || c == NULL || pthread_mutex_init(&c->lock, NULL) != 0) {
        free(p);
        free(c);
        return nomem(err);
    }
    p->cache = c;
    p->patterns = patterns;
    p->n = patterns != NULL ? n : 0;
    p->budget = budget;
    *out = p;
    return PC_OK;
}

void pc_compose_patterns_free(pc_compose_patterns *p)
{
    size_t i;
    if (p == NULL) {
        return;
    }
    for (i = 0; i < p->cache->nslots; i++) {
        slot *s = p->cache->slots[i];
        pc_compose_pattern_tile_release(s->tile);
        pthread_mutex_destroy(&s->lock);
        free(s);
    }
    pthread_mutex_destroy(&p->cache->lock);
    free(p->cache);
    free(p);
}

const pc_pattern *pc_compose_patterns_source(const pc_compose_patterns *p, size_t *n)
{
    *n = p != NULL ? p->n : 0;
    return p != NULL ? p->patterns : NULL;
}

/* The slot of pattern pat, created (and its bytes reserved) when the budget allows; NULL when it
 * doesn't (the caller converts uncached). Slots live as long as the object. */
static slot *find_slot(const pc_compose_patterns *p, const pc_pattern *pat)
{
    uint64_t pixels = (uint64_t)pat->width * (uint64_t)pat->height;
    /* Past the pixel cap the conversion fails anyway; don't let the byte count overflow. */
    uint64_t bytes = pixels <= PC_COMPOSE_MAX_PIXELS ? pixels * (uint64_t)sizeof(float[4]) : UINT64_MAX;
    slots *c = p->cache;
    slot *s = NULL;
    size_t i;
    pthread_mutex_lock(&c->lock);
    for (i = 0; i < c->nslots; i++) {
        if (c->slots[i]->key == pat) {
            s = c->slots[i];
            break;
        }
    }
    /* Reserve before conversion: pending conversions consume the budget too. */
    if (s == NULL && p->budget >= c->reserved && bytes <= (uint64_t)(p->budget - c->reserved) &&
        c->nslots < MAX_PREPARED) {
        s = calloc(1, sizeof *s);
        if (s != NULL && pthread_mutex_init(&s->lock, NULL) != 0) {
            free(s);
            s = NULL;
        }
        if (s != NULL) {
            s->key = pat;
            c->reserved += (size_t)bytes;
            c->slots[c->nslots++] = s;
        }
    }
    pthread_mutex_unlock(&c->lock);
    return s;
}

pc_status pc_compose_patterns_get(const pc_compose_patterns *p, const char *id, size_t id_len,
                                  const char *name, size_t name_len, pc_compose_pattern_tile **out,
                                  pc_error *err)
{
    const pc_pattern *pat;
    slot *s;
    pc_status st = PC_OK;
    if (p == NULL) {
        *out = NULL;
        return PC_OK;
    }
    pat = pc_pattern_find(p->patterns, p->n, id, id_len, name, name_len);
    if (pat == NULL || pc_pattern_is_empty(pat)) {
        *out = NULL;
        return PC_OK;
    }
    s = find_slot(p, pat);
    if (s == NULL) {
        return pc_compose_pattern_tile_new(pat, out, err);
    }
    pthread_mutex_lock(&s->lock);
    if (!s->done) {
        pc_compose_pattern_tile *t = NULL;
        st = pc_compose_pattern_tile_new(pat, &t, err);
        if (st == PC_OK) {
            s->tile = t;
            s->done = true;
        }
    }
    if (st == PC_OK) {
        *out = pc_compose_pattern_tile_retain(s->tile);
    }
    pthread_mutex_unlock(&s->lock);
    return st;
}

pc_status pc_compose_pattern_fill_render(const pc_compose_patterns *patterns, const pc_fill *f, pc_rect frame,
                                         pc_rect rect, pc_compose_buffer *out, pc_error *err)
{
    pc_compose_pattern_tile *t = NULL;
    pc_compose_placement place;
    pc_status st;
    if (f->kind != PC_FILL_PATTERN) {
        return pc_compose_buffer_transparent(rect, out, err);
    }
    st = pc_compose_patterns_get(patterns, f->u.pattern.id.ptr, f->u.pattern.id.len, f->u.pattern.name.ptr,
                                 f->u.pattern.name.len, &t, err);
    if (st != PC_OK) {
        return st;
    }
    if (t == NULL) {
        return pc_compose_buffer_transparent(rect, out, err);
    }
    place = pc_compose_placement_new(frame, f->u.pattern.link, f->u.pattern.phase, f->u.pattern.scale,
                                     f->u.pattern.angle);
    st = pc_compose_pattern_render(t, &place, rect, out, err);
    pc_compose_pattern_tile_release(t);
    return st;
}
