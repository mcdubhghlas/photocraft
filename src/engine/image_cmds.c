/* Image menu (port of crates/engine/src/image_cmds.rs, E3): Image Size, Canvas Size, Crop, Trim,
 * Mode (colour model and bit depth), Duplicate, Reveal All, and the right-angle Image Rotation /
 * flips. The palette / ink modes (Indexed, Color Table, Bitmap, Duotone) and Image Rotation ›
 * Arbitrary live in mode_cmds.c.
 *
 * for_each_surface is exported (pc_engine_image_for_each_surface) for E4 transform_cmds. The
 * canvas-geometry refresh of vector/type/smart content is routed through the E11/E6 refresh hooks.
 *
 * Never crash (AGENTS.md / REWRITE_PLAN rule 9): every dimension, offset and rectangle from params
 * is bounds-checked before allocation; absurd sizes are refused before anything changes. */
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/json.h"
#include "base/mem.h"
#include "color/color.h"
#include "color/convert.h"
#include "compose/adjust.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "doc/effects.h"
#include "geom/geom.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "algo/params.h"
#include "algo/resample.h"

#include "engine_internal.h"

/* ---- surface / layer walkers -------------------------------------------------------------- */

void pc_engine_image_for_each_surface(pc_layer *layers, size_t n, bool masks, pc_engine_surface_cb cb,
                                      void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        pc_layer *l = &layers[i];
        if (masks && l->has_mask) {
            cb(&l->mask.surface, true, ctx);
        }
        if (l->has_fill_cache) {
            cb(&l->fill_cache.surface, false, ctx);
        }
        switch (l->content.kind) {
        case PC_LAYER_RASTER:
            cb(&l->content.u.raster, false, ctx);
            break;
        case PC_LAYER_TEXT:
            if (l->content.u.text.has_cache) {
                cb(&l->content.u.text.cache, false, ctx);
            }
            break;
        case PC_LAYER_SHAPE:
            if (l->content.u.shape.has_cache) {
                cb(&l->content.u.shape.cache, false, ctx);
            }
            break;
        case PC_LAYER_SMART:
            if (l->content.u.smart.has_cache) {
                cb(&l->content.u.smart.cache, false, ctx);
            }
            /* Selection-derived smart-filter masks live in document coordinates: carry them with
             * the cache on every canvas resize/translation (#3077). */
            if (masks && l->content.u.smart.has_filter_mask) {
                cb(&l->content.u.smart.filter_mask.surface, true, ctx);
            }
            break;
        case PC_LAYER_GROUP:
            pc_engine_image_for_each_surface(l->content.u.group.children,
                                             l->content.u.group.nchildren, masks, cb, ctx);
            break;
        default:
            break;
        }
    }
}

typedef void (*layer_cb)(pc_layer *l, void *ctx);

static void for_each_layer(pc_layer *layers, size_t n, layer_cb cb, void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        cb(&layers[i], ctx);
        if (layers[i].content.kind == PC_LAYER_GROUP) {
            for_each_layer(layers[i].content.u.group.children, layers[i].content.u.group.nchildren, cb,
                           ctx);
        }
    }
}

/* ---- scale effects (Image Size's Scale Styles) -------------------------------------------- */

static void scale_effects(pc_effects *fx, float k)
{
    for (size_t i = 0; i < fx->nitems; i++) {
        pc_effect *e = &fx->items[i];
        switch (e->kind) {
        case PC_FX_DROP_SHADOW:
        case PC_FX_INNER_SHADOW:
            e->u.shadow.distance *= k;
            e->u.shadow.size *= k;
            break;
        case PC_FX_OUTER_GLOW:
        case PC_FX_INNER_GLOW:
            e->u.glow.size *= k;
            break;
        case PC_FX_STROKE:
            e->u.stroke.size *= k;
            if (e->u.stroke.paint.kind == PC_FX_PAINT_PATTERN) {
                e->u.stroke.paint.u.pattern.scale *= k;
            }
            break;
        case PC_FX_SATIN:
            e->u.satin.distance *= k;
            e->u.satin.size *= k;
            break;
        case PC_FX_BEVEL_EMBOSS:
            e->u.bevel.size *= k;
            e->u.bevel.soften *= k;
            break;
        case PC_FX_PATTERN_OVERLAY:
            e->u.pattern_overlay.scale *= k;
            break;
        default: /* colour / gradient overlays do not scale */
            break;
        }
    }
    if (k != 1.0f) { /* effects changed: the preserved PSD block no longer applies */
        pc_blob_release(fx->psd_raw);
        fx->psd_raw = NULL;
    }
}

/* Public wrapper so Layer > Layer Style > Scale Effects (layerstyle_gap_cmds.c) can reuse the exact
 * same scaling (Image Size's Scale Styles). */
void pc_engine_scale_effects(pc_effects *fx, float k)
{
    scale_effects(fx, k);
}

static void scale_effects_cb(pc_layer *l, void *ctx)
{
    float k = *(const float *)ctx;
    scale_effects(&l->effects, k);
}

/* ---- refresh vector/type/smart caches (canvas_geom::refresh) ------------------------------ */

static void refresh_rec(const pc_document *snap, pc_layer *layers, size_t n, bool all)
{
    for (size_t i = 0; i < n; i++) {
        pc_layer *l = &layers[i];
        switch (l->content.kind) {
        case PC_LAYER_GROUP:
            refresh_rec(snap, l->content.u.group.children, l->content.u.group.nchildren, all);
            break;
        case PC_LAYER_SHAPE:
            pc_engine_hook_refresh_shape(snap, l);
            break;
        case PC_LAYER_TEXT:
            if (all) {
                pc_engine_hook_refresh_text(snap, l);
            }
            break;
        case PC_LAYER_SMART:
            if (all) {
                pc_engine_hook_refresh_smart(snap, l);
            }
            break;
        default:
            break;
        }
    }
}

/* Re-render shape (always) and, when `all`, type and smart-object caches from a snapshot of the
 * just-moved document. Best-effort: on a clone OOM nothing is refreshed (the move still stands). */
static void refresh_doc(pc_document *doc, bool all)
{
    pc_document snap;
    if (pc_document_clone(doc, &snap, NULL) != PC_OK) {
        return;
    }
    refresh_rec(&snap, doc->layers, doc->nlayers, all);
    pc_document_fini(&snap);
}

/* ---- errors ------------------------------------------------------------------------------- */

__attribute__((format(printf, 3, 4))) static pc_status bad(pc_error *err, const char *cmd,
                                                           const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return pc_engine_err_bad_params(err, cmd, "%s", msg);
}

/* ---- resampling method -------------------------------------------------------------------- */

typedef enum { METHOD_NONE, METHOD_FILTER, METHOD_AUTOMATIC } method_kind;

typedef struct {
    method_kind kind;
    pc_algo_resample filter;
} method;

static method parse_resample(const char *s)
{
    method m = {METHOD_FILTER, PC_ALGO_RESAMPLE_BICUBIC};
    if (s == NULL) {
        return m;
    }
    if (strcmp(s, "none") == 0) {
        m.kind = METHOD_NONE;
    } else if (strcmp(s, "automatic") == 0) {
        m.kind = METHOD_AUTOMATIC;
    } else if (strcmp(s, "nearest") == 0 || strcmp(s, "nearestNeighbor") == 0) {
        m.filter = PC_ALGO_RESAMPLE_NEAREST;
    } else if (strcmp(s, "bilinear") == 0) {
        m.filter = PC_ALGO_RESAMPLE_BILINEAR;
    } else if (strcmp(s, "bicubicSmoother") == 0) {
        m.filter = PC_ALGO_RESAMPLE_BICUBIC_SMOOTHER;
    } else if (strcmp(s, "bicubicSharper") == 0) {
        m.filter = PC_ALGO_RESAMPLE_BICUBIC_SHARPER;
    } else if (strcmp(s, "bicubicAutomatic") == 0) {
        m.filter = PC_ALGO_RESAMPLE_BICUBIC_AUTOMATIC;
    } else if (strcmp(s, "lanczos") == 0) {
        m.filter = PC_ALGO_RESAMPLE_LANCZOS;
    } else if (strcmp(s, "preserveDetails") == 0) {
        m.filter = PC_ALGO_RESAMPLE_PRESERVE_DETAILS;
    }
    return m;
}

/* The largest raster Image Size / Crop resamples to: the decoders' default allocation budget. */
static uint64_t max_resample_bytes(void)
{
    return (sizeof(void *) >= 8) ? ((uint64_t)8 << 30) : ((uint64_t)2 << 30);
}

static pc_status check_resample_budget(pc_error *err, const char *cmd, const pc_document *doc,
                                       uint32_t nw, uint32_t nh, const char *hint)
{
    uint64_t bpp = pc_pixel_format_bytes_per_pixel(pc_document_pixel_format(doc));
    if (bpp == 0) {
        bpp = 1;
    }
    uint64_t px = (uint64_t)nw * (uint64_t)nh;
    uint64_t bytes = (px > UINT64_MAX / bpp) ? UINT64_MAX : px * bpp;
    if (bytes <= max_resample_bytes()) {
        return PC_OK;
    }
    uint64_t mp = px / 1000000u;
    uint64_t max_mp = max_resample_bytes() / bpp / 1000000u;
    return bad(err, cmd,
               "%u x %u px is %llu megapixels; resampling at this bit depth is limited to %llu "
               "megapixels. Choose a smaller size%s",
               nw, nh, (unsigned long long)mp, (unsigned long long)max_mp, hint);
}

/* ---- surface helpers ---------------------------------------------------------------------- */

static pc_status resize_in_canvas(pc_surface *s, double sx, double sy, pc_algo_resample filter,
                                  pc_rect canvas, pc_error *err)
{
    pc_surface out;
    pc_status rc = pc_algo_resize_surface_in_canvas(s, sx, sy, filter, canvas, &out, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface_fini(s);
    *s = out;
    return PC_OK;
}

static pc_status translate_in_place(pc_surface *s, int32_t dx, int32_t dy, pc_error *err)
{
    pc_surface out;
    pc_status rc = pc_algo_translate_surface(s, dx, dy, &out, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface_fini(s);
    *s = out;
    return PC_OK;
}

static pc_status crop_in_place(pc_surface *s, pc_rect keep, pc_error *err)
{
    pc_surface out;
    pc_status rc = pc_algo_crop_surface(s, keep, &out, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface_fini(s);
    *s = out;
    return PC_OK;
}

/* ---- resample_doc ------------------------------------------------------------------------- */

typedef struct {
    double sx, sy;
    pc_algo_resample filter;
    pc_rect canvas;
    pc_status rc;
    pc_error *err;
} resize_ctx;

static void resize_cb(pc_surface *surf, bool is_mask, void *vctx)
{
    resize_ctx *c = vctx;
    if (c->rc != PC_OK) {
        return;
    }
    pc_algo_resample f = is_mask ? PC_ALGO_RESAMPLE_BILINEAR : c->filter;
    c->rc = resize_in_canvas(surf, c->sx, c->sy, f, c->canvas, c->err);
}

/* Resamples the whole document (pixels, masks, channels, selection, effects, vectors, guides) to
 * nw x nh px with `filter`. Callers check check_resample_budget first. */
static pc_status resample_doc(pc_document *doc, uint32_t nw, uint32_t nh, pc_algo_resample filter,
                              pc_error *err)
{
    uint32_t ow = doc->size.width, oh = doc->size.height;
    double sx = (double)nw / (double)(ow > 0 ? ow : 1);
    double sy = (double)nh / (double)(oh > 0 ? oh : 1);
    if (fabs(sx - 1.0) < 1e-12 && fabs(sy - 1.0) < 1e-12) {
        return PC_OK;
    }
    pc_rect canvas = pc_rect_from_xywh(0, 0, ow, oh);
    resize_ctx c = {sx, sy, filter, canvas, PC_OK, err};
    pc_engine_image_for_each_surface(doc->layers, doc->nlayers, true, resize_cb, &c);
    if (c.rc != PC_OK) {
        return c.rc;
    }
    float k = (float)((sx + sy) / 2.0);
    for_each_layer(doc->layers, doc->nlayers, scale_effects_cb, &k);
    for (size_t i = 0; i < doc->nchannels; i++) {
        pc_status rc = resize_in_canvas(&doc->channels[i].surface, sx, sy, PC_ALGO_RESAMPLE_BILINEAR,
                                        canvas, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    if (doc->has_quick_mask) {
        pc_status rc = resize_in_canvas(&doc->quick_mask.surface, sx, sy, PC_ALGO_RESAMPLE_BILINEAR,
                                        canvas, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    if (doc->has_selection) {
        pc_status rc = resize_in_canvas(&doc->selection, sx, sy, PC_ALGO_RESAMPLE_BILINEAR, canvas, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    pc_affine a = {{sx, 0.0, 0.0, sy, 0.0, 0.0}};
    pc_engine_transform_geometry(doc, &a);
    doc->size = pc_size_make(nw, nh);
    refresh_doc(doc, true);
    return PC_OK;
}

/* ---- translate_doc ------------------------------------------------------------------------ */

static bool fits_translate(pc_rect r, int32_t dx, int32_t dy)
{
    if (pc_rect_is_empty(r)) {
        return true;
    }
    int64_t vals[2] = {(int64_t)r.x0 + dx, (int64_t)r.x1 + dx};
    int64_t vvals[2] = {(int64_t)r.y0 + dy, (int64_t)r.y1 + dy};
    for (int i = 0; i < 2; i++) {
        if (vals[i] < INT32_MIN || vals[i] > INT32_MAX || vvals[i] < INT32_MIN || vvals[i] > INT32_MAX) {
            return false;
        }
    }
    return true;
}

typedef struct {
    int32_t dx, dy;
    bool ok;
    pc_status rc;
    pc_error *err;
} translate_ctx;

static void translate_check_cb(pc_surface *surf, bool is_mask, void *vctx)
{
    (void)is_mask;
    translate_ctx *c = vctx;
    c->ok = c->ok && fits_translate(pc_surface_content_bounds(surf), c->dx, c->dy);
}

static void translate_apply_cb(pc_surface *surf, bool is_mask, void *vctx)
{
    (void)is_mask;
    translate_ctx *c = vctx;
    if (c->rc != PC_OK) {
        return;
    }
    c->rc = translate_in_place(surf, c->dx, c->dy, c->err);
}

static pc_status translate_doc(pc_document *doc, const char *cmd, int32_t dx, int32_t dy, pc_error *err)
{
    if (dx == 0 && dy == 0) {
        return PC_OK;
    }
    translate_ctx chk = {dx, dy, true, PC_OK, err};
    pc_engine_image_for_each_surface(doc->layers, doc->nlayers, true, translate_check_cb, &chk);
    bool ok = chk.ok;
    for (size_t i = 0; ok && i < doc->nchannels; i++) {
        ok = ok && fits_translate(pc_surface_content_bounds(&doc->channels[i].surface), dx, dy);
    }
    if (ok && doc->has_quick_mask) {
        ok = fits_translate(pc_surface_content_bounds(&doc->quick_mask.surface), dx, dy);
    }
    if (ok && doc->has_selection) {
        ok = fits_translate(pc_surface_content_bounds(&doc->selection), dx, dy);
    }
    if (!ok) {
        return bad(err, cmd,
                   "moving the document by (%d, %d) would push its pixels outside the 32-bit "
                   "coordinate range",
                   dx, dy);
    }
    translate_ctx app = {dx, dy, true, PC_OK, err};
    pc_engine_image_for_each_surface(doc->layers, doc->nlayers, true, translate_apply_cb, &app);
    if (app.rc != PC_OK) {
        return app.rc;
    }
    for (size_t i = 0; i < doc->nchannels; i++) {
        pc_status rc = translate_in_place(&doc->channels[i].surface, dx, dy, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    if (doc->has_quick_mask) {
        pc_status rc = translate_in_place(&doc->quick_mask.surface, dx, dy, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    if (doc->has_selection) {
        pc_status rc = translate_in_place(&doc->selection, dx, dy, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    pc_affine a = pc_affine_translate((double)dx, (double)dy);
    pc_engine_transform_geometry(doc, &a);
    return PC_OK;
}

pc_status pc_engine_translate_doc(pc_document *doc, const char *cmd, int32_t dx, int32_t dy,
                                  pc_error *err)
{
    return translate_doc(doc, cmd, dx, dy, err);
}

/* ---- crop_doc ----------------------------------------------------------------------------- */

typedef struct {
    pc_rect keep;
    pc_status rc;
    pc_error *err;
} crop_ctx;

static void crop_cb(pc_surface *surf, bool is_mask, void *vctx)
{
    (void)is_mask;
    crop_ctx *c = vctx;
    if (c->rc != PC_OK) {
        return;
    }
    c->rc = crop_in_place(surf, c->keep, c->err);
}

/* Fills the extension area the Background cannot leave transparent with the extension colour,
 * keeping the original pixels that survive the move (`old` is the old canvas in new coordinates). */
static pc_status fill_background_extension(pc_surface *surf, pc_rect canvas, pc_rect old,
                                           const float ext[4], pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(surf);
    float encoded[PC_RASTER_MAX_CHANNELS];
    float rgba[4] = {ext[0], ext[1], ext[2], 1.0f};
    size_t nch = pc_from_rgba(fmt, rgba, encoded);
    pc_rect keep = pc_rect_intersect(pc_surface_content_bounds(surf), old);
    uint8_t *saved = NULL;
    size_t saved_len = 0;
    if (!pc_rect_is_empty(keep)) {
        pc_status rc = pc_surface_to_interleaved(surf, keep, &saved, &saved_len, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    pc_status rc = pc_surface_fill_rect(surf, canvas, encoded, nch, err);
    if (rc == PC_OK && saved != NULL) {
        rc = pc_surface_write_interleaved(surf, keep, saved, saved_len, err);
    }
    free(saved);
    return rc;
}

static pc_status crop_doc(pc_document *doc, const char *cmd, pc_rect r, bool delete_pixels,
                          const float extension[4], pc_error *err)
{
    if (r.x0 == INT32_MIN || r.y0 == INT32_MIN) {
        return bad(err, cmd,
                   "the crop origin (%d, %d) can't be moved to (0, 0) within the 32-bit coordinate "
                   "range",
                   r.x0, r.y0);
    }
    int32_t dx = -r.x0, dy = -r.y0;
    pc_rect oc = pc_document_bounds(doc);
    bool extends = r.x0 < oc.x0 || r.y0 < oc.y0 || r.x1 > oc.x1 || r.y1 > oc.y1;
    if (delete_pixels) {
        crop_ctx c = {r, PC_OK, err};
        pc_engine_image_for_each_surface(doc->layers, doc->nlayers, false, crop_cb, &c);
        if (c.rc != PC_OK) {
            return c.rc;
        }
    }
    pc_status rc = translate_doc(doc, cmd, dx, dy, err);
    if (rc != PC_OK) {
        return rc;
    }
    doc->size = pc_size_make(pc_rect_width(r), pc_rect_height(r));
    if (extends) {
        pc_layer *bg = pc_document_background_mut(doc);
        if (bg != NULL && bg->content.kind == PC_LAYER_RASTER) {
            pc_rect old = pc_rect_translate(oc, dx, dy);
            rc = fill_background_extension(&bg->content.u.raster, pc_document_bounds(doc), old, extension,
                                           err);
            if (rc != PC_OK) {
                return rc;
            }
        }
    }
    refresh_doc(doc, false);
    return PC_OK;
}

/* ---- Image Size --------------------------------------------------------------------------- */

typedef struct {
    bool set_dpi;
    float dpi;
    method m;
    uint32_t nw, nh, ow, oh;
} image_size_ctx;

static pc_status image_size_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *ctx, pc_error *err)
{
    (void)ha;
    (void)a;
    image_size_ctx *c = ctx;
    if (c->set_dpi) {
        float r = c->dpi;
        doc->resolution_dpi = r < 1.0f ? 1.0f : (r > 10000.0f ? 10000.0f : r);
    }
    if (c->m.kind == METHOD_NONE) {
        return PC_OK;
    }
    pc_algo_resample filter = c->m.filter;
    if (c->m.kind == METHOD_AUTOMATIC) {
        filter = (c->nw > c->ow || c->nh > c->oh) ? PC_ALGO_RESAMPLE_PRESERVE_DETAILS
                                                   : PC_ALGO_RESAMPLE_BICUBIC_SHARPER;
    }
    return resample_doc(doc, c->nw, c->nh, filter, err);
}

static void set_sampler(pc_engine_session *s, pc_affine a)
{
    s->has_sampler_map = true;
    s->sampler_map = a;
}

static pc_status result_size(pc_json_value *out, uint32_t w, uint32_t h, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "width", w);
    yyjson_mut_obj_add_uint(d, root, "height", h);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static bool f64_param(yyjson_val *p, const char *key, double *out)
{
    yyjson_val *v = pc_json_get(p, key);
    return v != NULL && pc_json_as_f64(v, out);
}

static bool bool_param(yyjson_val *p, const char *key, bool def)
{
    yyjson_val *v = pc_json_get(p, key);
    bool b;
    if (v != NULL && pc_json_as_bool(v, &b)) {
        return b;
    }
    return def;
}

static const char *str_param(yyjson_val *p, const char *key, const char *def)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL) {
        return def;
    }
    const char *s = yyjson_get_str(v);
    return s != NULL ? s : def;
}

static uint32_t clamp_side(double v)
{
    double r = round(v);
    if (r < 1.0) {
        r = 1.0;
    } else if (r > 300000.0) {
        r = 300000.0;
    }
    return (uint32_t)r;
}

static pc_status run_image_size(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    uint32_t ow = doc->size.width, oh = doc->size.height;
    float old_dpi = doc->resolution_dpi;
    method m = parse_resample(str_param(p, "resample", "bicubic"));
    bool has_w = false, has_h = false;
    double pw = 0, ph = 0;
    has_w = f64_param(p, "width", &pw);
    has_h = f64_param(p, "height", &ph);
    yyjson_val *pcv = pc_json_get(p, "percent");
    if (pcv != NULL) {
        double pc_val;
        if (!pc_json_as_f64(pcv, &pc_val) || !isfinite(pc_val) || pc_val <= 0.0) {
            return bad(err, "image.imageSize", "`percent` must be a positive number");
        }
        if (!has_w) {
            pw = (double)ow * pc_val / 100.0;
            has_w = true;
        }
        if (!has_h) {
            ph = (double)oh * pc_val / 100.0;
            has_h = true;
        }
    }
    double res = 0;
    bool has_res = f64_param(p, "resolution", &res);
    if (bool_param(p, "keepPrintSize", false) && !has_w && !has_h && has_res) {
        double r = res < 1.0 ? 1.0 : (res > 10000.0 ? 10000.0 : res);
        double old = (double)old_dpi < 1.0 ? 1.0 : (double)old_dpi;
        pw = (double)ow * r / old;
        ph = (double)oh * r / old;
        has_w = true;
        has_h = true;
    }
    double nw_d, nh_d;
    if (has_w && has_h) {
        nw_d = pw;
        nh_d = ph;
    } else if (has_w) {
        nw_d = pw;
        nh_d = pw * (double)oh / (double)(ow > 0 ? ow : 1);
    } else if (has_h) {
        nw_d = ph * (double)ow / (double)(oh > 0 ? oh : 1);
        nh_d = ph;
    } else {
        nw_d = (double)ow;
        nh_d = (double)oh;
    }
    uint32_t nw = clamp_side(nw_d), nh = clamp_side(nh_d);
    bool resizing = m.kind != METHOD_NONE && (nw != ow || nh != oh);
    if (resizing) {
        pc_status rc = check_resample_budget(err, "image.imageSize", doc, nw, nh,
                                             ", or turn Resample off to change only the resolution");
        if (rc != PC_OK) {
            return rc;
        }
    }
    image_size_ctx ctx = {has_res, (float)res, m, nw, nh, ow, oh};
    if (resizing) {
        double sx = (double)nw / (double)(ow > 0 ? ow : 1);
        double sy = (double)nh / (double)(oh > 0 ? oh : 1);
        pc_affine a = {{sx, 0.0, 0.0, sy, 0.0, 0.0}};
        set_sampler(s, a);
    }
    pc_status rc = pc_engine_session_edit(s, "Image Size", image_size_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d2 = pc_engine_session_active(s);
    if (d2 == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_size sz = pc_doc_arc_get(d2->doc)->size;
    return result_size(out, sz.width, sz.height, err);
}

/* ---- Canvas Size -------------------------------------------------------------------------- */

static const struct {
    const char *name;
    double fx, fy;
} ANCHORS[9] = {
    {"topLeft", 0.0, 0.0},  {"top", 0.5, 0.0},    {"topRight", 1.0, 0.0},
    {"left", 0.0, 0.5},     {"center", 0.5, 0.5}, {"right", 1.0, 0.5},
    {"bottomLeft", 0.0, 1.0}, {"bottom", 0.5, 1.0}, {"bottomRight", 1.0, 1.0},
};

static pc_status anchor_factors(pc_error *err, const char *cmd, const char *a, double *fx, double *fy)
{
    for (int i = 0; i < 9; i++) {
        if (strcmp(ANCHORS[i].name, a) == 0) {
            *fx = ANCHORS[i].fx;
            *fy = ANCHORS[i].fy;
            return PC_OK;
        }
    }
    return bad(err, cmd,
               "`anchor` must be one of topLeft|top|topRight|left|center|right|bottomLeft|bottom|"
               "bottomRight");
}

static void extension_color(const pc_engine_session *s, yyjson_val *p, float out[4])
{
    const pc_engine_tool_state *t = pc_engine_session_tools(s);
    const char *spec = str_param(p, "extensionColor", "background");
    if (strcmp(spec, "foreground") == 0) {
        memcpy(out, t->foreground, sizeof(float) * 4);
    } else if (strcmp(spec, "white") == 0) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
    } else if (strcmp(spec, "black") == 0) {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
    } else if (strcmp(spec, "transparent") == 0) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
    } else if (spec[0] == '#') {
        if (!pc_engine_parse_hex(spec, strlen(spec), out)) {
            memcpy(out, t->background, sizeof(float) * 4);
        }
        out[3] = 1.0f;
    } else {
        memcpy(out, t->background, sizeof(float) * 4);
    }
}

typedef struct {
    int32_t dx, dy;
    uint32_t nw, nh, ow, oh;
    float ext[4];
} canvas_ctx;

static pc_status canvas_size_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *ctx, pc_error *err)
{
    (void)ha;
    (void)a;
    canvas_ctx *c = ctx;
    pc_status rc = translate_doc(doc, "image.canvasSize", c->dx, c->dy, err);
    if (rc != PC_OK) {
        return rc;
    }
    doc->size = pc_size_make(c->nw, c->nh);
    refresh_doc(doc, false);
    pc_rect canvas = pc_document_bounds(doc);
    pc_rect old = pc_rect_from_xywh(c->dx, c->dy, c->ow, c->oh);
    bool grows = c->nw > c->ow || c->nh > c->oh;
    if (c->ext[3] == 0.0f && grows && pc_document_background_mut(doc) != NULL) {
        /* A transparent extension that grows a locked Background must first make it a normal
         * paintable layer (extra_cmds::unlock_background). That primitive is owned by E12; until it
         * lands this branch is unsupported rather than silently leaving the Background locked. */
        return pc_fail(err, PC_ERR_UNSUPPORTED,
                       "a transparent Canvas Size extension of the Background needs Layer > New > "
                       "Layer from Background first (pending E12)");
    }
    if (c->ext[3] > 0.0f) {
        pc_layer *bg = pc_document_background_mut(doc);
        if (bg != NULL && bg->content.kind == PC_LAYER_RASTER) {
            rc = fill_background_extension(&bg->content.u.raster, canvas, old, c->ext, err);
            if (rc != PC_OK) {
                return rc;
            }
        }
    }
    return PC_OK;
}

static pc_status run_canvas_size(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    double ow = (double)doc->size.width, oh = (double)doc->size.height;
    bool relative = bool_param(p, "relative", false);
    double w, h;
    if (!f64_param(p, "width", &w)) {
        w = relative ? 0.0 : ow;
    }
    if (!f64_param(p, "height", &h)) {
        h = relative ? 0.0 : oh;
    }
    double nw_d = relative ? ow + w : w;
    double nh_d = relative ? oh + h : h;
    uint32_t nw = clamp_side(nw_d), nh = clamp_side(nh_d);
    double ax = 0.5, ay = 0.5;
    pc_status rc = anchor_factors(err, "image.canvasSize", str_param(p, "anchor", "center"), &ax, &ay);
    if (rc != PC_OK) {
        return rc;
    }
    int32_t dx = (int32_t)lround(((double)nw - ow) * ax);
    int32_t dy = (int32_t)lround(((double)nh - oh) * ay);
    float ext[4];
    extension_color(s, p, ext);
    set_sampler(s, pc_affine_translate((double)dx, (double)dy));
    canvas_ctx ctx = {dx, dy, nw, nh, (uint32_t)ow, (uint32_t)oh, {ext[0], ext[1], ext[2], ext[3]}};
    rc = pc_engine_session_edit(s, "Canvas Size", canvas_size_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(jd);
    yyjson_mut_obj_add_uint(jd, root, "width", nw);
    yyjson_mut_obj_add_uint(jd, root, "height", nh);
    yyjson_mut_val *off = yyjson_mut_arr(jd);
    yyjson_mut_arr_add_int(jd, off, dx);
    yyjson_mut_arr_add_int(jd, off, dy);
    yyjson_mut_obj_add_val(jd, root, "offset", off);
    yyjson_mut_doc_set_root(jd, root);
    return PC_OK;
}

/* ---- Crop --------------------------------------------------------------------------------- */

#define MAX_CROP_SIDE 300000
#define MAX_CROP_RESOLUTION 10000.0

typedef struct {
    bool has_size;
    uint32_t tw, th;
    bool has_res;
    float res;
    pc_algo_resample filter;
} crop_target;

/* Parses targetWidth/targetHeight (whole px, both or neither), resolution (ppi) and resample. */
static pc_status parse_crop_target(yyjson_val *p, crop_target *t, pc_error *err)
{
    const char *CMD = "image.crop";
    bool hw = false, hh = false;
    uint32_t tw = 0, th = 0;
    for (int which = 0; which < 2; which++) {
        const char *key = which == 0 ? "targetWidth" : "targetHeight";
        yyjson_val *v = pc_json_get(p, key);
        if (v == NULL || yyjson_is_null(v)) {
            continue;
        }
        uint64_t n;
        if (!pc_json_as_u64(v, &n) || n < 1 || n > (uint64_t)MAX_CROP_SIDE) {
            return bad(err, CMD, "`%s` must be a whole number of pixels from 1 to %d", key, MAX_CROP_SIDE);
        }
        if (which == 0) {
            tw = (uint32_t)n;
            hw = true;
        } else {
            th = (uint32_t)n;
            hh = true;
        }
    }
    if (hw != hh) {
        return bad(err, CMD, "`targetWidth` and `targetHeight` go together");
    }
    t->has_size = hw;
    t->tw = tw;
    t->th = th;
    t->has_res = false;
    t->res = 0.0f;
    yyjson_val *rv = pc_json_get(p, "resolution");
    if (rv != NULL && !yyjson_is_null(rv)) {
        double r;
        if (!pc_json_as_f64(rv, &r) || !isfinite(r) || r < 1.0 || r > MAX_CROP_RESOLUTION) {
            return bad(err, CMD, "`resolution` must be a number of pixels per inch from 1 to %g",
                       MAX_CROP_RESOLUTION);
        }
        t->has_res = true;
        t->res = (float)r;
    }
    /* resample: default bicubic; "none" only allowed without a target size. */
    bool filter_none = false;
    pc_algo_resample filter = PC_ALGO_RESAMPLE_BICUBIC;
    yyjson_val *sv = pc_json_get(p, "resample");
    if (sv != NULL && !yyjson_is_null(sv)) {
        const char *ss = yyjson_get_str(sv);
        if (ss == NULL) {
            return bad(err, CMD, "`resample` must be a string");
        }
        method m = parse_resample(ss);
        if (m.kind == METHOD_NONE) {
            filter_none = true;
        } else if (m.kind == METHOD_AUTOMATIC) {
            filter = PC_ALGO_RESAMPLE_BICUBIC_AUTOMATIC;
        } else {
            filter = m.filter;
        }
    }
    if (filter_none && t->has_size) {
        return bad(err, CMD,
                   "a `targetWidth` x `targetHeight` crop needs resampling: `resample` can't be "
                   "\"none\"");
    }
    t->filter = filter;
    return PC_OK;
}

typedef struct {
    pc_rect r;
    bool delete_pixels;
    float extension[4];
    crop_target target;
} crop_edit_ctx;

static pc_status crop_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *ctx, pc_error *err)
{
    (void)ha;
    (void)a;
    crop_edit_ctx *c = ctx;
    pc_status rc = crop_doc(doc, "image.crop", c->r, c->delete_pixels, c->extension, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    if (c->target.has_size) {
        rc = resample_doc(doc, c->target.tw, c->target.th, c->target.filter, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    if (c->target.has_res) {
        doc->resolution_dpi = c->target.res;
    }
    return PC_OK;
}

/* image.crop's optional `angle`, normalised to (-180, 180]; has_angle false when absent/null/zero. */
static pc_status crop_angle(yyjson_val *p, bool *has_angle, double *out, pc_error *err)
{
    *has_angle = false;
    yyjson_val *v = pc_json_get(p, "angle");
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    double a;
    if (!pc_json_as_f64(v, &a)) {
        return bad(err, "image.crop", "`angle` must be a number of degrees");
    }
    if (!isfinite(a) || fabs(a) > 3600.0) {
        return bad(err, "image.crop", "`angle` must be a finite number of degrees within +-3600");
    }
    a = fmod(a, 360.0);
    if (a < 0.0) {
        a += 360.0;
    }
    if (a > 180.0) {
        a -= 360.0;
    }
    if (fabs(a) >= 1e-9) {
        *has_angle = true;
        *out = a;
    }
    return PC_OK;
}

/* ---- rotated crop (Crop tool with an angle; image_cmds::rotated_crop) --------------------- */

static const double CROP_PI = 3.14159265358979323846; /* M_PI is not ISO C */

typedef struct {
    double angle;         /* clockwise-on-screen crop angle (degrees) */
    pc_rect target;       /* the upright crop rect in the rotated canvas */
    bool delete_pixels;
    float extension[4];   /* background colour for any uncovered area */
    crop_target target_opts;
} rotated_crop_ctx;

/* Inside the Crop compound: rotate the whole document upright (Image Rotation › Arbitrary by
 * `angle` counter-clockwise, so the frame becomes axis-aligned), then crop to the upright frame. */
static pc_status rotated_crop_body(pc_engine_session *s, void *vctx, pc_error *err)
{
    rotated_crop_ctx *c = vctx;
    char buf[96];
    (void)snprintf(buf, sizeof buf, "{\"angle\":%.17g,\"direction\":\"ccw\"}", c->angle);
    pc_json_value tmp = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, "image.rotation.arbitrary", buf, &tmp, err);
    pc_json_value_fini(&tmp);
    if (rc != PC_OK) {
        return rc;
    }
    /* Color Sampler points follow the crop's origin shift too (the rotation moved them above). */
    set_sampler(s, pc_affine_translate(-(double)c->target.x0, -(double)c->target.y0));
    crop_edit_ctx ectx;
    ectx.r = c->target;
    ectx.delete_pixels = c->delete_pixels;
    memcpy(ectx.extension, c->extension, sizeof(float) * 4);
    ectx.target = c->target_opts;
    return pc_engine_session_edit(s, "Crop", crop_edit, &ectx, err);
}

static pc_status run_rotated_crop(pc_engine_session *s, pc_rect r, double angle, bool delete_pixels,
                                  const crop_target *tgt, pc_json_value *out, pc_error *err)
{
    const char *CMD = "image.crop";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_size size = pc_doc_arc_get(d->doc)->size;
    double w = (double)pc_rect_width(r), h = (double)pc_rect_height(r);
    double cx = (double)r.x0 + w / 2.0, cy = (double)r.y0 + h / 2.0;
    /* Where the frame's centre lands under the rotation (clockwise by -angle about the centres). */
    pc_size nw = pc_engine_rotated_size(size, -angle);
    double rad = (-angle) * (CROP_PI / 180.0);
    double sn = sin(rad), cs = cos(rad);
    double dx = cx - (double)size.width / 2.0, dy = cy - (double)size.height / 2.0;
    double ncx = (double)nw.width / 2.0 + cs * dx - sn * dy;
    double ncy = (double)nw.height / 2.0 + sn * dx + cs * dy;
    double xf = round(ncx - w / 2.0), yf = round(ncy - h / 2.0);
    double limit = (double)(INT32_MAX / 2);
    if (!isfinite(xf) || !isfinite(yf) || fabs(xf) > limit || fabs(yf) > limit) {
        return bad(err, CMD, "the rotated frame is outside the 32-bit coordinate range");
    }
    int32_t x = (int32_t)xf, y = (int32_t)yf;
    int64_t x1 = (int64_t)x + (int64_t)pc_rect_width(r);
    int64_t y1 = (int64_t)y + (int64_t)pc_rect_height(r);
    if (x1 > INT32_MAX || y1 > INT32_MAX) {
        return bad(err, CMD, "the rotated frame is outside the 32-bit coordinate range");
    }
    rotated_crop_ctx ctx;
    ctx.angle = angle;
    ctx.target = pc_rect_make(x, y, (int32_t)x1, (int32_t)y1);
    ctx.delete_pixels = delete_pixels;
    const pc_engine_tool_state *t = pc_engine_session_tools(s);
    memcpy(ctx.extension, t->background, sizeof(float) * 4);
    ctx.target_opts = *tgt;
    pc_status rc = pc_engine_compound(s, "Crop", rotated_crop_body, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d2 = pc_engine_session_active(s);
    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(jd);
    yyjson_mut_obj_add_int(jd, root, "x", r.x0);
    yyjson_mut_obj_add_int(jd, root, "y", r.y0);
    yyjson_mut_obj_add_uint(jd, root, "width", pc_rect_width(r));
    yyjson_mut_obj_add_uint(jd, root, "height", pc_rect_height(r));
    yyjson_mut_obj_add_real(jd, root, "angle", angle);
    if (d2 != NULL) {
        const pc_document *doc = pc_doc_arc_get(d2->doc);
        yyjson_mut_obj_add_uint(jd, root, "documentWidth", doc->size.width);
        yyjson_mut_obj_add_uint(jd, root, "documentHeight", doc->size.height);
        yyjson_mut_obj_add_real(jd, root, "resolution", (double)doc->resolution_dpi);
    }
    yyjson_mut_doc_set_root(jd, root);
    return PC_OK;
}

static pc_status run_crop(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "image.crop";
    bool delete_pixels = bool_param(p, "deleteCroppedPixels", true);
    bool hx, hy, hw, hh;
    int32_t x = 0, y = 0, w = 0, h = 0;
    if (pc_engine_param_int_i32(CMD, p, "x", &hx, &x, err) != PC_OK ||
        pc_engine_param_int_i32(CMD, p, "y", &hy, &y, err) != PC_OK ||
        pc_engine_param_int_i32(CMD, p, "width", &hw, &w, err) != PC_OK ||
        pc_engine_param_int_i32(CMD, p, "height", &hh, &h, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    bool has_explicit = false;
    pc_rect explicit_rect = pc_rect_make(0, 0, 0, 0);
    if (hx && hy && hw && hh && w > 0 && h > 0) {
        if (w > MAX_CROP_SIDE || h > MAX_CROP_SIDE) {
            return bad(err, CMD, "%d x %d exceeds the %d px limit per side", w, h, MAX_CROP_SIDE);
        }
        int64_t x1 = (int64_t)x + w, y1 = (int64_t)y + h;
        if (x1 > INT32_MAX) {
            return bad(err, CMD, "`x` + `width` = %lld is outside the 32-bit coordinate range",
                       (long long)x1);
        }
        if (y1 > INT32_MAX) {
            return bad(err, CMD, "`y` + `height` = %lld is outside the 32-bit coordinate range",
                       (long long)y1);
        }
        explicit_rect = pc_rect_make(x, y, (int32_t)x1, (int32_t)y1);
        has_explicit = true;
    }
    crop_target target = {false, 0, 0, false, 0.0f, PC_ALGO_RESAMPLE_BICUBIC};
    pc_status rc = parse_crop_target(p, &target, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (target.has_size) {
        const pc_engine_doc_state *d = pc_engine_session_active(s);
        if (d == NULL) {
            return pc_engine_err_no_document(err);
        }
        rc = check_resample_budget(err, CMD, pc_doc_arc_get(d->doc), target.tw, target.th, "");
        if (rc != PC_OK) {
            return rc;
        }
    }
    bool has_angle = false;
    double angle = 0.0;
    rc = crop_angle(p, &has_angle, &angle, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (has_angle) {
        if (!has_explicit) {
            return bad(err, CMD, "an `angle` needs an explicit `x`, `y`, `width` and `height`");
        }
        /* The rotated-frame crop rotates the whole document upright (Image Rotation › Arbitrary)
         * and crops the now axis-aligned frame, all in one undo step. */
        return run_rotated_crop(s, explicit_rect, angle, delete_pixels, &target, out, err);
    }
    pc_rect r;
    if (has_explicit) {
        r = explicit_rect;
    } else {
        const pc_engine_doc_state *d = pc_engine_session_active(s);
        if (d != NULL) {
            const pc_document *doc = pc_doc_arc_get(d->doc);
            if (doc->has_selection) {
                r = pc_rect_intersect(pc_surface_content_bounds(&doc->selection), pc_document_bounds(doc));
            } else {
                r = pc_rect_make(0, 0, 0, 0);
            }
        } else {
            r = pc_rect_make(0, 0, 0, 0);
        }
    }
    if (pc_rect_is_empty(r)) {
        return pc_engine_err_other(err, "nothing to crop: pass x/y/width/height or make a selection");
    }
    set_sampler(s, pc_affine_translate(-(double)r.x0, -(double)r.y0));
    const pc_engine_tool_state *t = pc_engine_session_tools(s);
    crop_edit_ctx ctx;
    ctx.r = r;
    ctx.delete_pixels = delete_pixels;
    memcpy(ctx.extension, t->background, sizeof(float) * 4);
    ctx.target = target;
    rc = pc_engine_session_edit(s, "Crop", crop_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* Result: the frame, plus the document's size and resolution afterwards. */
    const pc_engine_doc_state *d2 = pc_engine_session_active(s);
    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(jd);
    yyjson_mut_obj_add_int(jd, root, "x", r.x0);
    yyjson_mut_obj_add_int(jd, root, "y", r.y0);
    yyjson_mut_obj_add_uint(jd, root, "width", pc_rect_width(r));
    yyjson_mut_obj_add_uint(jd, root, "height", pc_rect_height(r));
    if (d2 != NULL) {
        const pc_document *doc = pc_doc_arc_get(d2->doc);
        yyjson_mut_obj_add_uint(jd, root, "documentWidth", doc->size.width);
        yyjson_mut_obj_add_uint(jd, root, "documentHeight", doc->size.height);
        yyjson_mut_obj_add_real(jd, root, "resolution", (double)doc->resolution_dpi);
    }
    yyjson_mut_doc_set_root(jd, root);
    return PC_OK;
}

/* ---- Trim --------------------------------------------------------------------------------- */

typedef struct {
    pc_rect r;
    float extension[4];
} trim_ctx;

static pc_status trim_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *ctx, pc_error *err)
{
    (void)ha;
    (void)a;
    trim_ctx *c = ctx;
    return crop_doc(doc, "image.trim", c->r, true, c->extension, err);
}

static pc_status run_trim(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_rect canvas = pc_document_bounds(doc);
    pc_compose_buffer buf;
    if (pc_compose_flatten(doc, &buf, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const char *based = str_param(p, "basedOn", "transparent");
    size_t w = pc_rect_width(canvas);
    bool have_ref = false;
    float ref[4] = {0, 0, 0, 0};
    if (strcmp(based, "topLeft") == 0 && buf.len > 0) {
        memcpy(ref, buf.px[0], sizeof ref);
        have_ref = true;
    } else if (strcmp(based, "bottomRight") == 0 && buf.len > 0) {
        memcpy(ref, buf.px[buf.len - 1], sizeof ref);
        have_ref = true;
    }
    pc_rect b = pc_rect_make(0, 0, 0, 0);
    for (size_t i = 0; i < buf.len; i++) {
        const float *q = buf.px[i];
        bool differs;
        if (!have_ref) {
            differs = q[3] > 0.0f;
        } else {
            differs = false;
            for (int c = 0; c < 4; c++) {
                if (fabsf(q[c] * q[3] - ref[c] * ref[3]) > 0.5f / 255.0f) {
                    differs = true;
                }
            }
            if (fabsf(q[3] - ref[3]) > 0.5f / 255.0f) {
                differs = true;
            }
        }
        if (differs) {
            int32_t x = (int32_t)(w > 0 ? i % w : 0), y = (int32_t)(w > 0 ? i / w : 0);
            pc_rect cell = pc_rect_make(x, y, x + 1, y + 1);
            b = pc_rect_is_empty(b) ? cell : pc_rect_union(b, cell);
        }
    }
    pc_compose_buffer_fini(&buf);
    if (pc_rect_is_empty(b)) {
        return pc_engine_err_other(err, "nothing to trim to");
    }
    pc_rect r = pc_rect_make(bool_param(p, "left", true) ? b.x0 : canvas.x0,
                             bool_param(p, "top", true) ? b.y0 : canvas.y0,
                             bool_param(p, "right", true) ? b.x1 : canvas.x1,
                             bool_param(p, "bottom", true) ? b.y1 : canvas.y1);
    set_sampler(s, pc_affine_translate(-(double)r.x0, -(double)r.y0));
    const pc_engine_tool_state *t = pc_engine_session_tools(s);
    trim_ctx ctx;
    ctx.r = r;
    memcpy(ctx.extension, t->background, sizeof(float) * 4);
    pc_status rc = pc_engine_session_edit(s, "Trim", trim_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(jd);
    yyjson_mut_obj_add_int(jd, root, "x", r.x0);
    yyjson_mut_obj_add_int(jd, root, "y", r.y0);
    yyjson_mut_obj_add_uint(jd, root, "width", pc_rect_width(r));
    yyjson_mut_obj_add_uint(jd, root, "height", pc_rect_height(r));
    yyjson_mut_doc_set_root(jd, root);
    return PC_OK;
}

/* ---- Mode (colour model) ------------------------------------------------------------------ */

static pc_status run_mode_rgb(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return pc_engine_color_convert_mode(s, PC_COLOR_MODE_RGB, p, out, err);
}
static pc_status run_mode_grayscale(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return pc_engine_color_convert_mode(s, PC_COLOR_MODE_GRAYSCALE, p, out, err);
}
static pc_status run_mode_cmyk(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return pc_engine_color_convert_mode(s, PC_COLOR_MODE_CMYK, p, out, err);
}
static pc_status run_mode_lab(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return pc_engine_color_convert_mode(s, PC_COLOR_MODE_LAB, p, out, err);
}

/* ---- Mode (bit depth) --------------------------------------------------------------------- */

typedef struct {
    pc_compose_transfer t;
    bool encode;
} transfer_ctx;

static float transfer_map(void *vctx, float v)
{
    transfer_ctx *c = vctx;
    if (c->encode) {
        float e = pc_compose_transfer_encode(c->t, v);
        return e < 0.0f ? 0.0f : (e > 1.0f ? 1.0f : e);
    }
    return pc_compose_transfer_decode(c->t, v);
}

typedef struct {
    pc_sample_type depth;
    pc_status rc;
    pc_error *err;
} depth_ctx;

static void depth_convert_cb(pc_surface *surf, bool is_mask, void *vctx)
{
    (void)is_mask;
    depth_ctx *c = vctx;
    if (c->rc != PC_OK) {
        return;
    }
    pc_pixel_format f = pc_surface_format(surf);
    f.sample = c->depth;
    pc_surface out;
    c->rc = pc_surface_convert(surf, f, &out, c->err);
    if (c->rc == PC_OK) {
        pc_surface_fini(surf);
        *surf = out;
    }
}

pc_status pc_engine_convert_layers_depth(pc_layer *layers, size_t n, pc_sample_type depth, pc_error *err)
{
    depth_ctx dc = {depth, PC_OK, err};
    pc_engine_image_for_each_surface(layers, n, true, depth_convert_cb, &dc);
    return dc.rc;
}

typedef struct {
    pc_status rc;
    pc_error *err;
    transfer_ctx tc;
} map_color_ctx;

static void map_color_cb(pc_surface *surf, bool is_mask, void *vctx)
{
    (void)is_mask;
    map_color_ctx *c = vctx;
    if (c->rc != PC_OK) {
        return;
    }
    c->rc = pc_surface_map_color(surf, transfer_map, &c->tc, c->err);
}

typedef struct {
    pc_sample_type depth;
    pc_sample_type current;
    pc_color_mode mode;
} bit_depth_ctx;

static pc_status bit_depth_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *ctx, pc_error *err)
{
    (void)ha;
    (void)a;
    bit_depth_ctx *c = ctx;
    bool crosses = ((c->current == PC_SAMPLE_TYPE_F32) != (c->depth == PC_SAMPLE_TYPE_F32)) &&
                   (c->mode == PC_COLOR_MODE_RGB || c->mode == PC_COLOR_MODE_GRAYSCALE);
    bool to_linear = c->depth == PC_SAMPLE_TYPE_F32;
    pc_compose_transfer transfer =
        pc_compose_transfer_for_document(c->mode, to_linear ? c->current : c->depth);
    /* Colour samples cross the document's tone curve only when the depth crosses 32 bits: decoded
     * after widening, encoded before narrowing (so no linear value is quantised to 8/16 bits).
     * Masks, alpha and channels are not colour. */
    if (crosses && !to_linear) {
        map_color_ctx mc = {PC_OK, err, {transfer, true}};
        pc_engine_image_for_each_surface(doc->layers, doc->nlayers, false, map_color_cb, &mc);
        if (mc.rc != PC_OK) {
            return mc.rc;
        }
    }
    depth_ctx dc = {c->depth, PC_OK, err};
    pc_engine_image_for_each_surface(doc->layers, doc->nlayers, true, depth_convert_cb, &dc);
    if (dc.rc != PC_OK) {
        return dc.rc;
    }
    if (crosses && to_linear) {
        map_color_ctx mc = {PC_OK, err, {transfer, false}};
        pc_engine_image_for_each_surface(doc->layers, doc->nlayers, false, map_color_cb, &mc);
        if (mc.rc != PC_OK) {
            return mc.rc;
        }
    }
    for (size_t i = 0; i < doc->nchannels; i++) {
        pc_pixel_format f = pc_surface_format(&doc->channels[i].surface);
        f.sample = c->depth;
        pc_surface out;
        pc_status rc = pc_surface_convert(&doc->channels[i].surface, f, &out, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(&doc->channels[i].surface);
        doc->channels[i].surface = out;
    }
    if (doc->has_quick_mask) {
        pc_pixel_format f = pc_surface_format(&doc->quick_mask.surface);
        f.sample = c->depth;
        pc_surface out;
        pc_status rc = pc_surface_convert(&doc->quick_mask.surface, f, &out, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(&doc->quick_mask.surface);
        doc->quick_mask.surface = out;
    }
    doc->depth = c->depth;
    return PC_OK;
}

static pc_status convert_depth(pc_engine_session *s, pc_sample_type depth, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (doc->depth == depth) {
        return pc_engine_result_null(out, err); /* unchanged depth preserves the history branch */
    }
    bit_depth_ctx ctx = {depth, doc->depth, doc->mode};
    pc_status rc = pc_engine_session_edit(s, "Bit Depth", bit_depth_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_bits8(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return convert_depth(s, PC_SAMPLE_TYPE_U8, out, err);
}
static pc_status run_bits16(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return convert_depth(s, PC_SAMPLE_TYPE_U16, out, err);
}
static pc_status run_bits32(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return convert_depth(s, PC_SAMPLE_TYPE_F32, out, err);
}

/* ---- Duplicate ---------------------------------------------------------------------------- */

typedef struct {
    pc_layer_id old_id;
    pc_layer_id new_id;
} id_pair;

typedef struct {
    id_pair *pairs;
    size_t n, cap;
    bool ok;
} reid_ctx;

static void reid_cb(pc_layer *l, void *vctx)
{
    reid_ctx *c = vctx;
    pc_layer_id previous = l->id;
    pc_layer_id fresh = pc_layer_id_fresh();
    l->id = fresh;
    if (l->has_psd_id) {
        l->has_psd_id = false; /* a copied layer loses its PSD lyid */
    }
    if (!c->ok) {
        return;
    }
    id_pair *grown = pc_grow_array(c->pairs, &c->cap, c->n + 1, sizeof *c->pairs);
    if (grown == NULL) {
        c->ok = false;
        return;
    }
    c->pairs = grown;
    c->pairs[c->n].old_id = previous;
    c->pairs[c->n].new_id = fresh;
    c->n++;
}

static void remap_comp(pc_layer_comp *comp, const id_pair *pairs, size_t n)
{
    for (size_t i = 0; i < comp->nstates; i++) {
        for (size_t j = 0; j < n; j++) {
            if (comp->states[i].layer == pairs[j].old_id) {
                comp->states[i].layer = pairs[j].new_id;
                break;
            }
        }
    }
}

/* Assigns fresh ids to every layer of a copied document and updates comp references. */
pc_status pc_engine_reidentify_copied_layers(pc_document *doc, pc_error *err)
{
    pc_walk walk = {0};
    pc_status rc = pc_document_walk(doc, &walk, err);
    if (rc != PC_OK) { return rc; }
    size_t count = walk.len; pc_walk_fini(&walk);
    id_pair *pairs = pc_malloc_array(count ? count : 1, sizeof *pairs);
    if (!pairs) { return pc_fail(err, PC_ERR_NOMEM, "copied layer ids"); }
    reid_ctx c = {pairs, 0, count, true};
    for_each_layer(doc->layers, doc->nlayers, reid_cb, &c);
    if (c.ok) {
        for (size_t i = 0; i < doc->nlayer_comps; i++) {
            remap_comp(&doc->layer_comps[i], c.pairs, c.n);
        }
        if (doc->has_last_document_state) {
            remap_comp(&doc->last_document_state, c.pairs, c.n);
        }
    }
    free(c.pairs);
    return PC_OK;
}

static pc_status run_duplicate(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *src = pc_doc_arc_get(d->doc);
    pc_document doc;
    if (pc_document_clone(src, &doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    doc.id = pc_doc_id_fresh();
    /* Name: the given one, else "<name> copy". */
    const char *name = str_param(p, "name", NULL);
    pc_status rc;
    if (name != NULL) {
        rc = pc_str_set_c(&doc.name, name, err);
    } else {
        char buf[512];
        (void)snprintf(buf, sizeof buf, "%s copy", pc_str_c(&src->name));
        rc = pc_str_set_c(&doc.name, buf, err);
    }
    if (rc != PC_OK) {
        pc_document_fini(&doc);
        return rc;
    }
    if (bool_param(p, "mergedOnly", false)) {
        pc_pixel_format fmt = pc_document_pixel_format(&doc);
        pc_surface surf;
        pc_surface_init(&surf, fmt);
        pc_compose_buffer buf;
        if (pc_compose_flatten(&doc, &buf, err) != PC_OK) {
            pc_surface_fini(&surf);
            pc_document_fini(&doc);
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        pc_rect bounds = pc_document_bounds(&doc);
        size_t nch = pc_pixel_format_channels(fmt);
        float *vals = NULL;
        if (buf.len > 0 && nch > 0) {
            vals = pc_malloc_array(buf.len * nch, sizeof(float));
        }
        if ((buf.len > 0 && nch > 0 && vals == NULL)) {
            pc_compose_buffer_fini(&buf);
            pc_surface_fini(&surf);
            pc_document_fini(&doc);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory flattening the duplicate");
        }
        for (size_t i = 0; vals != NULL && i < buf.len; i++) {
            float enc[PC_RASTER_MAX_CHANNELS];
            size_t m = pc_from_rgba(fmt, buf.px[i], enc);
            memcpy(&vals[i * nch], enc, m * sizeof(float));
        }
        pc_status wr = PC_OK;
        if (vals != NULL) {
            wr = pc_surface_write_region(&surf, bounds, vals, buf.len * nch, err);
        }
        free(vals);
        pc_compose_buffer_fini(&buf);
        if (wr != PC_OK) {
            pc_surface_fini(&surf);
            pc_document_fini(&doc);
            return wr;
        }
        pc_surface_prune(&surf);
        pc_layer bg;
        pc_layer_content content;
        content.kind = PC_LAYER_RASTER;
        content.u.raster = surf;
        if (pc_layer_init(&bg, "Background", &content, err) != PC_OK) {
            pc_layer_content_fini(&content);
            pc_document_fini(&doc);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        /* Replace layers with the single flattened one; a merged copy keeps no comps. */
        for (size_t i = 0; i < doc.nlayers; i++) {
            pc_layer_fini(&doc.layers[i]);
        }
        free(doc.layers);
        pc_layer *one = pc_malloc_array(1, sizeof(pc_layer));
        if (one == NULL) {
            pc_layer_fini(&bg);
            doc.layers = NULL;
            doc.nlayers = 0;
            pc_document_fini(&doc);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory building the duplicate");
        }
        one[0] = bg;
        doc.layers = one;
        doc.nlayers = 1;
        for (size_t i = 0; i < doc.nlayer_comps; i++) {
            pc_layer_comp_fini(&doc.layer_comps[i]);
        }
        free(doc.layer_comps);
        doc.layer_comps = NULL;
        doc.nlayer_comps = 0;
        doc.has_last_applied_comp = false;
        if (doc.has_last_document_state) {
            pc_layer_comp_fini(&doc.last_document_state);
            doc.has_last_document_state = false;
        }
    }
    pc_status identified = pc_engine_reidentify_copied_layers(&doc, err);
    if (identified != PC_OK) { pc_document_fini(&doc); return identified; }
    /* Keep the name for the result before the document is moved into the session. */
    char name_copy[512];
    (void)snprintf(name_copy, sizeof name_copy, "%s", pc_str_c(&doc.name));
    size_t index = 0;
    pc_status add = pc_engine_session_add_document(s, &doc, NULL, &index, err);
    if (add != PC_OK) {
        pc_document_fini(&doc);
        return add;
    }
    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(jd);
    yyjson_mut_obj_add_uint(jd, root, "document", index);
    yyjson_mut_obj_add_strcpy(jd, root, "name", name_copy);
    yyjson_mut_doc_set_root(jd, root);
    return PC_OK;
}

/* ---- Reveal All --------------------------------------------------------------------------- */

static pc_rect reveal_union(const pc_layer *layers, size_t n, pc_rect acc)
{
    for (size_t i = 0; i < n; i++) {
        const pc_layer *l = &layers[i];
        const pc_surface *surf = pc_layer_surface(l);
        if (surf != NULL) {
            acc = pc_rect_union(acc, pc_surface_content_bounds(surf));
        }
        if (l->content.kind == PC_LAYER_GROUP) {
            acc = reveal_union(l->content.u.group.children, l->content.u.group.nchildren, acc);
        }
    }
    return acc;
}

static pc_status run_reveal_all(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_rect canvas = pc_document_bounds(doc);
    pc_rect all = reveal_union(doc->layers, doc->nlayers, canvas);
    bool changed = !(all.x0 == canvas.x0 && all.y0 == canvas.y0 && all.x1 == canvas.x1 &&
                     all.y1 == canvas.y1);
    if (changed) {
        char params[128];
        (void)snprintf(params, sizeof params,
                       "{\"x\":%d,\"y\":%d,\"width\":%u,\"height\":%u,\"deleteCroppedPixels\":false}",
                       all.x0, all.y0, pc_rect_width(all), pc_rect_height(all));
        pc_json_value tmp;
        pc_status rc = pc_engine_session_execute_json(s, "image.crop", params, &tmp, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_json_value_fini(&tmp);
    }
    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(jd);
    yyjson_mut_obj_add_bool(jd, root, "changed", changed);
    yyjson_mut_doc_set_root(jd, root);
    return PC_OK;
}

/* ---- right-angle Image Rotation / flips --------------------------------------------------- */

typedef struct {
    pc_engine_turn turn;
} turn_ctx2;

static pc_status turn_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *ctx, pc_error *err)
{
    (void)ha;
    (void)a;
    turn_ctx2 *c = ctx;
    return pc_engine_turn_canvas(doc, c->turn, err);
}

static pc_status run_turn(pc_engine_session *s, pc_engine_turn turn, const char *label, pc_json_value *out,
                          pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_affine a = pc_engine_turn_affine(turn, (double)doc->size.width, (double)doc->size.height);
    set_sampler(s, a);
    turn_ctx2 ctx = {turn};
    pc_status rc = pc_engine_session_edit(s, label, turn_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_flip_h(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return run_turn(s, PC_ENGINE_TURN_FLIP_H, "Flip Canvas Horizontal", out, err);
}
static pc_status run_flip_v(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return run_turn(s, PC_ENGINE_TURN_FLIP_V, "Flip Canvas Vertical", out, err);
}
static pc_status run_rotate_180(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return run_turn(s, PC_ENGINE_TURN_ROTATE_180, "Rotate Canvas", out, err);
}
static pc_status run_rotate_cw(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return run_turn(s, PC_ENGINE_TURN_CW90, "Rotate Canvas", out, err);
}
static pc_status run_rotate_ccw(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return run_turn(s, PC_ENGINE_TURN_CCW90, "Rotate Canvas", out, err);
}

/* ---- specs -------------------------------------------------------------------------------- */

static const char *const MENU_IMAGE[] = {"Image"};
static const char *const MENU_MODE[] = {"Image", "Mode"};
static const char *const MENU_ROT[] = {"Image", "Image Rotation"};

static const char *const P_MODE =
    "{\"profile\":\"<builtin id>|working|/path/to/profile.icc\"=working,\"intent\":\"perceptual|"
    "relative|saturation|absolute\"=\"relative\",\"bpc\":bool=true}";

static const pc_engine_command IMAGE[] = {
    {"image.imageSize", "Image Size\xE2\x80\xA6", MENU_IMAGE, 1, NULL,
     "{\"width\":px,\"height\":px,\"percent\":%?,\"resolution\":ppi,\"keepPrintSize\":bool=false,"
     "\"resample\":\"automatic|preserveDetails|bicubicSmoother|bicubicSharper|bicubicAutomatic|"
     "bicubic|nearest|bilinear|lanczos|none\"=\"bicubic\"} -> {width,height}",
     pc_engine_has_doc, run_image_size, true},
    {"image.canvasSize", "Canvas Size\xE2\x80\xA6", MENU_IMAGE, 1, NULL,
     "{\"width\":px,\"height\":px,\"relative\":bool=false,\"anchor\":\"topLeft|top|topRight|left|"
     "center|right|bottomLeft|bottom|bottomRight\"=\"center\",\"extensionColor\":\"background|"
     "foreground|white|black|transparent|#rrggbb\"=\"background\"}",
     pc_engine_has_doc, run_canvas_size, true},
    {"image.crop", "Crop", MENU_IMAGE, 1, NULL,
     "{\"x\":px,\"y\":px,\"width\":px,\"height\":px,\"angle\":-3600..3600=0,"
     "\"deleteCroppedPixels\":bool=true,\"targetWidth\":px,\"targetHeight\":px,\"resolution\":ppi,"
     "\"resample\":\"bicubic|bilinear|nearest|lanczos|preserveDetails\"=\"bicubic\"} -> "
     "{x,y,width,height,documentWidth,documentHeight,resolution}",
     pc_engine_has_doc, run_crop, true},
    {"image.trim", "Trim\xE2\x80\xA6", MENU_IMAGE, 1, NULL,
     "{\"basedOn\":\"transparent|topLeft|bottomRight\"=\"transparent\",\"top\":bool=true,"
     "\"bottom\":bool=true,\"left\":bool=true,\"right\":bool=true}",
     pc_engine_has_doc, run_trim, true},
    {"image.mode.rgb", "RGB Color", MENU_MODE, 2, NULL, P_MODE, pc_engine_has_doc, run_mode_rgb, true},
    {"image.mode.grayscale", "Grayscale", MENU_MODE, 2, NULL, P_MODE, pc_engine_has_doc,
     run_mode_grayscale, true},
    {"image.mode.cmyk", "CMYK Color", MENU_MODE, 2, NULL, P_MODE, pc_engine_has_doc, run_mode_cmyk, true},
    {"image.mode.lab", "Lab Color", MENU_MODE, 2, NULL, P_MODE, pc_engine_has_doc, run_mode_lab, true},
    {"image.mode.bits8", "8 Bits/Channel", MENU_MODE, 2, NULL, "{}", pc_engine_has_doc, run_bits8, true},
    {"image.mode.bits16", "16 Bits/Channel", MENU_MODE, 2, NULL, "{}", pc_engine_has_doc, run_bits16,
     true},
    {"image.mode.bits32", "32 Bits/Channel", MENU_MODE, 2, NULL, "{}", pc_engine_has_doc, run_bits32,
     true},
    {"image.duplicate", "Duplicate\xE2\x80\xA6", MENU_IMAGE, 1, NULL,
     "{\"name\":str,\"mergedOnly\":bool=false}", pc_engine_has_doc, run_duplicate, true},
    {"image.revealAll", "Reveal All", MENU_IMAGE, 1, NULL, "{}", pc_engine_has_doc, run_reveal_all, true},
    {"image.imageRotation.flipCanvasHorizontal", "Flip Canvas Horizontal", MENU_ROT, 2, NULL, "{}",
     pc_engine_has_doc, run_flip_h, true},
    {"image.imageRotation.flipCanvasVertical", "Flip Canvas Vertical", MENU_ROT, 2, NULL, "{}",
     pc_engine_has_doc, run_flip_v, true},
    {"image.imageRotation.180", "180\xC2\xB0", MENU_ROT, 2, NULL, "{}", pc_engine_has_doc,
     run_rotate_180, true},
    {"image.imageRotation.90cw", "90\xC2\xB0 Clockwise", MENU_ROT, 2, NULL, "{}", pc_engine_has_doc,
     run_rotate_cw, true},
    {"image.imageRotation.90ccw", "90\xC2\xB0 Counter Clockwise", MENU_ROT, 2, NULL, "{}",
     pc_engine_has_doc, run_rotate_ccw, true},
};

const pc_engine_command *pc_engine_image_specs(size_t *n)
{
    *n = sizeof IMAGE / sizeof IMAGE[0];
    return IMAGE;
}
