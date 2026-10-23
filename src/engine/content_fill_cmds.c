/* Edit > Content-Aware Fill: edit.contentAwareFill (port of edit_menu_cmds::content_aware_fill).
 *
 * The selection is filled by PatchMatch completion (photocraft_algo content_aware) sampling the
 * window around it (Auto / Rectangular / Custom), with colour and geometric adaptation, then the
 * result is blended in by the selection's coverage. Output goes into the layer, a new layer, or a
 * duplicate. Runs synchronously (the background-job path is a later refinement, as for
 * Content-Aware Scale). Never crashes (REWRITE_PLAN.md rule 9): params are validated, the window
 * size is bounded before allocating, and the one history step is all-or-nothing. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "algo/content_aware.h"
#include "base/json.h"
#include "base/num.h"
#include "base/str.h"
#include "color/color.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "geom/geom.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "allocation.h"
#include "content_fill_cmds.h"
#include "engine_internal.h"
#include "fill_cmds.h" /* pc_engine_fill_sampling_window / window_size */

#define CMD "edit.contentAwareFill"

static const char *const MENU_EDIT[] = {"Edit"};

/* The active raster layer id, or an error. */
static pc_status caf_layer(const pc_engine_session *s, pc_layer_id *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (!d->has_active_layer || doc == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no active layer");
    }
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    if (l == NULL || l->content.kind != PC_LAYER_RASTER) {
        return pc_fail(err, PC_ERR_INVALID, "Content-Aware Fill needs a pixel layer");
    }
    *out = d->active_layer;
    return PC_OK;
}

static pc_status caf_enabled(const pc_engine_session *s, pc_error *reason)
{
    pc_layer_id id = 0;
    pc_status rc = caf_layer(s, &id, reason);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (!doc->has_selection || pc_rect_is_empty(pc_surface_content_bounds(&doc->selection))) {
        return pc_fail(reason, PC_ERR_INVALID, "make a selection around what to remove");
    }
    return PC_OK;
}

/* Resolve the `channel` param (alpha channel index or name) to a surface, or NULL. */
static const pc_surface *channel_mask(const pc_document *doc, yyjson_val *v)
{
    if (v == NULL) {
        return NULL;
    }
    uint64_t idx = 0;
    if (pc_json_as_u64(v, &idx)) {
        return idx < doc->nchannels ? &doc->channels[idx].surface : NULL;
    }
    size_t sl = 0;
    const char *name = pc_json_as_str(v, &sl);
    if (name != NULL) {
        for (size_t i = 0; i < doc->nchannels; i++) {
            if (pc_str_eq_mem(&doc->channels[i].name, name, sl)) {
                return &doc->channels[i].surface;
            }
        }
    }
    return NULL;
}

/* `[x, y, w, h]` with saturating float->int, w/h >= 0. */
static bool rect_param(yyjson_val *p, const char *key, pc_rect *out)
{
    yyjson_val *a = p != NULL ? pc_json_get(p, key) : NULL;
    if (a == NULL || !yyjson_is_arr(a) || yyjson_arr_size(a) != 4) {
        return false;
    }
    double v[4];
    for (size_t i = 0; i < 4; i++) {
        if (!pc_json_as_f64(yyjson_arr_get(a, i), &v[i]) || !isfinite(v[i])) {
            return false;
        }
    }
    int32_t x = pc_f64_to_i32(round(v[0])), y = pc_f64_to_i32(round(v[1]));
    int32_t w = pc_f64_to_i32(round(v[2] < 0 ? 0 : v[2])), h = pc_f64_to_i32(round(v[3] < 0 ? 0 : v[3]));
    *out = pc_rect_make(x, y, pc_i32_sat_add(x, w), pc_i32_sat_add(y, h));
    return true;
}

static const char *str_or(yyjson_val *p, const char *key, const char *def)
{
    size_t l = 0;
    const char *s = p != NULL ? pc_json_as_str(pc_json_get(p, key), &l) : NULL;
    return s != NULL ? s : def;
}

static bool bool_or(yyjson_val *p, const char *key, bool def)
{
    bool b = def;
    yyjson_val *v = p != NULL ? pc_json_get(p, key) : NULL;
    if (v != NULL) {
        (void)pc_json_as_bool(v, &b);
    }
    return b;
}

struct caf_ctx {
    pc_layer_id id;
    int output; /* 0 current, 1 new, 2 duplicate */
    pc_rect window;
    pc_pixel_format fmt;
    size_t n;
    float *filled; /* w*h*n, holes filled + blended by coverage */
    float *cover;  /* w*h coverage (for the new-layer alpha) */
    uint64_t n_hole;
};

static pc_status caf_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    struct caf_ctx *c = vctx;
    pc_rect window = c->window;
    size_t w = (size_t)pc_rect_width(window), h = (size_t)pc_rect_height(window);
    size_t n = c->n;
    pc_layer_id target = c->id;
    if (c->output == 1) {
        /* A new transparent layer above the source. */
        pc_pixel_format lf = c->fmt;
        lf.alpha = true;
        size_t ln = pc_pixel_format_channels(lf);
        pc_str name;
        pc_status rc = pc_document_next_layer_name(doc, "Content-Aware Fill", &name, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_layer nl;
        rc = pc_layer_raster(&nl, pc_str_c(&name), lf, err);
        pc_str_fini(&name);
        if (rc != PC_OK) {
            return rc;
        }
        float *px = calloc(w * h * (ln ? ln : 1), sizeof *px);
        if (px == NULL) {
            pc_layer_fini(&nl);
            return pc_fail(err, PC_ERR_NOMEM, "content-aware fill");
        }
        for (size_t i = 0; i < w * h; i++) {
            float cov = c->cover[i];
            if (cov <= 0.0f) {
                continue;
            }
            float rgba[4];
            pc_to_rgba(c->fmt, &c->filled[i * n], n, rgba);
            rgba[3] *= cov;
            float enc[PC_RASTER_MAX_CHANNELS];
            (void)pc_from_rgba(lf, rgba, enc);
            for (size_t ch = 0; ch < ln; ch++) {
                px[i * ln + ch] = enc[ch];
            }
        }
        pc_surface *su = pc_layer_surface_mut(&nl);
        if (su != NULL) {
            rc = pc_surface_write_region(su, window, px, w * h * ln, err);
            pc_surface_prune(su);
        }
        free(px);
        if (rc != PC_OK) {
            pc_layer_fini(&nl);
            return rc;
        }
        pc_layer_id nid = 0;
        rc = pc_document_insert_above(doc, &c->id, &nl, &nid, err);
        if (rc != PC_OK) {
            pc_layer_fini(&nl);
            return rc;
        }
        *active = nid;
        return PC_OK;
    }
    if (c->output == 2) {
        /* Duplicate the source, fill into the copy. */
        const pc_layer *src = pc_document_layer(doc, c->id);
        if (src == NULL) {
            return pc_engine_err_no_layer(err, c->id);
        }
        pc_layer dup;
        pc_status rc = pc_layer_duplicate(src, &dup, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_str cn;
        rc = pc_document_copy_name(doc, pc_str_c(&src->name), strlen(pc_str_c(&src->name)), &cn, err);
        if (rc == PC_OK) {
            pc_str_fini(&dup.name);
            dup.name = cn;
        }
        pc_layer_id nid = 0;
        rc = pc_document_insert_above(doc, &c->id, &dup, &nid, err);
        if (rc != PC_OK) {
            pc_layer_fini(&dup);
            return rc;
        }
        target = nid;
        *active = nid;
    }
    pc_layer *l = pc_document_layer_mut(doc, target);
    if (l == NULL || l->content.kind != PC_LAYER_RASTER) {
        return pc_engine_err_no_layer(err, target);
    }
    pc_surface *surf = &l->content.u.raster;
    pc_status rc = pc_surface_write_region(surf, window, c->filled, w * h * n, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface_prune(surf);
    return PC_OK;
}

static pc_status run_caf(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id = 0;
    pc_status rc = caf_layer(s, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (!doc->has_selection) {
        return pc_engine_err_bad_params(err, CMD, "no selection");
    }
    pc_rect canvas = pc_document_bounds(doc);
    pc_rect hb = pc_rect_intersect(pc_surface_content_bounds(&doc->selection), canvas);
    if (pc_rect_is_empty(hb)) {
        return pc_engine_err_bad_params(err, CMD, "the selection is outside the canvas");
    }
    const pc_layer *l = pc_document_layer(doc, id);
    const pc_surface *lsurf = pc_layer_surface(l);
    if (lsurf == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    pc_pixel_format fmt = pc_surface_format(lsurf);
    size_t n = pc_pixel_format_channels(fmt);
    if (n == 0) {
        return pc_engine_err_other(err, "the layer has no channels");
    }

    const char *sampling = str_or(p, "sampling", "auto");
    const pc_surface *cmask = channel_mask(doc, p != NULL ? pc_json_get(p, "channel") : NULL);
    bool have_rect = false;
    pc_rect crect = PC_RECT_EMPTY;
    if (p != NULL && pc_json_get(p, "area") != NULL) {
        if (!rect_param(p, "area", &crect)) {
            return pc_engine_err_bad_params(err, CMD, "`area` must be [x, y, w, h]");
        }
        have_rect = true;
    }

    pc_rect window;
    if (strcmp(sampling, "auto") == 0) {
        window = pc_engine_fill_sampling_window(hb, canvas);
    } else if (strcmp(sampling, "rectangular") == 0) {
        yyjson_val *jm = p != NULL ? pc_json_get(p, "margin") : NULL;
        int64_t m = 0;
        if (jm != NULL && pc_json_as_i64(jm, &m)) {
            if (m < 0) {
                m = 0;
            }
            if (m > 100000) {
                m = 100000;
            }
            window = pc_rect_inflate(hb, (int32_t)m);
        } else {
            window = pc_engine_fill_sampling_window(hb, canvas);
        }
    } else if (strcmp(sampling, "custom") == 0) {
        pc_rect r;
        if (have_rect) {
            r = crect;
        } else if (cmask != NULL) {
            r = pc_surface_content_bounds(cmask);
        } else {
            return pc_engine_err_bad_params(
                err, CMD, "custom sampling needs `area` [x,y,w,h] or `channel` (alpha channel index or name)");
        }
        window = pc_rect_inflate(pc_rect_union(r, hb), 4);
    } else {
        return pc_engine_err_bad_params(err, CMD, "unknown sampling `%s` (auto|rectangular|custom)",
                                        sampling);
    }
    window = pc_rect_intersect(window, canvas);
    if (pc_rect_is_empty(window)) {
        return pc_engine_err_bad_params(err, CMD, "the sampling window is empty");
    }
    size_t w = 0, h = 0;
    rc = pc_engine_fill_window_size(window, &w, &h, err);
    if (rc != PC_OK) {
        return rc;
    }

    const char *output = str_or(p, "output", "current");
    int out_mode = strcmp(output, "new") == 0 ? 1 : strcmp(output, "duplicate") == 0 ? 2
                   : strcmp(output, "current") == 0                                  ? 0
                                                                                     : -1;
    if (out_mode < 0) {
        return pc_engine_err_bad_params(err, CMD, "`output` must be current|new|duplicate");
    }

    /* Sample All Layers (default: on only when the active layer has nothing in the window). */
    bool sample_all;
    yyjson_val *jsa = p != NULL ? pc_json_get(p, "sampleAllLayers") : NULL;
    if (jsa != NULL) {
        sample_all = false;
        (void)pc_json_as_bool(jsa, &sample_all);
    } else {
        sample_all = pc_rect_is_empty(pc_rect_intersect(pc_surface_content_bounds(lsurf), window));
    }

    size_t npx = w * h;
    size_t len = npx * n;
    float *img = NULL;
    rc = pc_engine_alloc_filled(len ? len : 1, sizeof(float), "content-aware fill", (void **)&img, err);
    if (rc != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (sample_all) {
        pc_compose_buffer buf;
        rc = pc_compose_render(doc, window, &buf, err);
        if (rc != PC_OK) {
            free(img);
            return rc;
        }
        for (size_t i = 0; i < buf.len && i < npx; i++) {
            (void)pc_from_rgba(fmt, buf.px[i], &img[i * n]);
        }
        pc_compose_buffer_fini(&buf);
    } else {
        float *region = NULL;
        size_t rl = 0;
        rc = pc_surface_read_region(lsurf, window, &region, &rl, err);
        if (rc != PC_OK) {
            free(img);
            return rc;
        }
        memcpy(img, region, (rl < len ? rl : len) * sizeof(float));
        free(region);
    }

    /* Hole (selection coverage > 0) + coverage floats. */
    bool *hole = calloc(npx ? npx : 1, sizeof *hole);
    bool *source = calloc(npx ? npx : 1, sizeof *source);
    float *cover = calloc(npx ? npx : 1, sizeof *cover);
    if (hole == NULL || source == NULL || cover == NULL) {
        free(img);
        free(hole);
        free(source);
        free(cover);
        return pc_fail(err, PC_ERR_NOMEM, "content-aware fill");
    }
    uint64_t n_hole = 0;
    for (size_t i = 0; i < npx; i++) {
        int32_t x = window.x0 + (int32_t)(i % w), y = window.y0 + (int32_t)(i / w);
        float cov = pc_surface_sample_channel(&doc->selection, x, y, 0);
        cover[i] = cov;
        if (cov > 0.0f) {
            hole[i] = true;
            n_hole++;
        }
    }
    /* Source mask. */
    bool auto_src = strcmp(sampling, "auto") == 0;
    if (strcmp(sampling, "custom") == 0 && have_rect) {
        for (size_t i = 0; i < npx; i++) {
            int32_t x = window.x0 + (int32_t)(i % w), y = window.y0 + (int32_t)(i / w);
            source[i] = pc_rect_contains(crect, x, y);
        }
    } else if (strcmp(sampling, "custom") == 0 && cmask != NULL) {
        for (size_t i = 0; i < npx; i++) {
            int32_t x = window.x0 + (int32_t)(i % w), y = window.y0 + (int32_t)(i / w);
            source[i] = pc_surface_sample_channel(cmask, x, y, 0) > 0.5f;
        }
    } else {
        for (size_t i = 0; i < npx; i++) {
            source[i] = true;
        }
    }
    if (auto_src) {
        bool *as = NULL;
        rc = pc_algo_ca_auto_sampling(w, h, n, img, len, hole, npx, NULL, &as, err);
        if (rc != PC_OK) {
            free(img);
            free(hole);
            free(source);
            free(cover);
            return rc;
        }
        free(source);
        source = as;
    }

    /* Options. */
    pc_algo_ca_fill_options opts = pc_algo_ca_fill_options_default();
    opts.color_adaptation = pc_algo_ca_color_level(str_or(p, "colorAdaptation", "default"));
    float rots[3];
    opts.rotation_count = pc_algo_ca_rotation_level(str_or(p, "rotationAdaptation", "none"), rots);
    opts.rotations = opts.rotation_count > 0 ? rots : NULL;
    opts.scale = bool_or(p, "scale", false);
    opts.mirror = bool_or(p, "mirror", false);
    uint64_t seed = 1;
    yyjson_val *jseed = p != NULL ? pc_json_get(p, "seed") : NULL;
    if (jseed != NULL) {
        (void)pc_json_as_u64(jseed, &seed);
    }
    opts.seed = seed;

    /* Keep a copy of the original to blend by coverage. */
    float *orig = NULL;
    if (pc_engine_alloc_filled(len ? len : 1, sizeof *orig, "content-aware fill", (void **)&orig, err) !=
        PC_OK) {
        free(img);
        free(hole);
        free(source);
        free(cover);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    memcpy(orig, img, len * sizeof *orig);
    rc = pc_algo_ca_fill_inplace(w, h, n, img, len, hole, source, npx, &opts, NULL, err);
    free(hole);
    free(source);
    if (rc != PC_OK) {
        free(img);
        free(orig);
        free(cover);
        return rc;
    }
    /* Blend the fill in by the coverage (holes only changed; elsewhere img == orig). */
    for (size_t i = 0; i < npx; i++) {
        float cov = cover[i];
        if (cov <= 0.0f) {
            continue;
        }
        for (size_t ch = 0; ch < n; ch++) {
            size_t j = i * n + ch;
            img[j] = orig[j] + (img[j] - orig[j]) * cov;
        }
    }
    free(orig);

    struct caf_ctx c;
    memset(&c, 0, sizeof c);
    c.id = id;
    c.output = out_mode;
    c.window = window;
    c.fmt = fmt;
    c.n = n;
    c.filled = img;
    c.cover = cover;
    c.n_hole = n_hole;
    const char *label = "Content-Aware Fill";
    rc = pc_engine_session_edit(s, label, caf_edit, &c, err);
    free(img);
    free(cover);
    if (rc != PC_OK) {
        return rc;
    }

    const pc_engine_doc_state *nd = pc_engine_session_active(s);
    pc_layer_id active = nd != NULL && nd->has_active_layer ? nd->active_layer : id;
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", active);
    yyjson_mut_obj_add_uint(rd, root, "filled", n_hole);
    yyjson_mut_val *win = yyjson_mut_arr(rd);
    yyjson_mut_arr_add_int(rd, win, window.x0);
    yyjson_mut_arr_add_int(rd, win, window.y0);
    yyjson_mut_arr_add_int(rd, win, pc_rect_width(window));
    yyjson_mut_arr_add_int(rd, win, pc_rect_height(window));
    yyjson_mut_obj_add_val(rd, root, "window", win);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

#define CAF_PARAMS                                                                                   \
    "{\"sampling\":\"auto|rectangular|custom\"=\"auto\",\"margin\":px? (rectangular),\"area\":[x,y,w,h]? " \
    "(custom),\"channel\":\"index|name\"? (custom),\"colorAdaptation\":\"none|default|high|veryHigh\"=" \
    "\"default\",\"rotationAdaptation\":\"none|low|medium|high|full\"=\"none\",\"scale\":bool=false,"  \
    "\"mirror\":bool=false,\"seed\":u64=1,\"sampleAllLayers\":bool?,\"output\":\"current|new|duplicate\"" \
    "=\"current\"} -> {layer,filled,window:[x,y,w,h]}"

static const pc_engine_command SPECS[] = {
    {CMD, "Content-Aware Fill…", MENU_EDIT, 1, NULL, CAF_PARAMS, caf_enabled, run_caf, true},
};

const pc_engine_command *pc_engine_content_fill_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
