/* A stroked shape layer split into its fill and its vector stroke, rasterised over the canvas
 * (port of crates/compose/src/shape_split.rs), for shapes with clipped layers or effects:
 * Photoshop draws the stroke above the clipped layers and interior effects. Cached per shape
 * state (32 entries, least recently used dropped first), so tiles reuse one rasterisation. */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "compose/internal.h"
#include "compose/keyhash.h"
#include "doc/json.h"

#define CAPACITY 32

/* (fill, stroke), last use, and the shape's pixels the stroke was fitted to (kept alive so their
 * tile addresses, part of the key, can't be reused). */
typedef struct entry {
    uint64_t key;
    pc_surface fill;
    pc_surface stroke;
    uint64_t tick;
    bool has_cache;
    pc_surface cache;
} entry;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static entry entries[CAPACITY];
static size_t nentries;
static uint64_t ticks;

static yyjson_mut_val *stroke_json(yyjson_mut_doc *d, const void *v)
{
    return pc_shape_stroke_to_json(d, v, 0);
}

static yyjson_mut_val *live_json(yyjson_mut_doc *d, const void *v)
{
    return pc_live_shape_to_json(d, v, 0);
}

/* The reference hashes canvas, depth and the Debug text of path, fill, stroke and live shape,
 * plus the cached pixels' identity. */
static pc_status shape_key(const pc_shape_layer *sh, pc_rect canvas, pc_sample_type depth, uint64_t *out,
                           pc_error *err)
{
    uint64_t h = PC_CMP_HASH0;
    pc_status st = PC_OK;
    pc_cmp_hash_rect(&h, canvas);
    pc_cmp_hash_u64(&h, (uint64_t)depth);
    pc_cmp_hash_path(&h, &sh->path);
    pc_cmp_hash_u64(&h, (uint64_t)sh->has_fill << 2 | (uint64_t)sh->has_stroke << 1 | (uint64_t)sh->has_live);
    if (sh->has_fill) {
        st = pc_cmp_hash_fill(&h, &sh->fill, err);
    }
    if (st == PC_OK && sh->has_stroke) {
        st = pc_cmp_hash_json(&h, stroke_json, &sh->stroke, err);
    }
    if (st == PC_OK && sh->has_live) {
        st = pc_cmp_hash_json(&h, live_json, &sh->live, err);
    }
    if (st != PC_OK) {
        return st;
    }
    pc_cmp_hash_u64(&h, sh->has_cache ? pc_cmp_surface_fp(&sh->cache) : 0);
    *out = h;
    return PC_OK;
}

/* Fits the stroke part to the shape's own pixels: where they reach past the fill, the stroke
 * covers what the fill doesn't ((pixels - fill) / (1 - fill)), in the pixels' colour where we
 * drew no stroke, and elsewhere in the colour that, drawn over the fill, gives back the pixels'
 * colour. Our stroke's own colour would be wrong where the file's fill covers more of the pixel
 * than ours (shapes snapped to whole pixels with a stroke thinner than a pixel). */
static pc_status fit_stroke(const pc_surface *fill, pc_surface *stroke, const pc_surface *cache,
                            pc_rect canvas, pc_error *err)
{
    pc_rect cb = pc_compose_content_bounds(cache);
    pc_rect sb = pc_compose_content_bounds(stroke);
    pc_rect r;
    size_t n;
    size_t i;
    float (*f)[4] = NULL;
    float (*s)[4] = NULL;
    float (*c)[4] = NULL;
    bool is_float = pc_surface_format(stroke).sample == PC_SAMPLE_TYPE_F32;
    pc_status st;
    /* Where either has pixels (the stroke may reach past the shape's pixels, and vice versa). */
    if (pc_rect_is_empty(cb)) {
        r = sb;
    } else if (pc_rect_is_empty(sb)) {
        r = cb;
    } else {
        r = pc_rect_union(cb, sb);
    }
    r = pc_rect_intersect(r, canvas);
    if (pc_rect_is_empty(r)) {
        return PC_OK;
    }
    st = pc_cmp_rect_pixels(r, &n, err);
    if (st != PC_OK) {
        return st;
    }
    f = pc_calloc_array(n, sizeof *f);
    s = pc_calloc_array(n, sizeof *s);
    c = pc_calloc_array(n, sizeof *c);
    if (f == NULL || s == NULL || c == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
        goto cleanup;
    }
    st = pc_surface_read_rgba_into(fill, r, f, n, err);
    if (st == PC_OK) {
        st = pc_surface_read_rgba_into(stroke, r, s, n, err);
    }
    if (st == PC_OK) {
        st = pc_surface_read_rgba_into(cache, r, c, n, err);
    }
    if (st != PC_OK) {
        goto cleanup;
    }
    /* The result overwrites s in place (each pixel reads only its own inputs). */
    for (i = 0; i < n; i++) {
        const float *fp = f[i];
        const float *cp = c[i];
        float *sp = s[i];
        float a;
        int k;
        if (!(fp[3] < 1.0f - 1e-3f)) {
            continue;
        }
        a = pc_clampf((cp[3] - fp[3]) / (1.0f - fp[3]), 0.0f, 1.0f);
        if (sp[3] <= 0.0f) {
            sp[0] = cp[0];
            sp[1] = cp[1];
            sp[2] = cp[2];
        } else if (a > 1e-3f) {
            /* Normal over the fill: a p + f (1 - a) fill = pixels (premultiplied). */
            float kk = fp[3] * (1.0f - a);
            for (k = 0; k < 3; k++) {
                float v = (cp[3] * cp[k] - kk * fp[k]) / a;
                /* Float RGB retains HDR values; integer samples use the normalized range. */
                sp[k] = is_float ? v : pc_clampf(v, 0.0f, 1.0f);
            }
        }
        sp[3] = a;
    }
    st = pc_surface_write_region(stroke, r, &s[0][0], n * 4, err);
cleanup:
    free(f);
    free(s);
    free(c);
    return st;
}

static void entry_fini(entry *e)
{
    pc_surface_fini(&e->fill);
    pc_surface_fini(&e->stroke);
    if (e->has_cache) {
        pc_surface_fini(&e->cache);
    }
    memset(e, 0, sizeof *e);
}

/* Clones of a cached entry's parts (under the lock). */
static pc_status lookup(uint64_t key, bool *hit, pc_surface *fill, pc_surface *stroke, pc_error *err)
{
    size_t i;
    pc_status st = PC_OK;
    *hit = false;
    pthread_mutex_lock(&lock);
    ticks++;
    for (i = 0; i < nentries; i++) {
        if (entries[i].key == key) {
            entries[i].tick = ticks;
            st = pc_surface_clone(&entries[i].fill, fill, err);
            if (st == PC_OK) {
                st = pc_surface_clone(&entries[i].stroke, stroke, err);
                if (st != PC_OK) {
                    pc_surface_fini(fill);
                }
            }
            *hit = st == PC_OK;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return st;
}

/* Caches clones of the parts; a failure to cache is not an error (the next call renders
 * again). */
static void store(uint64_t key, const pc_shape_layer *sh, const pc_surface *fill, const pc_surface *stroke)
{
    entry e;
    size_t i;
    size_t slot;
    memset(&e, 0, sizeof e);
    e.key = key;
    if (pc_surface_clone(fill, &e.fill, NULL) != PC_OK) {
        return;
    }
    if (pc_surface_clone(stroke, &e.stroke, NULL) != PC_OK) {
        entry_fini(&e);
        return;
    }
    if (sh->has_cache) {
        if (pc_surface_clone(&sh->cache, &e.cache, NULL) != PC_OK) {
            entry_fini(&e);
            return;
        }
        e.has_cache = true;
    }
    pthread_mutex_lock(&lock);
    /* Another thread may have stored the same shape meanwhile: replace it. */
    slot = nentries;
    for (i = 0; i < nentries; i++) {
        if (entries[i].key == key) {
            slot = i;
            break;
        }
    }
    if (slot == nentries && nentries >= CAPACITY) {
        slot = 0;
        for (i = 1; i < nentries; i++) {
            if (entries[i].tick < entries[slot].tick) {
                slot = i;
            }
        }
    }
    if (slot < nentries) {
        entry_fini(&entries[slot]);
    } else {
        nentries++;
    }
    e.tick = ticks;
    entries[slot] = e;
    pthread_mutex_unlock(&lock);
}

pc_status pc_compose_shape_split(const pc_shape_layer *sh, pc_rect canvas, pc_sample_type depth, bool *found,
                                 pc_surface *fill, pc_surface *stroke, pc_error *err)
{
    uint64_t key;
    bool hit = false;
    pc_shape_layer fill_only;
    pc_shape_layer stroke_only;
    pc_pixel_format fmt = PC_PIXEL_FORMAT_RGBA8;
    pc_surface fs;
    pc_surface ss;
    pc_status st;
    if (!sh->has_stroke) {
        *found = false;
        return PC_OK;
    }
    st = shape_key(sh, canvas, depth, &key, err);
    if (st == PC_OK) {
        st = lookup(key, &hit, &fs, &ss, err);
    }
    if (st != PC_OK) {
        return st;
    }
    if (!hit) {
        fmt.sample = depth;
        /* Shallow copies that only the renderer reads: the parts without the other half and
         * without the cached pixels. */
        fill_only = *sh;
        fill_only.has_stroke = false;
        fill_only.has_cache = false;
        stroke_only = *sh;
        stroke_only.has_fill = false;
        stroke_only.has_cache = false;
        st = pc_vec_render_shape(&fill_only, fmt, canvas, &fs, err);
        if (st != PC_OK) {
            return st;
        }
        st = pc_vec_render_shape(&stroke_only, fmt, canvas, &ss, err);
        if (st != PC_OK) {
            pc_surface_fini(&fs);
            return st;
        }
        if (sh->has_cache) {
            st = fit_stroke(&fs, &ss, &sh->cache, canvas, err);
        }
        if (st != PC_OK) {
            pc_surface_fini(&fs);
            pc_surface_fini(&ss);
            return st;
        }
        store(key, sh, &fs, &ss);
    }
    *fill = fs;
    *stroke = ss;
    *found = true;
    return PC_OK;
}
