/* The Eraser group's colour-aware tools: Magic Eraser (paint.magicEraser) and Background Eraser
 * (paint.backgroundEraser) (port of crates/engine/src/eraser_cmds.rs, E10).
 *
 * The plain Eraser is the Brush tool with erase set (paint.stroke / paint.pencil, brush_cmds).
 * Both tools here turn a Background layer into a normal layer ("Layer 0") first, since the
 * Background can't hold transparency; the Magic Eraser paints the background colour on other
 * layers with locked transparency, the Background Eraser refuses them. Both respect the selection,
 * work in the layer's own colour model and depth, and are one undo step per click / stroke. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "algo/erase.h"
#include "algo/selection.h"
#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "paint/bg_erase.h"
#include "paint/brush.h"
#include "paint/paint.h"
#include "paint/replace.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "brush_cmds.h"
#include "engine_internal.h"

static bool flag(yyjson_val *p, const char *k, bool d)
{
    bool v;
    yyjson_val *x = pc_json_get(p, k);
    return (x != NULL && pc_json_as_bool(x, &v)) ? v : d;
}

/* eraser_cmds::number: absent/null -> default; a finite number clamped to [lo,hi]; else BadParams. */
static pc_status number(yyjson_val *p, const char *cmd, const char *k, double def, double lo, double hi,
                        double *out, pc_error *err)
{
    yyjson_val *x = pc_json_get(p, k);
    if (x == NULL || yyjson_is_null(x)) {
        *out = def;
        return PC_OK;
    }
    double v;
    if (!pc_json_as_f64(x, &v) || !isfinite(v)) {
        return pc_engine_err_bad_params(err, cmd, "`%s` must be a number", k);
    }
    *out = v < lo ? lo : (v > hi ? hi : v);
    return PC_OK;
}

static pc_status sampling_param(yyjson_val *p, const char *cmd, pc_paint_sampling *out, pc_error *err)
{
    yyjson_val *x = pc_json_get(p, "sampling");
    if (x == NULL || yyjson_is_null(x)) {
        *out = PC_PAINT_SAMPLING_CONTINUOUS;
        return PC_OK;
    }
    size_t len = 0;
    const char *s = pc_json_as_str(x, &len);
    if (s != NULL && len == 10 && memcmp(s, "continuous", 10) == 0) {
        *out = PC_PAINT_SAMPLING_CONTINUOUS;
    } else if (s != NULL && len == 4 && memcmp(s, "once", 4) == 0) {
        *out = PC_PAINT_SAMPLING_ONCE;
    } else if (s != NULL && len == 16 && memcmp(s, "backgroundSwatch", 16) == 0) {
        *out = PC_PAINT_SAMPLING_BACKGROUND_SWATCH;
    } else {
        return pc_engine_err_bad_params(err, cmd, "`sampling` must be one of continuous|once|backgroundSwatch");
    }
    return PC_OK;
}

static pc_status limits_param(yyjson_val *p, const char *cmd, pc_paint_limits *out, pc_error *err)
{
    yyjson_val *x = pc_json_get(p, "limits");
    if (x == NULL || yyjson_is_null(x)) {
        *out = PC_PAINT_LIMITS_CONTIGUOUS;
        return PC_OK;
    }
    size_t len = 0;
    const char *s = pc_json_as_str(x, &len);
    if (s != NULL && len == 13 && memcmp(s, "discontiguous", 13) == 0) {
        *out = PC_PAINT_LIMITS_DISCONTIGUOUS;
    } else if (s != NULL && len == 10 && memcmp(s, "contiguous", 10) == 0) {
        *out = PC_PAINT_LIMITS_CONTIGUOUS;
    } else if (s != NULL && len == 9 && memcmp(s, "findEdges", 9) == 0) {
        *out = PC_PAINT_LIMITS_FIND_EDGES;
    } else {
        return pc_engine_err_bad_params(err, cmd, "`limits` must be one of discontiguous|contiguous|findEdges");
    }
    return PC_OK;
}

/* The active layer must hold pixels. */
static pc_status has_pixel_layer(const pc_engine_session *s, pc_error *reason)
{
    const pc_layer *l = pc_engine_active_layer_of(s, reason);
    if (l == NULL) {
        return PC_ERR_INVALID;
    }
    if (l->content.kind == PC_LAYER_RASTER) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "active layer is %s %s layer, not a pixel layer",
                   pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
}

static pc_status active_pixel_layer(pc_engine_session *s, const char *cmd, pc_layer_id *out, pc_error *err)
{
    pc_error why = {0};
    if (has_pixel_layer(s, &why) != PC_OK) {
        return pc_engine_err_bad_params(err, cmd, "%s", why.msg);
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL || !d->has_active_layer) {
        return pc_engine_err_bad_params(err, cmd, "no active layer");
    }
    *out = d->active_layer;
    return PC_OK;
}

static pc_status check_pixels_unlocked(const pc_document *doc, pc_layer_id id, pc_error *err)
{
    pc_locks locks = pc_document_effective_locks(doc, id);
    if (locks.pixels || locks.all) {
        const pc_layer *l = pc_document_layer(doc, id);
        return pc_engine_err_other(err, "Could not complete your request because the layer \"%s\" is locked",
                                   l != NULL ? pc_str_c(&l->name) : "");
    }
    return PC_OK;
}

/* extra_cmds::background_to_layer: if id is the Background, turn it into "Layer 0" (unlocked). */
static void background_to_layer(pc_document *doc, pc_layer_id id)
{
    pc_layer *bg = pc_document_background_mut(doc);
    if (bg != NULL && bg->id == id) {
        (void)pc_str_set_c(&bg->name, "Layer 0", NULL);
        bg->locks.transparency = false;
        bg->locks.position = false;
    }
}

static pc_status damage_result(pc_engine_session *s, pc_rect dmg, bool erased, pc_json_value *out,
                               pc_error *err)
{
    if (!pc_rect_is_empty(dmg)) {
        pc_engine_doc_state *st = pc_engine_session_active_mut(s);
        if (st != NULL) {
            st->last_damage = dmg;
            st->has_last_damage = true;
        }
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, root, "erased", erased);
    yyjson_mut_val *a = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, a, dmg.x0);
    yyjson_mut_arr_add_int(d, a, dmg.y0);
    yyjson_mut_arr_add_int(d, a, (int64_t)pc_rect_width(dmg));
    yyjson_mut_arr_add_int(d, a, (int64_t)pc_rect_height(dmg));
    yyjson_mut_obj_add_val(d, root, "damage", a);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ========================================================================================== */
/* Magic Eraser                                                                                */
/* ========================================================================================== */

struct magic_ctx {
    pc_layer_id id;
    const pc_algo_region *region;
    float amount;
    float bg[4];
    pc_rect damage;
};

static pc_status magic_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct magic_ctx *c = vctx;
    if (pc_document_layer(doc, c->id) == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    background_to_layer(doc, c->id);
    bool lock = pc_document_effective_locks(doc, c->id).transparency;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    pc_surface *surf = pc_layer_surface_mut(l);
    if (surf == NULL) {
        return pc_engine_err_other(err, "not a pixel layer");
    }
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    float lock_buf[PC_RASTER_MAX_CHANNELS];
    const float *lock_color = NULL;
    size_t lock_len = 0;
    if (lock) {
        pc_pixel_format fmt = pc_surface_format(surf);
        float rgba[4] = {c->bg[0], c->bg[1], c->bg[2], 1.0f};
        lock_len = pc_from_rgba(fmt, rgba, lock_buf);
        lock_color = lock_buf;
    }
    return pc_algo_erase_magic(surf, c->region, c->amount, sel, lock_color, lock_len, &c->damage, err);
}

static pc_status run_magic(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "paint.magicEraser";
    double dx, dy;
    yyjson_val *xv = pc_json_get(p, "x"), *yv = pc_json_get(p, "y");
    if (xv == NULL || !pc_json_as_f64(xv, &dx) || !isfinite(dx) || fabs(dx) >= 1e9) {
        return pc_engine_err_bad_params(err, cmd, "missing or invalid `x`");
    }
    if (yv == NULL || !pc_json_as_f64(yv, &dy) || !isfinite(dy) || fabs(dy) >= 1e9) {
        return pc_engine_err_bad_params(err, cmd, "missing or invalid `y`");
    }
    int32_t x = (int32_t)floor(dx), y = (int32_t)floor(dy);
    double tolerance, opacity;
    pc_status rc = number(p, cmd, "tolerance", 32.0, 0.0, 255.0, &tolerance, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = number(p, cmd, "opacity", 100.0, 1.0, 100.0, &opacity, err);
    if (rc != PC_OK) {
        return rc;
    }
    bool anti_alias = flag(p, "antiAlias", true);
    bool contiguous = flag(p, "contiguous", true);
    bool sample_all = flag(p, "sampleAllLayers", false);
    pc_layer_id id = 0;
    rc = active_pixel_layer(s, cmd, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    rc = check_pixels_unlocked(doc, id, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!pc_rect_contains(pc_document_bounds(doc), x, y)) {
        return damage_result(s, PC_RECT_EMPTY, false, out, err); /* off-canvas: nothing erased */
    }
    pc_rect area;
    uint8_t (*img)[4] = NULL;
    size_t ilen = 0;
    rc = pc_engine_sample_rgba8(s, sample_all, &area, &img, &ilen, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_algo_region region = {0};
    rc = pc_algo_selection_wand_region((const uint8_t (*)[4])img, ilen, area, x, y, (float)tolerance,
                                       contiguous, anti_alias, &region, err);
    free(img);
    if (rc != PC_OK) {
        pc_algo_region_fini(&region);
        return rc;
    }
    if (pc_algo_region_is_none(&region)) {
        pc_algo_region_fini(&region);
        return damage_result(s, PC_RECT_EMPTY, false, out, err);
    }
    struct magic_ctx ctx = {0};
    ctx.id = id;
    ctx.region = &region;
    ctx.amount = (float)(opacity / 100.0);
    memcpy(ctx.bg, s->tools.background, sizeof ctx.bg);
    ctx.damage = PC_RECT_EMPTY;
    rc = pc_engine_session_edit(s, "Magic Eraser", magic_edit, &ctx, err);
    pc_algo_region_fini(&region);
    if (rc != PC_OK) {
        return rc;
    }
    return damage_result(s, ctx.damage, true, out, err);
}

/* ========================================================================================== */
/* Background Eraser                                                                           */
/* ========================================================================================== */

struct bg_ctx {
    pc_layer_id id;
    const pc_paint_stroke *stroke;
    const pc_paint_bg_erase_settings *bs;
    pc_rect damage;
};

static pc_status bg_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct bg_ctx *c = vctx;
    background_to_layer(doc, c->id);
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    pc_surface *surf = pc_layer_surface_mut(l);
    if (surf == NULL) {
        return pc_engine_err_other(err, "not a pixel layer");
    }
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    return pc_paint_apply_background_eraser(surf, c->stroke, c->bs, sel, &c->damage, err);
}

static pc_status run_background(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "paint.backgroundEraser";
    pc_paint_point *pts = NULL;
    size_t npts = 0;
    pc_status rc = pc_engine_parse_points(p, cmd, &pts, &npts, err);
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t i = 0; i < npts; i++) {
        if (!isfinite(pts[i].x) || !isfinite(pts[i].y) || fabs(pts[i].x) > 1e7 || fabs(pts[i].y) > 1e7) {
            free(pts);
            return pc_engine_err_bad_params(err, cmd, "`points` must be finite canvas coordinates");
        }
    }
    pc_paint_brush brush = {0};
    rc = pc_engine_resolve_brush(s, p, cmd, &brush, err);
    if (rc != PC_OK) {
        free(pts);
        return rc;
    }
    pc_paint_bg_erase_settings bs;
    pc_paint_bg_erase_settings_default(&bs);
    rc = sampling_param(p, cmd, &bs.sampling, err);
    if (rc == PC_OK) {
        rc = limits_param(p, cmd, &bs.limits, err);
    }
    double tol = 50.0;
    if (rc == PC_OK) {
        rc = number(p, cmd, "tolerance", 50.0, 0.0, 100.0, &tol, err);
    }
    if (rc != PC_OK) {
        pc_paint_brush_fini(&brush);
        free(pts);
        return rc;
    }
    bs.tolerance = (float)(tol / 100.0);
    bs.protect_foreground = flag(p, "protectForegroundColor", false);
    memcpy(bs.foreground, s->tools.foreground, sizeof bs.foreground);
    memcpy(bs.background, s->tools.background, sizeof bs.background);

    pc_layer_id id = 0;
    rc = active_pixel_layer(s, cmd, &id, err);
    if (rc != PC_OK) {
        pc_paint_brush_fini(&brush);
        free(pts);
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        pc_paint_brush_fini(&brush);
        free(pts);
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    rc = check_pixels_unlocked(doc, id, err);
    if (rc != PC_OK) {
        pc_paint_brush_fini(&brush);
        free(pts);
        return rc;
    }
    if (pc_document_effective_locks(doc, id).transparency && !pc_document_is_background(doc, id)) {
        const pc_layer *l = pc_document_layer(doc, id);
        pc_paint_brush_fini(&brush);
        free(pts);
        return pc_engine_err_other(
            err, "Could not use the Background Eraser because the transparency of layer \"%s\" is locked",
            l != NULL ? pc_str_c(&l->name) : "");
    }

    pc_paint_stroke stroke = {0};
    stroke.brush = brush; /* moved */
    stroke.points = pts;
    stroke.count = npts;
    struct bg_ctx ctx = {0};
    ctx.id = id;
    ctx.stroke = &stroke;
    ctx.bs = &bs;
    ctx.damage = PC_RECT_EMPTY;
    rc = pc_engine_session_edit(s, "Background Eraser", bg_edit, &ctx, err);
    pc_paint_brush_fini(&brush);
    free(pts);
    if (rc != PC_OK) {
        return rc;
    }
    return damage_result(s, ctx.damage, true, out, err);
}

/* ========================================================================================== */
/* specs                                                                                       */
/* ========================================================================================== */

static const pc_engine_command SPECS[] = {
    {"paint.magicEraser", "Magic Eraser", NULL, 0, NULL,
     "{\"x\":px,\"y\":px,\"tolerance\":0..255=32,\"antiAlias\":bool=true,\"contiguous\":bool=true,"
     "\"sampleAllLayers\":bool=false,\"opacity\":1..100=100}",
     has_pixel_layer, run_magic, true},
    {"paint.backgroundEraser", "Background Eraser", NULL, 0, NULL,
     "{\"points\":[[x,y,pressure?],...],\"size\":px?,\"hardness\":0..1?,\"brush\":{...}?,\"preset\":name?,"
     "\"sampling\":\"continuous|once|backgroundSwatch\"=\"continuous\",\"limits\":\"discontiguous|"
     "contiguous|findEdges\"=\"contiguous\",\"tolerance\":0..100=50,\"protectForegroundColor\":bool=false,"
     "\"seed\":u64?}",
     has_pixel_layer, run_background, true},
};

const pc_engine_command *pc_engine_eraser_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
