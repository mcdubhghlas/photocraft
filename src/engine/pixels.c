/* Engine-layer pixel helpers (port of crates/engine/src/pixels.rs).
 *
 * E1 ports remap_surface (flips/rotations, used by canvas_geom). E4 adds the destructive fill and
 * clear helpers (try_fill_surface / try_clear_surface) and their tile engine (edit_tiles), which
 * Edit > Fill, Clear, Stroke and the transform commands consume. The adjustment helpers
 * (adjust_surface, ...) port with E8, composite_layers / merge_down with the layer chunk (E5).
 *
 * edit_tiles works one tile at a time, decoding and re-encoding pixels exactly as a whole-region
 * float round trip would, and copies a tile (un-shares its reference-counted storage) only when a
 * pixel in it actually changes, so tiles the selection never reaches stay shared with the undo
 * history (#4517). The reference runs the tiles in parallel with rayon; this port runs them in
 * order, which gives identical pixels (each tile is independent) and is deterministic under any
 * thread count -- a perf-only difference tracked for the Z1 performance pass. */
#include <stdlib.h>
#include <string.h>

#include "allocation.h"
#include "base/mem.h"
#include "base/num.h"
#include "compose/adjust.h"   /* E8: destructive adjustments route through compose adjust */
#include "compose/compose.h" /* E5: composite_layers / merge_down */
#include "engine_internal.h"

pc_surface pc_engine_remap_surface(const pc_surface *s, void (*map)(int32_t, int32_t, int32_t *, int32_t *,
                                                                    void *),
                                   void *ctx, pc_error *err)
{
    pc_surface out;
    float def[PC_RASTER_MAX_CHANNELS];
    size_t n = pc_surface_channels(s);
    pc_surface_default_pixel(s, def);
    pc_surface_init_default(&out, pc_surface_format(s), def, n);
    pc_rect r = pc_surface_content_bounds(s);
    if (pc_rect_is_empty(r)) {
        return out;
    }
    float *raw = NULL;
    size_t len = 0;
    if (pc_surface_read_region(s, r, &raw, &len, err) != PC_OK) {
        return out; /* out is a valid empty surface; caller sees err */
    }
    size_t w = pc_rect_width(r);
    size_t count = len / (n == 0 ? 1 : n);
    for (size_t i = 0; i < count; i++) {
        int32_t x = r.x0 + (int32_t)(i % w);
        int32_t y = r.y0 + (int32_t)(i / w);
        int32_t nx, ny;
        map(x, y, &nx, &ny, ctx);
        (void)pc_surface_write_pixel(&out, nx, ny, &raw[i * n], n, NULL);
    }
    free(raw);
    return out;
}

/* ---- the tile engine (port of pixels.rs edit_tiles) --------------------------------------- */

/* A selection's coverage over one tile (its channel 0), as fill_cmds::Mask in the reference. */
typedef struct {
    const uint8_t *bytes; /* the selection's tile bytes, or its one default pixel */
    size_t len;           /* `bytes` length: bpp for a default pixel, a full tile otherwise */
    pc_pixel_format fmt;
    pc_rect origin; /* the tile's pixel rect */
    size_t bpp;
} pc_mask;

static pc_mask mask_of(const pc_surface *sel, pc_tile tc)
{
    pc_mask m;
    const pc_raster_tile *t = pc_surface_tile(sel, tc);
    m.fmt = pc_surface_format(sel);
    m.bpp = pc_pixel_format_bytes_per_pixel(m.fmt);
    m.origin = pc_tile_rect(tc);
    if (t != NULL) {
        m.bytes = pc_raster_tile_bytes(t);
        m.len = pc_raster_tile_len(t);
    } else {
        m.bytes = pc_surface_default_bytes(sel);
        m.len = m.bpp; /* a default-pixel mask is one pixel long: every position reads it */
    }
    return m;
}

static float mask_at(const pc_mask *m, int32_t x, int32_t y)
{
    size_t i;
    if (m->len == m->bpp) {
        i = 0;
    } else {
        size_t row = (size_t)(uint32_t)(y - m->origin.y0) * (size_t)PC_TILE_SIZE;
        i = (row + (size_t)(uint32_t)(x - m->origin.x0)) * m->bpp;
    }
    if (i + m->bpp > m->len) {
        return 0.0f;
    }
    return pc_read_sample(m->bytes + i, m->bpp, m->fmt.sample, 0);
}

/* Fully selected over r? */
static bool mask_full(const pc_mask *m, pc_rect r)
{
    for (int32_t y = r.y0; y < r.y1; y++) {
        for (int32_t x = r.x0; x < r.x1; x++) {
            if (mask_at(m, x, y) < 1.0f) {
                return false;
            }
        }
    }
    return true;
}

/* A whole-tile shortcut result (pixels.rs TileEdit). */
typedef enum { PC_TILE_EDIT_NONE, PC_TILE_EDIT_PUT, PC_TILE_EDIT_DROP } pc_tile_edit_kind;
typedef struct {
    pc_tile_edit_kind kind;
    const pc_raster_tile *tile; /* borrowed (retained by edit_tiles) for PUT */
} pc_tile_edit;

/* The "not enough memory while <purpose> (<detail>); the document was not changed" variant the
 * reference uses for a failed tile clone/default (distinct from allocation's "for" wording). */
static pc_status edit_oom(pc_error *err, const char *purpose)
{
    return pc_engine_err_other(err, "not enough memory while %s (out of memory); the document was not changed",
                               purpose);
}

static pc_status put_one_tile(pc_surface *s, pc_tile tc, pc_raster_tile *tile, pc_error *err)
{
    pc_tile_entry e = {tc, tile}; /* takes over the reference, even on failure */
    return pc_surface_put_tiles(s, &e, 1, err);
}

static void drop_one_tile(pc_surface *s, pc_tile tc)
{
    pc_tile_entry *taken = NULL;
    size_t nt = 0;
    if (pc_surface_take_tiles(s, pc_tile_rect(tc), &taken, &nt, NULL) == PC_OK) {
        pc_tile_entries_free(taken, nt); /* releasing them removes them from the surface */
    }
}

/* Edit `s` over `area` tile by tile. `whole` may handle a tile lying wholly inside `area` and
 * fully selected in one go; otherwise `pixel` edits each covered pixel (decoded channel values in
 * and out; false = left untouched). */
static pc_status edit_tiles(pc_surface *s, pc_rect area, const pc_surface *selection, const char *purpose,
                            pc_tile_edit (*whole)(bool full, void *ctx), void *whole_ctx,
                            bool (*pixel)(float k, float *px, size_t n, void *ctx), void *pixel_ctx,
                            pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(s);
    size_t n = pc_pixel_format_channels(fmt);
    size_t bpp = pc_pixel_format_bytes_per_pixel(fmt);
    pc_tile first, last;
    if (!pc_rect_tile_range(area, &first, &last)) {
        return PC_OK; /* empty area */
    }
    uint64_t cols = (uint64_t)((int64_t)last.tx - (int64_t)first.tx) + 1;
    uint64_t rows = (uint64_t)((int64_t)last.ty - (int64_t)first.ty) + 1;
    uint64_t count64 = cols * rows;
    if (count64 > SIZE_MAX / sizeof(pc_tile)) {
        return pc_engine_alloc_error(err, purpose, "requested size overflowed");
    }
    size_t count = (size_t)count64;
    pc_tile *coords = NULL;
    pc_status st = pc_engine_alloc_capacity(count, sizeof(pc_tile), purpose, (void **)&coords, err);
    if (st != PC_OK) {
        return st;
    }
    size_t idx = 0;
    for (int64_t ty = first.ty; ty <= last.ty; ty++) {
        for (int64_t tx = first.tx; tx <= last.tx; tx++) {
            coords[idx].tx = (int32_t)tx;
            coords[idx].ty = (int32_t)ty;
            idx++;
        }
    }
    /* Untouched pixels read as the default where the surface has no tile. */
    pc_raster_tile *blank = NULL;
    for (size_t i = 0; i < count; i++) {
        if (pc_surface_tile(s, coords[i]) == NULL) {
            if (pc_surface_default_tile(s, &blank, err) != PC_OK) {
                free(coords);
                return edit_oom(err, purpose);
            }
            break;
        }
    }
    st = PC_OK;
    for (size_t i = 0; i < count && st == PC_OK; i++) {
        pc_tile tc = coords[i];
        pc_rect trect = pc_tile_rect(tc);
        pc_rect tr = pc_rect_intersect(trect, area);
        bool have_mask = selection != NULL;
        pc_mask mask;
        if (have_mask) {
            mask = mask_of(selection, tc);
        }
        bool full = pc_rect_eq(tr, trect) && (!have_mask || mask_full(&mask, tr));
        const pc_raster_tile *current = pc_surface_tile(s, tc);
        pc_tile_edit te = whole(full, whole_ctx);
        if (te.kind == PC_TILE_EDIT_PUT) {
            st = put_one_tile(s, tc, pc_raster_tile_retain((pc_raster_tile *)(uintptr_t)te.tile), err);
            continue;
        }
        if (te.kind == PC_TILE_EDIT_DROP) {
            if (current != NULL) {
                drop_one_tile(s, tc);
            }
            continue;
        }
        const pc_raster_tile *src = current != NULL ? current : blank;
        if (src == NULL) {
            continue; /* no tile and no blank: nothing to read (all coords had tiles) */
        }
        const uint8_t *src_bytes = pc_raster_tile_bytes(src);
        size_t src_len = pc_raster_tile_len(src);
        pc_raster_tile *out_tile = NULL;
        uint8_t *out_bytes = NULL;
        for (int32_t y = tr.y0; y < tr.y1 && st == PC_OK; y++) {
            size_t row = (size_t)(uint32_t)(y - trect.y0) * (size_t)PC_TILE_SIZE;
            for (int32_t x = tr.x0; x < tr.x1; x++) {
                size_t off = (row + (size_t)(uint32_t)(x - trect.x0)) * bpp;
                if (off + bpp > src_len) {
                    continue;
                }
                const uint8_t *old = src_bytes + off;
                float px[PC_RASTER_MAX_CHANNELS];
                for (size_t c = 0; c < n; c++) {
                    px[c] = pc_read_sample(old, bpp, fmt.sample, c);
                }
                float k = have_mask ? mask_at(&mask, x, y) : 1.0f;
                if (!pixel(k, px, n, pixel_ctx)) {
                    continue;
                }
                uint8_t enc[PC_RASTER_MAX_PIXEL_BYTES];
                pc_encode_pixel(fmt, px, n, enc);
                if (memcmp(enc, old, bpp) == 0) {
                    continue;
                }
                if (out_tile == NULL) {
                    out_tile = pc_raster_tile_retain((pc_raster_tile *)(uintptr_t)src);
                    out_bytes = pc_raster_tile_make_mut(&out_tile);
                    if (out_bytes == NULL) {
                        pc_raster_tile_release(out_tile);
                        out_tile = NULL;
                        st = edit_oom(err, purpose);
                        break;
                    }
                }
                memcpy(out_bytes + off, enc, bpp);
            }
        }
        if (out_tile != NULL && st == PC_OK) {
            st = put_one_tile(s, tc, out_tile, err); /* consumes the reference */
        } else {
            pc_raster_tile_release(out_tile);
        }
    }
    pc_raster_tile_release(blank);
    free(coords);
    return st;
}

/* ---- fill (port of pixels.rs try_fill_surface / fill_pixel) ------------------------------- */

/* One pixel of try_fill_surface: `color` over the decoded samples `px` (n of them) at coverage k. */
static void fill_pixel(pc_pixel_format fmt, float *px, size_t n, const float color[4], float k,
                       bool lock_transparency)
{
    float d[4];
    pc_to_rgba(fmt, px, n, d);
    float sa = color[3] * k;
    float oa = sa + d[3] * (1.0f - sa);
    float o[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (oa > 0.0f) {
        for (int c = 0; c < 3; c++) {
            o[c] = (color[c] * sa + d[c] * d[3] * (1.0f - sa)) / oa;
        }
    }
    o[3] = lock_transparency ? d[3] : oa;
    float enc[PC_RASTER_MAX_CHANNELS];
    size_t m = pc_from_rgba(fmt, o, enc);
    size_t count = n < m ? n : m;
    for (size_t i = 0; i < count; i++) {
        px[i] = enc[i];
    }
}

typedef struct {
    const pc_raster_tile *solid; /* NULL unless the fill is an opaque colour that may replace alpha */
} fill_whole_ctx;

static pc_tile_edit fill_whole(bool full, void *ctx)
{
    fill_whole_ctx *c = ctx;
    pc_tile_edit e = {PC_TILE_EDIT_NONE, NULL};
    if (c->solid != NULL && full) {
        e.kind = PC_TILE_EDIT_PUT;
        e.tile = c->solid;
    }
    return e;
}

typedef struct {
    pc_pixel_format fmt;
    float color[4];
    bool lock_transparency;
} fill_pixel_ctx;

static bool fill_pixel_cb(float k, float *px, size_t n, void *ctx)
{
    fill_pixel_ctx *c = ctx;
    if (k <= 0.0f) {
        return false;
    }
    fill_pixel(c->fmt, px, n, c->color, k, c->lock_transparency);
    return true;
}

pc_status pc_engine_try_fill_surface(pc_surface *s, pc_rect area, const float color[4],
                                     const pc_surface *selection, bool lock_transparency, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(s);
    /* Coverage 1 of an opaque colour replaces the colour (and the alpha, unless it is kept). An
     * F32 selection can carry fractional coverage the whole-tile shortcut cannot honour. */
    bool sel_not_f32 = selection == NULL || pc_surface_format(selection).sample != PC_SAMPLE_TYPE_F32;
    bool opaque = color[3] == 1.0f && (!lock_transparency || !fmt.alpha) && sel_not_f32;
    fill_whole_ctx wc = {NULL};
    pc_raster_tile *solid = NULL;
    if (opaque) {
        float rgba[4] = {color[0], color[1], color[2], 1.0f};
        float enc[PC_RASTER_MAX_CHANNELS];
        size_t m = pc_from_rgba(fmt, rgba, enc);
        if (pc_surface_solid_tile(s, enc, m, &solid, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        wc.solid = solid;
    }
    fill_pixel_ctx pc = {fmt, {color[0], color[1], color[2], color[3]}, lock_transparency};
    pc_status st = edit_tiles(s, area, selection, "filling pixels", fill_whole, &wc, fill_pixel_cb, &pc, err);
    pc_raster_tile_release(solid);
    return st;
}

/* ---- clear (port of pixels.rs try_clear_surface) ------------------------------------------ */

typedef struct {
    bool empty_default;
} clear_whole_ctx;

static pc_tile_edit clear_whole(bool full, void *ctx)
{
    clear_whole_ctx *c = ctx;
    pc_tile_edit e = {PC_TILE_EDIT_NONE, NULL};
    if (full && c->empty_default) {
        e.kind = PC_TILE_EDIT_DROP;
    }
    return e;
}

static bool clear_pixel_cb(float k, float *px, size_t n, void *ctx)
{
    (void)ctx;
    if (n == 0) {
        return false;
    }
    px[n - 1] *= 1.0f - k;
    if (px[n - 1] <= 0.0f) {
        for (size_t i = 0; i < n; i++) {
            px[i] = 0.0f;
        }
    }
    return true;
}

pc_status pc_engine_try_clear_surface(pc_surface *s, pc_rect area, const pc_surface *selection, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(s);
    if (!fmt.alpha) {
        return PC_OK;
    }
    size_t bpp = pc_pixel_format_bytes_per_pixel(fmt);
    const uint8_t *db = pc_surface_default_bytes(s);
    clear_whole_ctx wc = {true};
    for (size_t i = 0; i < bpp; i++) {
        if (db[i] != 0) {
            wc.empty_default = false;
            break;
        }
    }
    pc_status st = edit_tiles(s, area, selection, "erasing pixels", clear_whole, &wc, clear_pixel_cb, NULL, err);
    if (st == PC_OK) {
        pc_surface_prune(s);
    }
    return st;
}

/* ---- E5: compositing layers (port of pixels::composite_layers / merge_down) ---------------- */

/* Composite the visible layers of `solo` inside their visible composite bounds (effects included)
 * and write the result into a tiled surface. `background` is three channels or NULL. On OOM
 * returns a valid empty surface in *out and fills err. */
pc_status pc_engine_composite_layers(const pc_document *solo, pc_pixel_format fmt,
                                     const float *background, pc_surface *out, pc_error *err)
{
    pc_rect canvas = pc_document_bounds(solo);
    pc_rect area = PC_RECT_EMPTY;
    for (size_t i = 0; i < solo->nlayers; i++) {
        const pc_layer *layer = &solo->layers[i];
        if (!layer->visible) {
            continue;
        }
        /* Composite bounds include layer effects (a drop shadow reaches past the pixels); false
         * (Rust None) means the layer may draw anywhere, so use the whole canvas. */
        pc_rect b;
        if (!pc_compose_composite_bounds(layer, canvas, &b)) {
            b = canvas;
        }
        if (!pc_rect_is_empty(b)) {
            area = pc_rect_is_empty(area) ? b : pc_rect_union(area, b);
        }
    }
    area = pc_rect_intersect(area, canvas);
    if (pc_rect_is_empty(area)) {
        pc_surface_init(out, fmt);
        return PC_OK;
    }
    return pc_compose_composite_to_surface(solo, area, fmt, background, out, err);
}

/* Merge `upper` onto `lower` into one raster layer (Merge Down). The pair is composited through
 * the document's own profile; the result keeps the lower layer's id, blend mode and locks at full
 * opacity. On error *out is untouched. */
pc_status pc_engine_merge_down(const pc_document *doc, const pc_layer *lower, const pc_layer *upper,
                               pc_layer *out, pc_error *err)
{
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    pc_document tmp;
    pc_status rc = pc_document_init(&tmp, "merge", doc->size, fmt.mode, fmt.sample, err);
    if (rc != PC_OK) {
        return rc;
    }
    tmp.icc_profile = pc_blob_retain(doc->icc_profile);
    pc_layer *stack = calloc(2, sizeof *stack);
    if (stack == NULL) {
        pc_document_fini(&tmp);
        return pc_fail(err, PC_ERR_NOMEM, "merge down");
    }
    if ((rc = pc_layer_clone(lower, &stack[0], err)) != PC_OK) {
        free(stack);
        pc_document_fini(&tmp);
        return rc;
    }
    if ((rc = pc_layer_clone(upper, &stack[1], err)) != PC_OK) {
        pc_layer_fini(&stack[0]);
        free(stack);
        pc_document_fini(&tmp);
        return rc;
    }
    tmp.layers = stack;
    tmp.nlayers = 2;
    pc_surface s;
    rc = pc_engine_composite_layers(&tmp, fmt, NULL, &s, err);
    pc_document_fini(&tmp);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer merged;
    pc_layer_content content;
    content.kind = PC_LAYER_RASTER;
    content.u.raster = s; /* moved in */
    rc = pc_layer_init(&merged, pc_str_c(&lower->name), &content, err);
    if (rc != PC_OK) {
        pc_surface_fini(&s);
        return rc;
    }
    merged.id = lower->id;
    merged.blend = lower->blend;
    merged.opacity = 1.0f;
    /* Merge Down replaces the lower layer, so keep its editing restrictions (a merged Background
     * must remain opaque and position-locked). */
    merged.locks = lower->locks;
    *out = merged;
    return PC_OK;
}

/* ---- E8: destructive adjustments (port of pixels.rs adjust_surface / adjust_mask / mix) ----- *
 *
 * §7 deviation: the Rust `adjusted` has an exact stored-channel path for Lab/CMYK layers whose
 * adjustment Photoshop evaluates on the document's own channels (Invert/Posterize/Threshold); here
 * every mode routes through the compose RGBA buffer (pc_compose_adjust_apply_in itself reconstructs
 * the Lab/CMYK channels from that buffer). Values match for RGB documents (the bit-asserted case);
 * Lab/CMYK results take one extra RGBA round trip (the Lab/CMYK goldens are not bit-assertable yet,
 * REWRITE_PLAN.md §8 cms DotGain gap). The Rust runs the tiles in parallel for memory; this port
 * works a region in one pass, which gives identical pixels (perf-only, tracked for Z1). */

pc_status pc_engine_mix_through_selection(pc_pixel_format fmt, pc_rect r, const float (*orig)[4],
                                          const float (*edited)[4], size_t count,
                                          const pc_surface *selection, float **out, size_t *out_len,
                                          pc_error *err)
{
    size_t n = pc_pixel_format_channels(fmt);
    size_t w = (size_t)pc_rect_width(r);
    if (w == 0) {
        w = 1;
    }
    if (count == 0 || n == 0) {
        *out = NULL;
        *out_len = 0;
        return PC_OK;
    }
    size_t total;
    if (!pc_size_mul(count, n, &total)) {
        return pc_fail(err, PC_ERR_LIMIT, "mix selection size overflow");
    }
    float *res = pc_malloc_array(total, sizeof *res);
    if (res == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "mix selection");
    }
    float *coverage = pc_malloc_array(w, sizeof *coverage);
    if (coverage == NULL) {
        free(res);
        return pc_fail(err, PC_ERR_NOMEM, "mix selection coverage");
    }
    size_t rows = count / w;
    size_t wi = 0; /* write index into res */
    for (size_t j = 0; j < rows; j++) {
        if (selection != NULL) {
            pc_surface_sample_row_strided(selection, r.y0 + (int32_t)j, r.x0, 1, 0, coverage, w);
        } else {
            for (size_t x = 0; x < w; x++) {
                coverage[x] = 1.0f;
            }
        }
        for (size_t x = 0; x < w; x++) {
            size_t idx = j * w + x;
            float k = coverage[x];
            float mixed[4];
            for (int c = 0; c < 4; c++) {
                mixed[c] = orig[idx][c] + (edited[idx][c] - orig[idx][c]) * k;
            }
            float enc[PC_RASTER_MAX_CHANNELS];
            size_t m = pc_from_rgba(fmt, mixed, enc);
            for (size_t c = 0; c < m && wi < total; c++) {
                res[wi++] = enc[c];
            }
        }
    }
    free(coverage);
    *out = res;
    *out_len = wi;
    return PC_OK;
}

/* `s`'s pixels over `r` with `adj` applied through the selection, encoded into *out (owned, free()).
 * pixels.rs::adjusted. On OOM fills err and leaves *out NULL. */
static pc_status adjusted_region(const pc_surface *s, pc_rect r, const pc_adjustment *adj,
                                 const pc_surface *selection, pc_color_mode mode, float **out,
                                 size_t *out_len, pc_error *err)
{
    *out = NULL;
    *out_len = 0;
    pc_pixel_format fmt = pc_surface_format(s);
    size_t n = pc_pixel_format_channels(fmt);
    float *raw = NULL;
    size_t len = 0;
    if (pc_surface_read_region(s, r, &raw, &len, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    size_t count = (n == 0) ? 0 : len / n;
    if (count == 0) {
        free(raw);
        return PC_OK;
    }
    float(*orig)[4] = pc_malloc_array(count, sizeof *orig);
    float(*edited)[4] = pc_malloc_array(count, sizeof *edited);
    if (orig == NULL || edited == NULL) {
        free(raw);
        free(orig);
        free(edited);
        return pc_fail(err, PC_ERR_NOMEM, "adjust region");
    }
    for (size_t i = 0; i < count; i++) {
        pc_to_rgba(fmt, raw + i * n, n, orig[i]);
        for (int c = 0; c < 4; c++) {
            edited[i][c] = orig[i][c];
        }
    }
    free(raw);
    pc_compose_buffer buf = {r, edited, count};
    pc_compose_transfer transfer = pc_compose_transfer_for_document(mode, fmt.sample);
    bool has_depth = fmt.sample == PC_SAMPLE_TYPE_F32;
    /* Best-effort: apply_in may fail only on OOM, leaving buf partly adjusted; the caller then
     * writes what it has (matches the reference, which cannot fail here). */
    (void)pc_compose_adjust_apply_in(adj, &buf, transfer, has_depth, fmt.sample, mode, err);
    pc_status rc = pc_engine_mix_through_selection(fmt, r, (const float(*)[4])orig,
                                                   (const float(*)[4])edited, count, selection, out,
                                                   out_len, err);
    free(orig);
    free(edited);
    return rc;
}

void pc_engine_adjust_surface(pc_surface *s, const pc_adjustment *adj, const pc_surface *selection,
                              pc_color_mode mode)
{
    pc_rect r = pc_surface_content_bounds(s);
    if (pc_rect_is_empty(r)) {
        return;
    }
    float *data = NULL;
    size_t len = 0;
    if (adjusted_region(s, r, adj, selection, mode, &data, &len, NULL) != PC_OK || data == NULL) {
        free(data);
        return;
    }
    (void)pc_surface_write_region(s, r, data, len, NULL);
    free(data);
}

void pc_engine_adjust_mask(pc_surface *s, const pc_adjustment *adj, const pc_surface *selection)
{
    pc_color_mode mode = PC_COLOR_MODE_GRAYSCALE;
    if (selection != NULL) {
        pc_rect r = pc_surface_content_bounds(selection);
        if (pc_rect_is_empty(r)) {
            return;
        }
        float *data = NULL;
        size_t len = 0;
        if (adjusted_region(s, r, adj, selection, mode, &data, &len, NULL) == PC_OK && data != NULL) {
            (void)pc_surface_write_region(s, r, data, len, NULL);
            pc_surface_prune(s);
        }
        free(data);
        return;
    }
    pc_pixel_format fmt = pc_surface_format(s);
    size_t n = pc_surface_channels(s);
    /* The adjusted default value: a one-pixel surface whose default is the mask's, adjusted over a
     * unit rect (so a reveal-all mask's background can flip to hide-all). */
    float def[PC_RASTER_MAX_CHANNELS];
    pc_surface_default_pixel(s, def);
    pc_surface unit_src;
    pc_surface_init_default(&unit_src, fmt, def, n);
    pc_rect unit = pc_rect_make(0, 0, 1, 1);
    float *def_enc = NULL;
    size_t def_len = 0;
    if (adjusted_region(&unit_src, unit, adj, NULL, mode, &def_enc, &def_len, NULL) != PC_OK ||
        def_enc == NULL) {
        pc_surface_fini(&unit_src);
        free(def_enc);
        return;
    }
    pc_surface_fini(&unit_src);
    pc_surface out;
    pc_surface_init_default(&out, fmt, def_enc, def_len);
    free(def_enc);
    pc_rect r = pc_surface_tile_bounds(s);
    if (!pc_rect_is_empty(r)) {
        float *data = NULL;
        size_t len = 0;
        if (adjusted_region(s, r, adj, NULL, mode, &data, &len, NULL) == PC_OK && data != NULL) {
            (void)pc_surface_write_region(&out, r, data, len, NULL);
        }
        free(data);
    }
    pc_surface_prune(&out);
    pc_surface_fini(s);
    *s = out;
}
