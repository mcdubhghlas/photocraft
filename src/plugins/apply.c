/* Running a filter plug-in over a surface, band by band (port of crates/plugins/src/apply.rs).
 *
 * Bands are tile-aligned blocks, one tile row tall and as many tiles wide as fit in band_bytes
 * (and in half the plug-in's memory cap), so each worker encodes its own tiles and the merge is a
 * tile copy. Bands run a group at a time (one per pool thread), so peak memory stays near one band
 * per thread; band i is computed from its index, so a huge area costs no band list. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "plugins/internal.h"

uint32_t pc_plugin_format_code(pc_pixel_format f)
{
    static const uint32_t MODE[PC_COLOR_MODE_COUNT] = {
        [PC_COLOR_MODE_BITMAP] = 0,  [PC_COLOR_MODE_GRAYSCALE] = 1, [PC_COLOR_MODE_INDEXED] = 2,
        [PC_COLOR_MODE_RGB] = 3,     [PC_COLOR_MODE_CMYK] = 4,      [PC_COLOR_MODE_MULTICHANNEL] = 7,
        [PC_COLOR_MODE_DUOTONE] = 8, [PC_COLOR_MODE_LAB] = 9,
    };
    uint32_t depth = f.sample == PC_SAMPLE_TYPE_U8 ? 8u : f.sample == PC_SAMPLE_TYPE_U16 ? 16u : 32u;
    uint32_t mode = (unsigned)f.mode < PC_COLOR_MODE_COUNT ? MODE[f.mode] : 0u;
    return depth | ((uint32_t)(f.alpha ? 1 : 0) << 8) | (mode << 16);
}

static const char *mode_name(pc_color_mode m)
{
    switch (m) {
    case PC_COLOR_MODE_BITMAP:
        return "bitmap";
    case PC_COLOR_MODE_GRAYSCALE:
        return "grayscale";
    case PC_COLOR_MODE_INDEXED:
        return "indexed";
    case PC_COLOR_MODE_RGB:
        return "rgb";
    case PC_COLOR_MODE_CMYK:
        return "cmyk";
    case PC_COLOR_MODE_LAB:
        return "lab";
    case PC_COLOR_MODE_MULTICHANNEL:
        return "multichannel";
    case PC_COLOR_MODE_DUOTONE:
        return "duotone";
    }
    return "rgb";
}

pc_rect pc_plugin_output_area(const pc_plugin *p, const pc_surface *surface, pc_rect canvas,
                              const pc_surface *selection)
{
    const pc_plugin_manifest *m = pc_plugin_get_manifest(p);
    pc_rect content = pc_surface_content_bounds(surface);
    pc_rect extent = pc_rect_union(canvas, content);
    pc_rect area;
    if (m->area == PC_PLUGIN_AREA_CANVAS) {
        area = canvas;
    } else if (pc_rect_is_empty(content)) {
        area = PC_RECT_EMPTY;
    } else {
        area = pc_rect_intersect(pc_rect_inflate(content, (int32_t)m->overlap), extent);
    }
    if (selection != NULL) {
        area = pc_rect_intersect(area, pc_surface_content_bounds(selection));
    }
    return area;
}

/* Reads rect, repeating the edge pixels of extent outside it (like built-in neighbourhood
 * filters at the canvas edge). */
static pc_status read_clamped(const pc_surface *s, pc_rect rect, pc_rect extent, float **out, size_t *len,
                              pc_error *err)
{
    pc_rect inner = pc_rect_intersect(rect, extent);
    size_t n = pc_surface_channels(s), w = pc_rect_width(rect), h = pc_rect_height(rect), iw, total;
    float *src = NULL, *dst;
    size_t slen = 0;
    pc_status st;
    if (pc_rect_is_empty(inner) || pc_rect_eq(inner, rect)) {
        return pc_surface_read_region(s, rect, out, len, err);
    }
    st = pc_surface_read_region(s, inner, &src, &slen, err);
    if (st != PC_OK) {
        return st;
    }
    iw = pc_rect_width(inner);
    if (!pc_size_mul3(w, h, n, &total)) {
        free(src);
        return pc_fail(err, PC_ERR_LIMIT, "band too large");
    }
    dst = pc_calloc_array(total, sizeof *dst);
    if (dst == NULL) {
        free(src);
        return PC_PLUGIN_FAIL_NOMEM(err);
    }
    for (size_t row = 0; row < h; row++) {
        int32_t y = (int32_t)((int64_t)rect.y0 + (int64_t)row);
        size_t sy = (size_t)((int64_t)pc_clamp_i32(y, inner.y0, inner.y1 - 1) - inner.y0);
        for (size_t col = 0; col < w; col++) {
            int32_t x = (int32_t)((int64_t)rect.x0 + (int64_t)col);
            size_t sx = (size_t)((int64_t)pc_clamp_i32(x, inner.x0, inner.x1 - 1) - inner.x0);
            size_t si = (sy * iw + sx) * n, di = (row * w + col) * n;
            if (si + n <= slen && di + n <= total) {
                memcpy(dst + di, src + si, n * sizeof *dst);
            }
        }
    }
    free(src);
    *out = dst;
    *len = total;
    return PC_OK;
}

typedef struct band_ctx {
    const pc_plugin *p;
    const pc_surface *surface;
    const pc_surface *selection;
    pc_rect extent, canvas, area;
    pc_pixel_format fmt;
    int32_t ov;
    const pc_json_value *params;
    pc_plugin_deadline *d;
    /* Band geometry. */
    pc_tile t0, t1;
    int64_t across, per_row;
    /* The current group's results. */
    size_t first;
    pc_surface *results;
    pc_error *errors;
    bool *ok;
} band_ctx;

static int32_t sat32(int64_t v)
{
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

static pc_rect band_rect(const band_ctx *c, uint64_t i)
{
    int64_t ty = (int64_t)c->t0.ty + (int64_t)(i / (uint64_t)c->per_row);
    int64_t tx = (int64_t)c->t0.tx + (int64_t)(i % (uint64_t)c->per_row) * c->across;
    int64_t tx1 = tx + c->across < (int64_t)c->t1.tx + 1 ? tx + c->across : (int64_t)c->t1.tx + 1;
    /* Edges saturate at the end of the coordinate space (the reference's i32 arithmetic wraps
     * there and drops the band). */
    pc_rect r = pc_rect_make(sat32(tx * PC_TILE_SIZE), sat32(ty * PC_TILE_SIZE), sat32(tx1 * PC_TILE_SIZE),
                             sat32((ty + 1) * PC_TILE_SIZE));
    return pc_rect_intersect(r, c->area);
}

/* The params JSON the plug-in sees: the resolved params plus "_image". */
static pc_status band_params(const band_ctx *c, pc_rect input, pc_buf *out, pc_error *err)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc != NULL ? pc_json_normalized_copy(doc, pc_json_value_root(c->params)) : NULL;
    yyjson_mut_val *img = doc != NULL ? yyjson_mut_obj(doc) : NULL;
    pc_status st;
    bool ok;
    if (root == NULL || !yyjson_mut_is_obj(root)) {
        root = doc != NULL ? yyjson_mut_obj(doc) : NULL;
    }
    ok = root != NULL && img != NULL;
    ok = ok && yyjson_mut_obj_add_int(doc, img, "x", input.x0) &&
         yyjson_mut_obj_add_int(doc, img, "y", input.y0) &&
         yyjson_mut_obj_add_uint(doc, img, "width", pc_rect_width(input)) &&
         yyjson_mut_obj_add_uint(doc, img, "height", pc_rect_height(input)) &&
         yyjson_mut_obj_add_int(doc, img, "overlap", c->ov) &&
         yyjson_mut_obj_add_uint(doc, img, "canvasWidth", pc_rect_width(c->canvas)) &&
         yyjson_mut_obj_add_uint(doc, img, "canvasHeight", pc_rect_height(c->canvas)) &&
         yyjson_mut_obj_add_str(doc, img, "mode", mode_name(c->fmt.mode)) &&
         yyjson_mut_obj_add_uint(doc, img, "depth", pc_plugin_format_code(c->fmt) & 0xffu) &&
         yyjson_mut_obj_add_bool(doc, img, "alpha", c->fmt.alpha) &&
         yyjson_mut_obj_add_uint(doc, img, "channels", pc_pixel_format_channels(c->fmt)) &&
         yyjson_mut_obj_add_val(doc, root, "_image", img);
    if (!ok) {
        yyjson_mut_doc_free(doc);
        return PC_PLUGIN_FAIL_NOMEM(err);
    }
    st = pc_json_write(root, PC_JSON_SORT_KEYS, out, err);
    yyjson_mut_doc_free(doc);
    return st;
}

/* Runs one band and returns its output pixels (band w * h * channels). */
static pc_status run_band(const band_ctx *c, pc_rect band, float **out, pc_error *err)
{
    size_t n = pc_pixel_format_channels(c->fmt), bw = pc_rect_width(band), bh = pc_rect_height(band);
    size_t ov = (size_t)c->ov, total, dlen = 0;
    pc_rect input = pc_rect_inflate(band, c->ov);
    uint32_t iw = pc_rect_width(input), ih = pc_rect_height(input);
    float *data = NULL, *orig = NULL, *res = NULL, *sel = NULL;
    bool int_depth = c->fmt.sample != PC_SAMPLE_TYPE_F32;
    pc_buf params = PC_BUF_INIT;
    pc_status st = read_clamped(c->surface, input, c->extent, &data, &dlen, err);
    if (st == PC_OK) {
        st = band_params(c, input, &params, err);
    }
    if (st == PC_OK) {
        orig = pc_malloc_array(dlen > 0 ? dlen : 1, sizeof *orig);
        st = orig == NULL ? PC_PLUGIN_FAIL_NOMEM(err) : PC_OK;
    }
    if (st == PC_OK) {
        if (dlen > 0) {
            memcpy(orig, data, dlen * sizeof *orig);
        }
        st = pc_plugin_filter_buffer(c->p, data, dlen, iw, ih, (uint32_t)n, pc_plugin_format_code(c->fmt),
                                     params.data, params.len, c->d, err);
    }
    if (st == PC_OK && !pc_size_mul3(bw, bh, n, &total)) {
        st = pc_fail(err, PC_ERR_LIMIT, "band too large");
    }
    if (st == PC_OK) {
        res = pc_calloc_array(total > 0 ? total : 1, sizeof *res);
        sel = c->selection != NULL ? pc_calloc_array(bw > 0 ? bw : 1, sizeof *sel) : NULL;
        if (res == NULL || (c->selection != NULL && sel == NULL)) {
            st = PC_PLUGIN_FAIL_NOMEM(err);
        }
    }
    /* Keep the band (drop the overlap margins), sanitise and blend by the selection. */
    for (size_t row = 0; row < bh && st == PC_OK; row++) {
        if (c->selection != NULL) {
            size_t sn = pc_surface_channels(c->selection), slen = 0;
            float *srow = NULL;
            pc_rect r = pc_rect_make(band.x0, (int32_t)((int64_t)band.y0 + (int64_t)row), band.x1,
                                     (int32_t)((int64_t)band.y0 + (int64_t)row + 1));
            st = pc_surface_read_region(c->selection, r, &srow, &slen, err);
            if (st != PC_OK) {
                break;
            }
            if (sn == 0) {
                sn = 1;
            }
            for (size_t col = 0; col < bw; col++) {
                sel[col] = col * sn < slen ? pc_clampf(srow[col * sn], 0.0f, 1.0f) : 0.0f;
            }
            free(srow);
        }
        for (size_t col = 0; col < bw; col++) {
            size_t si = ((row + ov) * (bw + 2 * ov) + col + ov) * n, di = (row * bw + col) * n;
            float k = c->selection != NULL ? sel[col] : 1.0f;
            for (size_t ch = 0; ch < n; ch++) {
                float o = si + ch < dlen ? orig[si + ch] : 0.0f;
                float v = si + ch < dlen ? data[si + ch] : o;
                if (!isfinite(v)) {
                    v = o;
                }
                if (int_depth || (c->fmt.alpha && ch == n - 1)) {
                    v = pc_clampf(v, 0.0f, 1.0f);
                }
                if (di + ch < total) {
                    res[di + ch] = o + (v - o) * k;
                }
            }
        }
    }
    free(data);
    free(orig);
    free(sel);
    pc_buf_fini(&params);
    if (st != PC_OK) {
        free(res);
        return st;
    }
    *out = res;
    return PC_OK;
}

/* The tiles under band with the band's new pixels written in (the rest of each tile is the
 * original), in the surface's format: built on the worker thread. */
static pc_status encode_band(const pc_surface *surface, pc_rect band, const float *data, pc_surface *out,
                             pc_error *err)
{
    pc_surface s;
    float def[PC_RASTER_MAX_CHANNELS];
    pc_tile a, b;
    size_t n = pc_surface_channels(surface);
    pc_status st = PC_OK;
    pc_surface_default_pixel(surface, def);
    pc_surface_init_default(&s, pc_surface_format(surface), def, n);
    if (pc_rect_tile_range(band, &a, &b)) {
        for (int64_t ty = a.ty; ty <= b.ty && st == PC_OK; ty++) {
            for (int64_t tx = a.tx; tx <= b.tx && st == PC_OK; tx++) {
                pc_tile c = {(int32_t)tx, (int32_t)ty};
                const pc_raster_tile *t = pc_surface_tile(surface, c);
                uint8_t *bytes = NULL;
                st = pc_surface_tile_mut(&s, c, &bytes, err);
                if (st == PC_OK && t != NULL) {
                    memcpy(bytes, pc_raster_tile_bytes(t), pc_raster_tile_len(t));
                }
            }
        }
    }
    if (st == PC_OK) {
        size_t len;
        if (!pc_size_mul3(pc_rect_width(band), pc_rect_height(band), n, &len)) {
            st = pc_fail(err, PC_ERR_LIMIT, "band too large");
        } else {
            st = pc_surface_write_region(&s, band, data, len, err);
        }
    }
    if (st != PC_OK) {
        pc_surface_fini(&s);
        return st;
    }
    *out = s;
    return PC_OK;
}

static pc_status band_task(void *ctx, size_t chunk, size_t begin, size_t end)
{
    band_ctx *c = ctx;
    (void)chunk;
    for (size_t j = begin; j < end; j++) {
        size_t i = c->first + j;
        pc_rect band = band_rect(c, i);
        float *data = NULL;
        pc_error *e = &c->errors[j];
        pc_status st = run_band(c, band, &data, e);
        if (st == PC_OK) {
            st = encode_band(c->surface, band, data, &c->results[j], e);
        }
        free(data);
        c->ok[j] = st == PC_OK;
        if (st != PC_OK) {
            pc_plugin_deadline_abort(c->d, e);
        }
    }
    return PC_OK;
}

/* Copies every tile of band into out. */
static pc_status merge(pc_surface *out, const pc_surface *band, pc_error *err)
{
    size_t cursor = 0;
    pc_tile c;
    const pc_raster_tile *t;
    while (pc_surface_next_tile(band, &cursor, &c, &t)) {
        uint8_t *bytes = NULL;
        pc_status st = pc_surface_tile_mut(out, c, &bytes, err);
        if (st != PC_OK) {
            return st;
        }
        memcpy(bytes, pc_raster_tile_bytes(t), pc_raster_tile_len(t));
    }
    return PC_OK;
}

pc_status pc_plugin_apply(const pc_plugin *p, const pc_surface *surface, pc_rect canvas,
                          const pc_surface *selection, yyjson_val *params, pc_surface *out, pc_error *err)
{
    const pc_plugin_limits *lim = pc_plugin_get_limits(p);
    pc_json_value resolved = PC_JSON_VALUE_INIT;
    pc_surface result;
    band_ctx c;
    pc_pool *pool = pc_pool_global();
    size_t n, tile_bytes, room, in_bytes, group;
    uint64_t total;
    int64_t across, side;
    pc_status st = pc_plugin_manifest_resolve_params(pc_plugin_get_manifest(p), params, &resolved, err);
    if (st != PC_OK) {
        return st;
    }
    memset(&c, 0, sizeof c);
    c.p = p;
    c.surface = surface;
    c.selection = selection;
    c.canvas = canvas;
    c.area = pc_plugin_output_area(p, surface, canvas, selection);
    c.fmt = pc_surface_format(surface);
    c.ov = (int32_t)pc_plugin_get_manifest(p)->overlap;
    c.params = &resolved;
    c.extent = pc_rect_union(canvas, pc_surface_content_bounds(surface));
    st = pc_surface_clone(surface, &result, err);
    if (st != PC_OK) {
        pc_json_value_fini(&resolved);
        return st;
    }
    if (pc_rect_is_empty(c.area)) {
        pc_json_value_fini(&resolved);
        *out = result;
        return PC_OK;
    }
    n = pc_pixel_format_channels(c.fmt);
    tile_bytes = (size_t)PC_TILE_SIZE * PC_TILE_SIZE * n * 4;
    room = lim->max_memory_bytes / 2;
    across = (int64_t)((lim->band_bytes < room ? lim->band_bytes : room) / (tile_bytes > 0 ? tile_bytes : 1));
    across = across < 1 ? 1 : across > 1024 ? 1024 : across;
    side = across * PC_TILE_SIZE + 2 * (int64_t)c.ov;
    in_bytes = (size_t)side * (size_t)(PC_TILE_SIZE + 2 * c.ov) * n * 4;
    if (in_bytes > room) {
        pc_surface_fini(&result);
        pc_json_value_fini(&resolved);
        return PC_PLUGIN_FAIL_LIMIT(err, "memory budget (a band does not fit in the plug-in's memory)");
    }
    c.t0 = pc_tile_containing(c.area.x0, c.area.y0);
    c.t1 = pc_tile_containing(c.area.x1 - 1, c.area.y1 - 1);
    c.across = across;
    c.per_row = ((int64_t)c.t1.tx - c.t0.tx + 1 + across - 1) / across;
    total = (uint64_t)((int64_t)c.t1.ty - c.t0.ty + 1) * (uint64_t)c.per_row;
    /* An area of more tiles than any surface operation may touch (a canvas-wide filter on an
     * absurd canvas) is refused rather than run until the time budget (the reference would try,
     * and run out of memory). */
    if ((uint64_t)((int64_t)c.t1.ty - c.t0.ty + 1) * (uint64_t)((int64_t)c.t1.tx - c.t0.tx + 1) >
        PC_SURFACE_MAX_TILE_SPAN) {
        pc_surface_fini(&result);
        pc_json_value_fini(&resolved);
        return PC_PLUGIN_FAIL_LIMIT(err, "memory budget (the area spans too many tiles)");
    }
    group = pc_pool_threads(pool);
    if (group < 1) {
        group = 1;
    }
    c.results = pc_calloc_array(group, sizeof *c.results);
    c.errors = pc_calloc_array(group, sizeof *c.errors);
    c.ok = pc_calloc_array(group, sizeof *c.ok);
    st = c.results == NULL || c.errors == NULL || c.ok == NULL ? PC_PLUGIN_FAIL_NOMEM(err) : PC_OK;
    if (st == PC_OK) {
        st = pc_plugin_deadline_new(lim, &c.d, err);
    }
    for (uint64_t first = 0; first < total && st == PC_OK; first += group) {
        size_t count = total - first < group ? (size_t)(total - first) : group;
        c.first = (size_t)first;
        memset(c.ok, 0, group * sizeof *c.ok);
        (void)pc_pool_for(pool, count, 1, band_task, &c);
        for (size_t j = 0; j < count; j++) {
            if (st == PC_OK && !c.ok[j]) {
                if (err != NULL) {
                    *err = c.errors[j];
                }
                st = c.errors[j].code != PC_OK ? c.errors[j].code : PC_ERR_FORMAT;
            }
            if (c.ok[j]) {
                if (st == PC_OK) {
                    st = merge(&result, &c.results[j], err);
                }
                pc_surface_fini(&c.results[j]);
            }
        }
    }
    pc_plugin_deadline_free(c.d);
    free(c.results);
    free(c.errors);
    free(c.ok);
    pc_json_value_fini(&resolved);
    if (st != PC_OK) {
        pc_surface_fini(&result);
        return st;
    }
    pc_surface_prune(&result);
    *out = result;
    return PC_OK;
}
