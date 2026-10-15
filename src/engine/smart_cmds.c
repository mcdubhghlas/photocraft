/* Smart Objects: rendering, placement and source primitives (port of
 * crates/engine/src/smart_cmds.rs, E6). See smart_cmds.h for the deferral scope.
 *
 * §7 deviations (reported to the coordinator):
 *  - The smart-FILTER subsystem now renders: apply_smart_filters runs the stack on the placed
 *    source through E9's pc_engine_apply_filter_to_surface, and the layer.smartFilter.* editing
 *    commands live in smart_cmds_run.c. The stage cache (stack_cache) is a perf optimisation left
 *    out (the stack recomputes each render, giving identical pixels), so stack_cache_bytes/purge
 *    report 0. Recording a filter.* command onto a smart object (add_smart_filter) is still owed:
 *    it is wired in filters_cmds.c::edit_filter (E9's file), which is outside the subtype hook.
 *  - Layer > Smart Objects > Stack Mode rendering is deferred (a stack-mode smart object keeps its
 *    cache): it needs the algo stack-combine of the source's layers, not yet wired here.
 *  - The decoded-source content cache (SOURCE_CACHE) is a perf optimisation (Z1): sources decode
 *    on each render here, and source_decode_count reports 0.
 *  - shift() moves the cache/filter-mask with pc_algo_translate_surface (the reference's
 *    layer_multi_cmds::shift_surface, an engine-internal helper E5 did not export); a whole-pixel
 *    integer shift gives identical pixels. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/resample.h"
#include "algo/transform.h"
#include "algo/warp.h"
#include "base/blob.h"
#include "base/buf.h"
#include "base/file.h"
#include "base/json.h"
#include "base/str.h"
#include "color/blend.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "geom/geom.h"
#include "geom/warp.h"
#include "io/io.h"
#include "io/linked.h"
#include "io/svg.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "allocation.h"
#include "engine_internal.h"
#include "smart_cmds.h"

/* Largest linked file read from disk (as the reference's format::read_file cap, 2 GiB). */
#define SMART_MAX_FILE (2u * 1024u * 1024u * 1024u)

static bool rect_empty(pc_rect r)
{
    return r.x1 <= r.x0 || r.y1 <= r.y0;
}

/* ============================================================== source bytes */

static size_t base_name_off(const char *path, size_t len)
{
    size_t off = 0;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/' || path[i] == '\\') {
            off = i + 1;
        }
    }
    return off;
}

/* read_file: a linked file's raw bytes as a retained blob, or NULL (missing/empty path). */
static pc_status read_file_blob(const char *path, pc_blob **out)
{
    *out = NULL;
    if (path == NULL || path[0] == '\0') {
        return PC_OK;
    }
    pc_buf buf;
    pc_buf_init(&buf);
    if (pc_file_read(path, SMART_MAX_FILE, &buf, NULL) != PC_OK) {
        pc_buf_fini(&buf);
        return PC_OK; /* unreadable -> None, as the reference */
    }
    pc_status rc = pc_blob_new(buf.data, buf.len, out, NULL);
    pc_buf_fini(&buf);
    if (rc != PC_OK) {
        *out = NULL;
    }
    return PC_OK;
}

/* Embedded bytes, or Photoshop's embedded linked-layer data addressed by a Linked uuid. */
static pc_status embedded_source_bytes(const pc_metadata *meta, const pc_smart_source *src, bool *found,
                                       pc_str *name, pc_blob **bytes, pc_error *err)
{
    *found = false;
    if (!src->linked) {
        if (pc_str_clone(&src->file_name, name, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        *bytes = pc_blob_retain(src->bytes); /* may be NULL (empty) */
        *found = true;
        return PC_OK;
    }
    const char *path = pc_str_c(&src->path);
    bool lfound = false;
    pc_io_linked_file lf;
    memset(&lf, 0, sizeof lf);
    if (pc_io_find_linked_file(meta, path, strlen(path), &lfound, &lf, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (!lfound) {
        return PC_OK;
    }
    pc_status rc = pc_str_clone(&lf.file_name, name, err);
    if (rc == PC_OK) {
        rc = pc_blob_new(lf.bytes, lf.len, bytes, err);
    }
    pc_io_linked_file_fini(&lf);
    if (rc != PC_OK) {
        pc_str_fini(name);
        return PC_ERR_NOMEM;
    }
    *found = true;
    return PC_OK;
}

pc_status pc_engine_smart_source_bytes(const pc_metadata *meta, const pc_smart_source *src, bool *found,
                                       pc_str *name, pc_blob **bytes, pc_error *err)
{
    if (embedded_source_bytes(meta, src, found, name, bytes, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (*found) {
        return PC_OK;
    }
    if (!src->linked) {
        return PC_OK; /* embedded with no linked fallback */
    }
    const char *path = pc_str_c(&src->path);
    pc_blob *b = NULL;
    (void)read_file_blob(path, &b);
    if (b == NULL) {
        return PC_OK; /* None */
    }
    size_t len = strlen(path);
    size_t off = base_name_off(path, len);
    if (pc_str_set(name, path + off, len - off, err) != PC_OK) {
        pc_blob_release(b);
        return PC_ERR_NOMEM;
    }
    *bytes = b;
    *found = true;
    return PC_OK;
}

/* ============================================================== decode / place */

static pc_status decode_source(const char *name, const uint8_t *bytes, size_t len, pc_document *out,
                               pc_error *err)
{
    if (pc_engine_alloc_checkpoint("importing document", err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_io_import_result r;
    memset(&r, 0, sizeof r);
    if (pc_io_import(name, bytes, len, &r, err) != PC_OK) {
        return pc_fail(err, PC_ERR_FORMAT, "can't read smart object contents \"%s\"", name);
    }
    if (r.preview_only) {
        pc_io_import_result_fini(&r);
        return pc_fail(err, PC_ERR_FORMAT,
                       "can't read smart object contents \"%s\": only the embedded preview could be read", name);
    }
    *out = r.document;
    memset(&r.document, 0, sizeof r.document); /* moved out */
    pc_io_import_result_fini(&r);
    return PC_OK;
}

/* A compose buffer as a surface of `fmt` (buffer_to_surface). */
static pc_status buffer_to_surface(const pc_compose_buffer *buf, pc_pixel_format fmt, pc_surface *out,
                                   pc_error *err)
{
    pc_surface_init(out, fmt);
    if (buf->len == 0 || rect_empty(buf->rect)) {
        pc_surface_prune(out);
        return PC_OK;
    }
    float zero[4] = {0, 0, 0, 0};
    float tmp[PC_RASTER_MAX_CHANNELS];
    size_t n = pc_from_rgba(fmt, zero, tmp);
    size_t count = buf->len;
    float *data = NULL;
    if (pc_engine_alloc_capacity(count, n * sizeof *data, "smart object source", (void **)&data, err) != PC_OK) {
        pc_surface_fini(out);
        return PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < count; i++) {
        (void)pc_from_rgba(fmt, buf->px[i], &data[i * n]);
    }
    pc_status rc = pc_surface_write_region(out, buf->rect, data, count * n, err);
    free(data);
    if (rc != PC_OK) {
        pc_surface_fini(out);
        return rc;
    }
    pc_surface_prune(out);
    return PC_OK;
}

/* The composited source image of `bytes` in `fmt`; *bounds is the source's pixel box. */
static pc_status source_image(const char *name, const uint8_t *bytes, size_t len, pc_pixel_format fmt,
                              pc_surface *out, pc_rect *bounds, pc_error *err)
{
    pc_document doc;
    if (decode_source(name, bytes, len, &doc, err) != PC_OK) {
        return PC_ERR_FORMAT;
    }
    pc_compose_buffer buf;
    memset(&buf, 0, sizeof buf);
    pc_status rc = pc_compose_flatten(&doc, &buf, err);
    if (rc == PC_OK) {
        rc = buffer_to_surface(&buf, fmt, out, err);
    }
    *bounds = pc_document_bounds(&doc);
    pc_compose_buffer_fini(&buf);
    pc_document_fini(&doc);
    return rc;
}

/* An SVG source rendered at `scale` times its own size. */
static pc_status svg_source_image(const uint8_t *bytes, size_t len, pc_pixel_format fmt, double scale,
                                  pc_surface *out, pc_rect *bounds, pc_error *err)
{
    pc_compose_buffer buf;
    memset(&buf, 0, sizeof buf);
    if (pc_io_svg_rasterize(bytes, len, (float)scale, &buf, err) != PC_OK) {
        return pc_fail(err, PC_ERR_FORMAT, "can't render the SVG smart object");
    }
    *bounds = buf.rect;
    pc_status rc = buffer_to_surface(&buf, fmt, out, err);
    pc_compose_buffer_fini(&buf);
    return rc;
}

/* The scale a vector source is rasterised at (vector_scale). */
static double vector_scale(const pc_smart_object *sm)
{
    double a = sm->transform.m[0], b = sm->transform.m[1], c = sm->transform.m[2], d = sm->transform.m[3];
    double k = fmax(sqrt(a * a + b * b), sqrt(c * c + d * d));
    if (!isfinite(k)) {
        return 1.0;
    }
    double v = ceil(k * 16.0) / 16.0;
    if (v < 1.0 / 16.0) v = 1.0 / 16.0;
    if (v > 64.0) v = 64.0;
    return v;
}

static pc_algo_homography perspective_homography(const double p[9])
{
    pc_algo_homography h;
    for (int i = 0; i < 9; i++) {
        h.m[i] = p[i];
    }
    return h;
}

/* The source placed in document pixels, before any smart filter (place). */
static pc_status place(const char *name, const uint8_t *bytes, size_t len, pc_pixel_format fmt,
                       const pc_smart_object *sm, pc_surface *out, pc_error *err)
{
    const pc_warp *warp = sm->has_warp ? &sm->warp : NULL;
    if (!sm->has_stack_mode && !sm->has_warp && pc_io_is_svg(bytes, len)) {
        double k = vector_scale(sm);
        pc_surface img;
        pc_rect bounds;
        if (svg_source_image(bytes, len, fmt, k, &img, &bounds, err) != PC_OK) {
            return PC_ERR_FORMAT;
        }
        pc_status rc;
        if (sm->has_perspective) {
            pc_algo_homography h = perspective_homography(sm->perspective);
            h.m[0] /= k; h.m[1] /= k;
            h.m[3] /= k; h.m[4] /= k;
            h.m[6] /= k; h.m[7] /= k;
            rc = pc_algo_place_source_projective(&img, bounds, &h, NULL, out, err);
        } else {
            pc_affine t = sm->transform;
            t.m[0] /= k; t.m[1] /= k; t.m[2] /= k; t.m[3] /= k;
            rc = pc_algo_place_source(&img, bounds, &t, NULL, out, err);
        }
        pc_surface_fini(&img);
        return rc;
    }
    pc_surface img;
    pc_rect bounds;
    if (source_image(name, bytes, len, fmt, &img, &bounds, err) != PC_OK) {
        return PC_ERR_FORMAT;
    }
    pc_status rc;
    if (sm->has_perspective) {
        pc_algo_homography h = perspective_homography(sm->perspective);
        rc = pc_algo_place_source_projective(&img, bounds, &h, warp, out, err);
    } else {
        rc = pc_algo_place_source(&img, bounds, &sm->transform, warp, out, err);
    }
    pc_surface_fini(&img);
    return rc;
}

/* ============================================================== smart filters (E6, on E9's
 * pc_engine_apply_filter_to_surface). apply_smart_filters runs the stack on the placed source,
 * blending each visible filter with its blend mode and opacity, then mixing through the filter
 * mask. The stage cache (stack_cache) is a perf optimisation left out (§7): the stack recomputes
 * on each render, giving identical pixels. */

/* `top` blended over `base` with `mode` at `opacity` (a smart filter's blending options). */
static pc_status blend_surfaces(const pc_surface *base, const pc_surface *top, pc_blend_mode mode,
                                float opacity, pc_surface *out, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(base);
    pc_rect area = pc_rect_union(pc_surface_content_bounds(base), pc_surface_content_bounds(top));
    if (pc_surface_clone(base, out, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    if (pc_rect_is_empty(area)) {
        return PC_OK;
    }
    size_t n = pc_pixel_format_channels(fmt);
    float *b = NULL, *t = NULL, *o = NULL;
    size_t bl = 0, tl = 0;
    pc_status rc = pc_surface_read_region(base, area, &b, &bl, err);
    if (rc == PC_OK) {
        rc = pc_surface_read_region(top, area, &t, &tl, err);
    }
    if (rc == PC_OK && (o = malloc(bl > 0 ? bl * sizeof *o : 1)) == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (rc == PC_OK && n > 0) {
        size_t count = bl / n;
        for (size_t i = 0; i < count; i++) {
            float rb[4], rt[4], ro[4];
            pc_to_rgba(fmt, &b[i * n], n, rb);
            pc_to_rgba(fmt, &t[i * n], n, rt);
            pc_blend_composite(mode, rb, rt, opacity, ro);
            (void)pc_from_rgba(fmt, ro, &o[i * n]);
        }
        rc = pc_surface_write_region(out, area, o, bl, err);
    }
    free(b);
    free(t);
    free(o);
    if (rc == PC_OK) {
        pc_surface_prune(out);
    } else {
        pc_surface_fini(out);
    }
    return rc;
}

/* Mixes `filtered` over `base` through the filter mask (premultiplied, any channel layout). */
static pc_status mask_mix(const pc_surface *base, const pc_surface *filtered, const pc_layer_mask *mask,
                          pc_surface *out, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(base);
    pc_rect area = pc_rect_union(pc_surface_content_bounds(base), pc_surface_content_bounds(filtered));
    if (pc_surface_clone(filtered, out, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    if (pc_rect_is_empty(area)) {
        return PC_OK;
    }
    size_t n = pc_pixel_format_channels(fmt);
    float *b = NULL, *f = NULL, *o = NULL, *k = NULL;
    size_t bl = 0, fl = 0;
    pc_status rc = pc_surface_read_region(base, area, &b, &bl, err);
    if (rc == PC_OK) {
        rc = pc_surface_read_region(filtered, area, &f, &fl, err);
    }
    size_t count = n > 0 ? bl / n : 0;
    if (rc == PC_OK && (o = malloc(bl > 0 ? bl * sizeof *o : 1)) == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (rc == PC_OK && (k = malloc(count > 0 ? count * sizeof *k : 1)) == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (rc == PC_OK) {
        rc = pc_layer_mask_values_into(mask, area, k, count, err);
    }
    if (rc == PC_OK && n > 0) {
        bool alpha = fmt.alpha;
        for (size_t i = 0; i < count; i++) {
            const float *bp = &b[i * n], *fp = &f[i * n];
            float *op = &o[i * n];
            float m = k[i];
            m = m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m);
            if (!alpha) {
                for (size_t c = 0; c < n; c++) {
                    op[c] = bp[c] + (fp[c] - bp[c]) * m;
                }
                continue;
            }
            size_t a = n - 1;
            float oa = bp[a] + (fp[a] - bp[a]) * m;
            op[a] = oa;
            for (size_t c = 0; c < a; c++) {
                float pm = bp[c] * bp[a] + (fp[c] * fp[a] - bp[c] * bp[a]) * m;
                float v = oa > 0.0f ? pm / oa : 0.0f;
                op[c] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            }
        }
        rc = pc_surface_write_region(out, area, o, bl, err);
    }
    free(b);
    free(f);
    free(o);
    free(k);
    if (rc == PC_OK) {
        pc_surface_prune(out);
    } else {
        pc_surface_fini(out);
    }
    return rc;
}

/* A smart filter's stored params (a normalised pc_json_value) as an immutable yyjson_val the
 * apply primitive reads. *out_doc owns the value (yyjson_doc_free); *out_val may be NULL (null
 * params). */
static pc_status filter_params_val(const pc_smart_filter *f, yyjson_doc **out_doc, yyjson_val **out_val,
                                   pc_error *err)
{
    *out_doc = NULL;
    *out_val = NULL;
    yyjson_mut_val *root = pc_json_value_root(&f->params);
    if (root == NULL) {
        return PC_OK; /* null params */
    }
    pc_buf buf;
    pc_buf_init(&buf);
    pc_status rc = pc_json_write(root, 0, &buf, err);
    if (rc == PC_OK) {
        rc = pc_json_parse((const char *)buf.data, buf.len, out_doc, err);
    }
    pc_buf_fini(&buf);
    if (rc != PC_OK) {
        return rc;
    }
    *out_val = yyjson_doc_get_root(*out_doc);
    return PC_OK;
}

/* One visible smart filter over `cur`, blended with its blend mode and opacity. Unknown ids (a
 * Photoshop filter PhotoCraft does not implement) leave the pixels alone. */
static pc_status apply_smart_filter(const pc_smart_filter *f, const pc_surface *cur, pc_rect canvas,
                                    pc_rect extent, pc_surface *out, pc_error *err)
{
    yyjson_doc *pdoc = NULL;
    yyjson_val *pval = NULL;
    pc_status rc = filter_params_val(f, &pdoc, &pval, err);
    if (rc != PC_OK) {
        return rc;
    }
    bool applied = false;
    pc_surface filtered;
    rc = pc_engine_apply_filter_to_surface(pc_str_c(&f->command), pval, cur, canvas, NULL, extent,
                                           &filtered, &applied, err);
    if (pdoc != NULL) {
        yyjson_doc_free(pdoc);
    }
    if (rc != PC_OK) {
        return rc;
    }
    if (!applied) {
        return pc_surface_clone(cur, out, err); /* unknown filter: pixels unchanged */
    }
    float op = f->opacity;
    op = op < 0.0f ? 0.0f : (op > 1.0f ? 1.0f : op);
    if (f->blend == PC_BLEND_NORMAL && op >= 1.0f) {
        *out = filtered;
        return PC_OK;
    }
    rc = blend_surfaces(cur, &filtered, f->blend, op, out, err);
    pc_surface_fini(&filtered);
    return rc;
}

/* Runs the smart filter stack on the placed (unfiltered) content. The caller has checked that
 * there is at least one visible filter. */
static pc_status apply_smart_filters(const pc_surface *placed, const pc_smart_object *sm, pc_rect canvas,
                                     pc_surface *out, pc_error *err)
{
    /* Edge pixels repeat at the canvas union placed-content edge, as for layer filters. */
    pc_rect extent = pc_rect_union(canvas, pc_surface_content_bounds(placed));
    pc_surface cur;
    if (pc_surface_clone(placed, &cur, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_status rc = PC_OK;
    for (size_t i = 0; rc == PC_OK && i < sm->nsmart_filters; i++) {
        if (!sm->smart_filters[i].visible) {
            continue;
        }
        pc_surface next;
        rc = apply_smart_filter(&sm->smart_filters[i], &cur, canvas, extent, &next, err);
        if (rc == PC_OK) {
            pc_surface_fini(&cur);
            cur = next;
        }
    }
    if (rc != PC_OK) {
        pc_surface_fini(&cur);
        return rc;
    }
    if (sm->has_filter_mask && sm->filter_mask.enabled) {
        rc = mask_mix(placed, &cur, &sm->filter_mask, out, err);
        pc_surface_fini(&cur);
        return rc;
    }
    *out = cur;
    return PC_OK;
}

/* ============================================================== render / refresh */

static bool has_visible_filters(const pc_smart_object *sm)
{
    if (!sm->filters_enabled) {
        return false;
    }
    for (size_t i = 0; i < sm->nsmart_filters; i++) {
        if (sm->smart_filters[i].visible) {
            return true;
        }
    }
    return false;
}

pc_status pc_engine_smart_render(const pc_document *doc, const pc_smart_object *sm, bool *available,
                                 pc_surface *out, pc_error *err)
{
    *available = false;
    bool found = false;
    pc_str name = PC_STR_INIT;
    pc_blob *bytes = NULL;
    if (pc_engine_smart_source_bytes(&doc->metadata, &sm->source, &found, &name, &bytes, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (!found) {
        return PC_OK; /* source unavailable -> keep cache */
    }
    /* §7: Stack Mode still needs the deferred stack-combine (algo stack); a stack-mode smart
     * object keeps its cache. */
    if (sm->has_stack_mode) {
        pc_str_fini(&name);
        pc_blob_release(bytes);
        return PC_OK;
    }
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    pc_surface placed;
    pc_status rc = place(pc_str_c(&name), pc_blob_data(bytes), pc_blob_len(bytes), fmt, sm, &placed, err);
    pc_str_fini(&name);
    pc_blob_release(bytes);
    if (rc != PC_OK) {
        return rc;
    }
    if (!has_visible_filters(sm)) {
        *out = placed; /* no filters: the placed source is the appearance */
        *available = true;
        return PC_OK;
    }
    rc = apply_smart_filters(&placed, sm, pc_document_bounds(doc), out, err);
    pc_surface_fini(&placed);
    if (rc != PC_OK) {
        return rc;
    }
    *available = true;
    return PC_OK;
}

pc_status pc_engine_smart_refresh_layer(const pc_document *doc, pc_layer *l, bool *updated, pc_error *err)
{
    *updated = false;
    if (l->content.kind != PC_LAYER_SMART) {
        return PC_OK;
    }
    pc_smart_object *sm = &l->content.u.smart;
    bool available = false;
    pc_surface surf;
    if (pc_engine_smart_render(doc, sm, &available, &surf, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (!available) {
        return PC_OK; /* cache kept */
    }
    if (sm->has_cache) {
        pc_surface_fini(&sm->cache);
    }
    sm->cache = surf;
    sm->has_cache = true;
    *updated = true;
    return PC_OK;
}

pc_status pc_engine_smart_refresh(pc_document *doc, pc_layer_id id, bool *updated, pc_error *err)
{
    *updated = false;
    pc_layer *l = pc_document_layer_mut(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind != PC_LAYER_SMART) {
        return pc_fail(err, PC_ERR_INVALID, "the layer is not a smart object");
    }
    return pc_engine_smart_refresh_layer(doc, l, updated, err);
}

/* ============================================================== placement algebra */

pc_affine pc_engine_smart_snap_affine(pc_affine a)
{
    for (int i = 0; i < 6; i++) {
        double r = round(a.m[i]);
        if (fabs(a.m[i] - r) < 1e-9) {
            a.m[i] = r;
        }
    }
    return a;
}

pc_algo_homography pc_engine_smart_affine_homography(pc_affine a)
{
    pc_algo_homography h;
    h.m[0] = a.m[0]; h.m[1] = a.m[2]; h.m[2] = a.m[4];
    h.m[3] = a.m[1]; h.m[4] = a.m[3]; h.m[5] = a.m[5];
    h.m[6] = 0.0;    h.m[7] = 0.0;    h.m[8] = 1.0;
    return h;
}

void pc_engine_smart_placement(const pc_smart_object *sm, pc_algo_homography *out)
{
    if (sm->has_perspective) {
        *out = perspective_homography(sm->perspective);
    } else {
        *out = pc_engine_smart_affine_homography(sm->transform);
    }
}

static pc_algo_homography homography_mul(pc_algo_homography a, pc_algo_homography b)
{
    pc_algo_homography c;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            c.m[r * 3 + col] = a.m[r * 3 + 0] * b.m[0 * 3 + col] + a.m[r * 3 + 1] * b.m[1 * 3 + col] +
                               a.m[r * 3 + 2] * b.m[2 * 3 + col];
        }
    }
    return c;
}

static void homography_apply(const pc_algo_homography *h, double x, double y, double *ox, double *oy)
{
    pc_algo_homography_apply(h, x, y, ox, oy);
}

void pc_engine_smart_set_placement(pc_smart_object *sm, const pc_algo_homography *h)
{
    double n[9];
    if (fabs(h->m[8]) > 1e-12) {
        for (int i = 0; i < 9; i++) {
            n[i] = h->m[i] / h->m[8];
        }
    } else {
        for (int i = 0; i < 9; i++) {
            n[i] = h->m[i];
        }
    }
    if (fabs(n[6]) < 1e-12 && fabs(n[7]) < 1e-12) {
        pc_affine t = {{n[0], n[3], n[1], n[4], n[2], n[5]}};
        sm->transform = pc_engine_smart_snap_affine(t);
        sm->has_perspective = false;
        return;
    }
    pc_algo_homography hn = perspective_homography(n);
    double ox, oy, xx, xy, yx, yy;
    homography_apply(&hn, 0.0, 0.0, &ox, &oy);
    homography_apply(&hn, 1.0, 0.0, &xx, &xy);
    homography_apply(&hn, 0.0, 1.0, &yx, &yy);
    pc_affine t = {{xx - ox, xy - oy, yx - ox, yy - oy, ox, oy}};
    sm->transform = t;
    sm->has_perspective = true;
    for (int i = 0; i < 9; i++) {
        sm->perspective[i] = n[i];
    }
}

void pc_engine_smart_transform_placement(pc_smart_object *sm, pc_affine a)
{
    if (!sm->has_perspective) {
        sm->transform = pc_engine_smart_snap_affine(pc_affine_mul(a, sm->transform));
        return;
    }
    pc_algo_homography cur;
    pc_engine_smart_placement(sm, &cur);
    pc_algo_homography h = homography_mul(pc_engine_smart_affine_homography(a), cur);
    pc_engine_smart_set_placement(sm, &h);
}

static pc_status translate_in_place(pc_surface *s, int32_t dx, int32_t dy, pc_error *err)
{
    pc_surface moved;
    if (pc_algo_translate_surface(s, dx, dy, &moved, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_surface_fini(s);
    *s = moved;
    return PC_OK;
}

pc_status pc_engine_smart_shift(pc_smart_object *sm, int32_t dx, int32_t dy, pc_error *err)
{
    pc_engine_smart_transform_placement(sm, pc_affine_translate((double)dx, (double)dy));
    if (sm->has_cache) {
        if (translate_in_place(&sm->cache, dx, dy, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    }
    if (sm->has_filter_mask) {
        if (translate_in_place(&sm->filter_mask.surface, dx, dy, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    }
    return PC_OK;
}

/* ============================================================== stage cache (deferred) */

size_t pc_engine_smart_stack_cache_bytes(void)
{
    return 0; /* §7: the stage cache lives with the deferred filter subsystem */
}

size_t pc_engine_smart_purge_stack_cache(void)
{
    return 0;
}

uint32_t pc_engine_smart_source_decode_count(const uint8_t *bytes, size_t len, pc_pixel_format fmt)
{
    (void)bytes;
    (void)len;
    (void)fmt;
    return 0; /* §7: no decoded-source cache in this build */
}

/* ============================================================== HOOK SEAM (moved from hooks.c) */

/* E6: move the non-pixel geometry of a smart layer through `a` (placement only; pixels/caches are
 * remapped by canvas_geom itself). Relocated from hooks.c. */
void pc_engine_hook_transform_smart(pc_layer *l, const pc_affine *a)
{
    if (l->content.kind == PC_LAYER_SMART) {
        pc_engine_smart_transform_placement(&l->content.u.smart, *a);
    }
}

/* E6: re-render a smart layer after a geometry change. Relocated from hooks.c. Best effort: a
 * filtered or unreadable smart object keeps its cache (see §7). */
void pc_engine_hook_refresh_smart(const pc_document *snapshot, pc_layer *l)
{
    if (l->content.kind == PC_LAYER_SMART) {
        bool updated = false;
        (void)pc_engine_smart_refresh_layer(snapshot, l, &updated, NULL);
    }
}
