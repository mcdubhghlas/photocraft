/* The remaining Filter-menu standalone filters (port of crates/engine/src/filters_ext.rs, E-tail
 * EF2): Pixelate (Color Halftone, Crystallize, Facet, Fragment, Mezzotint, Pointillize), Stylize
 * (Diffuse, Extrude, Oil Paint, Tiles, Trace Contour, Wind), Distort (Shear, ZigZag), Render
 * (Fibers, Lens Flare, Lighting Effects, Relight), Reduce Noise, Smart/Lens/Shape Blur, the Blur
 * Gallery (Tilt-Shift, Iris/Field/Spin/Path Blur), Other (Custom, HSB/HSL) and Video (De-Interlace,
 * NTSC Colors).
 *
 * Every id routes through the shared filter runner (pc_engine_run_filter in filters_cmds.c):
 * selection, history, Last Filter and -- once E6 lands -- smart objects all work the same. This
 * module provides (1) the command id -> algo-filter mapping, pc_engine_ext_params_for, chained from
 * filters_cmds.c's params_for else-branch (core -> ext -> gallery, mirroring the reference's
 * `_ => filters_ext::params_for` / `_ => gallery_cmds::params_for` fall-through); (2) the choice
 * validation chained from filters_validate.inc; (3) run thunks that inject the session
 * foreground/background for the colour-reading filters (Fibers, Tiles, Pointillize), so the recorded
 * params reproduce the result, exactly as the reference's filters_ext::prepare does. The algorithm
 * runs through src/algo (pc_algo_apply_in), so results are bit-exact with the reference.
 *
 * DEFERRED (reported): filter.distort.displace needs a displacement map loaded from a file or
 * another document (filters_ext::resolve / resolve_map_path, file I/O) -- the E3 map-load work; its
 * algo def exists but the command is not registered here. Color to Alpha (filter.other.colorToAlpha)
 * is owned by ET2. Smart-object filter recording is the general E6/E9 run_filter smart gap.
 *
 * Never crashes on hostile params: every size is clamped, NaN/Inf are bounded (so halos and
 * allocations stay finite), and a parse failure is a graceful error. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/filter.h"
#include "algo/fx.h"         /* pc_algo_displace_params (Displace map field) */
#include "base/file.h"       /* pc_file_read (displace map from a path) */
#include "base/json.h"       /* pc_json_value (displace embedded-map params) */
#include "channel_cmds.h"
#include "compose/compose.h" /* pc_compose_flatten / render_layer (displace map image) */
#include "doc/doc.h"
#include "io/displace_map.h" /* pc_io_displace_embedded_map */

#include "engine_internal.h"

/* ---- param readers (filters_ext.rs f / i / b / s / seed) ---------------------------------- */

static float fget(yyjson_val *p, const char *k, float d)
{
    yyjson_val *v = pc_json_get(p, k);
    double got;
    if (v != NULL && pc_json_as_f64(v, &got)) {
        return (float)got;
    }
    return d;
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

/* filters_ext.rs seed(): i(p,"seed",0) as u32. Non-negative uint fits; otherwise wrap like `as`. */
static uint32_t seed_of(yyjson_val *p)
{
    yyjson_val *v = pc_json_get(p, "seed");
    if (v != NULL && yyjson_is_uint(v)) {
        uint64_t n = yyjson_get_uint(v);
        return (uint32_t)n;
    }
    double raw;
    if (v != NULL && pc_json_as_f64(v, &raw)) {
        double r = round(raw);
        if (!isfinite(r)) {
            return 0;
        }
        /* Rust `as u32` from an i64 wraps; emulate via a modular reduction that avoids UB. */
        double m = fmod(r, 4294967296.0);
        if (m < 0.0) {
            m += 4294967296.0;
        }
        return (uint32_t)m;
    }
    return 0;
}

/* Rust f32::clamp: assumes lo<=hi; NaN propagates (v<lo and v>hi both false). */
static float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* filters_ext.rs colour(): straight RGBA from [r,g,b(,a)] (per-index default 0,0,0,1) or "#rrggbb"
 * (a=1); otherwise the caller's default `def`. */
static void ext_colour(yyjson_val *p, const char *k, const float def[4], float out[4])
{
    yyjson_val *v = pc_json_get(p, k);
    if (v != NULL && yyjson_is_arr(v) && yyjson_arr_size(v) >= 3) {
        const double d4[4] = {0.0, 0.0, 0.0, 1.0};
        for (size_t j = 0; j < 4; j++) {
            yyjson_val *e = yyjson_arr_get(v, j);
            double n;
            out[j] = (e != NULL && pc_json_as_f64(e, &n)) ? (float)n : (float)d4[j];
        }
        return;
    }
    if (v != NULL && yyjson_is_str(v) && yyjson_get_len(v) == 7) {
        const char *h = yyjson_get_str(v);
        if (h != NULL && h[0] == '#') {
            for (int c = 0; c < 3; c++) {
                char a = h[1 + c * 2], b = h[2 + c * 2];
                int hi, lo;
                if (a >= '0' && a <= '9') {
                    hi = a - '0';
                } else if (a >= 'a' && a <= 'f') {
                    hi = a - 'a' + 10;
                } else if (a >= 'A' && a <= 'F') {
                    hi = a - 'A' + 10;
                } else {
                    hi = 0;
                }
                if (b >= '0' && b <= '9') {
                    lo = b - '0';
                } else if (b >= 'a' && b <= 'f') {
                    lo = b - 'a' + 10;
                } else if (b >= 'A' && b <= 'F') {
                    lo = b - 'A' + 10;
                } else {
                    lo = 0;
                }
                out[c] = (float)(hi * 16 + lo) / 255.0f;
            }
            out[3] = 1.0f;
            return;
        }
    }
    memcpy(out, def, 4 * sizeof(float));
}

static const float C_BLACK[4] = {0.0f, 0.0f, 0.0f, 1.0f};
static const float C_WHITE[4] = {1.0f, 1.0f, 1.0f, 1.0f};

/* An enum param sanitised to a valid serde string: the matching allowed value, else `def` (the
 * reference's match-with-default, so pc_algo_filter_from_json never rejects the tag). */
static const char *enum_or(const char *v, const char *const *allowed, size_t n, const char *def)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(v, allowed[i]) == 0) {
            return allowed[i];
        }
    }
    return def;
}

static const char *const MEZZOTINT_KINDS[] = {"fineDots",   "mediumDots",   "grainyDots",
                                              "coarseDots", "shortLines",   "mediumLines",
                                              "longLines",  "shortStrokes", "mediumStrokes",
                                              "longStrokes"};
static const char *const DIFFUSE_MODES[] = {"normal", "darkenOnly", "lightenOnly", "anisotropic"};
static const char *const EXTRUDE_KINDS[] = {"blocks", "pyramids"};
static const char *const TILE_FILLS[] = {"background", "foreground", "inverse", "unaltered"};
static const char *const WIND_METHODS[] = {"wind", "blast", "stagger"};
static const char *const ZIGZAG_STYLES[] = {"pondRipples", "outFromCenter", "aroundCenter"};
static const char *const LENS_TYPES[] = {"zoom", "prime35", "prime105", "moviePrime"};
static const char *const TEXTURES[] = {"none", "red", "green", "blue", "alpha", "luminance"};
static const char *const LIGHT_KINDS[] = {"spot", "point", "infinite"};
static const char *const SMART_QUALITIES[] = {"low", "medium", "high"};
static const char *const SMART_MODES[] = {"normal", "edgeOnly", "overlayEdge"};
static const char *const DEPTH_SOURCES[] = {"none", "transparency", "layerMask"};
static const char *const DISTRIBUTIONS[] = {"uniform", "gaussian"};
static const char *const BLUR_SHAPES[] = {"circle", "ring",    "square", "diamond", "triangle",
                                          "hexagon", "star",   "heart",  "cross"};
static const char *const HSB_MODELS[] = {"rgb", "hsb", "hsl"};

/* undefinedAreas -> algo serde name, with the caller's default. */
static const char *undefined_str(yyjson_val *p, const char *def)
{
    const char *s = sget(p, "undefinedAreas", def);
    if (strcmp(s, "repeat") == 0) {
        return "repeat";
    }
    if (strcmp(s, "transparent") == 0) {
        return "transparent";
    }
    return "wrap";
}

/* ---- JSON builders ------------------------------------------------------------------------ */

static void add_f(yyjson_mut_doc *d, yyjson_mut_val *o, const char *k, float v)
{
    yyjson_mut_obj_add_real(d, o, k, (double)v);
}
static void add_s(yyjson_mut_doc *d, yyjson_mut_val *o, const char *k, const char *v)
{
    yyjson_mut_obj_add_str(d, o, k, v);
}
static void add_b(yyjson_mut_doc *d, yyjson_mut_val *o, const char *k, bool v)
{
    yyjson_mut_obj_add_bool(d, o, k, v);
}
static void add_u(yyjson_mut_doc *d, yyjson_mut_val *o, const char *k, uint32_t v)
{
    yyjson_mut_obj_add_uint(d, o, k, (uint64_t)v);
}
static void add_color(yyjson_mut_doc *d, yyjson_mut_val *o, const char *k, const float c[4], int n)
{
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (int i = 0; i < n; i++) {
        yyjson_mut_arr_add_real(d, arr, (double)c[i]);
    }
    yyjson_mut_obj_add_val(d, o, k, arr);
}
static void add_point(yyjson_mut_doc *d, yyjson_mut_val *arr, float x, float y)
{
    yyjson_mut_val *pr = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, pr, (double)x);
    yyjson_mut_arr_add_real(d, pr, (double)y);
    yyjson_mut_arr_add_val(arr, pr);
}

/* A caller-supplied composite array (lights / pins / paths), deep-copied, or NULL when absent or
 * empty (the reference's list(): None -> the single-element fallback). */
static yyjson_mut_val *user_array(yyjson_mut_doc *d, yyjson_val *p, const char *key)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v != NULL && yyjson_is_arr(v) && yyjson_arr_size(v) > 0) {
        yyjson_mut_val *c = yyjson_val_mut_copy(d, v);
        if (c != NULL && yyjson_mut_is_arr(c)) {
            return c;
        }
    }
    return NULL;
}

/* Path speeds bypass the scalar clamp, and the halo casts one to i32 (#708), so a pass-through
 * path's speed is clamped to 0..500 here, as filters_ext.rs does. */
static void clamp_path_speeds(yyjson_mut_val *arr)
{
    yyjson_mut_arr_iter it;
    yyjson_mut_arr_iter_init(arr, &it);
    yyjson_mut_val *e;
    while ((e = yyjson_mut_arr_iter_next(&it)) != NULL) {
        if (!yyjson_mut_is_obj(e)) {
            continue;
        }
        yyjson_mut_val *sp = yyjson_mut_obj_get(e, "speed");
        if (sp != NULL && yyjson_mut_is_num(sp)) {
            float v = (float)yyjson_mut_get_num(sp);
            v = isfinite(v) ? clampf(v, 0.0f, 500.0f) : 0.0f;
            (void)yyjson_mut_set_real(sp, (double)v);
        }
    }
}

/* Spin pin angles are clamped (and NaN/Inf -> 0) so the blur stays finite. */
static void clamp_spin_angles(yyjson_mut_val *arr)
{
    yyjson_mut_arr_iter it;
    yyjson_mut_arr_iter_init(arr, &it);
    yyjson_mut_val *e;
    while ((e = yyjson_mut_arr_iter_next(&it)) != NULL) {
        if (!yyjson_mut_is_obj(e)) {
            continue;
        }
        yyjson_mut_val *a = yyjson_mut_obj_get(e, "angle");
        if (a != NULL && yyjson_mut_is_num(a)) {
            float v = (float)yyjson_mut_get_num(a);
            v = isfinite(v) ? clampf(v, -360.0f, 360.0f) : 0.0f;
            (void)yyjson_mut_set_real(a, (double)v);
        }
        yyjson_mut_val *ba = yyjson_mut_obj_get(e, "blurAngle");
        if (ba != NULL && yyjson_mut_is_num(ba)) {
            float v = (float)yyjson_mut_get_num(ba);
            v = isfinite(v) ? clampf(v, 0.0f, 360.0f) : 0.0f;
            (void)yyjson_mut_set_real(ba, (double)v);
        }
    }
}

/* ---- the single composite elements (list() fallback) -------------------------------------- */

static float blur_of(yyjson_val *p, float d)
{
    return clampf(fget(p, "blur", d), 0.0f, 500.0f);
}

static yyjson_mut_val *single_light(yyjson_mut_doc *d, yyjson_val *p)
{
    yyjson_mut_val *lt = yyjson_mut_obj(d);
    add_s(d, lt, "kind", enum_or(sget(p, "lightType", "spot"), LIGHT_KINDS, 3, "spot"));
    add_f(d, lt, "x", fget(p, "lightX", 0.25f));
    add_f(d, lt, "y", fget(p, "lightY", 0.2f));
    add_f(d, lt, "z", clampf(fget(p, "lightZ", 0.6f), 0.0f, 10.0f));
    add_f(d, lt, "targetX", fget(p, "targetX", 0.5f));
    add_f(d, lt, "targetY", fget(p, "targetY", 0.55f));
    add_f(d, lt, "angle", fget(p, "angle", 135.0f));
    add_f(d, lt, "elevation", clampf(fget(p, "elevation", 45.0f), 0.0f, 90.0f));
    float col[4];
    ext_colour(p, "color", C_WHITE, col);
    add_color(d, lt, "color", col, 3);
    add_f(d, lt, "intensity", clampf(fget(p, "intensity", 75.0f), -100.0f, 100.0f));
    add_f(d, lt, "cone", clampf(fget(p, "cone", 45.0f), 1.0f, 89.0f));
    add_f(d, lt, "hotspot", clampf(fget(p, "hotspot", 50.0f), 0.0f, 100.0f));
    add_f(d, lt, "radius", clampf(fget(p, "radius", 0.8f), 0.01f, 10.0f));
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    yyjson_mut_arr_add_val(arr, lt);
    return arr;
}

static yyjson_mut_val *single_iris(yyjson_mut_doc *d, yyjson_val *p)
{
    yyjson_mut_val *pin = yyjson_mut_obj(d);
    add_f(d, pin, "x", fget(p, "centerX", 0.5f));
    add_f(d, pin, "y", fget(p, "centerY", 0.5f));
    add_f(d, pin, "radiusX", fmaxf(fget(p, "radiusX", 0.35f), 0.001f));
    add_f(d, pin, "radiusY", fmaxf(fget(p, "radiusY", 0.25f), 0.001f));
    add_f(d, pin, "angle", fget(p, "angle", 0.0f));
    add_f(d, pin, "roundness", clampf(fget(p, "roundness", 0.0f), 0.0f, 100.0f));
    add_f(d, pin, "feather", clampf(fget(p, "feather", 0.5f), 0.0f, 0.99f));
    add_f(d, pin, "blur", blur_of(p, 15.0f));
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    yyjson_mut_arr_add_val(arr, pin);
    return arr;
}

static yyjson_mut_val *single_field(yyjson_mut_doc *d, yyjson_val *p)
{
    yyjson_mut_val *pin = yyjson_mut_obj(d);
    add_f(d, pin, "x", fget(p, "centerX", 0.5f));
    add_f(d, pin, "y", fget(p, "centerY", 0.5f));
    add_f(d, pin, "blur", blur_of(p, 15.0f));
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    yyjson_mut_arr_add_val(arr, pin);
    return arr;
}

static yyjson_mut_val *single_spin(yyjson_mut_doc *d, yyjson_val *p)
{
    yyjson_mut_val *pin = yyjson_mut_obj(d);
    float a = fget(p, "angle", 0.0f);
    add_f(d, pin, "x", fget(p, "centerX", 0.5f));
    add_f(d, pin, "y", fget(p, "centerY", 0.5f));
    add_f(d, pin, "radiusX", fmaxf(fget(p, "radiusX", 0.3f), 0.001f));
    add_f(d, pin, "radiusY", fmaxf(fget(p, "radiusY", 0.3f), 0.001f));
    add_f(d, pin, "angle", isfinite(a) ? clampf(a, -360.0f, 360.0f) : 0.0f);
    add_f(d, pin, "blurAngle", clampf(fget(p, "blurAngle", 15.0f), 0.0f, 360.0f));
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    yyjson_mut_arr_add_val(arr, pin);
    return arr;
}

static yyjson_mut_val *single_path(yyjson_mut_doc *d, yyjson_val *p)
{
    yyjson_mut_val *path = yyjson_mut_obj(d);
    yyjson_mut_val *pts = yyjson_mut_arr(d);
    add_point(d, pts, fget(p, "startX", 0.2f), fget(p, "startY", 0.5f));
    add_point(d, pts, fget(p, "endX", 0.8f), fget(p, "endY", 0.5f));
    yyjson_mut_obj_add_val(d, path, "points", pts);
    add_f(d, path, "speed", clampf(fget(p, "speed", 50.0f), 0.0f, 500.0f));
    add_f(d, path, "taper", clampf(fget(p, "taper", 0.0f), 0.0f, 100.0f));
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    yyjson_mut_arr_add_val(arr, path);
    return arr;
}

/* ---- finalize ----------------------------------------------------------------------------- */

static pc_status ext_finalize(yyjson_mut_doc *d, yyjson_mut_val *o, pc_algo_filter *out, pc_error *err)
{
    yyjson_mut_doc_set_root(d, o);
    yyjson_doc *imm = yyjson_mut_doc_imut_copy(d, NULL);
    pc_status rc;
    if (imm == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "filter params");
    } else {
        rc = pc_algo_filter_from_json(yyjson_doc_get_root(imm), out, err);
        yyjson_doc_free(imm);
    }
    yyjson_mut_doc_free(d);
    return rc;
}

/* ---- params_for (chained from filters_cmds.c) --------------------------------------------- */

pc_status pc_engine_ext_params_for(const char *id, yyjson_val *p, pc_algo_filter *out, bool *found,
                                   pc_error *err)
{
    *found = true;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "filter params");
    }
    yyjson_mut_val *o = yyjson_mut_obj(d);

    /* ---- Pixelate ---- */
    if (strcmp(id, "filter.pixelate.colorHalftone") == 0) {
        add_s(d, o, "filter", "colorHalftone");
        add_f(d, o, "maxRadius", clampf(fget(p, "maxRadius", 8.0f), 4.0f, 127.0f));
        yyjson_mut_val *ang = yyjson_mut_arr(d);
        yyjson_mut_arr_add_real(d, ang, (double)fget(p, "channel1", 108.0f));
        yyjson_mut_arr_add_real(d, ang, (double)fget(p, "channel2", 162.0f));
        yyjson_mut_arr_add_real(d, ang, (double)fget(p, "channel3", 90.0f));
        yyjson_mut_arr_add_real(d, ang, (double)fget(p, "channel4", 45.0f));
        yyjson_mut_obj_add_val(d, o, "angles", ang);
    } else if (strcmp(id, "filter.pixelate.crystallize") == 0) {
        add_s(d, o, "filter", "crystallize");
        add_f(d, o, "cellSize", clampf(fget(p, "cellSize", 10.0f), 3.0f, 300.0f));
        add_u(d, o, "seed", seed_of(p));
    } else if (strcmp(id, "filter.pixelate.facet") == 0) {
        add_s(d, o, "filter", "facet");
    } else if (strcmp(id, "filter.pixelate.fragment") == 0) {
        add_s(d, o, "filter", "fragment");
    } else if (strcmp(id, "filter.pixelate.mezzotint") == 0) {
        add_s(d, o, "filter", "mezzotint");
        add_s(d, o, "kind", enum_or(sget(p, "type", "fineDots"), MEZZOTINT_KINDS, 10, "fineDots"));
        add_u(d, o, "seed", seed_of(p));
    } else if (strcmp(id, "filter.pixelate.pointillize") == 0) {
        add_s(d, o, "filter", "pointillize");
        add_f(d, o, "cellSize", clampf(fget(p, "cellSize", 5.0f), 3.0f, 300.0f));
        add_u(d, o, "seed", seed_of(p));
        float bg[4];
        ext_colour(p, "background", C_WHITE, bg);
        add_color(d, o, "background", bg, 4);
    }
    /* ---- Stylize ---- */
    else if (strcmp(id, "filter.stylize.diffuse") == 0) {
        add_s(d, o, "filter", "diffuse");
        add_s(d, o, "mode", enum_or(sget(p, "mode", "normal"), DIFFUSE_MODES, 4, "normal"));
        add_u(d, o, "seed", seed_of(p));
    } else if (strcmp(id, "filter.stylize.extrude") == 0) {
        add_s(d, o, "filter", "extrude");
        add_s(d, o, "kind", enum_or(sget(p, "type", "blocks"), EXTRUDE_KINDS, 2, "blocks"));
        add_f(d, o, "size", clampf(fget(p, "size", 30.0f), 2.0f, 255.0f));
        add_f(d, o, "depth", clampf(fget(p, "depth", 30.0f), 1.0f, 255.0f));
        add_b(d, o, "levelBased", strcmp(sget(p, "depthMode", "random"), "levelBased") == 0);
        add_b(d, o, "solidFront", bget(p, "solidFrontFaces", false));
        add_b(d, o, "maskIncomplete", bget(p, "maskIncompleteBlocks", false));
        add_u(d, o, "seed", seed_of(p));
    } else if (strcmp(id, "filter.stylize.oilPaint") == 0) {
        add_s(d, o, "filter", "oilPaint");
        add_f(d, o, "stylization", clampf(fget(p, "stylization", 4.0f), 0.1f, 10.0f));
        add_f(d, o, "cleanliness", clampf(fget(p, "cleanliness", 5.0f), 0.0f, 10.0f));
        add_f(d, o, "scale", clampf(fget(p, "scale", 1.0f), 0.1f, 10.0f));
        add_f(d, o, "bristleDetail", clampf(fget(p, "bristleDetail", 5.0f), 0.0f, 10.0f));
        add_b(d, o, "lighting", bget(p, "lighting", true));
        add_f(d, o, "angle", fget(p, "angle", -60.0f));
        add_f(d, o, "shine", clampf(fget(p, "shine", 1.0f), 0.0f, 10.0f));
    } else if (strcmp(id, "filter.stylize.tiles") == 0) {
        add_s(d, o, "filter", "tiles");
        int32_t count = (int32_t)clampf(fget(p, "count", 10.0f), 1.0f, 99.0f);
        add_u(d, o, "count", (uint32_t)count);
        add_f(d, o, "maxOffset", clampf(fget(p, "maxOffset", 10.0f), 1.0f, 99.0f));
        add_s(d, o, "fill", enum_or(sget(p, "fill", "background"), TILE_FILLS, 4, "background"));
        float fg[4], bg[4];
        ext_colour(p, "foreground", C_BLACK, fg);
        ext_colour(p, "background", C_WHITE, bg);
        add_color(d, o, "foreground", fg, 4);
        add_color(d, o, "background", bg, 4);
        add_u(d, o, "seed", seed_of(p));
    } else if (strcmp(id, "filter.stylize.traceContour") == 0) {
        add_s(d, o, "filter", "traceContour");
        add_f(d, o, "level", clampf(fget(p, "level", 128.0f), 0.0f, 255.0f));
        add_b(d, o, "upper", strcmp(sget(p, "edge", "lower"), "upper") == 0);
    } else if (strcmp(id, "filter.stylize.wind") == 0) {
        add_s(d, o, "filter", "wind");
        add_s(d, o, "method", enum_or(sget(p, "method", "wind"), WIND_METHODS, 3, "wind"));
        add_b(d, o, "fromRight", strcmp(sget(p, "direction", "fromRight"), "fromLeft") != 0);
        add_u(d, o, "seed", seed_of(p));
    }
    /* ---- Distort ---- */
    else if (strcmp(id, "filter.distort.displace") == 0) {
        /* The displacement map (`map`) is #[serde(skip)] in algo: it is NOT built here but
         * resolved by pc_engine_filters_resolve_map (embedded PSD map / mapPath / mapDocument)
         * and pointed at caller-owned storage before the filter runs. */
        add_s(d, o, "filter", "displace");
        add_f(d, o, "horizontal", clampf(fget(p, "horizontal", 10.0f), -999.0f, 999.0f));
        add_f(d, o, "vertical", clampf(fget(p, "vertical", 10.0f), -999.0f, 999.0f));
        add_b(d, o, "stretch", strcmp(sget(p, "fit", "stretch"), "tile") != 0);
        add_s(d, o, "undefined", undefined_str(p, "repeat"));
    } else if (strcmp(id, "filter.distort.shear") == 0) {
        add_s(d, o, "filter", "shear");
        yyjson_mut_val *pts = user_array(d, p, "points");
        if (pts == NULL) {
            float a = clampf(fget(p, "amount", 0.0f), -100.0f, 100.0f) / 100.0f;
            pts = yyjson_mut_arr(d);
            add_point(d, pts, 0.0f, 0.0f);
            add_point(d, pts, 0.5f, a);
            add_point(d, pts, 1.0f, 0.0f);
        }
        yyjson_mut_obj_add_val(d, o, "points", pts);
        add_s(d, o, "undefined", undefined_str(p, "wrap"));
    } else if (strcmp(id, "filter.distort.zigZag") == 0) {
        add_s(d, o, "filter", "zigZag");
        add_f(d, o, "amount", clampf(fget(p, "amount", 10.0f), -100.0f, 100.0f));
        add_f(d, o, "ridges", clampf(fget(p, "ridges", 5.0f), 0.0f, 20.0f));
        add_s(d, o, "style", enum_or(sget(p, "style", "pondRipples"), ZIGZAG_STYLES, 3, "pondRipples"));
    }
    /* ---- Render ---- */
    else if (strcmp(id, "filter.render.fibers") == 0) {
        add_s(d, o, "filter", "fibers");
        add_f(d, o, "variance", clampf(fget(p, "variance", 16.0f), 1.0f, 64.0f));
        add_f(d, o, "strength", clampf(fget(p, "strength", 4.0f), 1.0f, 64.0f));
        add_u(d, o, "seed", seed_of(p));
        float fg[4], bg[4];
        ext_colour(p, "foreground", C_BLACK, fg);
        ext_colour(p, "background", C_WHITE, bg);
        add_color(d, o, "foreground", fg, 4);
        add_color(d, o, "background", bg, 4);
    } else if (strcmp(id, "filter.render.lensFlare") == 0) {
        add_s(d, o, "filter", "lensFlare");
        add_f(d, o, "brightness", clampf(fget(p, "brightness", 100.0f), 10.0f, 300.0f));
        add_f(d, o, "centerX", clampf(fget(p, "centerX", 0.5f), -0.5f, 1.5f));
        add_f(d, o, "centerY", clampf(fget(p, "centerY", 0.5f), -0.5f, 1.5f));
        add_s(d, o, "lens", enum_or(sget(p, "lens", "zoom"), LENS_TYPES, 4, "zoom"));
    } else if (strcmp(id, "filter.render.lightingEffects") == 0) {
        add_s(d, o, "filter", "lightingEffects");
        yyjson_mut_val *lights = user_array(d, p, "lights");
        if (lights == NULL) {
            lights = single_light(d, p);
        }
        yyjson_mut_obj_add_val(d, o, "lights", lights);
        add_f(d, o, "gloss", clampf(fget(p, "gloss", 0.0f), -100.0f, 100.0f));
        add_f(d, o, "metallic", clampf(fget(p, "metallic", 0.0f), -100.0f, 100.0f));
        add_f(d, o, "exposure", clampf(fget(p, "exposure", 0.0f), -100.0f, 100.0f));
        add_f(d, o, "ambience", clampf(fget(p, "ambience", 8.0f), -100.0f, 100.0f));
        add_s(d, o, "texture", enum_or(sget(p, "texture", "none"), TEXTURES, 6, "none"));
        add_f(d, o, "height", clampf(fget(p, "height", 50.0f), 0.0f, 100.0f));
        add_b(d, o, "whiteIsHigh", bget(p, "whiteIsHigh", true));
    } else if (strcmp(id, "filter.render.relight") == 0) {
        add_s(d, o, "filter", "relight");
        add_f(d, o, "angle", clampf(fget(p, "angle", 45.0f), -180.0f, 180.0f));
        add_f(d, o, "elevation", clampf(fget(p, "elevation", 40.0f), 0.0f, 90.0f));
        add_f(d, o, "intensity", clampf(fget(p, "intensity", 40.0f), 0.0f, 100.0f));
        add_f(d, o, "ambient", clampf(fget(p, "ambient", 55.0f), 0.0f, 100.0f));
        add_f(d, o, "warmth", clampf(fget(p, "warmth", 0.0f), -100.0f, 100.0f));
        add_f(d, o, "softness", clampf(fget(p, "softness", 25.0f), 1.0f, 100.0f));
    }
    /* ---- Noise ---- */
    else if (strcmp(id, "filter.noise.reduceNoise") == 0) {
        add_s(d, o, "filter", "reduceNoise");
        add_f(d, o, "strength", clampf(fget(p, "strength", 6.0f), 0.0f, 10.0f));
        add_f(d, o, "preserveDetails", clampf(fget(p, "preserveDetails", 60.0f), 0.0f, 100.0f));
        add_f(d, o, "reduceColorNoise", clampf(fget(p, "reduceColorNoise", 45.0f), 0.0f, 100.0f));
        add_f(d, o, "sharpenDetails", clampf(fget(p, "sharpenDetails", 25.0f), 0.0f, 100.0f));
        add_b(d, o, "removeJpegArtifact", bget(p, "removeJpegArtifact", false));
    }
    /* ---- Blur ---- */
    else if (strcmp(id, "filter.blur.smartBlur") == 0) {
        add_s(d, o, "filter", "smartBlur");
        add_f(d, o, "radius", clampf(fget(p, "radius", 3.0f), 0.1f, 100.0f));
        add_f(d, o, "threshold", clampf(fget(p, "threshold", 25.0f), 0.1f, 100.0f));
        add_s(d, o, "quality", enum_or(sget(p, "quality", "high"), SMART_QUALITIES, 3, "high"));
        add_s(d, o, "mode", enum_or(sget(p, "mode", "normal"), SMART_MODES, 3, "normal"));
    } else if (strcmp(id, "filter.blur.lensBlur") == 0) {
        add_s(d, o, "filter", "lensBlur");
        add_f(d, o, "radius", clampf(fget(p, "radius", 15.0f), 0.0f, 100.0f));
        const char *shape = sget(p, "shape", "hexagon");
        uint32_t blades = 6;
        if (strcmp(shape, "triangle") == 0) {
            blades = 3;
        } else if (strcmp(shape, "square") == 0) {
            blades = 4;
        } else if (strcmp(shape, "pentagon") == 0) {
            blades = 5;
        } else if (strcmp(shape, "heptagon") == 0) {
            blades = 7;
        } else if (strcmp(shape, "octagon") == 0) {
            blades = 8;
        }
        add_u(d, o, "blades", blades);
        add_f(d, o, "curvature", clampf(fget(p, "bladeCurvature", 0.0f), 0.0f, 100.0f));
        add_f(d, o, "rotation", fget(p, "rotation", 0.0f));
        add_s(d, o, "depth", enum_or(sget(p, "depthMap", "none"), DEPTH_SOURCES, 3, "none"));
        add_f(d, o, "focalDistance", clampf(fget(p, "focalDistance", 0.0f), 0.0f, 255.0f));
        add_b(d, o, "invertDepth", bget(p, "invert", false));
        add_f(d, o, "brightness", clampf(fget(p, "brightness", 0.0f), 0.0f, 100.0f));
        add_f(d, o, "threshold", clampf(fget(p, "threshold", 255.0f), 0.0f, 255.0f));
        add_f(d, o, "noise", clampf(fget(p, "noise", 0.0f), 0.0f, 100.0f));
        add_s(d, o, "distribution", enum_or(sget(p, "distribution", "uniform"), DISTRIBUTIONS, 2, "uniform"));
        add_b(d, o, "monochromatic", bget(p, "monochromatic", false));
        add_u(d, o, "seed", seed_of(p));
    } else if (strcmp(id, "filter.blur.shapeBlur") == 0) {
        add_s(d, o, "filter", "shapeBlur");
        add_f(d, o, "radius", clampf(fget(p, "radius", 10.0f), 5.0f, 1000.0f));
        add_s(d, o, "shape", enum_or(sget(p, "shape", "circle"), BLUR_SHAPES, 9, "circle"));
    }
    /* ---- Blur Gallery ---- */
    else if (strcmp(id, "filter.blurGallery.tiltShift") == 0) {
        add_s(d, o, "filter", "tiltShift");
        add_f(d, o, "blur", blur_of(p, 15.0f));
        add_f(d, o, "centerX", fget(p, "centerX", 0.5f));
        add_f(d, o, "centerY", fget(p, "centerY", 0.5f));
        add_f(d, o, "angle", fget(p, "angle", 0.0f));
        add_f(d, o, "focus", clampf(fget(p, "focus", 0.1f), 0.0f, 2.0f));
        add_f(d, o, "transition", clampf(fget(p, "transition", 0.15f), 0.001f, 2.0f));
    } else if (strcmp(id, "filter.blurGallery.irisBlur") == 0) {
        add_s(d, o, "filter", "irisBlur");
        yyjson_mut_val *pins = user_array(d, p, "pins");
        if (pins == NULL) {
            pins = single_iris(d, p);
        }
        yyjson_mut_obj_add_val(d, o, "pins", pins);
    } else if (strcmp(id, "filter.blurGallery.fieldBlur") == 0) {
        add_s(d, o, "filter", "fieldBlur");
        yyjson_mut_val *pins = user_array(d, p, "pins");
        if (pins == NULL) {
            pins = single_field(d, p);
        }
        yyjson_mut_obj_add_val(d, o, "pins", pins);
    } else if (strcmp(id, "filter.blurGallery.spinBlur") == 0) {
        add_s(d, o, "filter", "spinBlur");
        yyjson_mut_val *pins = user_array(d, p, "pins");
        if (pins != NULL) {
            clamp_spin_angles(pins);
        } else {
            pins = single_spin(d, p);
        }
        yyjson_mut_obj_add_val(d, o, "pins", pins);
    } else if (strcmp(id, "filter.blurGallery.pathBlur") == 0) {
        add_s(d, o, "filter", "pathBlur");
        yyjson_mut_val *paths = user_array(d, p, "paths");
        if (paths != NULL) {
            clamp_path_speeds(paths);
        } else {
            paths = single_path(d, p);
        }
        yyjson_mut_obj_add_val(d, o, "paths", paths);
    }
    /* ---- Other ---- */
    else if (strcmp(id, "filter.other.custom") == 0) {
        add_s(d, o, "filter", "custom");
        yyjson_mut_val *kern = yyjson_mut_arr(d);
        yyjson_val *uk = pc_json_get(p, "kernel");
        if (uk != NULL && yyjson_is_arr(uk)) {
            size_t n = yyjson_arr_size(uk);
            if (n > 25) {
                n = 25;
            }
            for (size_t i = 0; i < n; i++) {
                yyjson_val *e = yyjson_arr_get(uk, i);
                double v = 0.0;
                (void)pc_json_as_f64(e, &v);
                yyjson_mut_arr_add_real(d, kern, (double)clampf((float)v, -999.0f, 999.0f));
            }
        } else {
            for (int k = 0; k < 25; k++) {
                yyjson_mut_arr_add_real(d, kern, k == 12 ? 1.0 : 0.0);
            }
        }
        yyjson_mut_obj_add_val(d, o, "kernel", kern);
        add_f(d, o, "scale", clampf(fget(p, "scale", 1.0f), 1.0f, 9999.0f));
        add_f(d, o, "offset", clampf(fget(p, "offset", 0.0f), -9999.0f, 9999.0f));
    } else if (strcmp(id, "filter.other.hsbHsl") == 0) {
        add_s(d, o, "filter", "hsbHsl");
        add_s(d, o, "input", enum_or(sget(p, "inputMode", "rgb"), HSB_MODELS, 3, "rgb"));
        add_s(d, o, "output", enum_or(sget(p, "rowOrder", "hsb"), HSB_MODELS, 3, "hsb"));
    }
    /* ---- Video ---- */
    else if (strcmp(id, "filter.video.deInterlace") == 0) {
        add_s(d, o, "filter", "deInterlace");
        add_b(d, o, "eliminateEven", strcmp(sget(p, "eliminate", "oddFields"), "evenFields") == 0);
        add_b(d, o, "interpolate", strcmp(sget(p, "createBy", "interpolation"), "duplication") != 0);
    } else if (strcmp(id, "filter.video.ntscColors") == 0) {
        add_s(d, o, "filter", "ntscColors");
    } else {
        /* Not an ext filter: fall through to the Filter Gallery (filters_ext.rs's
         * `_ => gallery_cmds::params_for`). gallery leaves *found false for ids it does not own. */
        yyjson_mut_doc_free(d);
        return pc_engine_gallery_params_for(id, p, out, found, err);
    }

    return ext_finalize(d, o, out, err);
}

/* ---- choice validation (chained from filters_validate.inc) -------------------------------- */

static bool choice_has(const char *choices, const char *s, size_t slen)
{
    const char *c = choices;
    while (*c != '\0') {
        const char *e = c;
        while (*e != '\0' && *e != '|') {
            e++;
        }
        size_t len = (size_t)(e - c);
        if (len == slen && memcmp(c, s, slen) == 0) {
            return true;
        }
        c = (*e == '|') ? e + 1 : e;
    }
    return false;
}

static pc_status chk_choice(const char *id, yyjson_val *p, const char *key, const char *allowed,
                            pc_error *err)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL) {
        return PC_OK;
    }
    if (yyjson_is_str(v) && choice_has(allowed, yyjson_get_str(v), yyjson_get_len(v))) {
        return PC_OK;
    }
    return pc_engine_err_bad_params(err, id, "`%s` must be one of %s", key, allowed);
}

/* The reference's choices(id): only the documented choice keys are checked; numeric ranges clamp in
 * params_for and prepare injects extra keys (colours), so unknown keys pass. Returns PC_OK for ids
 * with no choice rules. */
pc_status pc_engine_ext_validate(const char *id, yyjson_val *p, pc_error *err)
{
    pc_status rc = PC_OK;
    if (strcmp(id, "filter.pixelate.mezzotint") == 0) {
        rc = chk_choice(id, p, "type",
                        "fineDots|mediumDots|grainyDots|coarseDots|shortLines|mediumLines|longLines|"
                        "shortStrokes|mediumStrokes|longStrokes",
                        err);
    } else if (strcmp(id, "filter.stylize.diffuse") == 0) {
        rc = chk_choice(id, p, "mode", "normal|darkenOnly|lightenOnly|anisotropic", err);
    } else if (strcmp(id, "filter.stylize.extrude") == 0) {
        rc = chk_choice(id, p, "type", "blocks|pyramids", err);
        if (rc == PC_OK) {
            rc = chk_choice(id, p, "depthMode", "random|levelBased", err);
        }
    } else if (strcmp(id, "filter.stylize.tiles") == 0) {
        rc = chk_choice(id, p, "fill", "background|foreground|inverse|unaltered", err);
    } else if (strcmp(id, "filter.stylize.traceContour") == 0) {
        rc = chk_choice(id, p, "edge", "lower|upper", err);
    } else if (strcmp(id, "filter.stylize.wind") == 0) {
        rc = chk_choice(id, p, "method", "wind|blast|stagger", err);
        if (rc == PC_OK) {
            rc = chk_choice(id, p, "direction", "fromRight|fromLeft", err);
        }
    } else if (strcmp(id, "filter.distort.displace") == 0) {
        rc = chk_choice(id, p, "fit", "stretch|tile", err);
        if (rc == PC_OK) {
            rc = chk_choice(id, p, "undefinedAreas", "repeat|wrap", err);
        }
    } else if (strcmp(id, "filter.distort.shear") == 0) {
        rc = chk_choice(id, p, "undefinedAreas", "wrap|repeat", err);
    } else if (strcmp(id, "filter.distort.zigZag") == 0) {
        rc = chk_choice(id, p, "style", "pondRipples|outFromCenter|aroundCenter", err);
    } else if (strcmp(id, "filter.render.lensFlare") == 0) {
        rc = chk_choice(id, p, "lens", "zoom|prime35|prime105|moviePrime", err);
    } else if (strcmp(id, "filter.render.lightingEffects") == 0) {
        rc = chk_choice(id, p, "lightType", "spot|point|infinite", err);
        if (rc == PC_OK) {
            rc = chk_choice(id, p, "texture", "none|red|green|blue|alpha|luminance", err);
        }
    } else if (strcmp(id, "filter.blur.smartBlur") == 0) {
        rc = chk_choice(id, p, "quality", "high|medium|low", err);
        if (rc == PC_OK) {
            rc = chk_choice(id, p, "mode", "normal|edgeOnly|overlayEdge", err);
        }
    } else if (strcmp(id, "filter.blur.lensBlur") == 0) {
        rc = chk_choice(id, p, "shape", "hexagon|triangle|square|pentagon|heptagon|octagon", err);
        if (rc == PC_OK) {
            rc = chk_choice(id, p, "depthMap", "none|transparency|layerMask", err);
        }
        if (rc == PC_OK) {
            rc = chk_choice(id, p, "distribution", "uniform|gaussian", err);
        }
    } else if (strcmp(id, "filter.blur.shapeBlur") == 0) {
        rc = chk_choice(id, p, "shape", "circle|ring|square|diamond|triangle|hexagon|star|heart|cross",
                        err);
    } else if (strcmp(id, "filter.other.hsbHsl") == 0) {
        rc = chk_choice(id, p, "inputMode", "rgb|hsb|hsl", err);
        if (rc == PC_OK) {
            rc = chk_choice(id, p, "rowOrder", "hsb|hsl|rgb", err);
        }
    } else if (strcmp(id, "filter.video.deInterlace") == 0) {
        rc = chk_choice(id, p, "eliminate", "oddFields|evenFields", err);
        if (rc == PC_OK) {
            rc = chk_choice(id, p, "createBy", "interpolation|duplication", err);
        }
    }
    return rc;
}

/* ---- Displace: displacement-map resolution (filters_ext.rs resolve / resolve_map_path) ----- *
 *
 * The map lives outside the recorded params (algo #[serde(skip)]): it is loaded here into
 * caller-owned storage and the filter's params borrow it for the run. */

/* A compose buffer (contiguous RGBA f32) as an owned algo image of 4 channels. *nonempty is false
 * (out untouched) for an empty buffer. */
static pc_status buffer_to_map(const pc_compose_buffer *buf, pc_algo_image *out, bool *nonempty,
                               pc_error *err)
{
    *nonempty = false;
    if (buf->len == 0 || pc_rect_is_empty(buf->rect)) {
        return PC_OK;
    }
    size_t count = buf->len * 4; /* buf->px is [len][4] contiguous */
    float *data = pc_malloc_array(count, sizeof *data);
    if (data == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "building a displacement map");
    }
    memcpy(data, buf->px, count * sizeof *data);
    if (pc_algo_image_wrap(out, buf->rect, 4, data, count, err) != PC_OK) {
        free(data);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    *nonempty = true;
    return PC_OK;
}

/* filters_ext.rs map_from_document: a flattened document (or one of its layers) as an RGBA map. */
static pc_status map_from_document_img(const pc_document *doc, bool has_layer, uint64_t layer,
                                       pc_algo_image *out, bool *nonempty, pc_error *err)
{
    *nonempty = false;
    pc_compose_buffer buf;
    memset(&buf, 0, sizeof buf);
    const pc_layer *l = has_layer ? pc_document_layer(doc, (pc_layer_id)layer) : NULL;
    pc_status rc;
    if (l != NULL) {
        const pc_surface *s = pc_layer_surface(l);
        pc_rect rect = pc_document_bounds(doc);
        if (s != NULL) {
            pc_rect cb = pc_surface_content_bounds(s);
            if (!pc_rect_is_empty(cb)) {
                rect = cb;
            }
        }
        rc = pc_compose_render_layer(l, rect, &buf, err);
    } else {
        /* No layer, or a layer id that is not in the document: flatten the whole document. */
        rc = pc_compose_flatten(doc, &buf, err);
    }
    if (rc == PC_OK) {
        rc = buffer_to_map(&buf, out, nonempty, err);
    }
    pc_compose_buffer_fini(&buf);
    return rc;
}

/* filters_ext.rs map_from_path: Displace's map read from a file (flattened, or its `mapLayer`). */
static pc_status map_from_path_img(const char *path, bool has_layer, uint64_t layer,
                                   pc_algo_image *out, bool *nonempty, pc_error *err)
{
    *nonempty = false;
    pc_buf bytes;
    pc_buf_init(&bytes);
    pc_error fe;
    pc_error_clear(&fe);
    if (pc_file_read(path, SIZE_MAX, &bytes, &fe) != PC_OK) {
        pc_buf_fini(&bytes);
        return pc_fail(err, PC_ERR_IO, "cannot read displacement map `%s`: %s", path, fe.msg);
    }
    pc_document doc;
    pc_error ie;
    pc_error_clear(&ie);
    if (pc_engine_import(path, bytes.data, bytes.len, &doc, &ie) != PC_OK) {
        pc_buf_fini(&bytes);
        return pc_fail(err, PC_ERR_FORMAT, "cannot open displacement map `%s`: %s", path, ie.msg);
    }
    pc_buf_fini(&bytes);
    pc_status rc = map_from_document_img(&doc, has_layer, layer, out, nonempty, err);
    pc_document_fini(&doc);
    return rc;
}

/* The non-empty "mapPath" string, or NULL. */
static const char *displace_map_path(yyjson_val *p)
{
    const char *path = sget(p, "mapPath", NULL);
    return (path != NULL && path[0] != '\0') ? path : NULL;
}

/* The session document named/addressed by the "mapDocument" value (name, saved path, or index),
 * or NULL. */
static const pc_document *find_map_document(pc_engine_session *s, yyjson_val *md)
{
    if (yyjson_is_str(md)) {
        const char *want = yyjson_get_str(md);
        if (want == NULL) {
            return NULL;
        }
        for (size_t i = 0; i < s->ndocs; i++) {
            const pc_document *doc = pc_doc_arc_get(s->docs[i].doc);
            if (doc != NULL && strcmp(pc_str_c(&doc->name), want) == 0) {
                return doc;
            }
            if (s->docs[i].path != NULL && strcmp(s->docs[i].path, want) == 0) {
                return doc;
            }
        }
        return NULL;
    }
    if (yyjson_is_uint(md)) {
        uint64_t k = yyjson_get_uint(md);
        if (k < s->ndocs) {
            return pc_doc_arc_get(s->docs[(size_t)k].doc);
        }
    }
    return NULL;
}

pc_status pc_engine_filters_resolve_map(pc_engine_session *s, pc_algo_filter *f, yyjson_val *p,
                                        bool require, pc_algo_image *map_out, bool *has_map,
                                        pc_error *err)
{
    *has_map = false;
    if (f == NULL || f->def == NULL || strcmp(f->def->tag, "displace") != 0) {
        return PC_OK;
    }
    pc_algo_displace_params *d = f->params;
    if (d == NULL || d->map != NULL) {
        return PC_OK; /* already carries a map */
    }

    bool has_layer = false;
    uint64_t layer = 0;
    yyjson_val *ml = pc_json_get(p, "mapLayer");
    if (ml != NULL && yyjson_is_uint(ml)) {
        has_layer = true;
        layer = yyjson_get_uint(ml);
    }

    /* 1. A Displace imported from PSD carries its embedded two-plane map. */
    if (p != NULL && yyjson_is_obj(p)) {
        pc_json_value jv;
        if (pc_json_value_from_val(p, &jv, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        bool efound = false;
        pc_algo_image emap;
        memset(&emap, 0, sizeof emap);
        pc_status erc = pc_io_displace_embedded_map(&jv, &efound, &emap, err);
        pc_json_value_fini(&jv);
        if (erc != PC_OK) {
            return erc;
        }
        if (efound) {
            *map_out = emap;
            d->map = map_out;
            *has_map = true;
            return PC_OK;
        }
    }

    /* 2. A file on disk (mapPath). */
    const char *path = displace_map_path(p);
    if (path != NULL) {
        bool nonempty = false;
        pc_algo_image img;
        memset(&img, 0, sizeof img);
        pc_status rc = map_from_path_img(path, has_layer, layer, &img, &nonempty, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (!nonempty) {
            return require ? pc_engine_err_other(err, "the displacement map is empty") : PC_OK;
        }
        *map_out = img;
        d->map = map_out;
        *has_map = true;
        return PC_OK;
    }

    /* 3. Another open document (mapDocument); needs the session. */
    if (s != NULL) {
        yyjson_val *md = pc_json_get(p, "mapDocument");
        if (md != NULL) {
            const pc_document *doc = find_map_document(s, md);
            if (doc == NULL) {
                return pc_engine_err_other(err, "displacement map document not found");
            }
            bool nonempty = false;
            pc_algo_image img;
            memset(&img, 0, sizeof img);
            pc_status rc = map_from_document_img(doc, has_layer, layer, &img, &nonempty, err);
            if (rc != PC_OK) {
                return rc;
            }
            if (!nonempty) {
                return require ? pc_engine_err_other(err, "the displacement map is empty") : PC_OK;
            }
            *map_out = img;
            d->map = map_out;
            *has_map = true;
            return PC_OK;
        }
    }

    if (require) {
        return pc_engine_err_other(err, "Displace needs a displacement map: pass `mapPath` or `mapDocument`");
    }
    return PC_OK;
}

/* ---- running ------------------------------------------------------------------------------ */

/* The ids whose rendering reads the foreground/background colours (filters_ext.rs uses_colours). */
static bool ext_uses_colours(const char *id)
{
    return strcmp(id, "filter.render.fibers") == 0 || strcmp(id, "filter.stylize.tiles") == 0 ||
           strcmp(id, "filter.pixelate.pointillize") == 0;
}

static void inject_colour(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, const float c[4])
{
    if (yyjson_mut_obj_get(obj, key) != NULL) {
        return;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (int i = 0; i < 4; i++) {
        yyjson_mut_arr_add_real(d, arr, (double)c[i]);
    }
    yyjson_mut_obj_add_val(d, obj, key, arr);
}

/* Inject the session colours (as filters_ext::prepare does, so Last Filter and smart filters
 * reproduce the result) when the filter reads them, then run through the shared filter runner. */
static pc_status ext_run(pc_engine_session *s, const char *id, yyjson_val *p, pc_json_value *out,
                         pc_error *err)
{
    if (!ext_uses_colours(id)) {
        return pc_engine_run_filter(s, id, p, out, err);
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "filter params");
    }
    yyjson_mut_val *o;
    if (p != NULL && yyjson_is_obj(p)) {
        o = yyjson_val_mut_copy(d, p);
        if (o == NULL || !yyjson_mut_is_obj(o)) {
            o = yyjson_mut_obj(d);
        }
    } else {
        o = yyjson_mut_obj(d);
    }
    inject_colour(d, o, "foreground", s->tools.foreground);
    inject_colour(d, o, "background", s->tools.background);
    yyjson_mut_doc_set_root(d, o);
    yyjson_doc *imm = yyjson_mut_doc_imut_copy(d, NULL);
    yyjson_mut_doc_free(d);
    if (imm == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "filter params");
    }
    pc_status rc = pc_engine_run_filter(s, id, yyjson_doc_get_root(imm), out, err);
    yyjson_doc_free(imm);
    return rc;
}

/* run() thunks: one per command, recovering the id (like filters_cmds.c's FILTER_RUN). */
#define EXT_RUN(fn, cid)                                                                            \
    static pc_status fn(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)      \
    {                                                                                               \
        return ext_run(s, cid, p, out, err);                                                        \
    }

EXT_RUN(run_color_halftone, "filter.pixelate.colorHalftone")
EXT_RUN(run_crystallize, "filter.pixelate.crystallize")
EXT_RUN(run_facet, "filter.pixelate.facet")
EXT_RUN(run_fragment, "filter.pixelate.fragment")
EXT_RUN(run_mezzotint, "filter.pixelate.mezzotint")
EXT_RUN(run_pointillize, "filter.pixelate.pointillize")
EXT_RUN(run_diffuse, "filter.stylize.diffuse")
EXT_RUN(run_extrude, "filter.stylize.extrude")
EXT_RUN(run_oil_paint, "filter.stylize.oilPaint")
EXT_RUN(run_tiles, "filter.stylize.tiles")
EXT_RUN(run_trace_contour, "filter.stylize.traceContour")
EXT_RUN(run_wind, "filter.stylize.wind")
EXT_RUN(run_displace, "filter.distort.displace")
EXT_RUN(run_shear, "filter.distort.shear")
EXT_RUN(run_zigzag, "filter.distort.zigZag")
EXT_RUN(run_fibers, "filter.render.fibers")
EXT_RUN(run_lens_flare, "filter.render.lensFlare")
EXT_RUN(run_lighting, "filter.render.lightingEffects")
EXT_RUN(run_reduce_noise, "filter.noise.reduceNoise")
EXT_RUN(run_smart_blur, "filter.blur.smartBlur")
EXT_RUN(run_lens_blur, "filter.blur.lensBlur")
EXT_RUN(run_shape_blur, "filter.blur.shapeBlur")
EXT_RUN(run_tilt_shift, "filter.blurGallery.tiltShift")
EXT_RUN(run_iris_blur, "filter.blurGallery.irisBlur")
EXT_RUN(run_field_blur, "filter.blurGallery.fieldBlur")
EXT_RUN(run_spin_blur, "filter.blurGallery.spinBlur")
EXT_RUN(run_path_blur, "filter.blurGallery.pathBlur")
EXT_RUN(run_custom, "filter.other.custom")
EXT_RUN(run_hsb_hsl, "filter.other.hsbHsl")
EXT_RUN(run_deinterlace, "filter.video.deInterlace")
EXT_RUN(run_ntsc, "filter.video.ntscColors")

/* ---- Relight: strict validation + a lock-aware enabled predicate (filters_ext.rs) --------- */

static pc_status relight_bad(pc_error *err, const char *msg)
{
    return pc_engine_err_bad_params(err, "filter.render.relight", "%s", msg);
}

static pc_status relight_num(yyjson_val *p, const char *key, float min, float max, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL) {
        return PC_OK;
    }
    char buf[96];
    if (!yyjson_is_num(v)) {
        (void)snprintf(buf, sizeof buf, "\"%s\" must be a number", key);
        return relight_bad(err, buf);
    }
    double x;
    if (!pc_json_as_f64(v, &x) || !isfinite(x)) {
        (void)snprintf(buf, sizeof buf, "\"%s\" must be finite", key);
        return relight_bad(err, buf);
    }
    if ((float)x < min || (float)x > max) {
        (void)snprintf(buf, sizeof buf, "\"%s\" must be in %g..%g", key, (double)min, (double)max);
        return relight_bad(err, buf);
    }
    return PC_OK;
}

static pc_status validate_relight(yyjson_val *p, pc_error *err)
{
    if (p == NULL || yyjson_is_null(p) || !yyjson_is_obj(p)) {
        return relight_bad(err, "params must be a JSON object");
    }
    pc_status rc = relight_num(p, "angle", -180.0f, 180.0f, err);
    if (rc == PC_OK) {
        rc = relight_num(p, "elevation", 0.0f, 90.0f, err);
    }
    if (rc == PC_OK) {
        rc = relight_num(p, "intensity", 0.0f, 100.0f, err);
    }
    if (rc == PC_OK) {
        rc = relight_num(p, "ambient", 0.0f, 100.0f, err);
    }
    if (rc == PC_OK) {
        rc = relight_num(p, "warmth", -100.0f, 100.0f, err);
    }
    if (rc == PC_OK) {
        rc = relight_num(p, "softness", 1.0f, 100.0f, err);
    }
    return rc;
}

static pc_status relight_enabled(const pc_engine_session *s, pc_error *reason)
{
    pc_status rc = pc_engine_filterable(s, reason);
    if (rc != PC_OK) {
        return rc;
    }
    if (pc_engine_edits_channel(s)) {
        return PC_OK;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL || !d->has_active_layer) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_locks locks = pc_document_effective_locks(doc, d->active_layer);
    if (locks.pixels || locks.all) {
        const pc_layer *l = pc_document_layer(doc, d->active_layer);
        return pc_fail(reason, PC_ERR_INVALID, "the layer \"%s\" is locked",
                       l != NULL ? pc_str_c(&l->name) : "");
    }
    return PC_OK;
}

static pc_status run_relight(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_status rc = validate_relight(p, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_run_filter(s, "filter.render.relight", p, out, err);
}

/* ---- specs -------------------------------------------------------------------------------- */

static const char *const MENU_PIXELATE[] = {"Filter", "Pixelate"};
static const char *const MENU_STYLIZE[] = {"Filter", "Stylize"};
static const char *const MENU_DISTORT[] = {"Filter", "Distort"};
static const char *const MENU_RENDER[] = {"Filter", "Render"};
static const char *const MENU_NOISE[] = {"Filter", "Noise"};
static const char *const MENU_BLUR[] = {"Filter", "Blur"};
static const char *const MENU_BLURGAL[] = {"Filter", "Blur Gallery"};
static const char *const MENU_OTHER[] = {"Filter", "Other"};
static const char *const MENU_VIDEO[] = {"Filter", "Video"};

static const pc_engine_command SPECS[] = {
    /* Pixelate */
    {"filter.pixelate.colorHalftone", "Color Halftone\xE2\x80\xA6", MENU_PIXELATE, 2, NULL,
     "{\"maxRadius\":4..127=8,\"channel1\":-360..360=108,\"channel2\":-360..360=162,\"channel3\":-360.."
     "360=90,\"channel4\":-360..360=45}",
     pc_engine_filterable, run_color_halftone, true},
    {"filter.pixelate.crystallize", "Crystallize\xE2\x80\xA6", MENU_PIXELATE, 2, NULL,
     "{\"cellSize\":3..300=10,\"seed\":u32=0}", pc_engine_filterable, run_crystallize, true},
    {"filter.pixelate.facet", "Facet", MENU_PIXELATE, 2, NULL, "{}", pc_engine_filterable, run_facet, true},
    {"filter.pixelate.fragment", "Fragment", MENU_PIXELATE, 2, NULL, "{}", pc_engine_filterable,
     run_fragment, true},
    {"filter.pixelate.mezzotint", "Mezzotint\xE2\x80\xA6", MENU_PIXELATE, 2, NULL,
     "{\"type\":\"fineDots|mediumDots|grainyDots|coarseDots|shortLines|mediumLines|longLines|"
     "shortStrokes|mediumStrokes|longStrokes\",\"seed\":u32=0}",
     pc_engine_filterable, run_mezzotint, true},
    {"filter.pixelate.pointillize", "Pointillize\xE2\x80\xA6", MENU_PIXELATE, 2, NULL,
     "{\"cellSize\":3..300=5,\"seed\":u32=0,\"background\":json}", pc_engine_filterable, run_pointillize,
     true},
    /* Stylize */
    {"filter.stylize.diffuse", "Diffuse\xE2\x80\xA6", MENU_STYLIZE, 2, NULL,
     "{\"mode\":\"normal|darkenOnly|lightenOnly|anisotropic\",\"seed\":u32=0}", pc_engine_filterable,
     run_diffuse, true},
    {"filter.stylize.extrude", "Extrude\xE2\x80\xA6", MENU_STYLIZE, 2, NULL,
     "{\"type\":\"blocks|pyramids\",\"size\":2..255=30,\"depth\":1..255=30,\"depthMode\":\"random|"
     "levelBased\",\"solidFrontFaces\":bool,\"maskIncompleteBlocks\":bool,\"seed\":u32=0}",
     pc_engine_filterable, run_extrude, true},
    {"filter.stylize.oilPaint", "Oil Paint\xE2\x80\xA6", MENU_STYLIZE, 2, NULL,
     "{\"stylization\":0.1..10=4,\"cleanliness\":0..10=5,\"scale\":0.1..10=1,\"bristleDetail\":0..10=5,"
     "\"lighting\":bool=true,\"angle\":-180..180=-60,\"shine\":0..10=1}",
     pc_engine_filterable, run_oil_paint, true},
    {"filter.stylize.tiles", "Tiles\xE2\x80\xA6", MENU_STYLIZE, 2, NULL,
     "{\"count\":1..99=10,\"maxOffset\":1..99=10,\"fill\":\"background|foreground|inverse|unaltered\","
     "\"seed\":u32=0,\"foreground\":json,\"background\":json}",
     pc_engine_filterable, run_tiles, true},
    {"filter.stylize.traceContour", "Trace Contour\xE2\x80\xA6", MENU_STYLIZE, 2, NULL,
     "{\"level\":0..255=128,\"edge\":\"lower|upper\"}", pc_engine_filterable, run_trace_contour, true},
    {"filter.stylize.wind", "Wind\xE2\x80\xA6", MENU_STYLIZE, 2, NULL,
     "{\"method\":\"wind|blast|stagger\",\"direction\":\"fromRight|fromLeft\",\"seed\":u32=0}",
     pc_engine_filterable, run_wind, true},
    /* Distort */
    {"filter.distort.displace", "Displace\xE2\x80\xA6", MENU_DISTORT, 2, NULL,
     "{\"horizontal\":-999..999=10,\"vertical\":-999..999=10,\"fit\":\"stretch|tile\",\"undefinedAreas\":"
     "\"repeat|wrap\",\"mapDocument\":doc,\"mapPath\":text,\"mapLayer\":json}",
     pc_engine_filterable, run_displace, true},
    {"filter.distort.shear", "Shear\xE2\x80\xA6", MENU_DISTORT, 2, NULL,
     "{\"amount\":-100..100=0,\"undefinedAreas\":\"wrap|repeat\",\"points\":json}", pc_engine_filterable,
     run_shear, true},
    {"filter.distort.zigZag", "ZigZag\xE2\x80\xA6", MENU_DISTORT, 2, NULL,
     "{\"amount\":-100..100=10,\"ridges\":0..20=5,\"style\":\"pondRipples|outFromCenter|aroundCenter\"}",
     pc_engine_filterable, run_zigzag, true},
    /* Render */
    {"filter.render.fibers", "Fibers\xE2\x80\xA6", MENU_RENDER, 2, NULL,
     "{\"variance\":1..64=16,\"strength\":1..64=4,\"seed\":u32=0,\"foreground\":json,\"background\":json}",
     pc_engine_filterable, run_fibers, true},
    {"filter.render.lensFlare", "Lens Flare\xE2\x80\xA6", MENU_RENDER, 2, NULL,
     "{\"brightness\":10..300=100,\"centerX\":0..1=0.5,\"centerY\":0..1=0.5,\"lens\":\"zoom|prime35|"
     "prime105|moviePrime\"}",
     pc_engine_filterable, run_lens_flare, true},
    {"filter.render.lightingEffects", "Lighting Effects\xE2\x80\xA6", MENU_RENDER, 2, NULL,
     "{\"lightType\":\"spot|point|infinite\",\"intensity\":-100..100=75,\"lightX\":0..1=0.25,\"lightY\":"
     "0..1=0.2,\"lightZ\":0..2=0.6,\"targetX\":0..1=0.5,\"targetY\":0..1=0.55,\"cone\":1..89=45,"
     "\"hotspot\":0..100=50,\"angle\":-180..180=135,\"elevation\":0..90=45,\"gloss\":-100..100=0,"
     "\"metallic\":-100..100=0,\"exposure\":-100..100=0,\"ambience\":-100..100=8,\"texture\":\"none|red|"
     "green|blue|alpha|luminance\",\"height\":0..100=50,\"whiteIsHigh\":bool=true,\"lights\":json}",
     pc_engine_filterable, run_lighting, true},
    {"filter.render.relight", "Relight\xE2\x80\xA6", MENU_RENDER, 2, NULL,
     "{\"angle\":-180..180=45,\"elevation\":0..90=40,\"intensity\":0..100=40,\"ambient\":0..100=55,"
     "\"warmth\":-100..100=0,\"softness\":1..100=25}",
     relight_enabled, run_relight, true},
    /* Noise */
    {"filter.noise.reduceNoise", "Reduce Noise\xE2\x80\xA6", MENU_NOISE, 2, NULL,
     "{\"strength\":0..10=6,\"preserveDetails\":0..100=60,\"reduceColorNoise\":0..100=45,"
     "\"sharpenDetails\":0..100=25,\"removeJpegArtifact\":bool}",
     pc_engine_filterable, run_reduce_noise, true},
    /* Blur */
    {"filter.blur.smartBlur", "Smart Blur\xE2\x80\xA6", MENU_BLUR, 2, NULL,
     "{\"radius\":0.1..100=3,\"threshold\":0.1..100=25,\"quality\":\"high|medium|low\",\"mode\":\"normal|"
     "edgeOnly|overlayEdge\"}",
     pc_engine_filterable, run_smart_blur, true},
    {"filter.blur.lensBlur", "Lens Blur\xE2\x80\xA6", MENU_BLUR, 2, NULL,
     "{\"radius\":0..100=15,\"shape\":\"hexagon|triangle|square|pentagon|heptagon|octagon\","
     "\"bladeCurvature\":0..100=0,\"rotation\":0..360=0,\"depthMap\":\"none|transparency|layerMask\","
     "\"focalDistance\":0..255=0,\"invert\":bool,\"brightness\":0..100=0,\"threshold\":0..255=255,"
     "\"noise\":0..100=0,\"distribution\":\"uniform|gaussian\",\"monochromatic\":bool,\"seed\":u32=0}",
     pc_engine_filterable, run_lens_blur, true},
    {"filter.blur.shapeBlur", "Shape Blur\xE2\x80\xA6", MENU_BLUR, 2, NULL,
     "{\"radius\":5..1000=10,\"shape\":\"circle|ring|square|diamond|triangle|hexagon|star|heart|cross\"}",
     pc_engine_filterable, run_shape_blur, true},
    /* Blur Gallery */
    {"filter.blurGallery.tiltShift", "Tilt-Shift\xE2\x80\xA6", MENU_BLURGAL, 2, NULL,
     "{\"blur\":0..500=15,\"centerX\":0..1=0.5,\"centerY\":0..1=0.5,\"angle\":-90..90=0,\"focus\":0..1="
     "0.1,\"transition\":0.01..1=0.15}",
     pc_engine_filterable, run_tilt_shift, true},
    {"filter.blurGallery.irisBlur", "Iris Blur\xE2\x80\xA6", MENU_BLURGAL, 2, NULL,
     "{\"blur\":0..500=15,\"centerX\":0..1=0.5,\"centerY\":0..1=0.5,\"radiusX\":0.01..1=0.35,\"radiusY\":"
     "0.01..1=0.25,\"angle\":-180..180=0,\"roundness\":0..100=0,\"feather\":0..0.99=0.5,\"pins\":json}",
     pc_engine_filterable, run_iris_blur, true},
    {"filter.blurGallery.fieldBlur", "Field Blur\xE2\x80\xA6", MENU_BLURGAL, 2, NULL,
     "{\"blur\":0..500=15,\"centerX\":0..1=0.5,\"centerY\":0..1=0.5,\"pins\":json}", pc_engine_filterable,
     run_field_blur, true},
    {"filter.blurGallery.spinBlur", "Spin Blur\xE2\x80\xA6", MENU_BLURGAL, 2, NULL,
     "{\"blurAngle\":0..360=15,\"centerX\":0..1=0.5,\"centerY\":0..1=0.5,\"radiusX\":0.01..1=0.3,"
     "\"radiusY\":0.01..1=0.3,\"angle\":-180..180=0,\"pins\":json}",
     pc_engine_filterable, run_spin_blur, true},
    {"filter.blurGallery.pathBlur", "Path Blur\xE2\x80\xA6", MENU_BLURGAL, 2, NULL,
     "{\"speed\":0..500=50,\"taper\":0..100=0,\"startX\":0..1=0.2,\"startY\":0..1=0.5,\"endX\":0..1=0.8,"
     "\"endY\":0..1=0.5,\"paths\":json}",
     pc_engine_filterable, run_path_blur, true},
    /* Other */
    {"filter.other.custom", "Custom\xE2\x80\xA6", MENU_OTHER, 2, NULL,
     "{\"kernel\":int[25],\"scale\":1..9999=1,\"offset\":-9999..9999=0}", pc_engine_filterable,
     run_custom, true},
    {"filter.other.hsbHsl", "HSB/HSL", MENU_OTHER, 2, NULL,
     "{\"inputMode\":\"rgb|hsb|hsl\",\"rowOrder\":\"hsb|hsl|rgb\"}", pc_engine_filterable, run_hsb_hsl,
     true},
    /* Video */
    {"filter.video.deInterlace", "De-Interlace\xE2\x80\xA6", MENU_VIDEO, 2, NULL,
     "{\"eliminate\":\"oddFields|evenFields\",\"createBy\":\"interpolation|duplication\"}",
     pc_engine_filterable, run_deinterlace, true},
    {"filter.video.ntscColors", "NTSC Colors", MENU_VIDEO, 2, NULL, "{}", pc_engine_filterable, run_ntsc,
     true},
};

const pc_engine_command *pc_engine_filters_ext_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
