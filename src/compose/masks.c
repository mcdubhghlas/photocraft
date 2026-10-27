/* A layer's effective mask (pixel mask x vector mask, each feathered) as one surface, cached per
 * mask state (port of crates/compose/src/masks.rs). Retention is bounded by entry count and by
 * accounted pixel bytes (PC_COMPOSE_MASK_CACHE_BUDGET); a purge also stops builds that started
 * before it from refilling the cache (a generation count). Concurrent builds of one cold mask are
 * not shared while running (nothing waits for another thread's build, as in fx_cache.c), but the
 * first finished build is kept and every later finisher returns its tiles. */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "compose/internal.h"
#include "compose/keyhash.h"

#define FEATHER_SIGMA 1.0f
#define MAX_FEATHER_SIGMA (1000.0f * FEATHER_SIGMA) /* Photoshop's feather tops out at 1000 px */
#define CAPACITY 64

/* A U16 mask keeps much smaller cached tiles than F32 while adding at most 1/65535 quantisation
 * error, finer than the final 8-bit composite precision. */
static const pc_pixel_format FORMAT = {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, false};

typedef struct entry {
    bool used;
    uint64_t key;
    uint64_t tick;
    size_t bytes;
    pc_surface surface;
    /* The key contains input tile addresses: keep those tiles alive until eviction. */
    bool has_input;
    pc_surface pixel_input;
} entry;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static entry cache[CAPACITY];
static uint64_t ticks;
static size_t cache_bytes;
static uint64_t generation;

static void entry_clear(entry *e)
{
    if (e->used) {
        pc_surface_fini(&e->surface);
        if (e->has_input) {
            pc_surface_fini(&e->pixel_input);
        }
    }
    memset(e, 0, sizeof *e);
}

size_t pc_compose_purge_mask_cache(void)
{
    size_t i;
    size_t bytes;
    pthread_mutex_lock(&lock);
    bytes = cache_bytes;
    for (i = 0; i < CAPACITY; i++) {
        entry_clear(&cache[i]);
    }
    cache_bytes = 0;
    ticks = 0;
    generation++;
    pthread_mutex_unlock(&lock);
    return bytes;
}

size_t pc_compose_mask_cache_bytes(void)
{
    size_t b;
    pthread_mutex_lock(&lock);
    b = cache_bytes;
    pthread_mutex_unlock(&lock);
    return b;
}

size_t pc_compose_mask_surface_bytes(const pc_surface *s)
{
    pc_pixel_format f = pc_surface_format(s);
    size_t px = pc_pixel_format_channels(f) * pc_sample_type_bytes(f.sample);
    size_t tile = (size_t)PC_TILE_SIZE * PC_TILE_SIZE * px;
    size_t n = pc_surface_tile_count(s);
    size_t total;
    if (!pc_size_mul(n, tile, &total) || !pc_size_add(total, px, &total)) {
        return SIZE_MAX;
    }
    return total;
}

/* Charges the whole source as well as the result; shared tiles are charged per entry. */
static size_t entry_bytes(const pc_surface *surface, const pc_surface *pixel_input)
{
    size_t a = pc_compose_mask_surface_bytes(surface);
    size_t b = pixel_input != NULL ? pc_compose_mask_surface_bytes(pixel_input) : 0;
    return a > SIZE_MAX - b ? SIZE_MAX : a + b;
}

float pc_compose_feather_sigma(float px)
{
    return isfinite(px) ? pc_clampf(px * FEATHER_SIGMA, 0.0f, MAX_FEATHER_SIGMA) : 0.0f;
}

bool pc_compose_has_feather(const pc_layer *layer)
{
    return (layer->has_mask && layer->mask.enabled && pc_compose_feather_sigma(layer->mask.feather) > 0.0f) ||
           (layer->has_vector_mask && layer->vector_mask.enabled &&
            pc_compose_feather_sigma(layer->vector_mask.feather) > 0.0f);
}

static uint64_t key_of(const pc_layer *layer, pc_rect canvas)
{
    uint64_t h = PC_CMP_HASH0;
    pc_cmp_hash_u64(&h, layer->id);
    pc_cmp_hash_rect(&h, canvas);
    pc_cmp_hash_vector_mask(&h, layer->has_vector_mask ? &layer->vector_mask : NULL);
    if (layer->has_mask) {
        const pc_layer_mask *m = &layer->mask;
        size_t cursor = 0;
        pc_tile c;
        const pc_raster_tile *t;
        float dp[PC_RASTER_MAX_CHANNELS];
        size_t n = pc_surface_channels(&m->surface);
        size_t i;
        pc_cmp_hash_u64(&h, m->enabled);
        pc_cmp_hash_f32(&h, m->density);
        pc_cmp_hash_f32(&h, m->feather);
        pc_surface_default_pixel(&m->surface, dp);
        for (i = 0; i < n; i++) {
            pc_cmp_hash_f32(&h, dp[i]);
        }
        /* Tile order of an open-addressing table depends on its history; sum per tile so equal
         * tile sets give equal keys. */
        {
            uint64_t sum = 0;
            while (pc_surface_next_tile(&m->surface, &cursor, &c, &t)) {
                uint64_t x = PC_CMP_HASH0;
                pc_cmp_hash_u64(&x, (uint64_t)(uint32_t)c.tx | (uint64_t)(uint32_t)c.ty << 32);
                pc_cmp_hash_u64(&x, (uint64_t)(uintptr_t)t);
                sum += x;
            }
            pc_cmp_hash_u64(&h, sum);
        }
    }
    return h;
}

/* One box pass: weight 1 on +-r around each sample and `end` (0..1) on the two samples just past
 * the window, so a fractional variance can be matched exactly (shared with the shape renderer). */
typedef struct box_pass {
    size_t r;
    float end;
    bool active;
} box_pass;

/* The box radius and fractional end weight whose single pass has variance sigma^2 / 3. */
static box_pass box_pass_for_sigma(float sigma)
{
    box_pass bp = {0, 0.0f, false};
    double sm;
    double v;
    double rf;
    double nn;
    double end;
    double r0;
    size_t r;
    if (!isfinite(sigma) || sigma <= 0.0f) {
        return bp;
    }
    /* Feathers top out at 1000 px; the cap keeps the radius arithmetic far from overflow. */
    sm = (double)(sigma < MAX_FEATHER_SIGMA ? sigma : MAX_FEATHER_SIGMA);
    v = sm * sm / 3.0;
    /* The largest whole radius whose box variance r(r+1)/3 is at most v. */
    r0 = floor((sqrt(12.0 * v + 1.0) - 1.0) / 2.0);
    if (!(r0 > 0.0)) {
        r0 = 0.0;
    }
    r = (size_t)r0;
    while (r > 0 && (double)r * ((double)r + 1.0) / 3.0 > v) {
        r--;
    }
    while ((double)(r + 1) * ((double)(r + 1) + 1.0) / 3.0 <= v) {
        r++;
    }
    rf = (double)r;
    nn = 2.0 * (double)r + 1.0;
    end = (nn * (rf * (rf + 1.0) / 3.0 - v)) / (2.0 * v - 2.0 * (rf + 1.0) * (rf + 1.0));
    /* f64::clamp(0, 1): NaN stays NaN. */
    end = end < 0.0 ? 0.0 : (end > 1.0 ? 1.0 : end);
    bp.r = r;
    bp.end = (float)end;
    bp.active = true;
    return bp;
}

/* One box pass over a line: reads the pre-pass values from `scratch` (a copy of `line`) and writes
 * the blurred result into `line` (edges clamp). */
static void blur_line(float *line, float *scratch, size_t len, const box_pass *bp)
{
    int64_t radius = (int64_t)bp->r;
    int64_t last = (int64_t)len - 1;
    float acc = -0.0f; /* Iterator::sum starts at -0.0 */
    float n;
    float below;
    int64_t j;
    size_t i;
    if (len == 0) {
        return;
    }
    memcpy(scratch, line, len * sizeof *line);
#define GET(ix) scratch[(size_t)((ix) < 0 ? 0 : ((ix) > last ? last : (ix)))]
    for (j = -radius; j <= radius; j++) {
        acc += GET(j);
    }
    n = (float)(2 * bp->r + 1) + 2.0f * bp->end;
    below = GET(-radius - 1);
    for (i = 0; i < len; i++) {
        int64_t ii = (int64_t)i;
        float above = GET(ii + radius + 1);
        float leaving = GET(ii - radius);
        line[i] = (acc + bp->end * (below + above)) / n;
        acc += above - leaving;
        below = leaving;
    }
#undef GET
}

/* Approximate Gaussian blur of a w x h plane: three box passes per axis (edges clamp). */
static pc_status gaussian(float *v, size_t w, size_t h, float sigma, pc_error *err)
{
    box_pass bp = box_pass_for_sigma(sigma);
    size_t len_max = w > h ? w : h;
    float *work;
    float *scratch;
    int axis;
    if (!bp.active || w == 0 || h == 0) {
        return PC_OK;
    }
    work = pc_malloc_array(len_max, sizeof *work);
    scratch = pc_malloc_array(len_max, sizeof *scratch);
    if (work == NULL || scratch == NULL) {
        free(work);
        free(scratch);
        return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
    }
    for (axis = 0; axis < 2; axis++) {
        size_t lines = axis ? w : h;
        size_t line_len = axis ? h : w;
        size_t k;
        for (k = 0; k < lines; k++) {
            size_t i;
            int rep;
            for (i = 0; i < line_len; i++) {
                work[i] = axis ? v[i * w + k] : v[k * w + i];
            }
            for (rep = 0; rep < 3; rep++) {
                blur_line(work, scratch, line_len, &bp);
            }
            for (i = 0; i < line_len; i++) {
                if (axis) {
                    v[i * w + k] = work[i];
                } else {
                    v[k * w + i] = work[i];
                }
            }
        }
    }
    free(work);
    free(scratch);
    return PC_OK;
}

static pc_rect grow(pc_rect r, float sigma)
{
    int32_t m;
    if (pc_rect_is_empty(r)) {
        return r;
    }
    m = pc_i32_sat_add(pc_f32_to_i32(ceilf(sigma * 3.0f)), 2);
    return pc_rect_make(pc_i32_sat_sub(r.x0, m), pc_i32_sat_sub(r.y0, m), pc_i32_sat_add(r.x1, m),
                        pc_i32_sat_add(r.y1, m));
}

/* The mask surface itself (no cache). */
static pc_status build(const pc_layer *layer, pc_rect canvas, const pc_vector_mask *vm,
                       const pc_layer_mask *pixel, float sv, float sp, pc_surface *out, pc_error *err)
{
    float v_out = 1.0f;
    float p_def = 1.0f;
    float def;
    pc_rect area = PC_RECT_EMPTY;
    double cb[4];
    pc_surface s;
    pc_status st = PC_OK;
    float *v = NULL;
    float *pm = NULL;
    size_t n = 0;
    size_t len = 0;
    pc_vec_compiled_mask compiled;
    (void)layer;
    /* Far outside the path the vector mask is constant. The mask is rendered without entering the
     * thread pool (render_sequential): another tile may be waiting on this build. */
    if (vm != NULL) {
        pc_rect far = pc_rect_from_xywh(pc_i32_wrapping_sub(canvas.x0, 1000000),
                                        pc_i32_wrapping_sub(canvas.y0, 1000000), 1, 1);
        st = pc_vec_compiled_mask_new(vm, &compiled, err);
        if (st != PC_OK) {
            return st;
        }
        st = pc_vec_compiled_mask_render_sequential(&compiled, far, &v, &len, err);
        if (st != PC_OK) {
            pc_vec_compiled_mask_fini(&compiled);
            return st;
        }
        v_out = len > 0 ? v[0] : 0.0f;
        free(v);
        v = NULL;
    }
    if (pixel != NULL) {
        float dp[PC_RASTER_MAX_CHANNELS];
        float d = 1.0f;
        pc_surface_default_pixel(&pixel->surface, dp);
        if (pc_surface_channels(&pixel->surface) > 0) {
            d = dp[0];
        }
        p_def = 1.0f - pixel->density * (1.0f - d);
    }
    /* Where the product can differ from p_def x v_out: the path's bounds, plus the pixel mask's
     * painted tiles when the vector mask lets them through outside the path, each grown by its
     * feather. */
    if (vm != NULL && pc_path_control_bounds(&vm->path, cb)) {
        pc_rect r = pc_rect_make(pc_i32_wrapping_sub(pc_f64_to_i32(floor(cb[0])), 2),
                                 pc_i32_wrapping_sub(pc_f64_to_i32(floor(cb[1])), 2),
                                 pc_i32_wrapping_add(pc_f64_to_i32(ceil(cb[2])), 2),
                                 pc_i32_wrapping_add(pc_f64_to_i32(ceil(cb[3])), 2));
        area = grow(r, sv);
    }
    if (pixel != NULL && v_out > 0.0f) {
        pc_rect b = grow(pc_surface_content_bounds(&pixel->surface), sp);
        if (pc_rect_is_empty(area)) {
            area = b;
        } else if (!pc_rect_is_empty(b)) {
            area = pc_rect_union(area, b);
        }
    }
    area = pc_rect_intersect(area, canvas);
    def = p_def * v_out;
    pc_surface_init_default(&s, FORMAT, &def, 1);
    if (!pc_rect_is_empty(area)) {
        size_t w = pc_cmp_w(area);
        size_t h = (size_t)pc_rect_height(area);
        size_t i;
        st = pc_cmp_rect_pixels(area, &n, err);
        if (st == PC_OK && vm != NULL) {
            st = pc_vec_compiled_mask_render_sequential(&compiled, area, &v, &len, err);
            if (st == PC_OK && len != n) {
                st = pc_fail(err, PC_ERR_INVALID, "compose: vector mask size mismatch");
            }
        } else if (st == PC_OK) {
            v = pc_malloc_array(n, sizeof *v);
            if (v == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
            }
            for (i = 0; v != NULL && i < n; i++) {
                v[i] = 1.0f;
            }
        }
        if (st == PC_OK) {
            st = gaussian(v, w, h, sv, err);
        }
        if (st == PC_OK && pixel != NULL) {
            pm = pc_malloc_array(n, sizeof *pm);
            if (pm == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
            }
            if (st == PC_OK) {
                st = pc_layer_mask_values_into(pixel, area, pm, n, err);
            }
            if (st == PC_OK) {
                st = gaussian(pm, w, h, sp, err);
            }
            for (i = 0; st == PC_OK && i < n; i++) {
                v[i] *= pm[i];
            }
        }
        if (st == PC_OK) {
            st = pc_surface_write_region(&s, area, v, n, err);
        }
    }
    free(v);
    free(pm);
    if (vm != NULL) {
        pc_vec_compiled_mask_fini(&compiled);
    }
    if (st != PC_OK) {
        pc_surface_fini(&s);
        return st;
    }
    *out = s;
    return PC_OK;
}

/* Cache::insert (under the lock): keeps e unless it is over budget or a purge happened since its
 * build started (gen); evicts least recently used entries for room. Takes e over either way. */
static void insert(entry *e, size_t budget, uint64_t gen)
{
    entry *slot = NULL;
    size_t used = 0;
    size_t i;
    if (e->bytes > budget || gen != generation) {
        entry_clear(e);
        return;
    }
    for (i = 0; i < CAPACITY; i++) {
        if (cache[i].used && cache[i].key == e->key) {
            cache_bytes -= cache[i].bytes < cache_bytes ? cache[i].bytes : cache_bytes;
            entry_clear(&cache[i]);
        }
    }
    for (;;) {
        entry *oldest = NULL;
        used = 0;
        for (i = 0; i < CAPACITY; i++) {
            if (cache[i].used) {
                used++;
                if (oldest == NULL || cache[i].tick < oldest->tick) {
                    oldest = &cache[i];
                }
            }
        }
        if (oldest == NULL || (used < CAPACITY && cache_bytes <= budget - e->bytes)) {
            break;
        }
        cache_bytes -= oldest->bytes < cache_bytes ? oldest->bytes : cache_bytes;
        entry_clear(oldest);
    }
    for (i = 0; i < CAPACITY && slot == NULL; i++) {
        if (!cache[i].used) {
            slot = &cache[i];
        }
    }
    if (slot == NULL) {
        entry_clear(e); /* unreachable: eviction leaves a free slot */
        return;
    }
    cache_bytes += e->bytes;
    ticks++;
    e->tick = ticks;
    *slot = *e;
}

pc_status pc_compose_combined_mask(const pc_layer *layer, pc_rect canvas, bool *found, pc_surface *out,
                                   pc_error *err)
{
    const pc_vector_mask *vm =
        layer->has_vector_mask && layer->vector_mask.enabled ? &layer->vector_mask : NULL;
    const pc_layer_mask *pixel = layer->has_mask && layer->mask.enabled ? &layer->mask : NULL;
    float sv = vm != NULL ? pc_compose_feather_sigma(vm->feather) : 0.0f;
    float sp = pixel != NULL ? pc_compose_feather_sigma(pixel->feather) : 0.0f;
    uint64_t k;
    size_t i;
    pc_surface s;
    pc_status st;
    uint64_t gen;
    if (vm == NULL && !pc_compose_has_feather(layer)) {
        *found = false;
        return PC_OK;
    }
    k = key_of(layer, canvas);
    pthread_mutex_lock(&lock);
    for (i = 0; i < CAPACITY; i++) {
        if (cache[i].used && cache[i].key == k) {
            ticks++;
            cache[i].tick = ticks;
            st = pc_surface_clone(&cache[i].surface, out, err);
            pthread_mutex_unlock(&lock);
            if (st == PC_OK) {
                *found = true;
            }
            return st;
        }
    }
    gen = generation;
    pthread_mutex_unlock(&lock);
    /* Never hold the cache lock while rasterising or feathering. */
    st = build(layer, canvas, vm, pixel, sv, sp, &s, err);
    if (st != PC_OK) {
        return st;
    }
    pthread_mutex_lock(&lock);
    for (i = 0; i < CAPACITY; i++) {
        if (cache[i].used && cache[i].key == k && gen == generation) {
            /* A concurrent build finished first: share its tiles. */
            pc_surface shared;
            if (pc_surface_clone(&cache[i].surface, &shared, NULL) == PC_OK) {
                ticks++;
                cache[i].tick = ticks;
                pthread_mutex_unlock(&lock);
                pc_surface_fini(&s);
                *out = shared;
                *found = true;
                return PC_OK;
            }
            break;
        }
    }
    {
        entry e;
        memset(&e, 0, sizeof e);
        if (pc_surface_clone(&s, &e.surface, NULL) == PC_OK) {
            e.used = true;
            e.key = k;
            if (layer->has_mask && pc_surface_clone(&layer->mask.surface, &e.pixel_input, NULL) == PC_OK) {
                e.has_input = true;
            }
            if (layer->has_mask && !e.has_input) {
                /* Without the pinned input the key could alias a future tile: don't cache. */
                entry_clear(&e);
            } else {
                e.bytes = entry_bytes(&e.surface, e.has_input ? &e.pixel_input : NULL);
                insert(&e, PC_COMPOSE_MASK_CACHE_BUDGET, gen);
            }
        }
    }
    pthread_mutex_unlock(&lock);
    *out = s;
    *found = true;
    return PC_OK;
}
