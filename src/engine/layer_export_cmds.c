/* Layer menu: Quick Export as PNG, Export As…, and Layer Style › Create Layer (port of
 * crates/engine/src/layer_menu_cmds.rs create_layer / layer_document / export_layer).
 *
 *  - layer.quickExportAsPng / layer.exportAs: the active layer rendered alone and trimmed to its
 *    pixels is written as a one-layer image (PNG for Quick Export; any format for Export As).
 *  - layer.layerStyle.createLayer: each enabled effect becomes its own pixel layer carrying the
 *    effect's blend mode and opacity (shadows/glows/outer strokes below the layer; inner effects
 *    and overlays above it, clipped); the layer keeps its pixels and loses its effects.
 *
 * Never crashes (REWRITE_PLAN.md rule 9): params and layer kinds are validated, sizes bounded, and
 * the one history step is all-or-nothing. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/buf.h"
#include "base/json.h"
#include "compose/compose.h"
#include "compose/effects.h"
#include "doc/doc.h"
#include "doc/effects.h"
#include "geom/geom.h"
#include "io/core.h"
#include "io/io.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "allocation.h"
#include "engine_internal.h"
#include "layer_cmds.h" /* pc_engine_layer_param */
#include "layer_export_cmds.h"

static const char *const MENU_LAYER[] = {"Layer"};
static const char *const MENU_LAYER_STYLE[] = {"Layer", "Layer Style"};

/* Render `alone` (a prepared, borrowed layer) in the context of `doc` (patterns, global light,
 * mode, depth, profiles) over `area`; *out is an owned compose buffer on success. */
static pc_status render_single(const pc_document *doc, const pc_layer *alone, pc_rect area,
                               pc_compose_buffer *out, pc_error *err)
{
    pc_document tmp;
    pc_status rc = pc_document_clone(doc, &tmp, err);
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t i = 0; i < tmp.nlayers; i++) {
        pc_layer_fini(&tmp.layers[i]);
    }
    free(tmp.layers);
    tmp.layers = NULL;
    tmp.nlayers = 0;
    if (tmp.has_selection) {
        pc_surface_fini(&tmp.selection);
        tmp.has_selection = false;
    }
    pc_layer *one = pc_malloc_array(1, sizeof *one);
    if (one == NULL) {
        pc_document_fini(&tmp);
        return pc_fail(err, PC_ERR_NOMEM, "render layer");
    }
    rc = pc_layer_clone(alone, &one[0], err);
    if (rc != PC_OK) {
        free(one);
        pc_document_fini(&tmp);
        return rc;
    }
    tmp.layers = one;
    tmp.nlayers = 1;
    rc = pc_compose_render(&tmp, area, out, err);
    pc_document_fini(&tmp);
    return rc;
}

/* The layer rendered alone (visible, blend ignored) and trimmed to its pixels, as a one-layer
 * document — what Export As does for a layer. *out is an initialised document on success. */
static pc_status layer_document(const pc_document *doc, pc_layer_id id, pc_document *out, pc_error *err)
{
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    pc_layer alone;
    pc_status rc = pc_layer_clone(l, &alone, err);
    if (rc != PC_OK) {
        return rc;
    }
    alone.visible = true;
    alone.blend = pc_layer_is_group(&alone) ? PC_BLEND_PASS_THROUGH : PC_BLEND_NORMAL;
    alone.clipped = false;
    pc_rect area = pc_document_bounds(doc);
    pc_compose_buffer buf;
    rc = render_single(doc, &alone, area, &buf, err);
    pc_layer_fini(&alone);
    if (rc != PC_OK) {
        return rc;
    }
    /* Trim to the pixels with alpha. */
    int32_t w = (int32_t)pc_rect_width(area);
    pc_rect b = PC_RECT_EMPTY;
    for (size_t i = 0; i < buf.len; i++) {
        if (buf.px[i][3] > 0.0f) {
            int32_t x = area.x0 + (int32_t)(i % (size_t)w);
            int32_t y = area.y0 + (int32_t)(i / (size_t)w);
            b = pc_rect_union(b, pc_rect_from_xywh(x, y, 1, 1));
        }
    }
    if (pc_rect_is_empty(b)) {
        pc_compose_buffer_fini(&buf);
        return pc_engine_err_other(err, "the layer is empty");
    }
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    size_t n = pc_pixel_format_channels(fmt);
    if (n == 0) {
        pc_compose_buffer_fini(&buf);
        return pc_engine_err_other(err, "the layer has no channels");
    }
    uint32_t bw = (uint32_t)pc_rect_width(b), bh = (uint32_t)pc_rect_height(b);
    const char *name = l->name.ptr != NULL ? l->name.ptr : "Layer";
    rc = pc_document_init(out, name, pc_size_make(bw, bh), doc->mode, doc->depth, err);
    if (rc != PC_OK) {
        pc_compose_buffer_fini(&buf);
        return rc;
    }
    out->resolution_dpi = doc->resolution_dpi;
    if (doc->icc_profile != NULL) {
        out->icc_profile = pc_blob_retain(doc->icc_profile);
    }
    float *rows = NULL;
    size_t want = (size_t)bw * (size_t)bh * n;
    rc = pc_engine_alloc_filled(want ? want : 1, sizeof(float), "export layer", (void **)&rows, err);
    if (rc != PC_OK) {
        pc_compose_buffer_fini(&buf);
        pc_document_fini(out);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    size_t k = 0;
    for (int32_t y = b.y0; y < b.y1; y++) {
        size_t row = (size_t)(y - area.y0) * (size_t)w;
        for (int32_t x = b.x0; x < b.x1; x++) {
            (void)pc_from_rgba(fmt, buf.px[row + (size_t)(x - area.x0)], &rows[k * n]);
            k++;
        }
    }
    pc_compose_buffer_fini(&buf);
    pc_layer nl;
    rc = pc_layer_raster(&nl, name, fmt, err);
    if (rc != PC_OK) {
        free(rows);
        pc_document_fini(out);
        return rc;
    }
    pc_surface *su = pc_layer_surface_mut(&nl);
    if (su != NULL) {
        rc = pc_surface_write_region(su, pc_rect_from_xywh(0, 0, bw, bh), rows, want, err);
        pc_surface_prune(su);
    }
    free(rows);
    if (rc != PC_OK) {
        pc_layer_fini(&nl);
        pc_document_fini(out);
        return rc;
    }
    /* Replace out's (empty) layer list with this one. */
    for (size_t i = 0; i < out->nlayers; i++) {
        pc_layer_fini(&out->layers[i]);
    }
    free(out->layers);
    out->layers = NULL;
    out->nlayers = 0;
    pc_layer_id nid = 0;
    rc = pc_document_insert_above(out, NULL, &nl, &nid, err);
    if (rc != PC_OK) {
        pc_layer_fini(&nl);
        pc_document_fini(out);
        return rc;
    }
    return PC_OK;
}

/* ---- Quick Export as PNG / Export As -------------------------------------------------------- */

static pc_status run_export_layer(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                  const char *cmd, bool png_only, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_layer_id id = 0;
    pc_status rc = pc_engine_layer_param(s, p, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_document ldoc;
    memset(&ldoc, 0, sizeof ldoc); /* layer_document initialises it on success */
    rc = layer_document(doc, id, &ldoc, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t plen = 0;
    const char *path = p != NULL ? pc_json_as_str(pc_json_get(p, "path"), &plen) : NULL;
    if (path == NULL || plen == 0) {
        pc_document_fini(&ldoc);
        return pc_engine_err_bad_params(err, cmd, "missing `path` (UIs ask for one)");
    }
    if (png_only) {
        bool is_png = plen >= 4 && (path[plen - 4] == '.') &&
                      (path[plen - 3] == 'p' || path[plen - 3] == 'P') &&
                      (path[plen - 2] == 'n' || path[plen - 2] == 'N') &&
                      (path[plen - 1] == 'g' || path[plen - 1] == 'G');
        if (!is_png) {
            pc_document_fini(&ldoc);
            return pc_engine_err_bad_params(err, cmd, "Quick Export as PNG writes .png files");
        }
    }
    double scale = 100.0;
    yyjson_val *js = p != NULL ? pc_json_get(p, "scale") : NULL;
    if (js != NULL && pc_json_as_f64(js, &scale)) {
        scale = isfinite(scale) ? scale : 100.0;
        scale = scale < 1.0 ? 1.0 : (scale > 1000.0 ? 1000.0 : scale);
    } else {
        scale = 100.0;
    }
    if (fabs(scale - 100.0) > 1e-3) {
        pc_engine_session *tmp = NULL;
        if (pc_engine_session_new(&tmp, err) != PC_OK) {
            pc_document_fini(&ldoc);
            return PC_ERR_NOMEM;
        }
        pc_document moved = ldoc; /* add_document takes it over */
        memset(&ldoc, 0, sizeof ldoc);
        if (pc_engine_session_add_document(tmp, &moved, NULL, NULL, err) != PC_OK) {
            pc_document_fini(&moved);
            pc_engine_session_free(tmp);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        const pc_engine_doc_state *td = pc_engine_session_active(tmp);
        uint32_t sw = td != NULL ? pc_doc_arc_get(td->doc)->size.width : 1;
        double nw = round((double)sw * scale / 100.0);
        if (nw < 1.0) {
            nw = 1.0;
        }
        char rj[96];
        (void)snprintf(rj, sizeof rj, "{\"width\":%.0f}", nw);
        pc_json_value sub = PC_JSON_VALUE_INIT;
        rc = pc_engine_session_execute_json(tmp, "image.imageSize", rj, &sub, err);
        pc_json_value_fini(&sub);
        if (rc != PC_OK) {
            pc_engine_session_free(tmp);
            return rc;
        }
        td = pc_engine_session_active(tmp);
        rc = pc_document_clone(pc_doc_arc_get(td->doc), &ldoc, err);
        pc_engine_session_free(tmp);
        if (rc != PC_OK) {
            return rc;
        }
    }

    uint32_t ow = ldoc.size.width, oh = ldoc.size.height;
    pc_io_export_opts opts = pc_io_export_opts_default();
    pc_io_export_result res = {0};
    rc = pc_io_export(&ldoc, path, &opts, &res, err);
    pc_document_fini(&ldoc);
    if (rc != PC_OK) {
        pc_io_export_result_fini(&res);
        return rc;
    }
    rc = pc_engine_write_file(path, res.bytes, res.len, err);
    size_t nbytes = res.len;
    pc_io_export_result_fini(&res);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "path", path);
    yyjson_mut_obj_add_uint(rd, root, "bytes", nbytes);
    yyjson_mut_obj_add_uint(rd, root, "width", ow);
    yyjson_mut_obj_add_uint(rd, root, "height", oh);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_quick_png(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_export_layer(s, p, out, "layer.quickExportAsPng", true, err);
}
static pc_status run_export_as(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return run_export_layer(s, p, out, "layer.exportAs", false, err);
}

/* ---- Layer Style › Create Layer ------------------------------------------------------------- */

static bool is_below(const pc_effect *e)
{
    switch (e->kind) {
    case PC_FX_DROP_SHADOW:
    case PC_FX_OUTER_GLOW:
        return true;
    case PC_FX_STROKE:
        return e->u.stroke.position == PC_STROKE_OUTSIDE;
    case PC_FX_BEVEL_EMBOSS:
        return e->u.bevel.style == PC_BEVEL_OUTER || e->u.bevel.style == PC_BEVEL_EMBOSS ||
               e->u.bevel.style == PC_BEVEL_PILLOW_EMBOSS;
    default:
        return false;
    }
}

/* The effect's shared blend/opacity slot, or NULL (Bevel & Emboss has none). */
static pc_fx_common *effect_common(pc_effect *e)
{
    switch (e->kind) {
    case PC_FX_DROP_SHADOW:
    case PC_FX_INNER_SHADOW:
        return &e->u.shadow.common;
    case PC_FX_OUTER_GLOW:
    case PC_FX_INNER_GLOW:
        return &e->u.glow.common;
    case PC_FX_STROKE:
        return &e->u.stroke.common;
    case PC_FX_COLOR_OVERLAY:
        return &e->u.color_overlay.common;
    case PC_FX_GRADIENT_OVERLAY:
        return &e->u.gradient_overlay.common;
    case PC_FX_PATTERN_OVERLAY:
        return &e->u.pattern_overlay.common;
    case PC_FX_SATIN:
        return &e->u.satin.common;
    default:
        return NULL;
    }
}

static pc_status cl_enabled(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (doc == NULL || !d->has_active_layer) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    if (l == NULL || !pc_compose_has_effects(l)) {
        return pc_fail(reason, PC_ERR_INVALID, "the layer has no layer effects");
    }
    return PC_OK;
}

struct cl_ctx {
    pc_layer_id id;
    size_t made;
};

static pc_status cl_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    struct cl_ctx *c = vctx;
    const pc_layer *lp = pc_document_layer(doc, c->id);
    if (lp == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    if (!lp->effects.enabled || lp->effects.nitems == 0) {
        return pc_engine_err_other(err, "the layer has no layer effects");
    }
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    size_t n = pc_pixel_format_channels(fmt);
    if (n == 0) {
        return pc_engine_err_other(err, "the layer has no channels");
    }
    pc_rect canvas = pc_document_bounds(doc);
    const pc_surface *lsurf = pc_layer_surface(lp);
    pc_rect area = lsurf != NULL ? pc_rect_union(pc_surface_content_bounds(lsurf), canvas) : canvas;
    area = pc_rect_inflate(area, 256);

    /* A clone of the source to render each effect alone over. */
    pc_layer base;
    pc_status rc = pc_layer_clone(lp, &base, err);
    if (rc != PC_OK) {
        return rc;
    }

    pc_layer below[PC_FX_KIND_COUNT];
    pc_layer above[PC_FX_KIND_COUNT];
    bool below_ok[PC_FX_KIND_COUNT] = {0}, above_ok[PC_FX_KIND_COUNT] = {0};
    size_t nbelow = 0, nabove = 0;
    const char *lname = base.name.ptr != NULL ? base.name.ptr : "Layer";

    for (size_t i = 0; i < lp->effects.nitems; i++) {
        const pc_effect *src = &lp->effects.items[i];
        if (!pc_effect_enabled(src)) {
            continue;
        }
        /* alone = the layer with one effect, own pixels hidden (fill opacity 0). */
        pc_layer alone;
        rc = pc_layer_clone(&base, &alone, err);
        if (rc != PC_OK) {
            goto fail;
        }
        alone.visible = true;
        alone.opacity = 1.0f;
        alone.blend = PC_BLEND_NORMAL;
        alone.clipped = false;
        if (alone.has_mask) {
            pc_layer_mask_fini(&alone.mask);
            alone.has_mask = false;
        }
        alone.fill_opacity = 0.0f;
        pc_effect e1;
        rc = pc_effect_clone(src, &e1, err);
        if (rc != PC_OK) {
            pc_layer_fini(&alone);
            goto fail;
        }
        pc_blend_mode blend = PC_BLEND_NORMAL;
        float opacity = 1.0f;
        pc_fx_common *cm = effect_common(&e1);
        if (cm != NULL) {
            blend = cm->blend;
            opacity = cm->opacity;
            cm->blend = PC_BLEND_NORMAL;
            cm->opacity = 1.0f;
        }
        /* Replace alone's effects with just e1. */
        pc_effects_fini(&alone.effects);
        memset(&alone.effects, 0, sizeof alone.effects);
        alone.effects.enabled = true;
        alone.effects.items = pc_malloc_array(1, sizeof(pc_effect));
        if (alone.effects.items == NULL) {
            pc_effect_fini(&e1);
            pc_layer_fini(&alone);
            rc = pc_fail(err, PC_ERR_NOMEM, "create layer");
            goto fail;
        }
        alone.effects.items[0] = e1; /* moved */
        alone.effects.nitems = 1;

        pc_compose_buffer buf;
        rc = render_single(doc, &alone, area, &buf, err);
        pc_layer_fini(&alone);
        if (rc != PC_OK) {
            goto fail;
        }
        /* Build the new raster layer named "<layer>'s <effect>". */
        char nm[512];
        (void)snprintf(nm, sizeof nm, "%s's %s", lname, pc_effect_label(src));
        pc_layer nl;
        rc = pc_layer_raster(&nl, nm, fmt, err);
        if (rc != PC_OK) {
            pc_compose_buffer_fini(&buf);
            goto fail;
        }
        pc_surface *su = pc_layer_surface_mut(&nl);
        if (su != NULL && buf.len > 0) {
            float *data = NULL;
            if (pc_engine_alloc_filled(buf.len * n, sizeof(float), "create layer", (void **)&data,
                                       err) == PC_OK) {
                for (size_t j = 0; j < buf.len; j++) {
                    (void)pc_from_rgba(fmt, buf.px[j], &data[j * n]);
                }
                (void)pc_surface_write_region(su, buf.rect, data, buf.len * n, err);
                pc_surface_prune(su);
                free(data);
            }
        }
        pc_compose_buffer_fini(&buf);
        nl.blend = blend;
        nl.opacity = opacity;
        if (is_below(src)) {
            below[nbelow] = nl;
            below_ok[nbelow] = true;
            nbelow++;
        } else {
            nl.clipped = true;
            above[nabove] = nl;
            above_ok[nabove] = true;
            nabove++;
        }
    }

    if (nbelow + nabove == 0) {
        pc_layer_fini(&base);
        return pc_engine_err_other(err, "the layer has no layer effects");
    }

    /* Clear the source's effects. */
    pc_layer *m = pc_document_layer_mut(doc, c->id);
    if (m == NULL) {
        rc = pc_engine_err_no_layer(err, c->id);
        goto fail;
    }
    pc_effects_fini(&m->effects);
    memset(&m->effects, 0, sizeof m->effects);
    m->effects.enabled = true;

    /* Below layers: each directly below the source (first inserted ends up furthest). */
    for (size_t i = 0; i < nbelow; i++) {
        pc_layer_id nid = 0;
        rc = pc_document_insert_above(doc, &c->id, &below[i], &nid, err);
        if (rc != PC_OK) {
            goto fail;
        }
        below_ok[i] = false; /* moved */
        (void)pc_document_shift(doc, nid, -1);
    }
    /* Above (clipped) layers: above0 directly above the source, the rest stacked on it. */
    pc_layer_id prev = c->id;
    for (size_t i = 0; i < nabove; i++) {
        pc_layer_id nid = 0;
        rc = pc_document_insert_above(doc, &prev, &above[i], &nid, err);
        if (rc != PC_OK) {
            goto fail;
        }
        above_ok[i] = false; /* moved */
        prev = nid;
    }
    pc_layer_fini(&base);
    *active = c->id;
    c->made = nbelow + nabove;
    return PC_OK;

fail:
    for (size_t i = 0; i < nbelow; i++) {
        if (below_ok[i]) {
            pc_layer_fini(&below[i]);
        }
    }
    for (size_t i = 0; i < nabove; i++) {
        if (above_ok[i]) {
            pc_layer_fini(&above[i]);
        }
    }
    pc_layer_fini(&base);
    return rc;
}

static pc_status run_create_layer(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    struct cl_ctx c = {0, 0};
    pc_status rc = pc_engine_layer_param(s, p, &c.id, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_engine_session_edit(s, "Create Layers", cl_edit, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layers", c.made);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const pc_engine_command SPECS[] = {
    {"layer.layerStyle.createLayer", "Create Layer", MENU_LAYER_STYLE, 2, NULL, "{\"layer\":id?}",
     cl_enabled, run_create_layer, true},
    {"layer.quickExportAsPng", "Quick Export as PNG", MENU_LAYER, 1, NULL,
     "{\"layer\":id?,\"path\":text,\"scale\":1..1000=100} -> {path,bytes,width,height} (the active "
     "layer rendered alone, trimmed, written as PNG)",
     pc_engine_has_doc, run_quick_png, true},
    {"layer.exportAs", "Export As…", MENU_LAYER, 1, NULL,
     "{\"layer\":id?,\"path\":text,\"scale\":1..1000=100} -> {path,bytes,width,height} (the active "
     "layer rendered alone, trimmed, written in the path's format)",
     pc_engine_has_doc, run_export_as, true},
};

const pc_engine_command *pc_engine_layer_export_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
