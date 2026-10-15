/* Edit > Fill (port of crates/engine/src/fill_cmds.rs, E4).
 *
 * Contents: Foreground / Background / Color / Content-Aware / Pattern / History / Black /
 * 50% Gray / White, blended with `mode` at `opacity`, optionally preserving the target's
 * transparency. The selection (or the whole canvas) is filled as one history step ("Fill").
 *
 * The blend engine (blend_into / blend_tile) works tile by tile. The reference runs the tiles in
 * parallel with rayon; this port runs them in order, which gives identical pixels (each tile is
 * independent) and is deterministic under any thread count -- a perf-only difference tracked for
 * the Z1 pass (REWRITE_PLAN.md §7, as pixels.c::edit_tiles already is).
 *
 * Deviation (§7): the `pattern` contents is deferred until the pattern library (E2b/E10 pattern_cmds)
 * is ported; it returns PC_ERR_UNSUPPORTED for now (never a crash). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/content_aware.h"
#include "base/json.h"
#include "base/mem.h"
#include "color/blend.h"
#include "color/color.h"
#include "color/convert.h"
#include "doc/doc.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "allocation.h"
#include "brush_cmds.h"
#include "channel_cmds.h"
#include "fill_cmds.h"
#include "layer_cmds.h"

#define CMD "edit.fill"

/* Every contents value, in the dialog's order (fill_cmds::CONTENTS). */
static const char *const CONTENTS[] = {"foreground", "background", "color", "contentAware",
                                       "pattern",    "history",    "black", "gray",
                                       "white"};

/* ---- the fill source --------------------------------------------------------------------- */

typedef enum { SRC_COLOR, SRC_CMYK, SRC_PIXELS } src_kind;

typedef struct {
    src_kind kind;
    float color[4]; /* COLOR / CMYK rgba */
    float cmyk[4];  /* CMYK inks */
    pc_rect rect;   /* PIXELS coordinate frame */
    float (*px)[4]; /* PIXELS straight RGBA, row-major over rect (owned) */
    size_t npx;
} fill_source;

static void source_fini(fill_source *s)
{
    if (s != NULL) {
        free(s->px);
        s->px = NULL;
    }
}

static void source_at(const fill_source *s, int32_t x, int32_t y, float out[4])
{
    if (s->kind == SRC_PIXELS) {
        if (!pc_rect_contains(s->rect, x, y)) {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            return;
        }
        size_t w = pc_rect_width(s->rect);
        size_t i = (size_t)(uint32_t)(y - s->rect.y0) * w + (size_t)(uint32_t)(x - s->rect.x0);
        if (i >= s->npx) {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            return;
        }
        memcpy(out, s->px[i], sizeof(float[4]));
        return;
    }
    memcpy(out, s->color, sizeof(float[4]));
}

/* The source's opaque colour as samples of fmt (alpha last), or false for a non-colour source. A
 * CMYK pick keeps its inks in a CMYK target (fill_cmds::encode_color). */
static bool encode_color(pc_pixel_format fmt, const fill_source *s, float enc[PC_RASTER_MAX_CHANNELS],
                         size_t *n)
{
    const float *rgba;
    const float *cmyk = NULL;
    if (s->kind == SRC_COLOR && s->color[3] >= 1.0f) {
        rgba = s->color;
    } else if (s->kind == SRC_CMYK) {
        rgba = s->color;
        cmyk = s->cmyk;
    } else {
        return false;
    }
    size_t m = pc_from_rgba(fmt, rgba, enc);
    if (cmyk != NULL && fmt.mode == PC_COLOR_MODE_CMYK) {
        for (size_t i = 0; i < 4 && i < m; i++) {
            enc[i] = cmyk[i];
        }
    }
    *n = m;
    return true;
}

/* ---- the selection coverage over one tile (fill_cmds::Mask) ------------------------------- */

typedef struct {
    const uint8_t *bytes;
    size_t len;
    pc_pixel_format fmt;
    pc_rect origin;
    size_t bpp;
} mask_t;

static mask_t mask_of(const pc_surface *sel, pc_tile tc)
{
    mask_t m;
    const pc_raster_tile *t = pc_surface_tile(sel, tc);
    m.fmt = pc_surface_format(sel);
    m.bpp = pc_pixel_format_bytes_per_pixel(m.fmt);
    m.origin = pc_tile_rect(tc);
    if (t != NULL) {
        m.bytes = pc_raster_tile_bytes(t);
        m.len = pc_raster_tile_len(t);
    } else {
        m.bytes = pc_surface_default_bytes(sel);
        m.len = m.bpp;
    }
    return m;
}

static float mask_at(const mask_t *m, int32_t x, int32_t y)
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

static bool mask_full(const mask_t *m, pc_rect r)
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

static bool mask_empty(const mask_t *m, pc_rect r)
{
    for (int32_t y = r.y0; y < r.y1; y++) {
        for (int32_t x = r.x0; x < r.x1; x++) {
            if (mask_at(m, x, y) > 0.0f) {
                return false;
            }
        }
    }
    return true;
}

/* ---- the blend ---------------------------------------------------------------------------- */

typedef struct {
    pc_blend_mode mode;
    float opacity;
    bool keep_alpha; /* Preserve Transparency, or locked transparency */
    bool restore;    /* Normal mode replaces the pixels, transparency included (History) */
} blend_t;

/* `d` moved towards `c` by `k` in premultiplied space (straight RGBA in and out). */
static void lerp_rgba(const float d[4], const float c[4], float k, float out[4])
{
    float a = d[3] * (1.0f - k) + c[3] * k;
    if (a <= 0.0f) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    for (int i = 0; i < 3; i++) {
        out[i] = (d[i] * d[3] * (1.0f - k) + c[i] * c[3] * k) / a;
    }
    out[3] = a;
}

/* Blend into one tile's encoded pixels (`bytes`, the tile at `origin`) over `tr`. */
static void blend_tile(uint8_t *bytes, pc_pixel_format fmt, pc_rect origin, pc_rect tr,
                       const fill_source *source, const mask_t *mask, blend_t b)
{
    size_t n = pc_pixel_format_channels(fmt);
    size_t bpp = pc_pixel_format_bytes_per_pixel(fmt);
    pc_blend_mode blend = (b.mode == PC_BLEND_DISSOLVE) ? PC_BLEND_NORMAL : b.mode;
    float enc[PC_RASTER_MAX_CHANNELS];
    size_t m = 0;
    bool have_opaque = false;
    uint8_t opaque_bytes[PC_RASTER_MAX_PIXEL_BYTES];
    size_t keep = bpp;
    if (b.mode == PC_BLEND_NORMAL && encode_color(fmt, source, enc, &m)) {
        size_t cnt = m < n ? m : n;
        pc_encode_pixel(fmt, enc, cnt, opaque_bytes);
        if (b.keep_alpha && fmt.alpha && n > 0) {
            keep = bpp - bpp / n; /* colour samples only (alpha is last) */
        }
        have_opaque = true;
    }
    for (int32_t y = tr.y0; y < tr.y1; y++) {
        size_t row = (size_t)(uint32_t)(y - origin.y0) * (size_t)PC_TILE_SIZE;
        for (int32_t x = tr.x0; x < tr.x1; x++) {
            float k = (mask != NULL ? mask_at(mask, x, y) : 1.0f) * b.opacity;
            if (k <= 0.0f) {
                continue;
            }
            size_t i = (row + (size_t)(uint32_t)(x - origin.x0)) * bpp;
            uint8_t *pb = bytes + i;
            if (k >= 1.0f && have_opaque) {
                memcpy(pb, opaque_bytes, keep);
                continue;
            }
            float px[PC_RASTER_MAX_CHANNELS];
            for (size_t c = 0; c < n; c++) {
                px[c] = pc_read_sample(pb, bpp, fmt.sample, c);
            }
            float d[4];
            pc_to_rgba(fmt, px, n, d);
            float c[4];
            source_at(source, x, y, c);
            if (b.mode == PC_BLEND_DISSOLVE) {
                k = (pc_dither_noise(x, y) < c[3] * k) ? 1.0f : 0.0f;
                c[3] = 1.0f;
                if (k <= 0.0f) {
                    continue;
                }
            }
            float o[4];
            if (b.restore && blend == PC_BLEND_NORMAL) {
                lerp_rgba(d, c, k, o);
            } else {
                pc_blend_composite(blend, d, c, k, o);
            }
            if (b.keep_alpha || !fmt.alpha) {
                o[3] = d[3];
            }
            size_t m2 = pc_from_rgba(fmt, o, enc);
            pc_encode_pixel(fmt, enc, m2 < n ? m2 : n, pb);
        }
    }
}

/* Blend `source` into `surf` over `area`: per pixel the selection coverage times the opacity, in
 * the blend mode (fill_cmds::blend_into). */
static pc_status blend_into(pc_surface *surf, pc_rect area, const fill_source *source,
                            const pc_surface *sel, blend_t b, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(surf);
    float enc[PC_RASTER_MAX_CHANNELS];
    size_t m = 0;
    pc_raster_tile *solid = NULL;
    if (b.mode == PC_BLEND_NORMAL && b.opacity >= 1.0f && (!b.keep_alpha || !fmt.alpha) &&
        encode_color(fmt, source, enc, &m)) {
        if (pc_surface_solid_tile(surf, enc, m, &solid, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }
    pc_tile_entry *entries = NULL;
    size_t nent = 0;
    pc_status rc = pc_surface_take_tiles(surf, area, &entries, &nent, err);
    if (rc != PC_OK) {
        pc_raster_tile_release(solid);
        return rc;
    }
    for (size_t i = 0; i < nent && rc == PC_OK; i++) {
        pc_tile tc = entries[i].coord;
        pc_rect trect = pc_tile_rect(tc);
        pc_rect tr = pc_rect_intersect(trect, area);
        bool have_mask = sel != NULL;
        mask_t mask;
        if (have_mask) {
            mask = mask_of(sel, tc);
        }
        if (solid != NULL && pc_rect_eq(tr, trect) && (!have_mask || mask_full(&mask, tr))) {
            pc_raster_tile_release(entries[i].tile);
            entries[i].tile = pc_raster_tile_retain(solid);
            continue;
        }
        if (b.opacity <= 0.0f || (have_mask && mask_empty(&mask, tr))) {
            continue; /* keep the tile as-is (shared with history) */
        }
        uint8_t *bytes = pc_raster_tile_make_mut(&entries[i].tile);
        if (bytes == NULL) {
            rc = pc_engine_alloc_error(err, "blending a pixel tile", "out of memory");
            break;
        }
        blend_tile(bytes, fmt, trect, tr, source, have_mask ? &mask : NULL, b);
    }
    if (rc == PC_OK) {
        rc = pc_surface_put_tiles(surf, entries, nent, err); /* consumes every entry's reference */
        free(entries);                                       /* the array itself is ours to free */
    } else {
        pc_tile_entries_free(entries, nent);
    }
    pc_raster_tile_release(solid);
    return rc;
}

/* ---- exported helpers --------------------------------------------------------------------- */

pc_status pc_engine_composite_over(pc_surface *surf, const pc_surface *src, pc_rect area,
                                   const pc_surface *limit, pc_error *err)
{
    fill_source source = {0};
    source.kind = SRC_PIXELS;
    source.rect = area;
    size_t w = pc_rect_width(area), h = pc_rect_height(area);
    source.npx = w * h;
    if (source.npx > 0) {
        source.px = pc_malloc_array(source.npx, sizeof(float[4]));
        if (source.px == NULL) {
            return pc_engine_alloc_error(err, "pasting pixels", "out of memory");
        }
        if (pc_surface_read_rgba_into(src, area, source.px, source.npx, err) != PC_OK) {
            source_fini(&source);
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
    }
    blend_t b = {PC_BLEND_NORMAL, 1.0f, false, false};
    pc_status rc = blend_into(surf, area, &source, limit, b, err);
    source_fini(&source);
    return rc;
}

pc_status pc_engine_fill_color(pc_surface *surf, pc_rect area, const float color[4],
                               const pc_surface *sel, pc_error *err)
{
    fill_source source = {0};
    source.kind = SRC_COLOR;
    memcpy(source.color, color, sizeof(float[4]));
    blend_t b = {PC_BLEND_NORMAL, 1.0f, false, false};
    return blend_into(surf, area, &source, sel, b, err);
}

pc_status pc_engine_blend_color_mask(pc_surface *surf, pc_rect area, const float color[4],
                                     const float *picked, const pc_surface *coverage, pc_blend_mode mode,
                                     float opacity, bool preserve_transparency, pc_error *err)
{
    fill_source source = {0};
    if (picked != NULL && color[3] >= 1.0f) {
        source.kind = SRC_CMYK;
        memcpy(source.color, color, sizeof(float[4]));
        memcpy(source.cmyk, picked, sizeof(float[4]));
    } else {
        source.kind = SRC_COLOR;
        memcpy(source.color, color, sizeof(float[4]));
    }
    blend_t b = {mode, opacity, preserve_transparency, false};
    return blend_into(surf, area, &source, coverage, b, err);
}

pc_rect pc_engine_fill_sampling_window(pc_rect hole, pc_rect canvas)
{
    return pc_algo_ca_sampling_window(pc_rect_intersect(hole, canvas), canvas);
}

pc_status pc_engine_fill_window_size(pc_rect window, size_t *w, size_t *h, pc_error *err)
{
    uint32_t ww = pc_rect_width(window), hh = pc_rect_height(window);
    if ((uint64_t)ww * (uint64_t)hh > 400000000ULL) {
        return pc_fail(err, PC_ERR_LIMIT, "the selection is too large for Content-Aware fill");
    }
    *w = ww;
    *h = hh;
    return PC_OK;
}

/* ---- param parsing ------------------------------------------------------------------------ */

static pc_status parse_contents(yyjson_val *p, const char **out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "contents");
    if (v == NULL || yyjson_is_null(v)) {
        *out = "color";
        return PC_OK;
    }
    size_t len = 0;
    const char *s = pc_json_as_str(v, &len);
    if (s == NULL) {
        return pc_engine_err_bad_params(err, CMD, "`contents` must be a string");
    }
    for (size_t i = 0; i < sizeof CONTENTS / sizeof CONTENTS[0]; i++) {
        if (strlen(CONTENTS[i]) == len) {
            bool eq = true;
            for (size_t k = 0; k < len; k++) {
                char a = s[k], c = CONTENTS[i][k];
                if (a >= 'A' && a <= 'Z') {
                    a = (char)(a + 32);
                }
                if (c >= 'A' && c <= 'Z') {
                    c = (char)(c + 32);
                }
                if (a != c) {
                    eq = false;
                    break;
                }
            }
            if (eq) {
                *out = CONTENTS[i];
                return PC_OK;
            }
        }
    }
    return pc_engine_err_bad_params(err, CMD, "unknown contents `%.*s`", (int)len, s);
}

static pc_status parse_mode(yyjson_val *p, pc_blend_mode *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "mode");
    if (v == NULL || yyjson_is_null(v)) {
        *out = PC_BLEND_NORMAL;
        return PC_OK;
    }
    size_t len = 0;
    const char *s = pc_json_as_str(v, &len);
    if (s == NULL) {
        return pc_engine_err_bad_params(err, CMD, "`mode` must be a blend mode name");
    }
    pc_blend_mode m;
    if (!pc_engine_blend_from_str(s, len, &m) || m == PC_BLEND_PASS_THROUGH) {
        return pc_engine_err_bad_params(err, CMD, "unknown blend mode `%.*s`", (int)len, s);
    }
    *out = m;
    return PC_OK;
}

static pc_status parse_opacity(yyjson_val *p, float *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "opacity");
    if (v == NULL || yyjson_is_null(v)) {
        *out = 1.0f;
        return PC_OK;
    }
    double o;
    if (!pc_json_as_f64(v, &o) || !isfinite(o)) {
        return pc_engine_err_bad_params(err, CMD, "`opacity` must be a number 0..100");
    }
    if (o < 0.0) {
        o = 0.0;
    } else if (o > 100.0) {
        o = 100.0;
    }
    *out = (float)(o / 100.0);
    return PC_OK;
}

static pc_status parse_flag(yyjson_val *p, const char *k, bool def, bool *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, k);
    if (v == NULL || yyjson_is_null(v)) {
        *out = def;
        return PC_OK;
    }
    if (!yyjson_is_bool(v)) {
        return pc_engine_err_bad_params(err, CMD, "`%s` must be true or false", k);
    }
    *out = yyjson_get_bool(v);
    return PC_OK;
}

/* ---- Content-Aware fill (fill_cmds::content_aware) ---------------------------------------- */

static pc_status content_aware(const pc_surface *surf, const pc_surface *sel, pc_rect canvas,
                               bool color_adaptation, uint64_t seed, fill_source *out, pc_error *err)
{
    pc_rect hole_bounds = pc_rect_intersect(pc_surface_content_bounds(sel), canvas);
    if (pc_rect_is_empty(hole_bounds)) {
        return pc_engine_err_other(err, "the selection is outside the canvas");
    }
    pc_rect window = pc_engine_fill_sampling_window(hole_bounds, canvas);
    size_t w = 0, h = 0;
    pc_status rc = pc_engine_fill_window_size(window, &w, &h, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_pixel_format fmt = pc_surface_format(surf);
    size_t n = pc_pixel_format_channels(fmt);
    float *img = NULL;
    size_t ilen = 0;
    rc = pc_engine_alloc_read_region(surf, window, "Content-Aware fill", &img, &ilen, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (ilen != w * h * n) {
        free(img);
        return pc_engine_err_other(err, "could not read the layer pixels");
    }
    bool *hole = NULL;
    rc = pc_engine_alloc_filled(w * h, sizeof(bool), "Content-Aware fill", (void **)&hole, err);
    if (rc != PC_OK) {
        free(img);
        return rc;
    }
    bool all_hole = (w * h) > 0;
    for (size_t i = 0; i < w * h; i++) {
        int32_t x = window.x0 + (int32_t)(i % w);
        int32_t y = window.y0 + (int32_t)(i / w);
        hole[i] = pc_surface_sample_channel(sel, x, y, 0) > 0.0f;
        if (!hole[i]) {
            all_hole = false;
        }
    }
    if (all_hole) {
        free(img);
        free(hole);
        return pc_engine_err_other(err, "Content-Aware fill needs pixels outside the selection to sample from");
    }
    bool *source = NULL;
    rc = pc_engine_alloc_capacity(w * h, sizeof(bool), "Content-Aware fill", (void **)&source, err);
    if (rc != PC_OK) {
        free(img);
        free(hole);
        return rc;
    }
    for (size_t i = 0; i < w * h; i++) {
        source[i] = true;
    }
    pc_algo_ca_fill_options opts = pc_algo_ca_fill_options_default();
    opts.color_adaptation = pc_algo_ca_color_level(color_adaptation ? "default" : "none");
    opts.rotations = NULL;
    opts.rotation_count = 0;
    opts.scale = false;
    opts.mirror = false;
    opts.seed = seed;
    float *filled = NULL;
    rc = pc_algo_ca_fill(w, h, n, img, ilen, hole, source, w * h, &opts, NULL, &filled, err);
    free(img);
    free(hole);
    free(source);
    if (rc != PC_OK) {
        return rc;
    }
    out->kind = SRC_PIXELS;
    out->rect = window;
    out->npx = w * h;
    out->px = pc_malloc_array(out->npx == 0 ? 1 : out->npx, sizeof(float[4]));
    if (out->px == NULL) {
        free(filled);
        return pc_engine_alloc_error(err, "Content-Aware fill", "out of memory");
    }
    size_t nn = n == 0 ? 1 : n;
    for (size_t i = 0; i < out->npx; i++) {
        pc_to_rgba(fmt, &filled[i * nn], n, out->px[i]);
    }
    free(filled);
    return PC_OK;
}

/* ---- the edit ----------------------------------------------------------------------------- */

struct fill_ctx {
    bool has_layer;
    pc_layer_id layer;
    yyjson_val *p;
    pc_rect area;
    const fill_source *source;
    blend_t blend;
};

static pc_status fill_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct fill_ctx *c = vctx;
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    pc_surface *surf = NULL;
    bool lock = false;
    pc_status rc = pc_engine_target_surface(doc, c->has_layer, c->layer, c->p, &surf, &lock, err);
    if (rc != PC_OK) {
        return rc;
    }
    blend_t b = c->blend;
    b.keep_alpha = b.keep_alpha || lock;
    return blend_into(surf, c->area, c->source, sel, b, err);
}

pc_status pc_engine_fill(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *contents = "color";
    pc_blend_mode mode = PC_BLEND_NORMAL;
    float opacity = 1.0f;
    bool preserve = false;
    pc_status rc = parse_contents(p, &contents, err);
    if (rc != PC_OK) {
        return rc;
    }
    if ((rc = parse_mode(p, &mode, err)) != PC_OK) {
        return rc;
    }
    if ((rc = parse_opacity(p, &opacity, err)) != PC_OK) {
        return rc;
    }
    if ((rc = parse_flag(p, "preserveTransparency", false, &preserve, err)) != PC_OK) {
        return rc;
    }
    bool channel = pc_engine_is_channel_target(p);
    bool has_layer = false;
    pc_layer_id id = 0;
    if (!channel) {
        rc = pc_engine_layer_param(s, p, &id, err);
        if (rc != PC_OK) {
            return rc;
        }
        has_layer = true;
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_rect doc_bounds = pc_document_bounds(doc);
    pc_rect area = doc->has_selection ? pc_surface_content_bounds(&doc->selection) : doc_bounds;
    const float *fg = pc_engine_session_tools(s)->foreground;
    const float *bg = pc_engine_session_tools(s)->background;

    fill_source source = {0};
    bool restore = (strcmp(contents, "history") == 0);

    if (strcmp(contents, "foreground") == 0 || strcmp(contents, "background") == 0 ||
        strcmp(contents, "color") == 0) {
        float c[4];
        if (strcmp(contents, "foreground") == 0) {
            memcpy(c, fg, sizeof c);
        } else if (strcmp(contents, "background") == 0) {
            memcpy(c, bg, sizeof c);
        } else {
            pc_engine_param_color(p, "color", fg, c);
        }
        float inks[4];
        if (pc_engine_tool_state_picked_cmyk(pc_engine_session_tools(s), c, inks) && c[3] >= 1.0f) {
            source.kind = SRC_CMYK;
            memcpy(source.color, c, sizeof c);
            memcpy(source.cmyk, inks, sizeof inks);
        } else {
            source.kind = SRC_COLOR;
            memcpy(source.color, c, sizeof c);
        }
    } else if (strcmp(contents, "black") == 0) {
        source.kind = SRC_COLOR;
        source.color[0] = source.color[1] = source.color[2] = 0.0f;
        source.color[3] = 1.0f;
    } else if (strcmp(contents, "gray") == 0) {
        source.kind = SRC_COLOR;
        source.color[0] = source.color[1] = source.color[2] = 128.0f / 255.0f;
        source.color[3] = 1.0f;
    } else if (strcmp(contents, "white") == 0) {
        source.kind = SRC_COLOR;
        source.color[0] = source.color[1] = source.color[2] = source.color[3] = 1.0f;
    } else if (strcmp(contents, "history") == 0) {
        yyjson_val *sv = pc_json_get(p, "state");
        bool has_state = false;
        size_t state = 0;
        if (sv != NULL && !yyjson_is_null(sv)) {
            uint64_t u;
            if (!pc_json_as_u64(sv, &u)) {
                return pc_engine_err_bad_params(err, CMD, "`state` must be a history state index");
            }
            has_state = true;
            state = (size_t)u;
        }
        size_t past = pc_history_past_len(st->history);
        pc_doc_arc *src_doc = NULL;
        bool src_is_current = false;
        if (has_state) {
            if (state == past) {
                src_is_current = true;
            } else {
                src_doc = pc_history_state(st->history, state);
                if (src_doc == NULL) {
                    return pc_engine_err_bad_params(err, CMD, "no history state %zu (0..=%zu)", state, past);
                }
            }
        } else {
            src_doc = pc_history_state(st->history, 0);
        }
        const pc_document *sdoc = src_is_current ? doc : (src_doc != NULL ? pc_doc_arc_get(src_doc) : doc);
        const pc_surface *src = NULL;
        if (pc_engine_read_target_surface(sdoc, has_layer, id, p, &src, err) != PC_OK) {
            return pc_engine_err_other(err, "the target did not exist (as pixels, a mask or a channel) in that history state");
        }
        source.kind = SRC_PIXELS;
        source.rect = area;
        size_t w = pc_rect_width(area), h = pc_rect_height(area);
        source.npx = w * h;
        if (source.npx > 0) {
            source.px = pc_malloc_array(source.npx, sizeof(float[4]));
            if (source.px == NULL) {
                return pc_engine_alloc_error(err, "reading Fill's history source", "out of memory");
            }
            if (pc_surface_read_rgba_into(src, area, source.px, source.npx, err) != PC_OK) {
                source_fini(&source);
                return err != NULL ? err->code : PC_ERR_INVALID;
            }
        }
    } else if (strcmp(contents, "contentAware") == 0) {
        if (!doc->has_selection) {
            return pc_engine_err_other(err, "Content-Aware fill needs a selection");
        }
        if (channel || pc_engine_is_mask_target(p)) {
            return pc_engine_err_other(err, "Content-Aware fill works on layer pixels only");
        }
        bool adapt = true;
        if ((rc = parse_flag(p, "colorAdaptation", true, &adapt, err)) != PC_OK) {
            return rc;
        }
        if (!has_layer) {
            return pc_engine_err_other(err, "no active layer");
        }
        const pc_layer *l = pc_document_layer(doc, id);
        const pc_surface *lsurf = l != NULL ? pc_layer_surface(l) : NULL;
        if (lsurf == NULL) {
            return pc_engine_err_other(err, "Content-Aware fill needs a pixel layer");
        }
        uint64_t seed = 1;
        yyjson_val *seedv = pc_json_get(p, "seed");
        if (seedv != NULL) {
            (void)pc_json_as_u64(seedv, &seed);
        }
        rc = content_aware(lsurf, &doc->selection, doc_bounds, adapt, seed, &source, err);
        if (rc != PC_OK) {
            return rc;
        }
    } else if (strcmp(contents, "pattern") == 0) {
        /* §7: deferred until pattern_cmds (E2b/E10) is ported. */
        return pc_fail(err, PC_ERR_UNSUPPORTED,
                       "Fill with a pattern is not available yet (the pattern library is unported)");
    } else {
        return pc_engine_err_bad_params(err, CMD, "unknown contents `%s`", contents);
    }

    area = pc_rect_intersect(area, doc_bounds);

    /* Build the result object. */
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        source_fini(&source);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "contents", contents);
    yyjson_mut_doc_set_root(rd, root);

    if (pc_rect_is_empty(area)) {
        source_fini(&source);
        return PC_OK;
    }

    struct fill_ctx ctx = {has_layer, id, p, area, &source, {mode, opacity, preserve, restore}};
    pc_cmyk_space *space = pc_engine_doc_cmyk_space(s);
    pc_cmyk_space *prev = space != NULL ? pc_cmyk_space_enter(space) : NULL;
    rc = pc_engine_session_edit(s, "Fill", fill_edit, &ctx, err);
    if (space != NULL) {
        pc_cmyk_space_leave(prev);
        pc_cmyk_space_release(space);
    }
    source_fini(&source);
    if (rc != PC_OK) {
        pc_json_value_fini(out);
        return rc;
    }
    return PC_OK;
}

/* A pixel layer, or a targeted alpha channel / Quick Mask (commands::has_pixel_or_channel). */
pc_status pc_engine_has_pixel_or_channel(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_edits_channel(s)) {
        return PC_OK;
    }
    pc_error why = {0};
    const pc_layer *l = pc_engine_active_layer_of(s, &why);
    if (l == NULL) {
        if (reason != NULL) {
            *reason = why;
        }
        return PC_ERR_INVALID;
    }
    if (l->content.kind == PC_LAYER_RASTER) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "active layer is %s %s layer, not a pixel layer",
                   pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
}

static const char *const MENU_EDIT[] = {"Edit"};

static const pc_engine_command SPECS[] = {
    {"edit.fill", "Fill…", MENU_EDIT, 1, "Shift+F5",
     "{\"contents\":\"foreground|background|color|contentAware|pattern|history|black|gray|white\"="
     "\"color\",\"color\":\"#rrggbb|[r,g,b,a]\"=foreground (contents=color),\"pattern\":id|name "
     "(contents=pattern),\"scale\":%=100,\"angle\":deg,\"state\":index? (contents=history; default "
     "the oldest state),\"colorAdaptation\":bool=true (contents=contentAware),\"mode\":\"normal|"
     "multiply|…\"=\"normal\",\"opacity\":0..100=100,\"preserveTransparency\":bool=false,\"target\":"
     "\"pixels\"|\"mask\"|\"quickMask\"|{\"channel\":i}?}",
     pc_engine_has_pixel_or_channel, pc_engine_fill, true},
};

const pc_engine_command *pc_engine_fill_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
