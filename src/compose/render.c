/* Top-level rendering (port of the rendering half of crates/compose/src/lib.rs): the render
 * context, tiled parallel rendering, bands, flatten to a surface, single layers, thumbnails.
 *
 * Tiles render on pc_pool_global(), each tile on one thread from the root of the layer tree, so
 * results don't depend on the thread count (D6). Every tile enters the document's CMYK space and
 * sets the Lab / HDR blend flags on its own thread (they are per thread). */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "base/rmath.h"
#include "compose/internal.h"

/* ---- context ---- */

static void vmasks_init(pc_cmp_vmasks *v)
{
    memset(v, 0, sizeof *v);
    pthread_mutex_init(&v->lock, NULL);
}

static void vmasks_fini(pc_cmp_vmasks *v)
{
    size_t i;
    for (i = 0; i < v->len; i++) {
        pc_vec_compiled_mask_fini(&v->slots[i]->mask);
        free(v->slots[i]);
    }
    free(v->slots);
    pthread_mutex_destroy(&v->lock);
    memset(v, 0, sizeof *v);
}

static pc_status ctx_init(pc_cmp_ctx_store *s, const pc_pattern *patterns, size_t npatterns, pc_error *err)
{
    pc_status st;
    memset(s, 0, sizeof *s);
    st = pc_compose_patterns_new(patterns, npatterns, PC_COMPOSE_PREPARED_PATTERN_BYTES, &s->patterns, err);
    if (st != PC_OK) {
        return st;
    }
    vmasks_init(&s->vmasks);
    pc_cmp_fx_local_init(&s->fx);
    s->cx.patterns = s->patterns;
    s->cx.vmasks = &s->vmasks;
    s->cx.fx = &s->fx;
    return PC_OK;
}

pc_status pc_cmp_ctx_init_doc(pc_cmp_ctx_store *s, const pc_document *doc, const pc_interrupt *intr,
                              pc_error *err)
{
    pc_status st = ctx_init(s, doc->patterns, doc->npatterns, err);
    if (st != PC_OK) {
        return st;
    }
    s->cx.doc = doc;
    s->cx.canvas = pc_document_bounds(doc);
    s->cx.transfer = pc_compose_transfer_for_document(doc->mode, doc->depth);
    s->cx.light = doc->global_light;
    s->cx.mode = doc->mode;
    s->cx.depth = doc->depth;
    s->cx.intr = intr;
    return PC_OK;
}

pc_status pc_cmp_ctx_init_layer(pc_cmp_ctx_store *s, pc_rect canvas, pc_error *err)
{
    pc_status st = ctx_init(s, NULL, 0, err);
    if (st != PC_OK) {
        return st;
    }
    s->cx.canvas = canvas;
    s->cx.transfer = (pc_compose_transfer){PC_COMPOSE_TRANSFER_SRGB, 0.0f};
    s->cx.light = pc_global_light_default();
    s->cx.mode = PC_COLOR_MODE_RGB;
    s->cx.depth = PC_SAMPLE_TYPE_F32;
    return PC_OK;
}

void pc_cmp_ctx_fini(pc_cmp_ctx_store *s)
{
    pc_compose_patterns_free(s->patterns);
    vmasks_fini(&s->vmasks);
    pc_cmp_fx_local_fini(&s->fx);
    memset(s, 0, sizeof *s);
}

pc_cmyk_space *pc_compose_cmyk_space(const pc_document *doc)
{
    if (doc->mode != PC_COLOR_MODE_CMYK) {
        return NULL;
    }
    return pc_cmyk_space_for_profile(pc_blob_data(doc->icc_profile), pc_blob_len(doc->icc_profile));
}

/* ---- one tile ---- */

/* The whole tree over rect on this thread, inside the document's colour setting. */
static pc_status render_one(const pc_document *doc, pc_rect rect, const pc_cmp_ctx *cx, pc_cmyk_space *cmyk,
                            pc_compose_buffer *out, pc_error *err)
{
    pc_cmyk_space *prev = pc_cmyk_space_enter(cmyk);
    pc_compose_buffer b;
    pc_status st = pc_cmp_backdrop(doc, rect, &b, err);
    if (st == PC_OK) {
        /* Lab documents mix Normal blending in CIELAB; 32-bit documents don't clip Add / Divide. */
        pc_psblend_set_lab_mix(doc->mode == PC_COLOR_MODE_LAB);
        pc_psblend_set_hdr(doc->depth == PC_SAMPLE_TYPE_F32);
        st = pc_cmp_composite_stack_at(doc->layers, doc->nlayers, &b, cx, PC_CMP_SCOPE_ROOT, err);
        pc_psblend_set_lab_mix(false);
        pc_psblend_set_hdr(false);
        if (st != PC_OK) {
            pc_compose_buffer_fini(&b);
        }
    }
    pc_cmyk_space_leave(prev);
    if (st == PC_OK) {
        *out = b;
    }
    return st;
}

typedef struct tiles_job {
    const pc_document *doc;
    const pc_cmp_ctx *cx;
    pc_cmyk_space *cmyk;
    pc_rect rect;
    int32_t tile;
    size_t across;
    pc_compose_buffer *out;
    pthread_mutex_t lock;
    size_t err_index;
    pc_error err;
} tiles_job;

static pc_rect tile_rect(const tiles_job *j, size_t idx)
{
    size_t bx = idx % j->across;
    size_t by = idx / j->across;
    int32_t x0 = (int32_t)((int64_t)j->rect.x0 + (int64_t)bx * j->tile);
    int32_t y0 = (int32_t)((int64_t)j->rect.y0 + (int64_t)by * j->tile);
    int32_t x1 = pc_i32_sat_add(x0, j->tile);
    int32_t y1 = pc_i32_sat_add(y0, j->tile);
    return pc_rect_make(x0, y0, x1 < j->rect.x1 ? x1 : j->rect.x1, y1 < j->rect.y1 ? y1 : j->rect.y1);
}

static pc_status tiles_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    tiles_job *j = ctx;
    size_t idx;
    size_t w = pc_cmp_w(j->rect);
    (void)chunk;
    for (idx = begin; idx < end; idx++) {
        pc_rect t = tile_rect(j, idx);
        pc_compose_buffer part;
        pc_error e;
        size_t pw = pc_cmp_w(t);
        size_t row;
        pc_status st;
        pc_error_clear(&e);
        st = render_one(j->doc, t, j->cx, j->cmyk, &part, &e);
        if (st != PC_OK) {
            /* Keep the error of the lowest failing tile (pc_pool_for reports its status). */
            pthread_mutex_lock(&j->lock);
            if (idx < j->err_index) {
                j->err_index = idx;
                j->err = e;
            }
            pthread_mutex_unlock(&j->lock);
            return st;
        }
        for (row = 0; row < part.len / (pw > 0 ? pw : 1); row++) {
            size_t o = ((size_t)(t.y0 - j->rect.y0) + row) * w + (size_t)(t.x0 - j->rect.x0);
            memcpy(j->out->px + o, part.px + row * pw, pw * sizeof *part.px);
        }
        pc_compose_buffer_fini(&part);
    }
    return PC_OK;
}

static void copy_err(pc_error *dst, const pc_error *src)
{
    if (dst != NULL) {
        *dst = *src;
    }
}

/* render_tiled_with: rect in tiles of `tile` with an existing context. */
static pc_status render_tiled_with(const pc_document *doc, pc_rect rect, int32_t tile, const pc_cmp_ctx *cx,
                                   pc_compose_buffer *out, pc_error *err)
{
    pc_cmyk_space *cmyk;
    pc_compose_buffer buf;
    tiles_job j;
    size_t down;
    size_t n;
    pc_status st;
    if (tile < 1) {
        tile = 1;
    }
    if (pc_rect_is_empty(rect)) {
        return pc_compose_buffer_transparent(rect, out, err);
    }
    st = pc_cmp_rect_pixels(rect, &n, err);
    if (st != PC_OK) {
        return st;
    }
    /* CMYK layers are read through the document's own CMYK profile (per-thread scope). */
    cmyk = pc_compose_cmyk_space(doc);
    if (pc_u32_as_i32(pc_rect_width(rect)) <= tile && pc_u32_as_i32(pc_rect_height(rect)) <= tile) {
        st = render_one(doc, rect, cx, cmyk, out, err);
        pc_cmyk_space_release(cmyk);
        return st;
    }
    /* Effect maps are built once, here, before any tile needs them. */
    st = pc_cmp_prepare_effects(doc->layers, doc->nlayers, rect, cx, cmyk, doc->mode == PC_COLOR_MODE_LAB,
                                doc->depth == PC_SAMPLE_TYPE_F32, err);
    if (st == PC_OK) {
        st = pc_compose_buffer_transparent(rect, &buf, err);
    }
    if (st != PC_OK) {
        pc_cmyk_space_release(cmyk);
        return st;
    }
    memset(&j, 0, sizeof j);
    j.doc = doc;
    j.cx = cx;
    j.cmyk = cmyk;
    j.rect = rect;
    j.tile = tile;
    j.across = (pc_cmp_w(rect) + (size_t)tile - 1) / (size_t)tile;
    down = ((size_t)pc_rect_height(rect) + (size_t)tile - 1) / (size_t)tile;
    j.out = &buf;
    j.err_index = SIZE_MAX;
    pthread_mutex_init(&j.lock, NULL);
    /* Tiles are written straight into the output, so the peak is the output plus the tiles in
     * flight. */
    st = pc_pool_for(pc_pool_global(), j.across * down, 1, tiles_chunk, &j);
    pthread_mutex_destroy(&j.lock);
    pc_cmyk_space_release(cmyk);
    if (st != PC_OK) {
        copy_err(err, &j.err);
        pc_compose_buffer_fini(&buf);
        return st;
    }
    *out = buf;
    return PC_OK;
}

pc_status pc_compose_render_tiled(const pc_document *doc, pc_rect rect, int32_t tile,
                                  const pc_interrupt *intr, pc_compose_buffer *out, pc_error *err)
{
    pc_cmp_ctx_store s;
    pc_status st = pc_cmp_ctx_init_doc(&s, doc, intr, err);
    if (st != PC_OK) {
        return st;
    }
    st = render_tiled_with(doc, rect, tile, &s.cx, out, err);
    pc_cmp_ctx_fini(&s);
    return st;
}

pc_status pc_compose_render(const pc_document *doc, pc_rect rect, pc_compose_buffer *out, pc_error *err)
{
    return pc_compose_render_tiled(doc, rect, PC_COMPOSE_RENDER_TILE, NULL, out, err);
}

pc_status pc_compose_flatten(const pc_document *doc, pc_compose_buffer *out, pc_error *err)
{
    return pc_compose_render(doc, pc_document_bounds(doc), out, err);
}

/* ---- bands ---- */

int32_t pc_compose_band_rows_for(uint32_t width, int32_t requested)
{
    int64_t want = requested > 0 ? (int64_t)requested
                                 : (int64_t)(PC_COMPOSE_BAND_PIXELS / (uint64_t)(width > 1 ? width : 1));
    int64_t tiles = (want + PC_COMPOSE_RENDER_TILE - 1) / PC_COMPOSE_RENDER_TILE;
    return (int32_t)(pc_clamp_i64(tiles, 1, (int64_t)1 << 16) * PC_COMPOSE_RENDER_TILE);
}

pc_status pc_compose_render_bands(const pc_document *doc, pc_rect rect, int32_t band_rows,
                                  const pc_interrupt *intr, pc_compose_band_sink sink, void *ctx,
                                  pc_error *err)
{
    pc_cmp_ctx_store s;
    int32_t rows;
    int32_t y;
    pc_status st;
    if (pc_rect_is_empty(rect)) {
        return PC_OK;
    }
    /* Keep prepared patterns and compiled vector masks across bands. */
    st = pc_cmp_ctx_init_doc(&s, doc, intr, err);
    if (st != PC_OK) {
        return st;
    }
    rows = pc_compose_band_rows_for(pc_rect_width(rect), band_rows);
    y = rect.y0;
    while (st == PC_OK && y < rect.y1) {
        int32_t y1 = pc_i32_sat_add(y, rows);
        pc_compose_buffer band;
        if (y1 > rect.y1) {
            y1 = rect.y1;
        }
        st = render_tiled_with(doc, pc_rect_make(rect.x0, y, rect.x1, y1), PC_COMPOSE_RENDER_TILE, &s.cx,
                               &band, err);
        if (st == PC_OK) {
            st = sink(ctx, &band, err);
            pc_compose_buffer_fini(&band);
        }
        y = y1;
    }
    pc_cmp_ctx_fini(&s);
    return st;
}

typedef struct flat_job {
    pc_surface *s;
    pc_pixel_format fmt;
    const float *bg;
    float *data;
    size_t cap;
} flat_job;

static pc_status flat_sink(void *ctx, pc_compose_buffer *band, pc_error *err)
{
    flat_job *j = ctx;
    size_t n = pc_pixel_format_channels(j->fmt);
    size_t total;
    size_t i;
    if (j->bg != NULL) {
        pc_compose_buffer_over_background(band, j->bg);
    }
    if (!pc_size_mul(band->len, n, &total)) {
        return pc_fail(err, PC_ERR_LIMIT, "compose: band too large");
    }
    if (total == 0) {
        return PC_OK; /* an empty band */
    }
    if (total > j->cap) {
        float *d = realloc(j->data, total * sizeof *d);
        if (d == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
        }
        j->data = d;
        j->cap = total;
    }
    for (i = 0; i < band->len; i++) {
        float px[PC_RASTER_MAX_CHANNELS];
        size_t c = pc_from_rgba(j->fmt, band->px[i], px);
        memcpy(j->data + i * n, px, (c < n ? c : n) * sizeof *px);
    }
    return pc_surface_write_region(j->s, band->rect, j->data, total, err);
}

pc_status pc_compose_flatten_to_surface(const pc_document *doc, pc_pixel_format fmt, const float *background,
                                        pc_surface *out, pc_error *err)
{
    pc_surface s;
    flat_job j;
    pc_status st;
    pc_surface_init(&s, fmt);
    memset(&j, 0, sizeof j);
    j.s = &s;
    j.fmt = fmt;
    j.bg = background;
    st = pc_compose_render_bands(doc, pc_document_bounds(doc), 0, NULL, flat_sink, &j, err);
    free(j.data);
    if (st != PC_OK) {
        pc_surface_fini(&s);
        return st;
    }
    pc_surface_prune(&s);
    *out = s;
    return PC_OK;
}

/* ---- single layers ---- */

pc_status pc_compose_render_layer(const pc_layer *layer, pc_rect rect, pc_compose_buffer *out, pc_error *err)
{
    pc_cmp_ctx_store s;
    pc_compose_buffer buf;
    pc_status st = pc_compose_buffer_transparent(rect, &buf, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_cmp_ctx_init_layer(&s, rect, err);
    if (st == PC_OK) {
        st = pc_cmp_composite_stack_at(layer, 1, &buf, &s.cx, PC_CMP_SCOPE_ROOT, err);
        pc_cmp_ctx_fini(&s);
    }
    if (st != PC_OK) {
        pc_compose_buffer_fini(&buf);
        return st;
    }
    *out = buf;
    return PC_OK;
}

/* ---- thumbnails ---- */

pc_status pc_compose_thumbnail_buffer(const pc_document *doc, uint32_t max_side, pc_compose_buffer *out,
                                      pc_error *err)
{
    pc_rect b = pc_document_bounds(doc);
    uint32_t bw = pc_rect_width(b);
    uint32_t bh = pc_rect_height(b);
    uint32_t longest = bw > bh ? bw : bh;
    float scale;
    uint32_t w;
    uint32_t h;
    uint32_t div;
    uint32_t k;
    if (longest < 1) {
        longest = 1;
    }
    scale = pc_rs_minf_k((float)max_side / (float)longest, 1.0f);
    w = pc_f32_to_u32(roundf((float)bw * scale));
    h = pc_f32_to_u32(roundf((float)bh * scale));
    w = w > 1 ? w : 1;
    h = h > 1 ? h : 1;
    div = max_side > 1 ? max_side : 1;
    div = div > UINT32_MAX / 2 ? UINT32_MAX : div * 2;
    k = longest / div;
    if (k >= 2 && pc_size_area(doc->size) > PC_COMPOSE_PROXY_THUMBNAIL_PIXELS &&
        pc_compose_proxy_faithful(doc)) {
        pc_document p;
        pc_status st = pc_compose_proxy_document(doc, k, &p, err);
        if (st != PC_OK) {
            return st;
        }
        st = pc_compose_render_reduced(&p, w, h, out, err);
        pc_document_fini(&p);
        return st;
    }
    return pc_compose_render_reduced(doc, w, h, out, err);
}

pc_status pc_compose_thumbnail(const pc_document *doc, uint32_t max_side, pc_rgba8_image *out, pc_error *err)
{
    pc_compose_buffer b;
    pc_status st = pc_compose_thumbnail_buffer(doc, max_side, &b, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_compose_buffer_to_rgba8(&b, out, err);
    pc_compose_buffer_fini(&b);
    return st;
}
