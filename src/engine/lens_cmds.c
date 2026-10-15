/* Filter > Lens Correction, Filter > Adaptive Wide Angle and Filter > Camera Raw Filter (port of
 * crates/engine/src/lens_cmds.rs, E-tail ET5). The algorithms live in src/algo (lens / wideangle /
 * camera_raw), so results are bit-exact with the reference; this module marshals the dialog-unit
 * JSON params into those algo structs, resolves EXIF-driven values from the document metadata, and
 * commits the result through the shared engine edit path.
 *
 * Each filter acts on the active pixel layer (inside the selection) and is undoable. The
 * smart-filter RE-RENDER of a stored lens/wide/raw filter is wired into
 * pc_engine_hook_filter_subtype (plugin_cmds.c) via pc_engine_lens_apply_to_surface, so a smart
 * object that already carries one (e.g. from PSD import) renders correctly.
 *
 * DEFERRED (reported in REWRITE_PLAN.md §8):
 *   - Recording a lens/wide/raw filter onto a smart-object layer needs smart_cmds::add_smart_filter,
 *     which E6 did not export (the same primitive filters_cmds.c::edit_filter and plugin_cmds.c wait
 *     on). A smart-object target is reported unsupported, as those modules do. Owner: E6/E12.
 *   - File > Automate > Lens Correction (the batch twin) needs the file-batch pipeline
 *     (file_cmds::process_files / batch_inputs), which is unported (E12 deferred the other
 *     automate batch commands the same way). Owner: batch pipeline (automate). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/camera_raw.h"
#include "algo/exif.h"
#include "algo/lens.h"
#include "algo/transform.h"
#include "algo/wideangle.h"
#include "cms/builtin.h"
#include "cms/profile.h"

#include "engine_internal.h"
#include "extra_cmds.h"  /* pc_engine_background_to_layer_for_mask */
#include "layer_cmds.h"  /* pc_engine_layer_param */
#include "smart_cmds.h"  /* E-tail: pc_engine_add_smart_filter (record a lens/wide/raw smart filter) */

#define LENS_ID "filter.lensCorrection"
#define WIDE_ID "filter.adaptiveWideAngle"
#define RAW_ID "filter.cameraRaw"

/* Largest area Camera Raw develops in one pass (lens_cmds.rs MAX_RAW_PIXELS, #962): the pipeline
 * holds the whole region as RGBA f32 plus source and result planes, so a larger document is refused
 * with an error rather than attempted. */
#define MAX_RAW_PIXELS 400000000ULL

/* ---- param readers (lens_cmds.rs num / flag) --------------------------------------------- */

static double num(yyjson_val *p, const char *k, double d)
{
    yyjson_val *v = p != NULL ? pc_json_get(p, k) : NULL;
    double got;
    if (v != NULL && pc_json_as_f64(v, &got) && isfinite(got)) {
        return got;
    }
    return d;
}

static bool flag(yyjson_val *p, const char *k, bool d)
{
    yyjson_val *v = p != NULL ? pc_json_get(p, k) : NULL;
    bool got;
    if (v != NULL && pc_json_as_bool(v, &got)) {
        return got;
    }
    return d;
}

static const char *sget(yyjson_val *p, const char *k, const char *d)
{
    yyjson_val *v = p != NULL ? pc_json_get(p, k) : NULL;
    if (v != NULL) {
        const char *s = yyjson_get_str(v);
        if (s != NULL) {
            return s;
        }
    }
    return d;
}

static double clampd(double v, double lo, double hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* ---- Lens Correction params --------------------------------------------------------------- */

/* Resolves command JSON into a lens correction over `frame`; `info` (NULL = none) resolves
 * "profile":"auto" from EXIF. *focal_used receives the focal length a generated profile used (for
 * the result echo), or -1 when no profile. */
static pc_status lens_params(const char *cmd, yyjson_val *p, pc_rect frame,
                             const pc_algo_camera_info *info, pc_algo_lens_correction *lc,
                             double *focal_used, pc_error *err)
{
    pc_algo_lens_correction_default(lc);
    *focal_used = -1.0;
    lc->correct_distortion = flag(p, "correctDistortion", true);
    lc->correct_vignette = flag(p, "correctVignette", true);
    lc->correct_ca = flag(p, "correctCA", true);
    lc->distortion = clampd(num(p, "distortion", 0.0), -100.0, 100.0);
    lc->red_cyan = clampd(num(p, "redCyan", 0.0), -100.0, 100.0);
    lc->blue_yellow = clampd(num(p, "blueYellow", 0.0), -100.0, 100.0);
    lc->vignette_amount = clampd(num(p, "vignetteAmount", 0.0), -100.0, 100.0);
    lc->vignette_midpoint = clampd(num(p, "vignetteMidpoint", 50.0), 0.0, 100.0);
    lc->vertical = clampd(num(p, "vertical", 0.0), -100.0, 100.0);
    lc->horizontal = clampd(num(p, "horizontal", 0.0), -100.0, 100.0);
    lc->angle = num(p, "angle", 0.0);
    lc->scale = clampd(num(p, "scale", 100.0), 50.0, 150.0);

    const char *edge = sget(p, "edge", "transparency");
    if (strcmp(edge, "transparency") == 0) {
        lc->edge.kind = PC_ALGO_LENS_EDGE_TRANSPARENCY;
    } else if (strcmp(edge, "edgeExtension") == 0 || strcmp(edge, "extension") == 0) {
        lc->edge.kind = PC_ALGO_LENS_EDGE_EXTENSION;
    } else if (strcmp(edge, "black") == 0) {
        lc->edge.kind = PC_ALGO_LENS_EDGE_COLOR;
        lc->edge.color[0] = lc->edge.color[1] = lc->edge.color[2] = 0.0f;
        lc->edge.color[3] = 1.0f;
    } else if (strcmp(edge, "white") == 0) {
        lc->edge.kind = PC_ALGO_LENS_EDGE_COLOR;
        lc->edge.color[0] = lc->edge.color[1] = lc->edge.color[2] = 1.0f;
        lc->edge.color[3] = 1.0f;
    } else if (strcmp(edge, "background") == 0 || strcmp(edge, "color") == 0) {
        lc->edge.kind = PC_ALGO_LENS_EDGE_COLOR;
        float def[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        pc_engine_param_color(p, "edgeColor", def, lc->edge.color);
    } else {
        return pc_engine_err_bad_params(err, cmd,
                                        "unknown edge `%s` (transparency|edgeExtension|black|white|color)",
                                        edge);
    }

    double focal = num(p, "focalLength", 0.0);
    const char *profile = sget(p, "profile", "none");
    if (strcmp(profile, "none") == 0 || profile[0] == '\0') {
        lc->has_profile = false;
    } else if (strcmp(profile, "generic") == 0) {
        double f = focal > 0.0 ? focal : 50.0;
        pc_algo_lens_generic_profile(f, &lc->profile);
        lc->has_profile = true;
        *focal_used = f;
    } else if (strcmp(profile, "auto") == 0) {
        bool have = false;
        double f = 0.0;
        if (info != NULL && info->focal_length_35mm.set) {
            f = info->focal_length_35mm.v;
            have = true;
        } else if (info != NULL && info->focal_length.set) {
            f = info->focal_length.v;
            have = true;
        } else if (focal > 0.0) {
            f = focal;
            have = true;
        }
        if (have) {
            pc_algo_lens_generic_profile(f, &lc->profile);
            lc->has_profile = true;
            *focal_used = f;
        } else {
            lc->has_profile = false;
        }
    } else {
        return pc_engine_err_bad_params(err, cmd, "unknown profile `%s` (none|auto|generic)", profile);
    }

    yyjson_val *line = p != NULL ? pc_json_get(p, "straighten") : NULL;
    if (line != NULL && yyjson_is_arr(line)) {
        yyjson_val *av = yyjson_arr_get(line, 0);
        yyjson_val *bv = yyjson_arr_get(line, 1);
        double a[2], b[2];
        bool ok = av != NULL && bv != NULL && yyjson_is_arr(av) && yyjson_is_arr(bv);
        if (ok) {
            ok = pc_json_as_f64(yyjson_arr_get(av, 0), &a[0]) && pc_json_as_f64(yyjson_arr_get(av, 1), &a[1]) &&
                 pc_json_as_f64(yyjson_arr_get(bv, 0), &b[0]) && pc_json_as_f64(yyjson_arr_get(bv, 1), &b[1]);
        }
        if (!ok) {
            return pc_engine_err_bad_params(err, cmd, "`straighten` is [[x0,y0],[x1,y1]]");
        }
        lc->angle += pc_algo_lens_straighten_angle(a, b);
    } else if (line != NULL) {
        return pc_engine_err_bad_params(err, cmd, "`straighten` is [[x0,y0],[x1,y1]]");
    }

    if (flag(p, "autoScale", false)) {
        lc->scale = pc_algo_lens_auto_scale(lc, frame);
    }
    return PC_OK;
}

/* Writes the resolved profile / angle / scale back into the params (for a smart filter that
 * re-applies without the original EXIF / straighten / autoScale context): resolved_lens_params. */
static yyjson_mut_val *resolved_lens_params(yyjson_mut_doc *d, yyjson_val *p,
                                            const pc_algo_lens_correction *lc, double focal)
{
    yyjson_mut_val *q = pc_json_normalized_copy_val(d, p);
    if (q == NULL || !yyjson_mut_is_obj(q)) {
        q = yyjson_mut_obj(d);
    }
    if (q == NULL) {
        return NULL;
    }
    const char *drop[] = {"straighten", "autoScale", "layer", "angle", "scale", "profile", "focalLength"};
    for (size_t i = 0; i < sizeof drop / sizeof *drop; i++) {
        (void)yyjson_mut_obj_remove_key(q, drop[i]);
    }
    yyjson_mut_obj_add_real(d, q, "angle", lc->angle);
    yyjson_mut_obj_add_real(d, q, "scale", lc->scale);
    if (lc->has_profile) {
        yyjson_mut_obj_add_str(d, q, "profile", "generic");
        yyjson_mut_obj_add_real(d, q, "focalLength", focal > 0.0 ? focal : 50.0);
    } else {
        yyjson_mut_obj_add_str(d, q, "profile", "none");
    }
    return q;
}

/* ---- Adaptive Wide Angle params ----------------------------------------------------------- */

static pc_status wide_params(const char *cmd, yyjson_val *p, const pc_algo_camera_info *info,
                             pc_algo_wide_angle *wa, pc_error *err)
{
    pc_status rc = pc_algo_wide_angle_from_json(p, wa, err);
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t i = 0; i < wa->n_constraints; i++) {
        const pc_algo_wide_constraint *c = &wa->constraints[i];
        if (!isfinite(c->a[0]) || !isfinite(c->a[1]) || !isfinite(c->b[0]) || !isfinite(c->b[1])) {
            pc_algo_wide_angle_fini(wa);
            return pc_engine_err_bad_params(err, cmd, "constraint points must be finite");
        }
    }
    wa->scale = clampd(wa->scale, 50.0, 150.0);
    wa->crop_factor = clampd(wa->crop_factor, 0.1, 10.0);
    if (wa->focal_length <= 0.0 && info != NULL) {
        if (info->focal_length.set) {
            wa->focal_length = info->focal_length.v;
            if (info->focal_length_35mm.set && info->focal_length.v != 0.0) {
                wa->crop_factor = clampd(info->focal_length_35mm.v / info->focal_length.v, 0.1, 10.0);
            }
        } else if (info->focal_length_35mm.set) {
            wa->focal_length = info->focal_length_35mm.v;
            wa->crop_factor = 1.0;
        }
    }
    return PC_OK;
}

/* ---- Camera Raw params -------------------------------------------------------------------- */

static pc_status raw_params(const char *cmd, yyjson_val *p, pc_algo_camera_raw *cr, pc_error *err)
{
    pc_status rc = pc_algo_camera_raw_from_json(p, cr, err);
    if (rc != PC_OK) {
        (void)cmd;
        return rc;
    }
    /* The noise stages turn these into a guided-filter radius and a blur sigma, so an out-of-range
     * value means unbounded work; clamp to the documented 0..100 (#707). */
    cr->noise_luminance = (float)clampd(cr->noise_luminance, 0.0, 100.0);
    cr->noise_luminance_detail = (float)clampd(cr->noise_luminance_detail, 0.0, 100.0);
    cr->noise_color = (float)clampd(cr->noise_color, 0.0, 100.0);
    cr->noise_color_detail = (float)clampd(cr->noise_color_detail, 0.0, 100.0);
    return PC_OK;
}

/* The transfer curve Camera Raw decodes `doc`'s values with: ProPhoto's gamma 1.8, Adobe RGB's
 * 563/256, 1 for linear sRGB, else 0 for the sRGB curve (lens_cmds.rs encoding_gamma). */
static float encoding_gamma(const pc_document *doc)
{
    pc_profile prof;
    pc_error e;
    pc_error_clear(&e);
    if (pc_engine_document_profile(doc, &prof, &e) != PC_OK) {
        return 0.0f;
    }
    float g = 0.0f;
    const struct {
        pc_builtin b;
        float gamma;
    } table[] = {
        {PC_BUILTIN_PROPHOTO_COMPAT, 1.8f},
        {PC_BUILTIN_ADOBE_RGB_COMPAT, 563.0f / 256.0f},
        {PC_BUILTIN_LINEAR_SRGB, 1.0f},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        const pc_profile *bp = pc_builtin_profile(table[i].b);
        if (bp != NULL && pc_profile_same_colors(&prof, bp)) {
            g = table[i].gamma;
            break;
        }
    }
    pc_profile_fini(&prof);
    return g;
}

/* ---- Camera Raw on a surface -------------------------------------------------------------- */

/* Camera Raw over `area` (RGB / Gray via RGBA, at the surface's depth). An area above
 * MAX_RAW_PIXELS, an empty area or an identity filter returns a clone (the command rejects an
 * oversized area with an error first). On success *out is initialised (pc_surface_fini). */
static pc_status camera_raw_surface(const pc_surface *surf, pc_rect area, const pc_algo_camera_raw *p,
                                    pc_surface *out, pc_error *err)
{
    pc_rect content = pc_surface_content_bounds(surf);
    area = pc_rect_intersect(area, pc_rect_union(content, area));
    uint64_t npix = (uint64_t)pc_rect_width(area) * (uint64_t)pc_rect_height(area);
    if (pc_rect_is_empty(area) || pc_algo_camera_raw_is_identity(p) || npix > MAX_RAW_PIXELS) {
        return pc_surface_clone(surf, out, err);
    }
    size_t len = (size_t)npix;
    size_t w = (size_t)pc_rect_width(area);
    size_t h = (size_t)pc_rect_height(area);
    pc_pixel_format fmt = pc_surface_format(surf);
    size_t n = pc_surface_channels(surf);

    float (*px)[4] = calloc(len > 0 ? len : 1, sizeof *px);
    if (px == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "camera raw");
    }
    pc_status rc = pc_surface_read_rgba_into(surf, area, px, len, err);
    if (rc != PC_OK) {
        free(px);
        return rc;
    }
    rc = pc_algo_camera_raw_develop(px, len, w, h, p, fmt.sample == PC_SAMPLE_TYPE_F32, err);
    if (rc != PC_OK) {
        free(px);
        return rc;
    }
    float *orig = NULL;
    size_t ol = 0;
    rc = pc_surface_read_region(surf, area, &orig, &ol, err);
    if (rc != PC_OK) {
        free(px);
        return rc;
    }
    float *data = calloc(len * n > 0 ? len * n : 1, sizeof *data);
    if (data == NULL) {
        free(px);
        free(orig);
        return pc_fail(err, PC_ERR_NOMEM, "camera raw");
    }
    for (size_t i = 0; i < len; i++) {
        float tmp[PC_RASTER_MAX_CHANNELS] = {0};
        pc_from_rgba(fmt, px[i], tmp);
        for (size_t c = 0; c < n; c++) {
            data[i * n + c] = tmp[c];
        }
        if (fmt.alpha && (i * n + n - 1) < ol) {
            data[i * n + n - 1] = orig[i * n + n - 1]; /* alpha untouched */
        }
    }
    rc = pc_surface_clone(surf, out, err);
    if (rc == PC_OK) {
        rc = pc_surface_write_region(out, area, data, len * n, err);
        if (rc == PC_OK) {
            pc_surface_prune(out);
        } else {
            pc_surface_fini(out);
        }
    }
    free(px);
    free(orig);
    free(data);
    return rc;
}

/* ---- mix_by_selection --------------------------------------------------------------------- */

/* Mixes `nw` over `od` by the selection's coverage; *out is initialised on success. */
static pc_status mix_by_selection(const pc_surface *od, const pc_surface *nw, const pc_surface *sel,
                                  pc_surface *out, pc_error *err)
{
    pc_rect area = pc_rect_union(pc_surface_content_bounds(od), pc_surface_content_bounds(nw));
    pc_pixel_format fmt = pc_surface_format(nw);
    pc_status rc = pc_surface_clone(nw, out, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (pc_rect_is_empty(area)) {
        return PC_OK;
    }
    /* `od` is read in `nw`'s format so the channels line up. */
    pc_surface od_conv;
    bool have_conv = false;
    const pc_surface *oldp = od;
    pc_pixel_format of = pc_surface_format(od);
    if (of.mode != fmt.mode || of.sample != fmt.sample || of.alpha != fmt.alpha) {
        rc = pc_surface_convert(od, fmt, &od_conv, err);
        if (rc != PC_OK) {
            pc_surface_fini(out);
            return rc;
        }
        have_conv = true;
        oldp = &od_conv;
    }
    size_t n = pc_surface_channels(nw);
    float *o = NULL, *w = NULL;
    size_t ol = 0, wl = 0;
    rc = pc_surface_read_region(oldp, area, &o, &ol, err);
    if (rc == PC_OK) {
        rc = pc_surface_read_region(nw, area, &w, &wl, err);
    }
    if (rc != PC_OK || ol != wl) {
        free(o);
        free(w);
        if (have_conv) {
            pc_surface_fini(&od_conv);
        }
        if (rc == PC_OK) {
            return PC_OK; /* mismatched regions: keep the plain filtered result */
        }
        pc_surface_fini(out);
        return rc;
    }
    float *r = calloc(ol > 0 ? ol : 1, sizeof *r);
    if (r == NULL) {
        free(o);
        free(w);
        if (have_conv) {
            pc_surface_fini(&od_conv);
        }
        pc_surface_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, "mix");
    }
    uint32_t aw = pc_rect_width(area);
    size_t pixels = n > 0 ? ol / n : 0;
    for (size_t i = 0; i < pixels; i++) {
        const float *op = &o[i * n];
        const float *np = &w[i * n];
        float *rp = &r[i * n];
        int32_t x = area.x0 + (int32_t)(aw != 0 ? i % aw : 0);
        int32_t y = area.y0 + (int32_t)(aw != 0 ? i / aw : 0);
        float k = pc_surface_sample_channel(sel, x, y, 0);
        if (k < 0.0f) {
            k = 0.0f;
        } else if (k > 1.0f) {
            k = 1.0f;
        }
        if (!fmt.alpha) {
            for (size_t c = 0; c < n; c++) {
                rp[c] = op[c] + (np[c] - op[c]) * k;
            }
            continue;
        }
        size_t a = n - 1;
        float oa = op[a] + (np[a] - op[a]) * k;
        rp[a] = oa;
        for (size_t c = 0; c < a; c++) {
            float pm = op[c] * op[a] + (np[c] * np[a] - op[c] * op[a]) * k;
            rp[c] = oa > 0.0f ? pm / oa : 0.0f;
        }
    }
    rc = pc_surface_write_region(out, area, r, ol, err);
    if (rc == PC_OK) {
        pc_surface_prune(out);
    } else {
        pc_surface_fini(out);
    }
    free(o);
    free(w);
    free(r);
    if (have_conv) {
        pc_surface_fini(&od_conv);
    }
    return rc;
}

/* ---- exported re-render hook (lens_cmds.rs apply_to_surface) ------------------------------- *
 * Re-applies a stored lens / wide / camera-raw smart filter on a surface; *applied is false for
 * other ids (the subtype hook then falls through to the core filter table). Never crashes. */
pc_status pc_engine_lens_apply_to_surface(const char *id, yyjson_val *params, const pc_surface *surf,
                                          pc_rect canvas, pc_surface *out, bool *applied, pc_error *err)
{
    *applied = false;
    if (id == NULL || surf == NULL || out == NULL) {
        return PC_OK;
    }
    if (strcmp(id, LENS_ID) == 0) {
        pc_algo_lens_correction lc;
        double focal;
        if (lens_params(LENS_ID, params, canvas, NULL, &lc, &focal, err) != PC_OK) {
            pc_error_clear(err);
            return PC_OK; /* a stored filter that no longer parses is skipped (Rust None) */
        }
        pc_status rc = pc_algo_lens_correct(surf, canvas, &lc, out, err);
        if (rc != PC_OK) {
            pc_error_clear(err);
            return PC_OK;
        }
        *applied = true;
        return PC_OK;
    }
    if (strcmp(id, WIDE_ID) == 0) {
        pc_algo_wide_angle wa;
        if (wide_params(WIDE_ID, params, NULL, &wa, err) != PC_OK) {
            pc_error_clear(err);
            return PC_OK;
        }
        pc_algo_interp interp = pc_algo_interp_parse(sget(params, "interpolation", "bicubic"));
        pc_status rc = pc_algo_wide_apply(surf, canvas, &wa, interp, out, err);
        pc_algo_wide_angle_fini(&wa);
        if (rc != PC_OK) {
            pc_error_clear(err);
            return PC_OK;
        }
        *applied = true;
        return PC_OK;
    }
    if (strcmp(id, RAW_ID) == 0) {
        pc_algo_camera_raw cr;
        if (raw_params(RAW_ID, params, &cr, err) != PC_OK) {
            pc_error_clear(err);
            return PC_OK;
        }
        pc_status rc = camera_raw_surface(surf, canvas, &cr, out, err);
        pc_algo_camera_raw_fini(&cr);
        if (rc != PC_OK) {
            pc_error_clear(err);
            return PC_OK;
        }
        *applied = true;
        return PC_OK;
    }
    return PC_OK;
}

/* ---- running the commands ----------------------------------------------------------------- */

enum lens_kind { LK_LENS, LK_WIDE, LK_RAW };

struct lens_ctx {
    enum lens_kind kind;
    const pc_algo_lens_correction *lc;
    const pc_algo_wide_angle *wa;
    const pc_algo_camera_raw *cr;
    pc_algo_interp interp;
    bool float_bg;
    pc_layer_id layer;
    const char *cmd;              /* command id, recorded on a smart object */
    const yyjson_mut_val *stored; /* resolved params to record on a smart object (borrowed) */
};

/* Produces the filtered surface for ctx over the canvas (*out initialised on success). */
static pc_status lens_apply(const struct lens_ctx *c, const pc_surface *surf, pc_rect canvas,
                            pc_surface *out, pc_error *err)
{
    switch (c->kind) {
    case LK_LENS:
        return pc_algo_lens_correct(surf, canvas, c->lc, out, err);
    case LK_WIDE:
        return pc_algo_wide_apply(surf, canvas, c->wa, c->interp, out, err);
    case LK_RAW:
    default:
        return camera_raw_surface(surf, pc_rect_union(canvas, pc_surface_content_bounds(surf)), c->cr,
                                  out, err);
    }
}

static pc_status edit_lens(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                           pc_error *err)
{
    (void)has_active;
    (void)active;
    struct lens_ctx *c = vctx;
    pc_layer_id id = c->layer;
    pc_rect canvas = pc_document_bounds(doc);
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    pc_locks locks = pc_document_effective_locks(doc, id);
    bool background = pc_document_is_background(doc, id);
    pc_layer *l = pc_document_layer_mut(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind == PC_LAYER_SMART) {
        /* Non-destructive: record the resolved filter and re-render from the source (the stored
         * one re-applies through the subtype hook, pc_engine_lens_apply_to_surface). */
        pc_smart_filter sf;
        memset(&sf, 0, sizeof sf);
        pc_status rc = pc_str_set(&sf.command, c->cmd, strlen(c->cmd), err);
        if (rc == PC_OK) {
            rc = pc_json_value_from_mut(c->stored, &sf.params, err);
        }
        if (rc != PC_OK) {
            pc_smart_filter_fini(&sf);
            return rc;
        }
        sf.blend = PC_BLEND_NORMAL;
        sf.opacity = 1.0f;
        sf.visible = true;
        return pc_engine_add_smart_filter(doc, id, &sf, sel, err);
    }
    if (locks.all || locks.pixels) {
        return pc_engine_err_other(err, "layer \"%s\" is locked", pc_str_c(&l->name));
    }
    /* A Background layer gains transparency when the edge mode exposes it (as in Photoshop). */
    if (c->float_bg && background) {
        pc_engine_background_to_layer_for_mask(doc, id);
        l = pc_document_layer_mut(doc, id);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, id);
        }
    }
    if (l->content.kind != PC_LAYER_RASTER) {
        return pc_engine_err_other(err, "this filter needs a pixel layer");
    }
    pc_surface *surf = &l->content.u.raster;
    pc_surface filtered;
    pc_status rc = lens_apply(c, surf, canvas, &filtered, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (sel != NULL) {
        pc_surface mixed;
        rc = mix_by_selection(surf, &filtered, sel, &mixed, err);
        pc_surface_fini(&filtered);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(surf);
        *surf = mixed;
    } else {
        pc_surface_fini(surf);
        *surf = filtered;
    }
    return PC_OK;
}

static pc_status camera_info(const pc_document *doc, pc_algo_camera_info *info, bool *have)
{
    *have = false;
    if (doc->metadata.exif == NULL) {
        return PC_OK;
    }
    pc_error e;
    pc_error_clear(&e);
    if (pc_algo_exif_read(pc_blob_data(doc->metadata.exif), pc_blob_len(doc->metadata.exif), info, &e) == PC_OK) {
        *have = true;
    }
    return PC_OK;
}

static pc_status run_common(pc_engine_session *s, struct lens_ctx *c, const char *label,
                            pc_json_value *out, yyjson_mut_val *(*result)(yyjson_mut_doc *, void *),
                            void *rctx, pc_error *err)
{
    pc_status rc = pc_engine_session_edit(s, label, edit_lens, c, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = result != NULL ? result(rd, rctx) : yyjson_mut_obj(rd);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- Lens Correction ---------------------------------------------------------------------- */

struct lens_result {
    pc_layer_id layer;
    const pc_algo_lens_correction *lc;
    double focal;
};

static yyjson_mut_val *lens_result_obj(yyjson_mut_doc *d, void *vctx)
{
    struct lens_result *r = vctx;
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, o, "layer", r->layer);
    if (r->lc->has_profile) {
        yyjson_mut_obj_add_strcpy(d, o, "profile", r->lc->profile.name);
    } else {
        yyjson_mut_obj_add_null(d, o, "profile");
    }
    yyjson_mut_obj_add_real(d, o, "angle", r->lc->angle);
    yyjson_mut_obj_add_real(d, o, "scale", r->lc->scale);
    /* Resolved params echo (straighten/autoScale/layer folded in). */
    yyjson_mut_val *params = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, params, "profile", r->lc->has_profile ? "generic" : "none");
    if (r->lc->has_profile && r->focal > 0.0) {
        yyjson_mut_obj_add_real(d, params, "focalLength", r->focal);
    }
    yyjson_mut_obj_add_real(d, params, "angle", r->lc->angle);
    yyjson_mut_obj_add_real(d, params, "scale", r->lc->scale);
    yyjson_mut_obj_add_val(d, o, "params", params);
    return o;
}

static pc_status run_lens(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_rect frame = pc_document_bounds(doc);
    pc_algo_camera_info info;
    bool have_info = false;
    (void)camera_info(doc, &info, &have_info);

    pc_algo_lens_correction lc;
    double focal = -1.0;
    pc_status rc = lens_params(LENS_ID, p, frame, have_info ? &info : NULL, &lc, &focal, err);
    if (have_info) {
        pc_algo_camera_info_fini(&info);
    }
    if (rc != PC_OK) {
        return rc;
    }

    pc_layer_id layer;
    rc = pc_engine_layer_param(s, p, &layer, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *sd = yyjson_mut_doc_new(NULL);
    if (sd == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    struct lens_ctx c = {.kind = LK_LENS,
                         .lc = &lc,
                         .float_bg = lc.edge.kind == PC_ALGO_LENS_EDGE_TRANSPARENCY,
                         .layer = layer,
                         .cmd = LENS_ID,
                         .stored = resolved_lens_params(sd, p, &lc, focal)};
    struct lens_result r = {.layer = layer, .lc = &lc, .focal = focal};
    rc = run_common(s, &c, "Lens Correction", out, lens_result_obj, &r, err);
    yyjson_mut_doc_free(sd);
    return rc;
}

/* ---- Adaptive Wide Angle ------------------------------------------------------------------ */

struct wide_result {
    pc_layer_id layer;
    const pc_algo_wide_angle *wa;
    pc_rect frame;
};

static yyjson_mut_val *wide_result_obj(yyjson_mut_doc *d, void *vctx)
{
    struct wide_result *r = vctx;
    pc_algo_wide_camera cam;
    pc_algo_wide_camera_init(&cam, r->wa, r->frame);
    const char *model = cam.model == PC_ALGO_WIDE_FISHEYE        ? "fisheye"
                        : cam.model == PC_ALGO_WIDE_PERSPECTIVE   ? "perspective"
                        : cam.model == PC_ALGO_WIDE_FULL_SPHERICAL ? "fullSpherical"
                                                                   : "auto";
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, o, "layer", r->layer);
    yyjson_mut_obj_add_str(d, o, "model", model);
    yyjson_mut_obj_add_real(d, o, "focalPx", cam.f);
    yyjson_mut_obj_add_uint(d, o, "constraints", r->wa->n_constraints);
    return o;
}

static pc_status run_wide(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_rect frame = pc_document_bounds(doc);
    pc_algo_camera_info info;
    bool have_info = false;
    (void)camera_info(doc, &info, &have_info);

    pc_algo_wide_angle wa;
    pc_status rc = wide_params(WIDE_ID, p, have_info ? &info : NULL, &wa, err);
    if (have_info) {
        pc_algo_camera_info_fini(&info);
    }
    if (rc != PC_OK) {
        return rc;
    }

    pc_layer_id layer;
    rc = pc_engine_layer_param(s, p, &layer, err);
    if (rc != PC_OK) {
        pc_algo_wide_angle_fini(&wa);
        return rc;
    }
    pc_algo_interp interp = pc_algo_interp_parse(sget(p, "interpolation", "bicubic"));
    yyjson_mut_doc *sd = yyjson_mut_doc_new(NULL);
    if (sd == NULL) {
        pc_algo_wide_angle_fini(&wa);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    /* The recorded params are the resolved settings; the stored smart filter re-applies at bicubic
     * through the subtype hook, so interpolation is not recorded (lens_cmds::run_filter). */
    struct lens_ctx c = {.kind = LK_WIDE,
                         .wa = &wa,
                         .interp = interp,
                         .float_bg = true,
                         .layer = layer,
                         .cmd = WIDE_ID,
                         .stored = pc_algo_wide_angle_to_json(sd, &wa)};
    struct wide_result r = {.layer = layer, .wa = &wa, .frame = frame};
    rc = run_common(s, &c, "Adaptive Wide Angle", out, wide_result_obj, &r, err);
    yyjson_mut_doc_free(sd);
    pc_algo_wide_angle_fini(&wa);
    return rc;
}

/* ---- Camera Raw Filter -------------------------------------------------------------------- */

struct raw_result {
    pc_layer_id layer;
    bool identity;
};

static yyjson_mut_val *raw_result_obj(yyjson_mut_doc *d, void *vctx)
{
    struct raw_result *r = vctx;
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, o, "layer", r->layer);
    yyjson_mut_obj_add_bool(d, o, "identity", r->identity);
    return o;
}

static pc_status run_raw(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);

    pc_algo_camera_raw cr;
    pc_status rc = raw_params(RAW_ID, p, &cr, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* The document's transfer curve goes with the settings; a missing encodingGamma is taken from
     * the document's profile (so Whites/Blacks/Shadows/Highlights act on the same light). */
    if (p == NULL || pc_json_get(p, "encodingGamma") == NULL) {
        float g = encoding_gamma(doc);
        if (g > 0.0f) {
            cr.encoding_gamma = (float)(round((double)g * 1e4) / 1e4);
        }
    }
    /* New settings are strict; stored smart filters re-apply through the lenient raw_params. */
    rc = pc_algo_camera_raw_validate(&cr, err);
    if (rc != PC_OK) {
        pc_algo_camera_raw_fini(&cr);
        return rc;
    }

    pc_layer_id layer;
    rc = pc_engine_layer_param(s, p, &layer, err);
    if (rc != PC_OK) {
        pc_algo_camera_raw_fini(&cr);
        return rc;
    }
    bool identity = pc_algo_camera_raw_is_identity(&cr);
    /* The pixel path works on the canvas plus the layer's content in one buffer; reject an area
     * too large to develop before touching anything (#962). */
    if (!identity) {
        const pc_layer *l = pc_document_layer(doc, layer);
        pc_rect area = pc_document_bounds(doc);
        if (l != NULL && l->content.kind == PC_LAYER_RASTER) {
            area = pc_rect_union(area, pc_surface_content_bounds(&l->content.u.raster));
        }
        uint64_t npix = (uint64_t)pc_rect_width(area) * (uint64_t)pc_rect_height(area);
        if (npix > MAX_RAW_PIXELS) {
            pc_status e = pc_engine_err_bad_params(
                err, RAW_ID, "the image is too large for Camera Raw (%ux%u px; max %llu megapixels)",
                pc_rect_width(area), pc_rect_height(area), (unsigned long long)(MAX_RAW_PIXELS / 1000000ULL));
            pc_algo_camera_raw_fini(&cr);
            return e;
        }
    }
    yyjson_mut_doc *sd = yyjson_mut_doc_new(NULL);
    if (sd == NULL) {
        pc_algo_camera_raw_fini(&cr);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    struct lens_ctx c = {.kind = LK_RAW,
                         .cr = &cr,
                         .float_bg = false,
                         .layer = layer,
                         .cmd = RAW_ID,
                         .stored = pc_algo_camera_raw_to_json(sd, &cr)};
    struct raw_result r = {.layer = layer, .identity = identity};
    rc = run_common(s, &c, "Camera Raw Filter", out, raw_result_obj, &r, err);
    yyjson_mut_doc_free(sd);
    pc_algo_camera_raw_fini(&cr);
    return rc;
}

/* ---- enabled predicates ------------------------------------------------------------------- */

static pc_status raw_enabled(const pc_engine_session *s, pc_error *reason)
{
    pc_status rc = pc_engine_filterable(s, reason);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (doc->mode == PC_COLOR_MODE_RGB || doc->mode == PC_COLOR_MODE_GRAYSCALE) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID,
                   "Camera Raw Filter needs an RGB or Grayscale document (this one is %s)",
                   pc_color_mode_name(doc->mode));
}

/* ---- specs -------------------------------------------------------------------------------- */

static const char *const MENU_FILTER[] = {"Filter"};

static const pc_engine_command SPECS[] = {
    {LENS_ID, "Lens Correction\xE2\x80\xA6", MENU_FILTER, 1, "Cmd+Shift+R",
     "{\"profile\":\"none|auto|generic\",\"focalLength\":mm=0,\"correctDistortion\":bool=true,"
     "\"correctVignette\":bool=true,\"correctCA\":bool=true,\"autoScale\":bool=false,"
     "\"distortion\":-100..100=0,\"redCyan\":-100..100=0,\"blueYellow\":-100..100=0,"
     "\"vignetteAmount\":-100..100=0,\"vignetteMidpoint\":0..100=50,\"vertical\":-100..100=0,"
     "\"horizontal\":-100..100=0,\"angle\":-180..180=0,\"scale\":50..150=100,"
     "\"edge\":\"transparency|edgeExtension|black|white\",\"straighten\":[[x,y],[x,y]]?}",
     pc_engine_filterable, run_lens, true},
    {WIDE_ID, "Adaptive Wide Angle\xE2\x80\xA6", MENU_FILTER, 1, "Cmd+Alt+Shift+A",
     "{\"model\":\"auto|fisheye|perspective|fullSpherical\",\"focalLength\":0..200=0,"
     "\"cropFactor\":0.1..10=1,\"scale\":50..150=100,"
     "\"constraints\":[{\"a\":[x,y],\"b\":[x,y],\"orientation\":\"free|horizontal|vertical\"}]}",
     pc_engine_filterable, run_wide, true},
    {RAW_ID, "Camera Raw Filter\xE2\x80\xA6", MENU_FILTER, 1, "Cmd+Shift+A",
     "{\"temperature\":-100..100=0,\"tint\":-100..100=0,\"exposure\":-5..5=0,\"contrast\":-100..100=0,"
     "\"highlights\":-100..100=0,\"shadows\":-100..100=0,\"whites\":-100..100=0,\"blacks\":-100..100=0,"
     "\"texture\":-100..100=0,\"clarity\":-100..100=0,\"dehaze\":-100..100=0,\"vibrance\":-100..100=0,"
     "\"saturation\":-100..100=0,\"curveHighlights\":-100..100=0,\"curveLights\":-100..100=0,"
     "\"curveDarks\":-100..100=0,\"curveShadows\":-100..100=0,\"curveSplits\":[25,50,75],"
     "\"pointCurve\":[[in,out]],\"pointCurveRed\":[[in,out]],\"pointCurveGreen\":[[in,out]],"
     "\"pointCurveBlue\":[[in,out]],\"hslHue\":[8],\"hslSat\":[8],\"hslLum\":[8],"
     "\"gradeShadows\":{\"hue\":deg,\"sat\":0..100,\"lum\":-100..100},\"gradeMidtones\":{},"
     "\"gradeHighlights\":{},\"gradeGlobal\":{},\"gradeBlending\":0..100=50,\"gradeBalance\":-100..100=0,"
     "\"sharpenAmount\":0..150=0,\"sharpenRadius\":0.5..3=1,\"sharpenDetail\":0..100=25,"
     "\"sharpenMasking\":0..100=0,\"noiseLuminance\":0..100=0,\"noiseLuminanceDetail\":0..100=50,"
     "\"noiseColor\":0..100=0,\"noiseColorDetail\":0..100=50,\"grainAmount\":0..100=0,"
     "\"grainSize\":0..100=25,\"grainRoughness\":0..100=50,\"vignetteAmount\":-100..100=0,"
     "\"vignetteMidpoint\":0..100=50,\"vignetteRoundness\":-100..100=0,\"vignetteFeather\":0..100=50,"
     "\"vignetteHighlights\":0..100=0,"
     "\"vignetteStyle\":\"highlightPriority|colorPriority|paintOverlay\",\"seed\":u32=0}",
     raw_enabled, run_raw, true},
};

const pc_engine_command *pc_engine_lens_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
