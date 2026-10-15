/* Filter menu commands backed by photocraft-algo (port of crates/engine/src/filters.rs, E9).
 *
 * Each command acts on the active pixel layer, respects the selection and is undoable;
 * filter.lastFilter re-runs the most recent filter. The algorithm runs through src/algo
 * (pc_algo_apply_in), so results are bit-exact with the reference; this module only marshals the
 * dialog-unit JSON params into an algo filter (params_for) and commits the result.
 *
 * DEFERRED sub-paths (reported in REWRITE_PLAN.md §8), each returning a clear error rather than a
 * wrong result:
 *   - channel / mask / Quick Mask targets: need channel_cmds::channel_surface_for_filter as a
 *     MUTABLE primitive, which E7 did not export (it exposes only the const read_target_surface).
 *   - smart-object layers: record a smart filter via smart_cmds::add_smart_filter (E6, unported).
 *   - Color to Alpha's Background->layer promotion (extra_cmds) is only reached by an ext filter,
 *     which this chunk does not register.
 * The common case -- a raster layer, the Pixels target -- is fully supported, as is Last Filter,
 * parameter validation (the filter_validate dispatch hook, moved here from hooks.c) and the
 * lock-transparency alpha restore. Background jobs run inline (pc_engine_session_edit); the
 * reference's edit_job cancellation is a perf/UX feature left for Z1. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/filter.h"
#include "channel_cmds.h"
#include "engine_internal.h"
#include "extra_cmds.h"
#include "layer_cmds.h"

/* ---- param readers (filters.rs f / i / b / s / seed) -------------------------------------- */

static float fget(yyjson_val *p, const char *k, float d)
{
    yyjson_val *v = pc_json_get(p, k);
    double got;
    if (v != NULL && pc_json_as_f64(v, &got)) {
        return (float)got;
    }
    return d;
}

/* as_i64, else round(as_f64), clamped to i32 (Rust saturating clamp); default d. */
static int32_t iget(yyjson_val *p, const char *k, int32_t d)
{
    yyjson_val *v = pc_json_get(p, k);
    if (v == NULL) {
        return d;
    }
    double raw;
    long long got;
    if (yyjson_is_int(v)) {
        got = (long long)yyjson_get_sint(v);
    } else if (yyjson_is_uint(v)) {
        uint64_t u = yyjson_get_uint(v);
        got = u > (uint64_t)INT64_MAX ? INT64_MAX : (long long)u;
    } else if (pc_json_as_f64(v, &raw)) {
        double r = round(raw);
        if (!(r >= (double)INT64_MIN)) {
            got = INT64_MIN;
        } else if (r >= (double)INT64_MAX) {
            got = INT64_MAX;
        } else {
            got = (long long)r;
        }
    } else {
        return d;
    }
    if (got < INT32_MIN) {
        return INT32_MIN;
    }
    if (got > INT32_MAX) {
        return INT32_MAX;
    }
    return (int32_t)got;
}

static bool bget(yyjson_val *p, const char *k, bool d)
{
    yyjson_val *v = pc_json_get(p, k);
    bool got;
    if (v != NULL && pc_json_as_bool(v, &got)) {
        return got;
    }
    return d;
}

static const char *sget(yyjson_val *p, const char *k, const char *d)
{
    yyjson_val *v = pc_json_get(p, k);
    if (v != NULL && yyjson_is_str(v)) {
        const char *s = yyjson_get_str(v);
        if (s != NULL) {
            return s;
        }
    }
    return d;
}

static uint32_t seed_of(yyjson_val *p)
{
    yyjson_val *v = pc_json_get(p, "seed");
    if (v != NULL && yyjson_is_uint(v)) {
        uint64_t n = yyjson_get_uint(v);
        if (n <= UINT32_MAX) {
            return (uint32_t)n;
        }
    }
    int32_t i = iget(p, "seed", 0);
    return (uint32_t)i;
}

static float clampf(float v, float lo, float hi)
{
    /* Rust f32::clamp: NaN propagates; assumes lo<=hi. */
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* Color to Alpha's lenient channel/threshold mapping (color_to_alpha_cmds::unit): a finite value
 * clamped to 0..1, else the default (color_to_alpha_cmds validates strictly before this). */
static float c2a_unit(float v, float def)
{
    return isfinite(v) ? clampf(v, 0.0f, 1.0f) : def;
}

static float c2a_num(yyjson_val *p, const char *k, float def)
{
    yyjson_val *v = pc_json_get(p, k);
    double g;
    if (v != NULL && pc_json_as_f64(v, &g)) {
        return c2a_unit((float)g, def);
    }
    return def;
}

/* Command-param undefinedAreas -> algo serde name. */
static const char *undefined_str(yyjson_val *p)
{
    const char *s = sget(p, "undefinedAreas", "wrap");
    if (strcmp(s, "repeat") == 0) {
        return "repeat";
    }
    if (strcmp(s, "transparent") == 0) {
        return "transparent";
    }
    return "wrap";
}

static const char *preserve_str(yyjson_val *p)
{
    return strcmp(sget(p, "preserve", "squareness"), "roundness") == 0 ? "roundness" : "squareness";
}

/* ---- params_for: command id + JSON -> an algo filter --------------------------------------- *
 *
 * Builds {"filter":tag, fields...} with the reference's clamps and defaults using the ALGO serde
 * field names, then parses it with pc_algo_filter_from_json. *found is set false for ids this
 * module does not handle. */

static void add_f(yyjson_mut_doc *d, yyjson_mut_val *o, const char *k, float v)
{
    yyjson_mut_obj_add_real(d, o, k, (double)v);
}
static void add_s(yyjson_mut_doc *d, yyjson_mut_val *o, const char *k, const char *v)
{
    yyjson_mut_obj_add_str(d, o, k, v);
}
/* An i32 param: non-negative values go in as yyjson UINT (the algo reads a SINT 0 as the `-0.0`
 * marker, so add_int(0) would be rejected by the i32 reader; text "0" parses as UINT too). */
static void add_i32(yyjson_mut_doc *d, yyjson_mut_val *o, const char *k, int32_t v)
{
    if (v >= 0) {
        yyjson_mut_obj_add_uint(d, o, k, (uint64_t)v);
    } else {
        yyjson_mut_obj_add_int(d, o, k, (int64_t)v);
    }
}

/* The one-step preset filters: a fixed algo filter and their own history-step name. */
static const char *preset_label(const char *id)
{
    if (strcmp(id, "filter.blur.blur") == 0) {
        return "Blur";
    }
    if (strcmp(id, "filter.blur.blurMore") == 0) {
        return "Blur More";
    }
    if (strcmp(id, "filter.sharpen.sharpen") == 0) {
        return "Sharpen";
    }
    if (strcmp(id, "filter.sharpen.sharpenMore") == 0) {
        return "Sharpen More";
    }
    if (strcmp(id, "filter.sharpen.sharpenEdges") == 0) {
        return "Sharpen Edges";
    }
    if (strcmp(id, "filter.noise.despeckle") == 0) {
        return "Despeckle";
    }
    return NULL;
}

static pc_status params_for(const char *id, yyjson_val *p, pc_algo_filter *out, bool *found,
                            pc_error *err)
{
    *found = true;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "filter params");
    }
    yyjson_mut_val *o = yyjson_mut_obj(d);
    bool ok = true;

    if (strcmp(id, "filter.blur.blur") == 0) {
        add_s(d, o, "filter", "gaussianBlur");
        add_f(d, o, "radius", 0.6f);
    } else if (strcmp(id, "filter.blur.blurMore") == 0) {
        add_s(d, o, "filter", "gaussianBlur");
        add_f(d, o, "radius", 1.4f);
    } else if (strcmp(id, "filter.sharpen.sharpen") == 0) {
        add_s(d, o, "filter", "unsharpMask");
        add_f(d, o, "amount", 60.0f);
        add_f(d, o, "radius", 0.5f);
        add_f(d, o, "threshold", 0.0f);
    } else if (strcmp(id, "filter.sharpen.sharpenMore") == 0) {
        add_s(d, o, "filter", "unsharpMask");
        add_f(d, o, "amount", 150.0f);
        add_f(d, o, "radius", 0.6f);
        add_f(d, o, "threshold", 0.0f);
    } else if (strcmp(id, "filter.sharpen.sharpenEdges") == 0) {
        add_s(d, o, "filter", "unsharpMask");
        add_f(d, o, "amount", 100.0f);
        add_f(d, o, "radius", 0.8f);
        add_f(d, o, "threshold", 6.0f);
    } else if (strcmp(id, "filter.noise.despeckle") == 0) {
        add_s(d, o, "filter", "surfaceBlur");
        add_f(d, o, "radius", 2.0f);
        add_f(d, o, "threshold", 12.0f);
    } else if (strcmp(id, "filter.blur.gaussianBlur") == 0) {
        add_s(d, o, "filter", "gaussianBlur");
        add_f(d, o, "radius", clampf(fget(p, "radius", 1.0f), 0.1f, 1000.0f));
    } else if (strcmp(id, "filter.blur.boxBlur") == 0) {
        add_s(d, o, "filter", "boxBlur");
        add_f(d, o, "radius", clampf(fget(p, "radius", 1.0f), 1.0f, 2000.0f));
    } else if (strcmp(id, "filter.blur.motionBlur") == 0) {
        add_s(d, o, "filter", "motionBlur");
        add_f(d, o, "angle", fget(p, "angle", 0.0f));
        add_f(d, o, "distance", clampf(fget(p, "distance", 10.0f), 1.0f, 2000.0f));
    } else if (strcmp(id, "filter.blur.radialBlur") == 0) {
        add_s(d, o, "filter", "radialBlur");
        add_f(d, o, "amount", clampf(fget(p, "amount", 10.0f), 1.0f, 100.0f));
        add_s(d, o, "method", strcmp(sget(p, "method", "spin"), "zoom") == 0 ? "zoom" : "spin");
        const char *q = sget(p, "quality", "good");
        add_s(d, o, "quality",
              strcmp(q, "draft") == 0 ? "draft" : (strcmp(q, "best") == 0 ? "best" : "good"));
        add_f(d, o, "centerX", fget(p, "centerX", 0.5f));
        add_f(d, o, "centerY", fget(p, "centerY", 0.5f));
    } else if (strcmp(id, "filter.blur.surfaceBlur") == 0) {
        add_s(d, o, "filter", "surfaceBlur");
        add_f(d, o, "radius", clampf(fget(p, "radius", 5.0f), 1.0f, 100.0f));
        add_f(d, o, "threshold", clampf(fget(p, "threshold", 15.0f), 2.0f, 255.0f));
    } else if (strcmp(id, "filter.sharpen.unsharpMask") == 0) {
        add_s(d, o, "filter", "unsharpMask");
        add_f(d, o, "amount", clampf(fget(p, "amount", 50.0f), 1.0f, 500.0f));
        add_f(d, o, "radius", clampf(fget(p, "radius", 1.0f), 0.1f, 1000.0f));
        add_f(d, o, "threshold", clampf(fget(p, "threshold", 0.0f), 0.0f, 255.0f));
    } else if (strcmp(id, "filter.sharpen.smartSharpen") == 0) {
        add_s(d, o, "filter", "smartSharpen");
        add_f(d, o, "amount", clampf(fget(p, "amount", 100.0f), 1.0f, 500.0f));
        add_f(d, o, "radius", clampf(fget(p, "radius", 1.0f), 0.1f, 64.0f));
        add_f(d, o, "reduceNoise", clampf(fget(p, "reduceNoise", 10.0f), 0.0f, 100.0f));
    } else if (strcmp(id, "filter.other.highPass") == 0) {
        add_s(d, o, "filter", "highPass");
        add_f(d, o, "radius", clampf(fget(p, "radius", 10.0f), 0.1f, 1000.0f));
    } else if (strcmp(id, "filter.noise.addNoise") == 0) {
        add_s(d, o, "filter", "addNoise");
        add_f(d, o, "amount", clampf(fget(p, "amount", 12.5f), 0.1f, 400.0f));
        add_s(d, o, "distribution",
              strcmp(sget(p, "distribution", "uniform"), "gaussian") == 0 ? "gaussian" : "uniform");
        yyjson_mut_obj_add_bool(d, o, "monochromatic", bget(p, "monochromatic", false));
        yyjson_mut_obj_add_uint(d, o, "seed", seed_of(p));
    } else if (strcmp(id, "filter.noise.median") == 0) {
        add_s(d, o, "filter", "median");
        add_f(d, o, "radius", clampf(fget(p, "radius", 1.0f), 1.0f, 500.0f));
    } else if (strcmp(id, "filter.noise.dustAndScratches") == 0) {
        add_s(d, o, "filter", "dustAndScratches");
        add_f(d, o, "radius", clampf(fget(p, "radius", 1.0f), 1.0f, 500.0f));
        add_f(d, o, "threshold", clampf(fget(p, "threshold", 0.0f), 0.0f, 255.0f));
    } else if (strcmp(id, "filter.other.minimum") == 0) {
        add_s(d, o, "filter", "minimum");
        add_f(d, o, "radius", clampf(fget(p, "radius", 1.0f), 0.2f, 500.0f));
        add_s(d, o, "preserve", preserve_str(p));
    } else if (strcmp(id, "filter.other.maximum") == 0) {
        add_s(d, o, "filter", "maximum");
        add_f(d, o, "radius", clampf(fget(p, "radius", 1.0f), 0.2f, 500.0f));
        add_s(d, o, "preserve", preserve_str(p));
    } else if (strcmp(id, "filter.other.offset") == 0) {
        add_s(d, o, "filter", "offset");
        add_i32(d, o, "horizontal", iget(p, "horizontal", 0));
        add_i32(d, o, "vertical", iget(p, "vertical", 0));
        add_s(d, o, "undefined", undefined_str(p));
    } else if (strcmp(id, "filter.other.colorToAlpha") == 0) {
        add_s(d, o, "filter", "colorToAlpha");
        float col[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        pc_engine_param_color(p, "color", col, col);
        yyjson_mut_val *carr = yyjson_mut_arr(d);
        if (carr != NULL) {
            yyjson_mut_arr_add_real(d, carr, (double)c2a_unit(col[0], 1.0f));
            yyjson_mut_arr_add_real(d, carr, (double)c2a_unit(col[1], 1.0f));
            yyjson_mut_arr_add_real(d, carr, (double)c2a_unit(col[2], 1.0f));
            yyjson_mut_arr_add_real(d, carr, 1.0);
            yyjson_mut_obj_add_val(d, o, "color", carr);
        } else {
            ok = false;
        }
        add_f(d, o, "transparencyThreshold", c2a_num(p, "transparencyThreshold", 0.0f));
        add_f(d, o, "opacityThreshold", c2a_num(p, "opacityThreshold", 1.0f));
    } else if (strcmp(id, "filter.pixelate.mosaic") == 0) {
        add_s(d, o, "filter", "mosaic");
        add_f(d, o, "cellSize", clampf(fget(p, "cellSize", 10.0f), 2.0f, 200.0f));
    } else if (strcmp(id, "filter.stylize.emboss") == 0) {
        add_s(d, o, "filter", "emboss");
        add_f(d, o, "angle", fget(p, "angle", 135.0f));
        add_f(d, o, "height", clampf(fget(p, "height", 3.0f), 1.0f, 100.0f));
        add_f(d, o, "amount", clampf(fget(p, "amount", 100.0f), 1.0f, 500.0f));
    } else if (strcmp(id, "filter.stylize.findEdges") == 0) {
        add_s(d, o, "filter", "findEdges");
    } else if (strcmp(id, "filter.stylize.solarize") == 0) {
        add_s(d, o, "filter", "solarize");
    } else if (strcmp(id, "filter.distort.twirl") == 0) {
        add_s(d, o, "filter", "twirl");
        add_f(d, o, "angle", clampf(fget(p, "angle", 50.0f), -999.0f, 999.0f));
    } else if (strcmp(id, "filter.distort.pinch") == 0) {
        add_s(d, o, "filter", "pinch");
        add_f(d, o, "amount", clampf(fget(p, "amount", 50.0f), -100.0f, 100.0f));
    } else if (strcmp(id, "filter.distort.spherize") == 0) {
        add_s(d, o, "filter", "spherize");
        add_f(d, o, "amount", clampf(fget(p, "amount", 100.0f), -100.0f, 100.0f));
        const char *mode = sget(p, "mode", "normal");
        add_s(d, o, "mode",
              strcmp(mode, "horizontalOnly") == 0 ? "horizontalOnly"
              : (strcmp(mode, "verticalOnly") == 0 ? "verticalOnly" : "normal"));
    } else if (strcmp(id, "filter.distort.wave") == 0) {
        add_s(d, o, "filter", "wave");
        int32_t gen = iget(p, "generators", 5);
        if (gen < 1) {
            gen = 1;
        }
        if (gen > 999) {
            gen = 999;
        }
        yyjson_mut_obj_add_uint(d, o, "generators", (uint64_t)(uint32_t)gen);
        add_f(d, o, "wavelengthMin", clampf(fget(p, "wavelengthMin", 10.0f), 1.0f, 998.0f));
        add_f(d, o, "wavelengthMax", clampf(fget(p, "wavelengthMax", 120.0f), 2.0f, 999.0f));
        add_f(d, o, "amplitudeMin", clampf(fget(p, "amplitudeMin", 5.0f), 1.0f, 998.0f));
        add_f(d, o, "amplitudeMax", clampf(fget(p, "amplitudeMax", 35.0f), 1.0f, 999.0f));
        const char *t = sget(p, "type", "sine");
        add_s(d, o, "waveType",
              strcmp(t, "triangle") == 0 ? "triangle" : (strcmp(t, "square") == 0 ? "square" : "sine"));
        add_s(d, o, "undefined", undefined_str(p));
        yyjson_mut_obj_add_uint(d, o, "seed", seed_of(p));
    } else if (strcmp(id, "filter.distort.ripple") == 0) {
        add_s(d, o, "filter", "ripple");
        add_f(d, o, "amount", clampf(fget(p, "amount", 100.0f), -999.0f, 999.0f));
        const char *sz = sget(p, "size", "medium");
        add_s(d, o, "size",
              strcmp(sz, "small") == 0 ? "small" : (strcmp(sz, "large") == 0 ? "large" : "medium"));
    } else if (strcmp(id, "filter.distort.polarCoordinates") == 0) {
        add_s(d, o, "filter", "polarCoordinates");
        add_s(d, o, "mode",
              strcmp(sget(p, "mode", "rectangularToPolar"), "polarToRectangular") == 0
                  ? "polarToRectangular"
                  : "rectangularToPolar");
    } else {
        /* Not a core filter: try the Filter Gallery (filter.filterGallery / filter.gallery.*),
         * mirroring the reference's `_ => filters_ext::params_for(id, p)` fallthrough. gallery
         * params_for leaves *found false for ids it does not own. */
        yyjson_mut_doc_free(d);
        return pc_engine_gallery_params_for(id, p, out, found, err);
    }

    pc_status rc = PC_OK;
    if (ok) {
        yyjson_mut_doc_set_root(d, o);
        yyjson_doc *imm = yyjson_mut_doc_imut_copy(d, NULL);
        if (imm == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "filter params");
        } else {
            rc = pc_algo_filter_from_json(yyjson_doc_get_root(imm), out, err);
            yyjson_doc_free(imm);
        }
    }
    yyjson_mut_doc_free(d);
    return rc;
}

/* Is id a filter this module runs (params_for succeeds)? Used by last_filter and lastFilter. */
static bool is_known_filter(const char *id)
{
    if (strncmp(id, "filter.", 7) != 0) {
        return false;
    }
    pc_algo_filter f;
    bool found = false;
    pc_error e;
    pc_error_clear(&e);
    if (params_for(id, NULL, &f, &found, &e) == PC_OK && found) {
        pc_algo_filter_fini(&f);
        return true;
    }
    return found; /* a known id whose build failed is still "known" */
}

/* ---- lock-transparency alpha restore (keep_alpha) ----------------------------------------- */

static void keep_alpha(const pc_surface *before, pc_surface *after, pc_rect area)
{
    pc_pixel_format bf = pc_surface_format(before);
    pc_pixel_format af = pc_surface_format(after);
    if (!af.alpha || bf.mode != af.mode || bf.sample != af.sample || bf.alpha != af.alpha) {
        return;
    }
    if (pc_rect_is_empty(area)) {
        return;
    }
    size_t n = pc_surface_channels(after);
    float *ob = NULL, *nw = NULL;
    size_t ol = 0, nl = 0;
    if (pc_surface_read_region(before, area, &ob, &ol, NULL) != PC_OK) {
        return;
    }
    if (pc_surface_read_region(after, area, &nw, &nl, NULL) != PC_OK) {
        free(ob);
        return;
    }
    if (ol == nl) {
        for (size_t i = 0; i + n <= nl; i += n) {
            nw[i + n - 1] = ob[i + n - 1];
        }
        (void)pc_surface_write_region(after, area, nw, nl, NULL);
    }
    free(ob);
    free(nw);
    pc_surface_prune(after);
}

/* ---- running ----------------------------------------------------------------------------- */

static pc_status filterable(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_edits_channel(s)) {
        return PC_OK;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (!d->has_active_layer) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    if (l == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    if (l->content.kind == PC_LAYER_RASTER) {
        return PC_OK;
    }
    if (l->content.kind == PC_LAYER_SMART && l->content.u.smart.has_cache) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "filters need a pixel layer (active layer is %s %s layer)",
                   pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
}

struct filter_ctx {
    const pc_algo_filter *filter;
    bool has_layer;
    pc_layer_id layer;
};

static pc_status edit_filter(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                             pc_error *err)
{
    struct filter_ctx *c = vctx;
    pc_layer_id id;
    if (c->has_layer) {
        id = c->layer;
    } else if (*has_active) {
        id = *active;
    } else {
        return pc_engine_err_other(err, "no active layer");
    }
    /* A filter whose job is transparency (Color to Alpha) turns the Background into a normal
     * layer first, as the erasers do: the Background can't hold transparency (filters::run_filter). */
    bool mt = pc_algo_filter_makes_transparency(c->filter);
    if (mt) {
        pc_engine_background_to_layer_for_mask(doc, id);
    }
    pc_locks locks = pc_document_effective_locks(doc, id);
    pc_layer *l = pc_document_layer_mut(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (locks.pixels || locks.all) {
        return pc_engine_err_other(
            err, "Could not complete your request because the layer \"%s\" is locked", pc_str_c(&l->name));
    }
    if (l->content.kind == PC_LAYER_SMART) {
        return pc_engine_err_other(
            err, "smart-object filters are not yet supported in this build (E6 smart filters)");
    }
    if (l->content.kind != PC_LAYER_RASTER) {
        return pc_engine_err_other(err, "not a pixel layer");
    }
    pc_surface *surf = &l->content.u.raster;
    /* Putting the old alpha back (keep_alpha below) would leave unmixed colours fully opaque, so a
     * transparency-making filter refuses a transparency-locked raster layer (filters::run_filter). */
    if (locks.transparency && mt) {
        return pc_engine_err_other(
            err, "Could not complete your request because the transparency of layer \"%s\" is locked",
            pc_str_c(&l->name));
    }

    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    bool has_sel = sel != NULL;
    pc_rect sel_bounds = has_sel ? pc_surface_content_bounds(sel) : pc_rect_make(0, 0, 0, 0);
    pc_rect doc_bounds = pc_document_bounds(doc);
    pc_rect bounds = (has_sel && !pc_rect_is_empty(sel_bounds)) ? sel_bounds : doc_bounds;

    bool restore = locks.transparency && !mt;
    pc_surface before;
    bool have_before = false;
    if (restore) {
        if (pc_surface_clone(surf, &before, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        have_before = true;
    }
    pc_rect content = pc_surface_content_bounds(surf);
    pc_rect area = pc_algo_output_area(c->filter, content, bounds, has_sel ? &sel_bounds : NULL);
    pc_rect extent = pc_rect_union(doc_bounds, content);
    pc_surface result;
    pc_status rc = pc_algo_apply_in(surf, c->filter, area, bounds, sel, extent, &result, err);
    if (rc != PC_OK) {
        if (have_before) {
            pc_surface_fini(&before);
        }
        return rc;
    }
    pc_surface_fini(surf);
    *surf = result;
    if (have_before) {
        keep_alpha(&before, surf, area);
        pc_surface_fini(&before);
    }
    return PC_OK;
}

/* Shared runner for a filter command id with JSON params p. */
static pc_status run_filter(pc_engine_session *s, const char *id, yyjson_val *p, pc_json_value *out,
                            pc_error *err)
{
    pc_status rc = pc_engine_hook_filter_validate(id, p, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* Deferred: a channel / mask / Quick Mask target (needs a mutable channel surface, E7). */
    if (pc_engine_is_channel_target(p) ||
        (sget(p, "target", NULL) != NULL && strcmp(sget(p, "target", ""), "pixels") != 0)) {
        return pc_engine_err_other(
            err, "filtering a channel or mask is not yet supported in this build (E7 channel surface)");
    }

    bool found = false;
    pc_algo_filter filter;
    rc = params_for(id, p, &filter, &found, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!found) {
        return pc_engine_err_other(err, "unknown filter %s", id);
    }

    struct filter_ctx ctx = {&filter, false, 0};
    yyjson_val *lv = pc_json_get(p, "layer");
    if (lv != NULL && yyjson_is_uint(lv)) {
        ctx.layer = (pc_layer_id)yyjson_get_uint(lv);
        ctx.has_layer = true;
    } else {
        const pc_engine_doc_state *d = pc_engine_session_active(s);
        if (d == NULL || !d->has_active_layer) {
            pc_algo_filter_fini(&filter);
            return pc_engine_err_other(err, "no active layer");
        }
        ctx.layer = d->active_layer;
        ctx.has_layer = true;
    }

    const char *preset = preset_label(id);
    const char *label = preset != NULL ? preset : pc_algo_filter_label(&filter);

    rc = pc_engine_session_edit(s, label, edit_filter, &ctx, err);
    if (rc != PC_OK) {
        pc_algo_filter_fini(&filter);
        return rc;
    }

    /* {"layer": id, "filter": <serialized filter>} */
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        pc_algo_filter_fini(&filter);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", ctx.layer);
    yyjson_mut_val *fj = pc_algo_filter_to_json(rd, &filter);
    yyjson_mut_obj_add_val(rd, root, "filter", fj != NULL ? fj : yyjson_mut_null(rd));
    yyjson_mut_doc_set_root(rd, root);
    pc_algo_filter_fini(&filter);
    return PC_OK;
}

/* ---- the exported apply primitive (filters::apply_filter_to_surface) ---------------------- *
 *
 * The pixel function behind every filter.* command, exposed for smart filters (E6), smart
 * adjustments (E8) and the Filter Gallery. Smart-filter subtypes (Liquify / Lens / recorded
 * adjustments / plug-ins) dispatch through the subtype hook first; the rest build an algo filter
 * with the shared params_for mapping and run it on a copy of the surface. *applied is false for
 * ids that are not pixel filters (Rust None), *out is then untouched. Never crashes. */
pc_status pc_engine_apply_filter_to_surface(const char *id, yyjson_val *params, const pc_surface *surf,
                                            pc_rect bounds, const pc_surface *selection, pc_rect canvas,
                                            pc_surface *out, bool *applied, pc_error *err)
{
    *applied = false;
    if (id == NULL || surf == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "apply_filter_to_surface: null argument");
    }
    /* Liquify / Puppet / Perspective Warp, Lens / Camera Raw, image.adjustments.* and plug-in
     * smart filters are dispatched by the owning chunks through the subtype hook. */
    pc_status rc = pc_engine_hook_filter_subtype(id, params, surf, bounds, selection, canvas, out, applied, err);
    if (rc != PC_OK || *applied) {
        return rc;
    }
    /* The core filter table (and, when registered, the gallery / ext filters chained by
     * params_for). An unknown id leaves the pixels alone, like the reference's `None`. */
    bool found = false;
    pc_algo_filter f;
    rc = params_for(id, params, &f, &found, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!found) {
        return PC_OK; /* not a pixel filter: *applied stays false */
    }
    /* Displacement-map resolution (filters_ext::resolve_map_path, file I/O) is deferred to E3; the
     * core filters do not read a map. */
    pc_rect content = pc_surface_content_bounds(surf);
    pc_rect sel_bounds;
    const pc_rect *selp = NULL;
    if (selection != NULL) {
        sel_bounds = pc_surface_content_bounds(selection);
        selp = &sel_bounds;
    }
    pc_rect area = pc_algo_output_area(&f, content, bounds, selp);
    pc_rect extent = pc_rect_union(canvas, content);
    rc = pc_algo_apply_in(surf, &f, area, bounds, selection, extent, out, err);
    pc_algo_filter_fini(&f);
    if (rc == PC_OK) {
        *applied = true;
    }
    return rc;
}

/* Exposed for gallery_cmds.c (which routes filter.filterGallery / filter.gallery.* through the
 * shared filter runner, as the reference's gallery commands call crate::filters::run_filter). */
pc_status pc_engine_run_filter(pc_engine_session *s, const char *id, yyjson_val *p, pc_json_value *out,
                               pc_error *err)
{
    return run_filter(s, id, p, out, err);
}

/* Exposed for gallery_cmds.c as the gallery commands' `enabled` predicate (filters::has_filterable_layer). */
pc_status pc_engine_filterable(const pc_engine_session *s, pc_error *reason)
{
    return filterable(s, reason);
}

/* run() thunks: the id is recovered from the command spec via a small table lookup. */
#define FILTER_RUN(fn, cid)                                                                        \
    static pc_status fn(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)     \
    {                                                                                              \
        return run_filter(s, cid, p, out, err);                                                    \
    }

FILTER_RUN(run_gaussian, "filter.blur.gaussianBlur")
FILTER_RUN(run_blur, "filter.blur.blur")
FILTER_RUN(run_blur_more, "filter.blur.blurMore")
FILTER_RUN(run_sharpen, "filter.sharpen.sharpen")
FILTER_RUN(run_sharpen_more, "filter.sharpen.sharpenMore")
FILTER_RUN(run_sharpen_edges, "filter.sharpen.sharpenEdges")
FILTER_RUN(run_despeckle, "filter.noise.despeckle")
FILTER_RUN(run_box, "filter.blur.boxBlur")
FILTER_RUN(run_motion, "filter.blur.motionBlur")
FILTER_RUN(run_radial, "filter.blur.radialBlur")
FILTER_RUN(run_surface, "filter.blur.surfaceBlur")
FILTER_RUN(run_unsharp, "filter.sharpen.unsharpMask")
FILTER_RUN(run_smart_sharpen, "filter.sharpen.smartSharpen")
FILTER_RUN(run_add_noise, "filter.noise.addNoise")
FILTER_RUN(run_median, "filter.noise.median")
FILTER_RUN(run_dust, "filter.noise.dustAndScratches")
FILTER_RUN(run_mosaic, "filter.pixelate.mosaic")
FILTER_RUN(run_emboss, "filter.stylize.emboss")
FILTER_RUN(run_find_edges, "filter.stylize.findEdges")
FILTER_RUN(run_solarize, "filter.stylize.solarize")
FILTER_RUN(run_twirl, "filter.distort.twirl")
FILTER_RUN(run_pinch, "filter.distort.pinch")
FILTER_RUN(run_spherize, "filter.distort.spherize")
FILTER_RUN(run_wave, "filter.distort.wave")
FILTER_RUN(run_ripple, "filter.distort.ripple")
FILTER_RUN(run_polar, "filter.distort.polarCoordinates")
FILTER_RUN(run_high_pass, "filter.other.highPass")
FILTER_RUN(run_minimum, "filter.other.minimum")
FILTER_RUN(run_maximum, "filter.other.maximum")
FILTER_RUN(run_offset, "filter.other.offset")

/* ---- Last Filter ------------------------------------------------------------------------- */

/* The most recent filter.* command in the journal that this module runs; *id borrows the journal
 * entry, *params_json is an owned compact copy with "layer" removed (free()), or false if none. */
static bool last_filter(const pc_engine_session *s, const char **id, char **params_json)
{
    size_t n = pc_engine_session_journal_len(s);
    for (size_t k = n; k > 0; k--) {
        const char *jid = NULL;
        const yyjson_mut_val *jp = NULL;
        if (!pc_engine_session_journal_at(s, k - 1, &jid, &jp)) {
            continue;
        }
        if (jid == NULL || !is_known_filter(jid)) {
            continue;
        }
        /* Copy params into a fresh doc, drop "layer". */
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        if (d == NULL) {
            return false;
        }
        yyjson_mut_val *src = NULL;
        memcpy(&src, &jp, sizeof src); /* drop const without a cast-qual warning */
        yyjson_mut_val *copy = src != NULL ? yyjson_mut_val_mut_copy(d, src) : yyjson_mut_obj(d);
        if (copy != NULL && yyjson_mut_is_obj(copy)) {
            yyjson_mut_obj_remove_key(copy, "layer");
        } else {
            copy = yyjson_mut_obj(d);
        }
        yyjson_mut_doc_set_root(d, copy);
        char *txt = yyjson_mut_write(d, 0, NULL);
        yyjson_mut_doc_free(d);
        if (txt == NULL) {
            return false;
        }
        *id = jid;
        *params_json = txt;
        return true;
    }
    return false;
}

static pc_status last_filter_enabled(const pc_engine_session *s, pc_error *reason)
{
    pc_status rc = filterable(s, reason);
    if (rc != PC_OK) {
        return rc;
    }
    const char *id = NULL;
    char *pj = NULL;
    if (!last_filter(s, &id, &pj)) {
        return pc_fail(reason, PC_ERR_INVALID, "no filter applied yet");
    }
    free(pj);
    return PC_OK;
}

static pc_status run_last_filter(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const char *id = NULL;
    char *pj = NULL;
    if (!last_filter(s, &id, &pj)) {
        return pc_engine_err_other(err, "no filter applied yet");
    }
    /* Re-run the underlying filter with its recorded params (minus layer), retargeted at the
     * active layer. Note: unlike the reference, which redirects at dispatch, this journals as
     * filter.lastFilter; action-replay parity is left for E12. */
    yyjson_doc *jd = NULL;
    pc_status rc = pc_json_parse(pj, strlen(pj), &jd, err);
    free(pj);
    if (rc != PC_OK) {
        return rc;
    }
    rc = run_filter(s, id, yyjson_doc_get_root(jd), out, err);
    yyjson_doc_free(jd);
    return rc;
}

/* ---- the filter_validate dispatch hook (moved here from hooks.c) -------------------------- */

#include "filters_validate.inc"

pc_status pc_engine_hook_filter_validate(const char *id, yyjson_val *params, pc_error *err)
{
    return filters_validate_params(id, params, err);
}

/* ---- specs ------------------------------------------------------------------------------- */

static const char *const MENU_BLUR[] = {"Filter", "Blur"};
static const char *const MENU_SHARPEN[] = {"Filter", "Sharpen"};
static const char *const MENU_NOISE[] = {"Filter", "Noise"};
static const char *const MENU_PIXELATE[] = {"Filter", "Pixelate"};
static const char *const MENU_STYLIZE[] = {"Filter", "Stylize"};
static const char *const MENU_DISTORT[] = {"Filter", "Distort"};
static const char *const MENU_OTHER[] = {"Filter", "Other"};
static const char *const MENU_FILTER[] = {"Filter"};

static const pc_engine_command SPECS[] = {
    {"filter.blur.gaussianBlur", "Gaussian Blur\xE2\x80\xA6", MENU_BLUR, 2, NULL,
     "{\"radius\":0.1..1000=1}", filterable, run_gaussian, true},
    {"filter.blur.blur", "Blur", MENU_BLUR, 2, NULL, "{}", filterable, run_blur, true},
    {"filter.blur.blurMore", "Blur More", MENU_BLUR, 2, NULL, "{}", filterable, run_blur_more, true},
    {"filter.sharpen.sharpen", "Sharpen", MENU_SHARPEN, 2, NULL, "{}", filterable, run_sharpen, true},
    {"filter.sharpen.sharpenMore", "Sharpen More", MENU_SHARPEN, 2, NULL, "{}", filterable,
     run_sharpen_more, true},
    {"filter.sharpen.sharpenEdges", "Sharpen Edges", MENU_SHARPEN, 2, NULL, "{}", filterable,
     run_sharpen_edges, true},
    {"filter.noise.despeckle", "Despeckle", MENU_NOISE, 2, NULL, "{}", filterable, run_despeckle, true},
    {"filter.blur.boxBlur", "Box Blur\xE2\x80\xA6", MENU_BLUR, 2, NULL, "{\"radius\":1..2000=1}",
     filterable, run_box, true},
    {"filter.blur.motionBlur", "Motion Blur\xE2\x80\xA6", MENU_BLUR, 2, NULL,
     "{\"angle\":-360..360=0,\"distance\":1..2000=10}", filterable, run_motion, true},
    {"filter.blur.radialBlur", "Radial Blur\xE2\x80\xA6", MENU_BLUR, 2, NULL,
     "{\"amount\":1..100=10,\"method\":\"spin|zoom\",\"quality\":\"draft|good|best\"=\"good\","
     "\"centerX\":0..1=0.5,\"centerY\":0..1=0.5}",
     filterable, run_radial, true},
    {"filter.blur.surfaceBlur", "Surface Blur\xE2\x80\xA6", MENU_BLUR, 2, NULL,
     "{\"radius\":1..100=5,\"threshold\":2..255=15}", filterable, run_surface, true},
    {"filter.sharpen.unsharpMask", "Unsharp Mask\xE2\x80\xA6", MENU_SHARPEN, 2, NULL,
     "{\"amount\":1..500=50,\"radius\":0.1..1000=1,\"threshold\":0..255=0}", filterable, run_unsharp,
     true},
    {"filter.sharpen.smartSharpen", "Smart Sharpen\xE2\x80\xA6", MENU_SHARPEN, 2, NULL,
     "{\"amount\":1..500=100,\"radius\":0.1..64=1,\"reduceNoise\":0..100=10}", filterable,
     run_smart_sharpen, true},
    {"filter.noise.addNoise", "Add Noise\xE2\x80\xA6", MENU_NOISE, 2, NULL,
     "{\"amount\":0.1..400=12.5,\"distribution\":\"uniform|gaussian\",\"monochromatic\":bool,"
     "\"seed\":u32=0}",
     filterable, run_add_noise, true},
    {"filter.noise.median", "Median\xE2\x80\xA6", MENU_NOISE, 2, NULL, "{\"radius\":1..500=1}",
     filterable, run_median, true},
    {"filter.noise.dustAndScratches", "Dust & Scratches\xE2\x80\xA6", MENU_NOISE, 2, NULL,
     "{\"radius\":1..500=1,\"threshold\":0..255=0}", filterable, run_dust, true},
    {"filter.pixelate.mosaic", "Mosaic\xE2\x80\xA6", MENU_PIXELATE, 2, NULL, "{\"cellSize\":2..200=10}",
     filterable, run_mosaic, true},
    {"filter.stylize.emboss", "Emboss\xE2\x80\xA6", MENU_STYLIZE, 2, NULL,
     "{\"angle\":-180..180=135,\"height\":1..100=3,\"amount\":1..500=100}", filterable, run_emboss,
     true},
    {"filter.stylize.findEdges", "Find Edges", MENU_STYLIZE, 2, NULL, "{}", filterable, run_find_edges,
     true},
    {"filter.stylize.solarize", "Solarize", MENU_STYLIZE, 2, NULL, "{}", filterable, run_solarize, true},
    {"filter.distort.twirl", "Twirl\xE2\x80\xA6", MENU_DISTORT, 2, NULL, "{\"angle\":-999..999=50}",
     filterable, run_twirl, true},
    {"filter.distort.pinch", "Pinch\xE2\x80\xA6", MENU_DISTORT, 2, NULL, "{\"amount\":-100..100=50}",
     filterable, run_pinch, true},
    {"filter.distort.spherize", "Spherize\xE2\x80\xA6", MENU_DISTORT, 2, NULL,
     "{\"amount\":-100..100=100,\"mode\":\"normal|horizontalOnly|verticalOnly\"}", filterable,
     run_spherize, true},
    {"filter.distort.wave", "Wave\xE2\x80\xA6", MENU_DISTORT, 2, NULL,
     "{\"generators\":1..999=5,\"wavelengthMin\":1..998=10,\"wavelengthMax\":2..999=120,"
     "\"amplitudeMin\":1..998=5,\"amplitudeMax\":1..999=35,\"type\":\"sine|triangle|square\","
     "\"undefinedAreas\":\"wrap|repeat\",\"seed\":u32=0}",
     filterable, run_wave, true},
    {"filter.distort.ripple", "Ripple\xE2\x80\xA6", MENU_DISTORT, 2, NULL,
     "{\"amount\":-999..999=100,\"size\":\"small|medium|large\"}", filterable, run_ripple, true},
    {"filter.distort.polarCoordinates", "Polar Coordinates\xE2\x80\xA6", MENU_DISTORT, 2, NULL,
     "{\"mode\":\"rectangularToPolar|polarToRectangular\"}", filterable, run_polar, true},
    {"filter.other.highPass", "High Pass\xE2\x80\xA6", MENU_OTHER, 2, NULL, "{\"radius\":0.1..1000=10}",
     filterable, run_high_pass, true},
    {"filter.other.minimum", "Minimum\xE2\x80\xA6", MENU_OTHER, 2, NULL,
     "{\"radius\":0.2..500=1,\"preserve\":\"squareness|roundness\"}", filterable, run_minimum, true},
    {"filter.other.maximum", "Maximum\xE2\x80\xA6", MENU_OTHER, 2, NULL,
     "{\"radius\":0.2..500=1,\"preserve\":\"squareness|roundness\"}", filterable, run_maximum, true},
    {"filter.other.offset", "Offset\xE2\x80\xA6", MENU_OTHER, 2, NULL,
     "{\"horizontal\":px=0,\"vertical\":px=0,\"undefinedAreas\":\"wrap|repeat|transparent\"}",
     filterable, run_offset, true},
    {"filter.lastFilter", "Last Filter", MENU_FILTER, 1, "Cmd+Alt+F", "{}", last_filter_enabled,
     run_last_filter, true},
};

const pc_engine_command *pc_engine_filters_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
