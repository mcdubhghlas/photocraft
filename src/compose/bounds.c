/* pc_surface_content_bounds with the per-tile scan cached by tile identity (port of
 * crates/compose/src/bounds.rs), so layer bounds cost a hash lookup per tile instead of a pixel
 * scan per render tile.
 *
 * The reference keys the cache by tile address and checks a Weak<Tile> to see that the tile is
 * still the one scanned. C tiles have no weak references, so each entry holds a real reference:
 * the address can't be reused while it is cached, and a shared tile is never written in place
 * (so a hit is always exact). Entries are dropped first-in first-out beyond PINNED_BYTES of
 * pinned tile memory or MAX_ENTRIES entries. */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "compose/internal.h"

#define BUCKETS 16384u
#define MAX_ENTRIES 65536u
#define PINNED_BYTES ((size_t)64 << 20)

typedef struct node {
    const pc_raster_tile *key;
    pc_raster_tile *tile; /* the reference that pins key */
    uint8_t dp[PC_RASTER_MAX_PIXEL_BYTES];
    size_t bpp;
    bool has;
    uint16_t b[4]; /* tile-local x0, y0, x1, y1 */
    struct node *chain;
    struct node *older;
    struct node *newer;
} node;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static node **buckets; /* BUCKETS heads, allocated on first use */
static node *oldest;
static node *newest;
static size_t count;
static size_t pinned;

static size_t slot_of(const void *p)
{
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 17;
    x *= (uintptr_t)0x9e3779b97f4a7c15ull;
    return (size_t)(x >> 7) & (BUCKETS - 1);
}

/* Pixels of the tile that differ from dp (tile-local half-open bounds). */
static bool scan(const uint8_t *bytes, size_t len, const uint8_t *dp, size_t bpp, uint16_t out[4])
{
    const size_t ts = PC_TILE_SIZE;
    size_t x0 = SIZE_MAX;
    size_t y0 = SIZE_MAX;
    size_t x1 = 0;
    size_t y1 = 0;
    size_t y;
    if (bpp == 0 || len < ts * ts * bpp) {
        return false;
    }
    for (y = 0; y < ts; y++) {
        const uint8_t *row = bytes + y * ts * bpp;
        size_t first = 0;
        size_t last;
        while (first < ts && memcmp(row + first * bpp, dp, bpp) == 0) {
            first++;
        }
        if (first == ts) {
            continue;
        }
        last = ts - 1;
        while (last > first && memcmp(row + last * bpp, dp, bpp) == 0) {
            last--;
        }
        if (first < x0) {
            x0 = first;
        }
        if (last + 1 > x1) {
            x1 = last + 1;
        }
        if (y < y0) {
            y0 = y;
        }
        y1 = y + 1;
    }
    if (x0 == SIZE_MAX) {
        return false;
    }
    out[0] = (uint16_t)x0;
    out[1] = (uint16_t)y0;
    out[2] = (uint16_t)x1;
    out[3] = (uint16_t)y1;
    return true;
}

static void unlink_node(node *n)
{
    node **pp = &buckets[slot_of(n->key)];
    while (*pp != NULL && *pp != n) {
        pp = &(*pp)->chain;
    }
    if (*pp == n) {
        *pp = n->chain;
    }
    if (n->older != NULL) {
        n->older->newer = n->newer;
    } else {
        oldest = n->newer;
    }
    if (n->newer != NULL) {
        n->newer->older = n->older;
    } else {
        newest = n->older;
    }
    count--;
    pinned -= pc_raster_tile_len(n->tile);
    pc_raster_tile_release(n->tile);
    free(n);
}

static void evict(void)
{
    while (oldest != NULL && (count > MAX_ENTRIES || pinned > PINNED_BYTES)) {
        unlink_node(oldest);
    }
}

void pc_compose_purge_bounds_cache(void)
{
    pthread_mutex_lock(&lock);
    while (oldest != NULL) {
        unlink_node(oldest);
    }
    pthread_mutex_unlock(&lock);
}

/* The cached (or freshly scanned) bounds of one tile; caches when it can. */
static bool tile_bounds(const pc_raster_tile *t, const uint8_t *dp, size_t bpp, uint16_t out[4])
{
    node *n;
    size_t slot = slot_of(t);
    for (n = buckets[slot]; n != NULL; n = n->chain) {
        if (n->key == t && n->bpp == bpp && memcmp(n->dp, dp, bpp) == 0) {
            memcpy(out, n->b, sizeof n->b);
            return n->has;
        }
    }
    n = calloc(1, sizeof *n);
    if (n == NULL) {
        return scan(pc_raster_tile_bytes(t), pc_raster_tile_len(t), dp, bpp, out);
    }
    n->key = t;
    n->tile = pc_raster_tile_retain((pc_raster_tile *)(uintptr_t)t);
    memcpy(n->dp, dp, bpp);
    n->bpp = bpp;
    n->has = scan(pc_raster_tile_bytes(t), pc_raster_tile_len(t), dp, bpp, n->b);
    n->chain = buckets[slot];
    buckets[slot] = n;
    n->older = newest;
    if (newest != NULL) {
        newest->newer = n;
    } else {
        oldest = n;
    }
    newest = n;
    count++;
    pinned += pc_raster_tile_len(n->tile);
    memcpy(out, n->b, sizeof n->b);
    {
        bool has = n->has;
        evict(); /* may drop n itself when one tile exceeds the budget */
        return has;
    }
}

pc_rect pc_compose_content_bounds(const pc_surface *s)
{
    pc_rect out = PC_RECT_EMPTY;
    size_t bpp;
    const uint8_t *dp;
    size_t cursor = 0;
    pc_tile coord;
    const pc_raster_tile *t;
    if (pc_surface_tile_count(s) == 0) {
        return PC_RECT_EMPTY;
    }
    bpp = pc_pixel_format_bytes_per_pixel(pc_surface_format(s));
    dp = pc_surface_default_bytes(s);
    pthread_mutex_lock(&lock);
    if (buckets == NULL) {
        buckets = calloc(BUCKETS, sizeof *buckets);
        if (buckets == NULL) {
            pthread_mutex_unlock(&lock);
            return pc_surface_content_bounds(s);
        }
    }
    while (pc_surface_next_tile(s, &cursor, &coord, &t)) {
        uint16_t b[4];
        if (tile_bounds(t, dp, bpp, b)) {
            pc_rect o = pc_tile_rect(coord);
            /* Wrapping like the reference's release build (tile coordinates past 2^23). */
            pc_rect r = pc_rect_make(pc_i32_wrapping_add(o.x0, b[0]), pc_i32_wrapping_add(o.y0, b[1]),
                                     pc_i32_wrapping_add(o.x0, b[2]), pc_i32_wrapping_add(o.y0, b[3]));
            out = pc_rect_is_empty(out) ? r : pc_rect_union(out, r);
        }
    }
    pthread_mutex_unlock(&lock);
    return out;
}
