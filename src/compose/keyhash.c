/* Cache keys: order-sensitive 64-bit hashes of masks, surfaces and serialised layer state (the
 * reference hashes `format!("{:?}")` strings and tile addresses with DefaultHasher; any stable
 * hash works since keys never leave the process). */
#include <string.h>

#include "base/buf.h"
#include "compose/keyhash.h"
#include "doc/json.h"

void pc_cmp_hash_bytes(uint64_t *h, const void *p, size_t n)
{
    const uint8_t *b = p;
    uint64_t x = *h;
    size_t i;
    for (i = 0; i < n; i++) {
        x ^= b[i];
        x *= 0x100000001b3ull;
    }
    *h = x;
}

void pc_cmp_hash_u64(uint64_t *h, uint64_t v)
{
    pc_cmp_hash_bytes(h, &v, sizeof v);
}

void pc_cmp_hash_f32(uint64_t *h, float v)
{
    uint32_t u;
    memcpy(&u, &v, sizeof u);
    pc_cmp_hash_u64(h, u);
}

void pc_cmp_hash_f64(uint64_t *h, double v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof u);
    pc_cmp_hash_u64(h, u);
}

void pc_cmp_hash_rect(uint64_t *h, pc_rect r)
{
    pc_cmp_hash_u64(h, (uint64_t)(uint32_t)r.x0 | (uint64_t)(uint32_t)r.y0 << 32);
    pc_cmp_hash_u64(h, (uint64_t)(uint32_t)r.x1 | (uint64_t)(uint32_t)r.y1 << 32);
}

static void hash_point(uint64_t *h, pc_point p)
{
    pc_cmp_hash_f64(h, p.x);
    pc_cmp_hash_f64(h, p.y);
}

void pc_cmp_hash_path(uint64_t *h, const pc_path *p)
{
    size_t i;
    size_t k;
    pc_cmp_hash_u64(h, (uint64_t)p->fill_rule << 1 | (uint64_t)p->inverted);
    pc_cmp_hash_u64(h, p->nsubpaths);
    for (i = 0; i < p->nsubpaths; i++) {
        const pc_subpath *s = &p->subpaths[i];
        pc_cmp_hash_u64(h, (uint64_t)s->op << 1 | (uint64_t)s->closed);
        pc_cmp_hash_u64(h, s->nknots);
        for (k = 0; k < s->nknots; k++) {
            hash_point(h, s->knots[k].anchor);
            hash_point(h, s->knots[k].in_ctrl);
            hash_point(h, s->knots[k].out_ctrl);
            pc_cmp_hash_u64(h, s->knots[k].smooth);
        }
    }
}

void pc_cmp_hash_vector_mask(uint64_t *h, const pc_vector_mask *m)
{
    if (m == NULL) {
        pc_cmp_hash_u64(h, 0);
        return;
    }
    pc_cmp_hash_u64(h, 1u | (uint64_t)m->enabled << 1 | (uint64_t)m->linked << 2);
    pc_cmp_hash_f32(h, m->density);
    pc_cmp_hash_f32(h, m->feather);
    pc_cmp_hash_path(h, &m->path);
}

uint64_t pc_cmp_surface_fp(const pc_surface *s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    pc_pixel_format f = pc_surface_format(s);
    float dp[PC_RASTER_MAX_CHANNELS];
    size_t n = pc_surface_channels(s);
    size_t i;
    size_t cursor = 0;
    pc_tile c;
    const pc_raster_tile *t;
    uint64_t tiles = pc_surface_tile_count(s);
    pc_cmp_hash_u64(&h, (uint64_t)f.mode | (uint64_t)f.sample << 8 | (uint64_t)f.alpha << 16);
    /* Missing tiles read the default pixel, so tile identity alone is not pixel identity. */
    pc_surface_default_pixel(s, dp);
    for (i = 0; i < n; i++) {
        pc_cmp_hash_f32(&h, dp[i]);
    }
    /* Order-independent sum over (tile address, coordinate). */
    while (pc_surface_next_tile(s, &cursor, &c, &t)) {
        uint64_t x =
            (uint64_t)(uintptr_t)t ^ ((uint64_t)(uint32_t)c.tx << 40) ^ ((uint64_t)(uint32_t)c.ty << 8);
        x *= 0x9e3779b97f4a7c15ull;
        tiles += x ^ (x >> 29);
    }
    pc_cmp_hash_u64(&h, tiles);
    return h;
}

pc_status pc_cmp_hash_json(uint64_t *h, pc_cmp_json_fn fn, const void *v, pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root;
    pc_buf b;
    pc_status st;
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
    }
    root = fn(d, v);
    if (root == NULL) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
    }
    pc_buf_init(&b);
    st = pc_json_write(root, 0, &b, err);
    if (st == PC_OK) {
        pc_cmp_hash_bytes(h, b.data, b.len);
        pc_cmp_hash_u64(h, b.len);
    }
    pc_buf_fini(&b);
    yyjson_mut_doc_free(d);
    return st;
}

static yyjson_mut_val *fill_json(yyjson_mut_doc *d, const void *v)
{
    return pc_fill_to_json(d, v, 0);
}

static yyjson_mut_val *adjustment_json(yyjson_mut_doc *d, const void *v)
{
    return pc_adjustment_to_json(d, v, 0);
}

static yyjson_mut_val *effect_json(yyjson_mut_doc *d, const void *v)
{
    return pc_effect_to_json(d, v, 0);
}

static yyjson_mut_val *artboard_json(yyjson_mut_doc *d, const void *v)
{
    return pc_artboard_to_json(d, v, 0);
}

pc_status pc_cmp_hash_fill(uint64_t *h, const pc_fill *f, pc_error *err)
{
    return pc_cmp_hash_json(h, fill_json, f, err);
}

pc_status pc_cmp_hash_adjustment(uint64_t *h, const pc_adjustment *a, pc_error *err)
{
    return pc_cmp_hash_json(h, adjustment_json, a, err);
}

pc_status pc_cmp_hash_artboard(uint64_t *h, const pc_group *g, pc_error *err)
{
    if (!g->has_artboard) {
        pc_cmp_hash_u64(h, 0);
        return PC_OK;
    }
    pc_cmp_hash_u64(h, 1);
    return pc_cmp_hash_json(h, artboard_json, &g->artboard, err);
}

pc_status pc_cmp_hash_effects(uint64_t *h, const pc_effects *e, pc_error *err)
{
    size_t i;
    pc_cmp_hash_u64(h, (uint64_t)e->enabled | (uint64_t)e->has_reference << 1);
    if (e->has_reference) {
        pc_cmp_hash_f64(h, e->reference[0]);
        pc_cmp_hash_f64(h, e->reference[1]);
    }
    pc_cmp_hash_u64(h, e->nitems);
    for (i = 0; i < e->nitems; i++) {
        pc_status st = pc_cmp_hash_json(h, effect_json, &e->items[i], err);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}
