/* Retouching tools as commands (port of crates/engine/src/retouch_cmds.rs, E10-retouch): Clone
 * Stamp, Pattern Stamp, Healing Brush, Spot Healing Brush, Dodge, Burn, Sponge, Blur, Sharpen,
 * Smudge and History Brush.
 *
 * Every command takes a Photoshop-style brush (`points`, `size`, `hardness`, `opacity`, `flow`,
 * `spacing`, `layer`), respects the active selection as a mask and the layer's transparency lock,
 * and works at any depth (8/16/32f) and colour model. Like the Brush, each paints the targeted
 * surface (layer pixels, the layer mask, an alpha channel or the Quick Mask). The pixel
 * algorithms live in src/algo (poisson, inpaint, retouch) and the dab machinery in
 * src/paint/retouch.c; this module only parses parameters and wires them together. Never crashes
 * on hostile params (clamp/bound everything; LiveStroke/LiveCommit previews are not ported, so a
 * commit renders the stroke fresh, matching the reference).
 *
 * Patch, Content-Aware Move and Remove (background jobs) are deferred to a job-command-path
 * follow-up (REWRITE_PLAN.md §8). */
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "algo/inpaint.h"
#include "algo/poisson.h"
#include "algo/retouch.h"
#include "base/buf.h"
#include "base/json.h"
#include "color/blend.h"
#include "color/color.h"
#include "compose/compose.h"
#include "compose/pattern.h"
#include "doc/doc.h"
#include "doc/pattern.h"
#include "paint/brush.h"
#include "paint/render.h"
#include "paint/retouch.h"
#include "paint/rng.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "brush_cmds.h"
#include "channel_cmds.h"
#include "engine_internal.h"
#include "fill_cmds.h"
#include "pattern_cmds.h"
#include "presets.h"

/* ---- small param helpers (retouch_cmds.rs num/flag/string/point) -------------------------- */

typedef enum sample_layers { SAMPLE_CURRENT, SAMPLE_CURRENT_AND_BELOW, SAMPLE_ALL } sample_layers;

static float num_p(yyjson_val *p, const char *k, float d)
{
    yyjson_val *x = pc_json_get(p, k);
    double v;
    return (x != NULL && pc_json_as_f64(x, &v)) ? (float)v : d;
}

static bool flag_p(yyjson_val *p, const char *k, bool d)
{
    yyjson_val *x = pc_json_get(p, k);
    bool v;
    return (x != NULL && pc_json_as_bool(x, &v)) ? v : d;
}

static const char *string_p(yyjson_val *p, const char *k, const char *d, size_t *len)
{
    yyjson_val *x = pc_json_get(p, k);
    size_t l = 0;
    const char *s = (x != NULL) ? pc_json_as_str(x, &l) : NULL;
    if (s == NULL) {
        *len = strlen(d);
        return d;
    }
    *len = l;
    return s;
}

/* [x,y] number pair from param k (retouch_cmds::point). */
static bool point_p(yyjson_val *p, const char *k, double *x, double *y)
{
    yyjson_val *a = pc_json_get(p, k);
    if (a == NULL || !yyjson_is_arr(a) || yyjson_arr_size(a) < 2) {
        return false;
    }
    return pc_json_as_f64(yyjson_arr_get(a, 0), x) && pc_json_as_f64(yyjson_arr_get(a, 1), y);
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- brush + stroke parsing (stroke_brush / parse_brush) ---------------------------------- */

/* The brush a retouching stroke paints with: the tool's brush with the shared params applied. *out
 * is overwritten (not finalised); the caller pc_paint_brush_fini's it. */
static void stroke_brush(const pc_engine_session *s, yyjson_val *p, pc_paint_brush *out)
{
    pc_paint_brush_copy(out, &s->tools.brush);
    float size = num_p(p, "size", out->size);
    out->size = size < 1.0f ? 1.0f : size;
    out->hardness = clampf(num_p(p, "hardness", out->hardness * 100.0f), 0.0f, 100.0f) / 100.0f;
    out->opacity = clampf(num_p(p, "opacity", 100.0f), 1.0f, 100.0f) / 100.0f;
    out->flow = clampf(num_p(p, "flow", 100.0f), 1.0f, 100.0f) / 100.0f;
    out->spacing = clampf(num_p(p, "spacing", out->spacing * 100.0f), 1.0f, 1000.0f) / 100.0f;
    out->erase = false;
}

/* Parse the shared brush params into a stroke (points + brush) on a layer. has_layer is false when
 * an alpha channel or the Quick Mask is targeted. *stroke is left finalise-able (pc_paint_stroke_fini). */
static pc_status parse_brush(pc_engine_session *s, yyjson_val *p, const char *cmd, pc_paint_stroke *stroke,
                             bool *has_layer, pc_layer_id *layer, pc_error *err)
{
    pc_paint_point *pts = NULL;
    size_t npts = 0;
    pc_status rc = pc_engine_parse_points(p, cmd, &pts, &npts, err); /* missing/empty + check_coords */
    if (rc != PC_OK) {
        return rc;
    }
    pc_paint_brush brush = {0};
    stroke_brush(s, p, &brush);
    rc = pc_engine_validate_brush(&brush, cmd, err);
    if (rc != PC_OK) {
        pc_paint_brush_fini(&brush);
        free(pts);
        return rc;
    }
    if (pc_engine_is_channel_target(p)) {
        *has_layer = false;
        *layer = 0;
    } else {
        uint64_t id;
        yyjson_val *lv = pc_json_get(p, "layer");
        if (lv != NULL && pc_json_as_u64(lv, &id)) {
            *has_layer = true;
            *layer = (pc_layer_id)id;
        } else {
            const pc_engine_doc_state *d = pc_engine_session_active(s);
            if (d == NULL || !d->has_active_layer) {
                pc_paint_brush_fini(&brush);
                free(pts);
                return pc_engine_err_other(err, "no active layer");
            }
            *has_layer = true;
            *layer = d->active_layer;
        }
    }
    stroke->brush = brush; /* moved */
    stroke->points = pts;
    stroke->count = npts;
    return PC_OK;
}

static pc_status sample_layers_of(yyjson_val *p, const char *cmd, sample_layers *out, pc_error *err)
{
    size_t len = 0;
    const char *s = string_p(p, "sampleLayer", "current", &len);
    if (len == 7 && memcmp(s, "current", 7) == 0) {
        *out = SAMPLE_CURRENT;
    } else if (len == 15 && memcmp(s, "currentAndBelow", 15) == 0) {
        *out = SAMPLE_CURRENT_AND_BELOW;
    } else if (len == 3 && memcmp(s, "all", 3) == 0) {
        *out = SAMPLE_ALL;
    } else {
        return pc_engine_err_bad_params(err, cmd, "unknown sampleLayer `%.*s` (current|currentAndBelow|all)",
                                        (int)len, s);
    }
    return PC_OK;
}

static pc_status blend_param(yyjson_val *p, const char *cmd, pc_blend_mode *out, pc_error *err)
{
    yyjson_val *mv = pc_json_get(p, "mode");
    size_t len = 0;
    const char *m = (mv != NULL) ? pc_json_as_str(mv, &len) : NULL;
    if (m == NULL) {
        *out = PC_BLEND_NORMAL; /* absent or non-string -> Normal */
        return PC_OK;
    }
    if (!pc_engine_blend_from_str(m, len, out)) {
        return pc_engine_err_bad_params(err, cmd, "unknown blend mode `%.*s`", (int)len, m);
    }
    return PC_OK;
}

/* Clamp results into the storable range: integer depths to 0..1, float depths only below at 0;
 * alpha always 0..1 (retouch_cmds::clamp_samples). */
static void clamp_samples(pc_pixel_format fmt, float *data, size_t len)
{
    size_t n = pc_pixel_format_channels(fmt);
    if (n == 0) {
        return;
    }
    size_t a = SIZE_MAX;
    (void)pc_paint_alpha_index(fmt, &a);
    bool is_float = fmt.sample == PC_SAMPLE_TYPE_F32;
    for (size_t i = 0; i + n <= len; i += n) {
        for (size_t c = 0; c < n; c++) {
            float *v = &data[i + c];
            if (c == a || !is_float) {
                *v = clampf(*v, 0.0f, 1.0f);
            } else if (*v < 0.0f) {
                *v = 0.0f;
            }
        }
    }
}

/* ---- sampling sources (composite_region / sample / clone_sample) -------------------------- */

static pc_status composite_region(const pc_document *pre, bool has_layer, pc_layer_id layer,
                                  sample_layers which, pc_rect rect, pc_pixel_format fmt,
                                  pc_paint_region *out, pc_error *err)
{
    pc_document cdoc = {0};
    pc_status rc = pc_document_clone(pre, &cdoc, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (which == SAMPLE_CURRENT_AND_BELOW && has_layer) {
        pc_walk w = {0};
        if (pc_document_walk(&cdoc, &w, err) == PC_OK) {
            size_t pos = SIZE_MAX;
            for (size_t i = 0; i < w.len; i++) {
                if (w.items[i].layer->id == layer) {
                    pos = i;
                    break;
                }
            }
            if (pos != SIZE_MAX) {
                for (size_t i = pos + 1; i < w.len; i++) {
                    const size_t *path = &w.paths[w.items[i].path_start];
                    pc_layer *l = pc_document_layer_at_mut(&cdoc, path, w.items[i].depth + 1);
                    if (l != NULL) {
                        l->visible = false;
                    }
                }
            }
        }
        pc_walk_fini(&w);
    }
    pc_compose_buffer buf = {0};
    rc = pc_compose_render(&cdoc, rect, &buf, err);
    pc_document_fini(&cdoc);
    if (rc != PC_OK) {
        return rc;
    }
    size_t n = pc_pixel_format_channels(fmt);
    rc = pc_paint_region_new(rect, n, out, err);
    if (rc != PC_OK) {
        pc_compose_buffer_fini(&buf);
        return rc;
    }
    float enc[PC_RASTER_MAX_CHANNELS];
    size_t count = (n > 0) ? out->len / n : 0;
    if (count > buf.len) {
        count = buf.len;
    }
    for (size_t i = 0; i < count; i++) {
        pc_from_rgba(fmt, buf.px[i], enc);
        memcpy(&out->data[i * n], enc, n * sizeof(float));
    }
    pc_compose_buffer_fini(&buf);
    return PC_OK;
}

static pc_status region_sample(const pc_document *pre, bool has_layer, pc_layer_id layer,
                              const pc_surface *surf, sample_layers which, pc_rect rect,
                              pc_paint_region *out, pc_error *err)
{
    if (which == SAMPLE_CURRENT) {
        return pc_paint_region_read(surf, rect, out, err);
    }
    return composite_region(pre, has_layer, layer, which, rect, pc_surface_format(surf), out, err);
}

/* Source pixels for destination `rect`: a translated read, or a bilinear resample. */
static pc_status clone_sample(const pc_document *pre, bool has_layer, pc_layer_id layer,
                             const pc_surface *surf, sample_layers which, pc_rect rect,
                             const pc_clone_mapping *map, const char *cmd, pc_paint_region *out, pc_error *err)
{
    if (pc_engine_clone_is_translation(map)) {
        int32_t dx, dy;
        pc_engine_clone_offset(map, &dx, &dy);
        pc_rect src = pc_rect_translate(rect, dx, dy);
        if (pc_rect_width(src) != pc_rect_width(rect) || pc_rect_height(src) != pc_rect_height(rect)) {
            return pc_engine_err_bad_params(
                err, cmd, "source offset [%d, %d] is too far: the sample window leaves the coordinate range",
                dx, dy);
        }
        pc_status rc = region_sample(pre, has_layer, layer, surf, which, src, out, err);
        if (rc != PC_OK) {
            return rc;
        }
        out->rect = rect; /* relabel (same width/height) */
        return PC_OK;
    }
    pc_rect srect = pc_engine_clone_source_rect(map, rect);
    pc_paint_region src = {0};
    pc_status rc = region_sample(pre, has_layer, layer, surf, which, srect, &src, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t a = SIZE_MAX;
    bool has_a = pc_paint_alpha_index(pc_surface_format(surf), &a);
    rc = pc_engine_clone_resample(&src, rect, map, has_a, a, out, err);
    pc_paint_region_fini(&src);
    return rc;
}

/* ---- the undoable stroke wrapper (run_stroke) --------------------------------------------- */

struct retouch_edit {
    bool has_layer;
    pc_layer_id layer;
    yyjson_val *p;
    void *tool;
    pc_status (*work)(void *tool, const pc_document *pre, pc_surface *surf, const pc_surface *sel, bool lock,
                      pc_rect *dmg, pc_error *err);
    pc_rect damage; /* out */
};

static pc_status retouch_edit_cb(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct retouch_edit *c = vctx;
    pc_document pre = {0};
    pc_status rc = pc_document_clone(doc, &pre, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface *surf = NULL;
    bool lock = false;
    rc = pc_engine_target_surface(doc, c->has_layer, c->layer, c->p, &surf, &lock, err);
    if (rc != PC_OK) {
        pc_document_fini(&pre);
        return rc;
    }
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    pc_rect dmg = PC_RECT_EMPTY;
    rc = c->work(c->tool, &pre, surf, sel, lock, &dmg, err);
    pc_document_fini(&pre);
    if (rc == PC_OK) {
        c->damage = dmg;
    }
    return rc;
}

static pc_status run_stroke(pc_engine_session *s, const char *label, struct retouch_edit *ctx,
                           pc_rect *out_dmg, pc_error *err)
{
    ctx->damage = PC_RECT_EMPTY;
    pc_status rc = pc_engine_session_edit(s, label, retouch_edit_cb, ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        st->last_damage = ctx->damage;
        st->has_last_damage = true;
    }
    *out_dmg = ctx->damage;
    return PC_OK;
}

/* ---- result JSON -------------------------------------------------------------------------- */

static pc_status damage_only(pc_json_value *out, pc_rect dmg, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *a = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, a, dmg.x0);
    yyjson_mut_arr_add_int(d, a, dmg.y0);
    yyjson_mut_arr_add_int(d, a, (int64_t)pc_rect_width(dmg));
    yyjson_mut_arr_add_int(d, a, (int64_t)pc_rect_height(dmg));
    yyjson_mut_obj_add_val(d, root, "damage", a);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* Clone Stamp / Healing Brush result: offset used + next sample point for non-aligned strokes. */
static pc_status clone_result(pc_json_value *out, pc_rect dmg, int32_t ox, int32_t oy, bool aligned,
                             yyjson_val *p, const pc_paint_point *first, pc_error *err)
{
    double sx, sy;
    if (!point_p(p, "source", &sx, &sy)) {
        sx = first->x + (double)ox;
        sy = first->y + (double)oy;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *da = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, da, dmg.x0);
    yyjson_mut_arr_add_int(d, da, dmg.y0);
    yyjson_mut_arr_add_int(d, da, (int64_t)pc_rect_width(dmg));
    yyjson_mut_arr_add_int(d, da, (int64_t)pc_rect_height(dmg));
    yyjson_mut_obj_add_val(d, root, "damage", da);
    yyjson_mut_val *off = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, off, ox);
    yyjson_mut_arr_add_int(d, off, oy);
    yyjson_mut_obj_add_val(d, root, "offset", off);
    yyjson_mut_obj_add_bool(d, root, "aligned", aligned);
    if (aligned) {
        yyjson_mut_obj_add_null(d, root, "nextSource");
    } else {
        yyjson_mut_val *ns = yyjson_mut_arr(d);
        yyjson_mut_arr_add_real(d, ns, sx);
        yyjson_mut_arr_add_real(d, ns, sy);
        yyjson_mut_obj_add_val(d, root, "nextSource", ns);
    }
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ========================================================================================== */
/* Clone Stamp                                                                                 */
/* ========================================================================================== */

struct clone_work {
    const pc_paint_stroke *stroke;
    bool has_layer;
    pc_layer_id layer;
    sample_layers which;
    pc_blend_mode mode;
    const pc_clone_mapping *map;
    const char *cmd;
};

static pc_status clone_work_fn(void *tool, const pc_document *pre, pc_surface *surf, const pc_surface *sel,
                              bool lock, pc_rect *dmg, pc_error *err)
{
    struct clone_work *c = tool;
    pc_rect bounds = PC_RECT_EMPTY;
    float *cov = NULL;
    size_t ncov = 0;
    pc_status rc = pc_paint_stroke_coverage(c->stroke, &bounds, &cov, &ncov, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (pc_rect_is_empty(bounds)) {
        free(cov);
        *dmg = PC_RECT_EMPTY;
        return PC_OK;
    }
    pc_paint_region paint = {0};
    rc = clone_sample(pre, c->has_layer, c->layer, surf, c->which, bounds, c->map, c->cmd, &paint, err);
    if (rc != PC_OK) {
        free(cov);
        return rc;
    }
    rc = pc_paint_apply_coverage(surf, bounds, cov, ncov, c->stroke->brush.opacity, sel, lock, &paint,
                                 c->mode, dmg, err);
    pc_paint_region_fini(&paint);
    free(cov);
    return rc;
}

static pc_status clone_stamp(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "paint.cloneStamp";
    pc_paint_stroke stroke = {0};
    bool has_layer = false;
    pc_layer_id layer = 0;
    pc_status rc = parse_brush(s, p, CMD, &stroke, &has_layer, &layer, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (stroke.count == 0 || stroke.points == NULL) {
        pc_paint_stroke_fini(&stroke);
        return pc_engine_err_bad_params(err, CMD, "`points` is empty");
    }
    sample_layers which;
    pc_blend_mode mode;
    pc_clone_mapping map;
    if ((rc = sample_layers_of(p, CMD, &which, err)) != PC_OK ||
        (rc = blend_param(p, CMD, &mode, err)) != PC_OK ||
        (rc = pc_engine_clone_mapping(s, p, stroke.points[0].x, stroke.points[0].y, CMD, &map, err)) != PC_OK) {
        pc_paint_stroke_fini(&stroke);
        return rc;
    }
    bool aligned = flag_p(p, "aligned", true);
    int32_t ox, oy;
    pc_engine_clone_offset(&map, &ox, &oy);
    struct clone_work cw = {&stroke, has_layer, layer, which, mode, &map, CMD};
    struct retouch_edit ed = {has_layer, layer, p, &cw, clone_work_fn, PC_RECT_EMPTY};
    pc_rect dmg = PC_RECT_EMPTY;
    rc = run_stroke(s, "Clone Stamp", &ed, &dmg, err);
    if (rc != PC_OK) {
        pc_paint_stroke_fini(&stroke);
        return rc;
    }
    rc = clone_result(out, dmg, ox, oy, aligned, p, &stroke.points[0], err);
    pc_paint_stroke_fini(&stroke);
    return rc;
}

/* ========================================================================================== */
/* Healing Brush                                                                               */
/* ========================================================================================== */

/* Grow a coverage map by `m` pixels of zero coverage (a Dirichlet boundary for the solve).
 * *grown is the new rect; *out an owned float array (free()) of grown w*h coverage values. */
static pc_status pad_coverage(pc_rect bounds, const float *cov, size_t ncov, int32_t m, pc_rect *grown,
                             float **out, size_t *out_n, pc_error *err)
{
    pc_rect g = pc_rect_inflate(bounds, m);
    size_t w = (size_t)pc_rect_width(bounds), gw = (size_t)pc_rect_width(g), gh = (size_t)pc_rect_height(g);
    size_t n = gw * gh;
    float *buf = calloc(n > 0 ? n : 1, sizeof(float));
    if (buf == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "pad_coverage");
    }
    for (size_t i = 0; i < ncov && w > 0; i++) {
        size_t x = i % w + (size_t)m, y = i / w + (size_t)m;
        if (y * gw + x < n) {
            buf[y * gw + x] = cov[i];
        }
    }
    *grown = g;
    *out = buf;
    *out_n = n;
    return PC_OK;
}

/* Seamless-clone src into dst over the mask (both regions cover the same rect). */
static pc_status heal_region(pc_pixel_format fmt, const pc_paint_region *src, const pc_paint_region *dst,
                            const bool *mask, pc_paint_region *out, pc_error *err)
{
    pc_status rc = pc_paint_region_clone(dst, out, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t w = pc_paint_region_width(dst);
    size_t n = dst->ch;
    pc_rect bbox = PC_RECT_EMPTY;
    for (size_t i = 0; i < w * pc_paint_region_height(dst); i++) {
        if (mask[i]) {
            int32_t x = dst->rect.x0 + (int32_t)(i % w), y = dst->rect.y0 + (int32_t)(i / w);
            bbox = pc_rect_union(bbox, pc_rect_make(x, y, x + 1, y + 1));
        }
    }
    if (pc_rect_is_empty(bbox)) {
        return PC_OK;
    }
    pc_rect r = pc_rect_intersect(pc_rect_inflate(bbox, 1), dst->rect);
    pc_paint_region sc = {0}, dc = {0};
    rc = pc_paint_region_crop(src, r, &sc, err);
    if (rc == PC_OK) {
        rc = pc_paint_region_crop(dst, r, &dc, err);
    }
    if (rc != PC_OK) {
        pc_paint_region_fini(&sc);
        pc_paint_region_fini(&dc);
        pc_paint_region_fini(out);
        return rc;
    }
    size_t rw = (size_t)pc_rect_width(r), rh = (size_t)pc_rect_height(r);
    bool *m = malloc((rw * rh > 0 ? rw * rh : 1) * sizeof(bool));
    if (m == NULL) {
        pc_paint_region_fini(&sc);
        pc_paint_region_fini(&dc);
        pc_paint_region_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, "heal mask");
    }
    for (int32_t y = r.y0; y < r.y1; y++) {
        for (int32_t x = r.x0; x < r.x1; x++) {
            size_t di = (size_t)(y - dst->rect.y0) * w + (size_t)(x - dst->rect.x0);
            m[(size_t)(y - r.y0) * rw + (size_t)(x - r.x0)] = mask[di];
        }
    }
    float *solved = NULL;
    rc = pc_algo_poisson_seamless_clone(rw, rh, n, sc.data, dc.data, dc.len, m, rw * rh, &solved, err);
    free(m);
    pc_paint_region_fini(&sc);
    pc_paint_region_fini(&dc);
    if (rc != PC_OK) {
        pc_paint_region_fini(out);
        return rc;
    }
    clamp_samples(fmt, solved, rw * rh * n);
    for (int32_t y = r.y0; y < r.y1; y++) {
        float *orow = pc_paint_region_px(out, r.x0, y);
        if (orow != NULL) {
            memcpy(orow, &solved[(size_t)(y - r.y0) * rw * n], rw * n * sizeof(float));
        }
    }
    free(solved);
    return PC_OK;
}

struct heal_work {
    const pc_paint_stroke *stroke;
    bool has_layer;
    pc_layer_id layer;
    sample_layers which;
    pc_blend_mode mode;
    const pc_clone_mapping *map;
    const char *cmd;
};

static pc_status heal_work_fn(void *tool, const pc_document *pre, pc_surface *surf, const pc_surface *sel,
                             bool lock, pc_rect *dmg, pc_error *err)
{
    struct heal_work *c = tool;
    pc_rect bounds = PC_RECT_EMPTY;
    float *cov = NULL;
    size_t ncov = 0;
    pc_status rc = pc_paint_stroke_coverage(c->stroke, &bounds, &cov, &ncov, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (pc_rect_is_empty(bounds)) {
        free(cov);
        *dmg = PC_RECT_EMPTY;
        return PC_OK;
    }
    pc_rect g = PC_RECT_EMPTY;
    float *gcov = NULL;
    size_t gn = 0;
    rc = pad_coverage(bounds, cov, ncov, 2, &g, &gcov, &gn, err);
    free(cov);
    if (rc != PC_OK) {
        return rc;
    }
    pc_pixel_format fmt = pc_surface_format(surf);
    pc_paint_region src = {0}, dst = {0}, healed = {0};
    bool *mask = NULL;
    rc = clone_sample(pre, c->has_layer, c->layer, surf, c->which, g, c->map, c->cmd, &src, err);
    if (rc == PC_OK) {
        if (c->which == SAMPLE_CURRENT) {
            rc = pc_paint_region_read(surf, g, &dst, err);
        } else {
            rc = composite_region(pre, c->has_layer, c->layer, c->which, g, fmt, &dst, err);
        }
    }
    if (rc == PC_OK) {
        mask = calloc((gn > 0 ? gn : 1), sizeof(bool));
        if (mask == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "heal mask");
        } else {
            for (size_t i = 0; i < gn; i++) {
                mask[i] = gcov[i] > 0.0f;
            }
        }
    }
    if (rc == PC_OK) {
        rc = heal_region(fmt, &src, &dst, mask, &healed, err);
    }
    if (rc == PC_OK) {
        rc = pc_paint_apply_coverage(surf, g, gcov, gn, c->stroke->brush.opacity, sel, lock, &healed,
                                     c->mode, dmg, err);
    }
    free(mask);
    free(gcov);
    pc_paint_region_fini(&src);
    pc_paint_region_fini(&dst);
    pc_paint_region_fini(&healed);
    return rc;
}

static pc_status healing_brush(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "paint.healingBrush";
    pc_paint_stroke stroke = {0};
    bool has_layer = false;
    pc_layer_id layer = 0;
    pc_status rc = parse_brush(s, p, CMD, &stroke, &has_layer, &layer, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (stroke.count == 0 || stroke.points == NULL) {
        pc_paint_stroke_fini(&stroke);
        return pc_engine_err_bad_params(err, CMD, "`points` is empty");
    }
    sample_layers which;
    pc_blend_mode mode;
    pc_clone_mapping map;
    if ((rc = sample_layers_of(p, CMD, &which, err)) != PC_OK ||
        (rc = blend_param(p, CMD, &mode, err)) != PC_OK ||
        (rc = pc_engine_clone_mapping(s, p, stroke.points[0].x, stroke.points[0].y, CMD, &map, err)) != PC_OK) {
        pc_paint_stroke_fini(&stroke);
        return rc;
    }
    bool aligned = flag_p(p, "aligned", true);
    int32_t ox, oy;
    pc_engine_clone_offset(&map, &ox, &oy);
    struct heal_work hw = {&stroke, has_layer, layer, which, mode, &map, CMD};
    struct retouch_edit ed = {has_layer, layer, p, &hw, heal_work_fn, PC_RECT_EMPTY};
    pc_rect dmg = PC_RECT_EMPTY;
    rc = run_stroke(s, "Healing Brush", &ed, &dmg, err);
    if (rc != PC_OK) {
        pc_paint_stroke_fini(&stroke);
        return rc;
    }
    rc = clone_result(out, dmg, ox, oy, aligned, p, &stroke.points[0], err);
    pc_paint_stroke_fini(&stroke);
    return rc;
}

/* ========================================================================================== */
/* History Brush                                                                               */
/* ========================================================================================== */

/* The History Brush's source surface: the targeted surface in history state `state` (default: the
 * oldest held state). *out is an owned surface (pc_surface_fini). */
static pc_status history_source(pc_engine_session *s, bool has_layer, pc_layer_id layer, yyjson_val *p,
                               const char *cmd, pc_surface *out, pc_error *err)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    size_t past = pc_history_past_len(st->history);
    const pc_document *source_doc = NULL;
    yyjson_val *sv = pc_json_get(p, "state");
    uint64_t sidx;
    if (sv != NULL && pc_json_as_u64(sv, &sidx)) {
        if ((size_t)sidx == past) {
            source_doc = pc_doc_arc_get(st->doc);
        } else {
            pc_doc_arc *a = pc_history_state(st->history, (size_t)sidx);
            if (a == NULL) {
                return pc_engine_err_bad_params(err, cmd, "no history state %llu (0..=%zu)",
                                                (unsigned long long)sidx, past);
            }
            source_doc = pc_doc_arc_get(a);
        }
    } else {
        pc_doc_arc *a = pc_history_state(st->history, 0);
        source_doc = (a != NULL) ? pc_doc_arc_get(a) : pc_doc_arc_get(st->doc);
    }
    const pc_surface *surf = NULL;
    if (pc_engine_read_target_surface(source_doc, has_layer, layer, p, &surf, err) != PC_OK || surf == NULL) {
        return pc_engine_err_other(
            err, "the target did not exist (as pixels, a mask or a channel) in that history state");
    }
    return pc_surface_clone(surf, out, err);
}

struct history_work {
    const pc_paint_stroke *stroke;
    const pc_surface *src; /* owned by caller; already in the target format on entry if possible */
    pc_blend_mode mode;
};

static pc_status history_work_fn(void *tool, const pc_document *pre, pc_surface *surf, const pc_surface *sel,
                                bool lock, pc_rect *dmg, pc_error *err)
{
    (void)pre;
    struct history_work *c = tool;
    pc_pixel_format fmt = pc_surface_format(surf);
    pc_surface conv = {0};
    const pc_surface *src = c->src;
    bool own = false;
    pc_pixel_format sf = pc_surface_format(src);
    if (!pc_pixel_format_eq(sf, fmt)) {
        pc_status rc = pc_surface_convert(src, fmt, &conv, err);
        if (rc != PC_OK) {
            return rc;
        }
        src = &conv;
        own = true;
    }
    pc_rect bounds = PC_RECT_EMPTY;
    float *cov = NULL;
    size_t ncov = 0;
    pc_status rc = pc_paint_stroke_coverage(c->stroke, &bounds, &cov, &ncov, err);
    if (rc == PC_OK) {
        if (pc_rect_is_empty(bounds)) {
            *dmg = PC_RECT_EMPTY;
        } else {
            pc_paint_region paint = {0};
            rc = pc_paint_region_read(src, bounds, &paint, err);
            if (rc == PC_OK) {
                rc = pc_paint_apply_coverage(surf, bounds, cov, ncov, c->stroke->brush.opacity, sel, lock,
                                             &paint, c->mode, dmg, err);
            }
            pc_paint_region_fini(&paint);
        }
    }
    free(cov);
    if (own) {
        pc_surface_fini(&conv);
    }
    return rc;
}

static pc_status history_brush(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "paint.historyBrush";
    pc_paint_stroke stroke = {0};
    bool has_layer = false;
    pc_layer_id layer = 0;
    pc_status rc = parse_brush(s, p, CMD, &stroke, &has_layer, &layer, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_blend_mode mode;
    pc_surface src = {0};
    if ((rc = blend_param(p, CMD, &mode, err)) != PC_OK ||
        (rc = history_source(s, has_layer, layer, p, CMD, &src, err)) != PC_OK) {
        pc_paint_stroke_fini(&stroke);
        return rc;
    }
    struct history_work hw = {&stroke, &src, mode};
    struct retouch_edit ed = {has_layer, layer, p, &hw, history_work_fn, PC_RECT_EMPTY};
    pc_rect dmg = PC_RECT_EMPTY;
    rc = run_stroke(s, "History Brush", &ed, &dmg, err);
    pc_surface_fini(&src);
    pc_paint_stroke_fini(&stroke);
    if (rc != PC_OK) {
        return rc;
    }
    return damage_only(out, dmg, err);
}

/* The sequential-dab tools (Dodge/Burn/Sponge/Blur/Sharpen/Smudge), Spot Healing and Pattern
 * Stamp live in these includes; they use the helpers above. */
#include "retouch_dab.inc"
#include "retouch_spot.inc"
#include "retouch_pattern.inc"
/* Patch / Content-Aware Move / Remove (job-based in the reference; run synchronously here, §7). */
#include "retouch_jobs.inc"

/* ========================================================================================== */
/* specs                                                                                       */
/* ========================================================================================== */

/* The brush params shared by every retouching command, plus the tool's own `extra`. */
#define RETOUCH_BRUSH_PARAMS(extra)                                                                        \
    "{\"points\":[[x,y,pressure?],...],\"size\":1..5000 px=tool size,\"hardness\":0..100=tool hardness,"   \
    "\"opacity\":1..100=100,\"flow\":1..100=100,\"spacing\":1..1000 (% of size)=Brush Settings spacing,"   \
    "\"layer\":id?=active,\"target\":\"pixels\"|\"mask\"|\"quickMask\"|{\"channel\":i}=Channels panel "     \
    "target" extra "}"

static const pc_engine_command SPECS[] = {
    {"paint.cloneStamp", "Clone Stamp", NULL, 0, NULL,
     RETOUCH_BRUSH_PARAMS(",\"source\":[sx,sy] (sampled under the first point) | \"offset\":[dx,dy],"
                          "\"aligned\":bool=true,\"sampleLayer\":\"current|currentAndBelow|all\"=\"current\","
                          "\"mode\":\"normal|multiply|…\"=\"normal\" → {\"damage\",\"offset\",\"aligned\","
                          "\"nextSource\"}"),
     pc_engine_has_paintable, clone_stamp, true},
    {"paint.patternStamp", "Pattern Stamp", NULL, 0, NULL,
     RETOUCH_BRUSH_PARAMS(",\"pattern\":id|name?=Patterns panel current,\"scale\":1..1000 %=100,"
                          "\"angle\":deg=0,\"aligned\":bool=true,\"impressionist\":bool=false,"
                          "\"mode\":\"normal|multiply|…\"=\"normal\",\"phase\":[px,py]? → {\"damage\","
                          "\"phase\",\"aligned\",\"pattern\"}"),
     pattern_stamp_enabled, pattern_stamp_run, true},
    {"paint.healingBrush", "Healing Brush", NULL, 0, NULL,
     RETOUCH_BRUSH_PARAMS(",\"source\":[sx,sy] | \"offset\":[dx,dy],\"aligned\":bool=true,"
                          "\"sampleLayer\":\"current|currentAndBelow|all\"=\"current\",\"mode\":\"normal|…\""
                          "=\"normal\" → {\"damage\",\"offset\",\"aligned\",\"nextSource\"}"),
     pc_engine_has_paintable, healing_brush, true},
    {"paint.spotHealing", "Spot Healing Brush", NULL, 0, NULL,
     RETOUCH_BRUSH_PARAMS(",\"type\":\"contentAware|createTexture|proximityMatch\"=\"contentAware\","
                          "\"sampleAllLayers\":bool=false"),
     pc_engine_has_paintable, spot_healing, true},
    {"paint.patch", "Patch", NULL, 0, NULL,
     "{\"offset\":[dx,dy] (how far the selection was dragged),\"mode\":\"source|destination\"=\"source\","
     "\"contentAware\":bool=false (fill the selection content-aware from the dragged-to place; a background "
     "job),\"structure\":1..7=4 (content-aware: 3..7 copy the middle exactly, 1..2 re-synthesise more of "
     "it),\"color\":0..10=0 (content-aware: how far the copy's level adapts to the selection's "
     "surroundings),\"sampleAllLayers\":bool=false (content-aware),\"layer\":id?=active,\"target\":\"pixels\""
     "|\"mask\"|\"quickMask\"|{\"channel\":i}=Channels panel target} → {\"damage\",\"offset\"}",
     patch_enabled, patch_run, true},
    {"paint.contentAwareMove", "Content-Aware Move", NULL, 0, NULL,
     "{\"offset\":[dx,dy] (how far the selection was dragged),\"mode\":\"move|extend\"=\"move\",\"structure\""
     ":1..7=4 (3..7 copy the content exactly but for a band about a patch wide along its edge; 2 widens the "
     "band, 1 re-synthesises the content too),\"color\":0..10=0 (0 keeps the content's level; higher values "
     "bring it towards its new place, by up to about 0.04 × color),\"sampleAllLayers\":bool=false,\"scaleX\""
     ":%=100,\"scaleY\":%=100 (Transform On Drop: 1..10000, negative flips),\"rotate\":deg=0 (Transform On "
     "Drop, clockwise; scale and rotation are about the selection's centre, before the offset; a transformed "
     "patch may reach past the canvas edge),\"layer\":id?=active,\"target\":\"pixels\"|\"mask\"|\"quickMask\""
     "|{\"channel\":i}=Channels panel target} → {\"damage\",\"offset\",\"mode\",\"transformed\"?} (a "
     "background job; the selection moves, and is transformed, with the content)",
     move_enabled, content_aware_move, true},
    {"paint.remove", "Remove", NULL, 0, NULL,
     RETOUCH_BRUSH_PARAMS(",\"strokes\":[[[x,y,pressure?],…],…]? (several strokes removed together, as one "
                          "job and one undo step; else \"points\"),\"closeLoops\":bool=true (a stroke around "
                          "an object removes what it encloses),\"sampleAllLayers\":bool=false,\"newLayer\":"
                          "bool=false (the fill goes to a new layer above the target, which stays as it was) "
                          "→ {\"damage\",\"layer\"?} (a background job)"),
     pc_engine_has_paintable, remove_run, true},
    {"paint.dodge", "Dodge", NULL, 0, NULL,
     RETOUCH_BRUSH_PARAMS(",\"range\":\"shadows|midtones|highlights\"=\"midtones\",\"exposure\":1..100=50,"
                          "\"protectTones\":bool=true"),
     pc_engine_has_paintable, run_dodge, true},
    {"paint.burn", "Burn", NULL, 0, NULL,
     RETOUCH_BRUSH_PARAMS(",\"range\":\"shadows|midtones|highlights\"=\"midtones\",\"exposure\":1..100=50,"
                          "\"protectTones\":bool=true"),
     pc_engine_has_paintable, run_burn, true},
    {"paint.sponge", "Sponge", NULL, 0, NULL,
     RETOUCH_BRUSH_PARAMS(",\"mode\":\"desaturate|saturate\"=\"desaturate\",\"vibrance\":bool=true (flow = "
                          "sponge strength)"),
     pc_engine_has_paintable, run_sponge, true},
    {"paint.blur", "Blur", NULL, 0, NULL,
     RETOUCH_BRUSH_PARAMS(",\"strength\":1..100=50,\"sampleAllLayers\":bool=false"),
     pc_engine_has_paintable, run_blur, true},
    {"paint.sharpen", "Sharpen", NULL, 0, NULL,
     RETOUCH_BRUSH_PARAMS(",\"strength\":1..100=50,\"protectDetail\":bool=true,\"sampleAllLayers\":bool=false"),
     pc_engine_has_paintable, run_sharpen, true},
    {"paint.smudge", "Smudge", NULL, 0, NULL,
     RETOUCH_BRUSH_PARAMS(",\"strength\":1..100=50,\"fingerPainting\":bool=false (starts with the foreground "
                          "colour),\"sampleAllLayers\":bool=false"),
     pc_engine_has_paintable, run_smudge, true},
    {"paint.historyBrush", "History Brush", NULL, 0, NULL,
     RETOUCH_BRUSH_PARAMS(",\"state\":index (into history.entries; default 0 = the oldest held state, "
                          "normally the Open snapshot)"),
     pc_engine_has_paintable, history_brush, true},
};

const pc_engine_command *pc_engine_retouch_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
