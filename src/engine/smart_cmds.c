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
#include "doc/vector.h" /* pc_path_transform (shift_layer vector mask) */
#include "format/format.h" /* pc_format_save_to_bytes (encode_source) */
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
#include "vector_cmds.h" /* pc_engine_transform_shape (shift_layer shape) */

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

/* Run the session's authorize gate for a linked-source `path` under command `cmd`. PC_OK when no
 * gate is set. */
static pc_status authorize_path(pc_engine_session *s, const char *cmd, const char *path, pc_error *err)
{
    if (s == NULL || s->authorize == NULL) {
        return PC_OK;
    }
    yyjson_mut_doc *jd = yyjson_mut_doc_new(NULL);
    if (jd == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "authorize");
    }
    yyjson_mut_val *o = yyjson_mut_obj(jd);
    yyjson_mut_obj_add_str(jd, o, "path", path);
    yyjson_mut_doc_set_root(jd, o);
    yyjson_doc *imm = yyjson_mut_doc_imut_copy(jd, NULL);
    yyjson_mut_doc_free(jd);
    if (imm == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "authorize");
    }
    pc_status rc = s->authorize(cmd, yyjson_doc_get_root(imm), err);
    yyjson_doc_free(imm);
    return rc;
}

pc_status pc_engine_smart_authorized_source_bytes(pc_engine_session *s, const char *cmd,
                                                  const pc_metadata *meta, const pc_smart_source *src,
                                                  bool *found, pc_str *name, pc_blob **bytes,
                                                  pc_error *err)
{
    /* Embedded bytes and PSD-uuid-embedded data never reach the filesystem, so no gate. */
    if (embedded_source_bytes(meta, src, found, name, bytes, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (*found || !src->linked) {
        return PC_OK;
    }
    /* A genuine linked file on disk: recheck the path before reading it. */
    const char *path = pc_str_c(&src->path);
    pc_status arc = authorize_path(s, cmd, path, err);
    if (arc != PC_OK) {
        return arc;
    }
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

pc_status pc_engine_smart_authorize_source(pc_engine_session *s, const char *cmd,
                                           const pc_document *doc, const pc_smart_source *src,
                                           pc_error *err)
{
    if (s == NULL || s->authorize == NULL || !src->linked) {
        return PC_OK;
    }
    /* Embedded PSD data addressed by a Linked uuid never reaches the filesystem: no gate. */
    const char *path = pc_str_c(&src->path);
    bool lfound = false;
    pc_io_linked_file lf;
    memset(&lf, 0, sizeof lf);
    if (pc_io_find_linked_file(&doc->metadata, path, strlen(path), &lfound, &lf, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (lfound) {
        pc_io_linked_file_fini(&lf);
        return PC_OK;
    }
    return authorize_path(s, cmd, path, err);
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

pc_status pc_engine_smart_decode_source(const char *name, const uint8_t *bytes, size_t len,
                                        pc_document *out, pc_error *err)
{
    return decode_source(name, bytes, len, out, err);
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

/* ============================================= recording (add_smart_filter, E-tail) */

/* The smart object `id` of `doc`, or NULL with *err set (no such layer / not a smart object). */
static pc_smart_object *add_smart_mut(pc_document *doc, pc_layer_id id, pc_error *err)
{
    pc_layer *l = pc_document_layer_mut(doc, id);
    if (l == NULL) {
        (void)pc_engine_err_no_layer(err, id);
        return NULL;
    }
    if (l->content.kind != PC_LAYER_SMART) {
        (void)pc_engine_err_other(err, "the layer is not a smart object");
        return NULL;
    }
    return &l->content.u.smart;
}

/* A selection becomes a filter mask (reveal=enabled, linked, full density, no feather). */
static pc_status mask_from_selection(const pc_surface *sel, pc_layer_mask *out, pc_error *err)
{
    if (pc_surface_clone(sel, &out->surface, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    out->enabled = true;
    out->linked = true;
    out->density = 1.0f;
    out->feather = 0.0f;
    return PC_OK;
}

/* Append *sf to the stack (ownership moved: *sf is zeroed on success, untouched on OOM). */
static pc_status push_filter(pc_smart_object *sm, pc_smart_filter *sf, pc_error *err)
{
    pc_smart_filter *grown = realloc(sm->smart_filters, (sm->nsmart_filters + 1) * sizeof *grown);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    sm->smart_filters = grown;
    sm->smart_filters[sm->nsmart_filters++] = *sf;
    memset(sf, 0, sizeof *sf);
    return PC_OK;
}

pc_status pc_engine_add_smart_filter(pc_document *doc, pc_layer_id id, pc_smart_filter *sf,
                                     const pc_surface *selection, pc_error *err)
{
    pc_rect canvas = pc_document_bounds(doc);
    pc_smart_object *sm = add_smart_mut(doc, id, err);
    if (sm == NULL) {
        pc_smart_filter_fini(sf);
        return PC_ERR_INVALID;
    }
    /* renderable = the source can be read (the path is the document's own, so no authorize gate). */
    bool found = false;
    pc_str name = PC_STR_INIT;
    pc_blob *bytes = NULL;
    if (pc_engine_smart_source_bytes(&doc->metadata, &sm->source, &found, &name, &bytes, err) != PC_OK) {
        pc_smart_filter_fini(sf);
        return PC_ERR_INVALID;
    }
    pc_str_fini(&name);
    pc_blob_release(bytes);

    if (found) {
        /* A selection becomes the filter mask (as in Photoshop) when there is none yet. */
        if (selection != NULL && !sm->has_filter_mask) {
            pc_layer_mask mask;
            memset(&mask, 0, sizeof mask);
            if (mask_from_selection(selection, &mask, err) != PC_OK) {
                pc_smart_filter_fini(sf);
                return PC_ERR_NOMEM;
            }
            sm->filter_mask = mask;
            sm->has_filter_mask = true;
        }
        if (push_filter(sm, sf, err) != PC_OK) {
            pc_smart_filter_fini(sf);
            return PC_ERR_NOMEM;
        }
        bool updated = false;
        if (pc_engine_smart_refresh(doc, id, &updated, err) != PC_OK) {
            return PC_ERR_INVALID;
        }
        if (!updated) {
            return pc_engine_err_other(
                err, "the smart object's contents are unavailable (missing linked file?), so it "
                     "can't be re-rendered");
        }
        return PC_OK;
    }

    /* Without a renderable source, apply to the cached pixels and still record the filter. */
    pc_rect bounds = canvas;
    if (selection != NULL) {
        pc_rect sb = pc_surface_content_bounds(selection);
        if (!pc_rect_is_empty(sb)) {
            bounds = sb;
        }
    }
    if (!sm->has_cache) {
        pc_smart_filter_fini(sf);
        return pc_engine_err_other(err, "smart object has no pixels");
    }
    yyjson_doc *pdoc = NULL;
    yyjson_val *pval = NULL;
    if (filter_params_val(sf, &pdoc, &pval, err) != PC_OK) {
        pc_smart_filter_fini(sf);
        return PC_ERR_INVALID;
    }
    pc_surface out;
    bool applied = false;
    pc_status rc = pc_engine_apply_filter_to_surface(pc_str_c(&sf->command), pval, &sm->cache, bounds,
                                                     selection, canvas, &out, &applied, err);
    if (pdoc != NULL) {
        yyjson_doc_free(pdoc);
    }
    if (rc != PC_OK) {
        pc_smart_filter_fini(sf);
        return rc;
    }
    if (!applied) {
        pc_status e = pc_engine_err_other(err, "unknown filter %s", pc_str_c(&sf->command));
        pc_smart_filter_fini(sf);
        return e;
    }
    pc_surface_fini(&sm->cache);
    sm->cache = out;
    if (push_filter(sm, sf, err) != PC_OK) {
        pc_smart_filter_fini(sf);
        return PC_ERR_NOMEM;
    }
    return PC_OK;
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

/* ============================================= Convert to Smart Object (layer_to_smart / etc.) */

/* Move a surface by whole pixels (translate_surface); best-effort on OOM (leaves it as it was). */
static void shift_surface(pc_surface *s, int32_t dx, int32_t dy)
{
    pc_surface moved;
    if (pc_algo_translate_surface(s, dx, dy, &moved, NULL) == PC_OK) {
        pc_surface_fini(s);
        *s = moved;
    }
}

/* Move a layer (any kind) by whole pixels without re-rendering anything (shift_layer). */
static void shift_layer(pc_layer *l, int32_t dx, int32_t dy)
{
    if (dx == 0 && dy == 0) {
        return;
    }
    pc_affine a = pc_affine_translate((double)dx, (double)dy);
    if (l->has_mask) {
        shift_surface(&l->mask.surface, dx, dy);
    }
    if (l->has_vector_mask) {
        pc_path np;
        if (pc_path_transform(&l->vector_mask.path, a, &np, NULL) == PC_OK) {
            pc_path_fini(&l->vector_mask.path);
            l->vector_mask.path = np;
        }
    }
    if (l->has_fill_cache) {
        shift_surface(&l->fill_cache.surface, dx, dy);
    }
    switch (l->content.kind) {
    case PC_LAYER_RASTER:
        shift_surface(&l->content.u.raster, dx, dy);
        break;
    case PC_LAYER_TEXT: {
        pc_text_layer *t = &l->content.u.text;
        t->transform = pc_affine_mul(a, t->transform);
        if (t->has_cache) {
            shift_surface(&t->cache, dx, dy);
        }
        break;
    }
    case PC_LAYER_SHAPE: {
        pc_shape_layer *sh = &l->content.u.shape;
        pc_engine_transform_shape(sh, &a);
        if (sh->has_cache) {
            shift_surface(&sh->cache, dx, dy);
        }
        break;
    }
    case PC_LAYER_SMART:
        (void)pc_engine_smart_shift(&l->content.u.smart, dx, dy, NULL);
        break;
    case PC_LAYER_GROUP: {
        pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            shift_layer(&g->children[i], dx, dy);
        }
        break;
    }
    default: /* adjustment, fill: no pixels to move */
        break;
    }
}

void pc_engine_smart_shift_layer(pc_layer *l, int32_t dx, int32_t dy)
{
    shift_layer(l, dx, dy);
}

/* layers_to_smart's subtree bounds: a group spans its children, an adjustment/fill spans the
 * canvas, and any other layer spans its surface. */
static pc_rect subtree_bounds(const pc_layer *l, pc_rect canvas)
{
    if (l->content.kind == PC_LAYER_GROUP) {
        pc_rect b = PC_RECT_EMPTY;
        const pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            b = pc_rect_union(b, subtree_bounds(&g->children[i], canvas));
        }
        return b;
    }
    if (l->content.kind == PC_LAYER_ADJUSTMENT || l->content.kind == PC_LAYER_FILL) {
        return canvas;
    }
    const pc_surface *s = pc_layer_surface(l);
    return s != NULL ? pc_surface_content_bounds(s) : PC_RECT_EMPTY;
}

static bool layer_has_fx(const pc_layer *l)
{
    if (l->effects.enabled && l->effects.nitems > 0) {
        return true;
    }
    if (l->content.kind == PC_LAYER_GROUP) {
        const pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            if (layer_has_fx(&g->children[i])) {
                return true;
            }
        }
    }
    return false;
}

static bool layer_has_nested_smart(const pc_layer *l)
{
    if (l->content.kind == PC_LAYER_SMART) {
        return true;
    }
    if (l->content.kind == PC_LAYER_GROUP) {
        const pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            if (layer_has_nested_smart(&g->children[i])) {
                return true;
            }
        }
    }
    return false;
}

/* Build the smart-object layer for `layers` (ownership moved in; freed before return): render them
 * as a nested .pcraft document cropped to their rendered bounds, with the composite as the cache.
 * On error the layers are freed and *out is untouched (layers_to_smart). */
pc_status pc_engine_layers_to_smart(const pc_document *doc, const char *name, pc_layer *layers,
                                    size_t n, pc_layer *out, pc_error *err)
{
    pc_rect canvas = pc_document_bounds(doc);
    pc_pixel_format fmt = pc_document_pixel_format(doc);

    size_t namelen = strlen(name);
    char *subname = malloc(namelen + 8);
    if (subname == NULL) {
        for (size_t i = 0; i < n; i++) {
            pc_layer_fini(&layers[i]);
        }
        free(layers);
        return pc_fail(err, PC_ERR_NOMEM, "convert to smart object");
    }
    memcpy(subname, name, namelen);
    memcpy(subname + namelen, ".pcraft", 8);

    pc_document sub;
    pc_status rc = pc_document_init(&sub, subname, doc->size, doc->mode, doc->depth, err);
    if (rc != PC_OK) {
        free(subname);
        for (size_t i = 0; i < n; i++) {
            pc_layer_fini(&layers[i]);
        }
        free(layers);
        return rc;
    }
    sub.resolution_dpi = doc->resolution_dpi;
    sub.icc_profile = pc_blob_retain(doc->icc_profile);
    sub.global_light = doc->global_light;

    bool has_fx = false, nested = false;
    for (size_t i = 0; i < n; i++) {
        has_fx = has_fx || layer_has_fx(&layers[i]);
        nested = nested || layer_has_nested_smart(&layers[i]);
    }
    /* Pattern fills and pattern effects reference document patterns by id: keep them. */
    if (doc->npatterns > 0) {
        sub.patterns = pc_malloc_array(doc->npatterns, sizeof *sub.patterns);
        if (sub.patterns == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "convert to smart object");
            goto fail_sub;
        }
        for (size_t i = 0; i < doc->npatterns; i++) {
            if (pc_pattern_clone(&doc->patterns[i], &sub.patterns[i], err) != PC_OK) {
                for (size_t k = 0; k < i; k++) {
                    pc_pattern_fini(&sub.patterns[k]);
                }
                free(sub.patterns);
                sub.patterns = NULL;
                rc = err != NULL ? err->code : PC_ERR_NOMEM;
                goto fail_sub;
            }
        }
        sub.npatterns = doc->npatterns;
    }
    /* Nested PSD placed layers find their embedded files in the global blocks. */
    if (nested && doc->metadata.npsd_global_blocks > 0) {
        size_t gn = doc->metadata.npsd_global_blocks;
        sub.metadata.psd_global_blocks = pc_malloc_array(gn, sizeof *sub.metadata.psd_global_blocks);
        if (sub.metadata.psd_global_blocks == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "convert to smart object");
            goto fail_sub;
        }
        for (size_t i = 0; i < gn; i++) {
            sub.metadata.psd_global_blocks[i] = doc->metadata.psd_global_blocks[i];
            sub.metadata.psd_global_blocks[i].data = pc_blob_retain(doc->metadata.psd_global_blocks[i].data);
        }
        sub.metadata.npsd_global_blocks = gn;
    }

    pc_rect region = canvas;
    for (size_t i = 0; i < n; i++) {
        region = pc_rect_union(region, subtree_bounds(&layers[i], canvas));
    }
    if (has_fx) {
        region = pc_rect_make(region.x0 - 256, region.y0 - 256, region.x1 + 256, region.y1 + 256);
    }
    /* Ownership of the layer array moves into the sub-document. */
    free(sub.layers);
    sub.layers = layers;
    sub.nlayers = n;
    layers = NULL;

    pc_compose_buffer buf;
    memset(&buf, 0, sizeof buf);
    rc = pc_compose_render(&sub, region, &buf, err);
    if (rc != PC_OK) {
        goto fail_sub;
    }
    /* The rendered content's tight bounds (opaque pixels). */
    uint32_t bw = pc_rect_width(buf.rect);
    pc_rect b = PC_RECT_EMPTY;
    for (size_t i = 0; i < buf.len; i++) {
        if (buf.px[i][3] > 0.0f) {
            int32_t x = buf.rect.x0 + (int32_t)(i % bw);
            int32_t y = buf.rect.y0 + (int32_t)(i / bw);
            b = pc_rect_union(b, pc_rect_make(x, y, x + 1, y + 1));
        }
    }
    if (pc_rect_is_empty(b)) {
        pc_compose_buffer_fini(&buf);
        rc = pc_engine_err_other(err, "the layer is empty, so there is nothing to convert");
        goto fail_sub;
    }
    pc_surface cache;
    rc = buffer_to_surface(&buf, fmt, &cache, err);
    pc_compose_buffer_fini(&buf);
    if (rc != PC_OK) {
        goto fail_sub;
    }
    pc_surface cropped;
    if (pc_algo_crop_surface(&cache, b, &cropped, err) != PC_OK) {
        pc_surface_fini(&cache);
        rc = err != NULL ? err->code : PC_ERR_NOMEM;
        goto fail_sub;
    }
    pc_surface_fini(&cache);
    cache = cropped;
    pc_surface_prune(&cache);

    for (size_t i = 0; i < sub.nlayers; i++) {
        shift_layer(&sub.layers[i], -b.x0, -b.y0);
    }
    sub.size = (pc_size){pc_rect_width(b), pc_rect_height(b)};

    pc_buf bytes = PC_BUF_INIT;
    pc_format_save_opts opts = {NULL, NULL};
    rc = pc_format_save_to_bytes(&sub, &opts, &bytes, err);
    if (rc != PC_OK) {
        pc_surface_fini(&cache);
        pc_buf_fini(&bytes);
        rc = pc_fail(err, PC_ERR_INVALID, "can't encode smart object contents");
        goto fail_sub;
    }
    pc_blob *blob = NULL;
    rc = pc_blob_new(bytes.data, bytes.len, &blob, err);
    pc_buf_fini(&bytes);
    if (rc != PC_OK) {
        pc_surface_fini(&cache);
        goto fail_sub;
    }

    pc_smart_source source;
    source.linked = false;
    source.bytes = blob;
    source.path = (pc_str)PC_STR_INIT;
    source.file_name = (pc_str)PC_STR_INIT;
    if (pc_str_set(&source.file_name, subname, strlen(subname), err) != PC_OK) {
        pc_blob_release(blob);
        pc_surface_fini(&cache);
        rc = err != NULL ? err->code : PC_ERR_NOMEM;
        goto fail_sub;
    }

    pc_smart_object so;
    pc_affine placement = pc_affine_translate((double)b.x0, (double)b.y0);
    pc_smart_object_init(&so, &source, placement, &cache); /* takes over source + cache */

    pc_layer_content content = {0};
    content.kind = PC_LAYER_SMART;
    content.u.smart = so;
    rc = pc_layer_init(out, name, &content, err);
    if (rc != PC_OK) {
        pc_layer_content_fini(&content);
        goto fail_sub;
    }
    pc_document_fini(&sub);
    free(subname);
    return PC_OK;

fail_sub:
    if (layers != NULL) {
        for (size_t i = 0; i < n; i++) {
            pc_layer_fini(&layers[i]);
        }
        free(layers);
    }
    pc_document_fini(&sub);
    free(subname);
    return rc;
}

/* The smart-object layer replacing `l` in `doc`: the layer (with its mask, effects and fill
 * opacity) becomes a nested document; visibility, opacity, blend, clipping and label stay on the
 * smart layer (layer_to_smart). */
pc_status pc_engine_layer_to_smart(const pc_document *doc, const pc_layer *l, pc_layer *out, pc_error *err)
{
    bool background = pc_document_is_background(doc, l->id);
    pc_layer inner;
    if (pc_layer_clone(l, &inner, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    inner.visible = true;
    inner.opacity = 1.0f;
    inner.clipped = false;
    inner.locks = (pc_locks){0};
    if (!(pc_layer_is_group(&inner) && inner.blend == PC_BLEND_PASS_THROUGH)) {
        inner.blend = PC_BLEND_NORMAL;
    }
    if (background) {
        if (pc_str_set(&inner.name, "Layer 0", 7, err) != PC_OK) {
            pc_layer_fini(&inner);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }
    /* Keep the original name for the smart object (and the nested document). */
    char *keep = NULL;
    size_t keeplen = l->name.len;
    keep = malloc(keeplen + 1);
    if (keep == NULL) {
        pc_layer_fini(&inner);
        return pc_fail(err, PC_ERR_NOMEM, "convert to smart object");
    }
    memcpy(keep, pc_str_c(&l->name), keeplen);
    keep[keeplen] = '\0';

    pc_layer *arr = pc_malloc_array(1, sizeof *arr);
    if (arr == NULL) {
        pc_layer_fini(&inner);
        free(keep);
        return pc_fail(err, PC_ERR_NOMEM, "convert to smart object");
    }
    arr[0] = inner;
    pc_status rc = pc_engine_layers_to_smart(doc, keep, arr, 1, out, err); /* consumes arr */
    if (rc != PC_OK) {
        free(keep);
        return rc;
    }
    if (background) {
        (void)pc_str_set(&out->name, "Layer 0", 7, NULL);
    }
    out->visible = l->visible;
    out->opacity = l->opacity;
    out->blend = (l->blend == PC_BLEND_PASS_THROUGH) ? PC_BLEND_NORMAL : l->blend;
    out->clipped = l->clipped;
    out->label = l->label;
    if (!background) {
        out->locks = l->locks;
    }
    free(keep);
    return PC_OK;
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
