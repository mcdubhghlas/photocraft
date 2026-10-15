/* Image > Adjustments and Layer > New Adjustment Layer commands (port of the ADJ table in
 * crates/engine/src/commands.rs plus crates/engine/src/adjust_cmds.rs).
 *
 * The per-kind adjustments are registered as BOTH a destructive `image.adjustments.<kind>` and an
 * adjustment layer `layer.newAdjustmentLayer.<kind>`, sharing one run fn each (the kind comes from
 * the engine's injected `__kind`). The destructive-only Shadows/Highlights, Replace Color, Match
 * Color and HDR Toning use the tone maths in src/algo. Adjustments route through the compositor's
 * bit-exact adjust engine (pixels::adjust_surface).
 *
 * DEFERRED in this build (graceful PC_ERR_* / UNSUPPORTED, never a crash), with owners:
 *   - a smart object records the adjustment as a smart filter -> E9-full (apply_filter_to_surface)
 *     + E6 add_smart_filter; here a smart-object target is edited destructively only if raster.
 *   - a targeted alpha / Quick Mask channel for a destructive adjustment -> needs the mutable
 *     channel surface (E7 channel_surface_for_filter export); reported unsupported.
 *   - the Levels/Curves eyedropper -> needs the merged-composite sampler (E10 sample_cmds).
 *   - a "path" vector mask for a new adjustment layer -> needs vector path parsing export (E11).
 *   - Match Color `sourceLayer` -> needs single-layer render export. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "adjust_params.h"
#include "algo/selection.h"
#include "algo/tone.h"
#include "base/mem.h"
#include "brush_cmds.h"   /* pc_engine_paint_surface, pc_engine_is_mask_target */
#include "channel_cmds.h" /* pc_engine_is_channel_target, pc_engine_edits_channel */
#include "cms/lutfile.h"
#include "compose/compose.h"
#include "engine_internal.h"
#include "layer_cmds.h" /* pc_engine_layer_param */

/* ---- param helpers ------------------------------------------------------------------------ */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static float fnum(yyjson_val *p, const char *key, float def)
{
    if (p == NULL || !yyjson_is_obj(p)) {
        return def;
    }
    yyjson_val *v = yyjson_obj_get(p, key);
    if (v == NULL || !yyjson_is_num(v)) {
        return def;
    }
    double x = yyjson_get_num(v);
    return isfinite(x) ? (float)x : def;
}

static bool bnum(yyjson_val *p, const char *key, bool def)
{
    if (p == NULL || !yyjson_is_obj(p)) {
        return def;
    }
    yyjson_val *v = yyjson_obj_get(p, key);
    return (v != NULL && yyjson_is_bool(v)) ? yyjson_get_bool(v) : def;
}

static const char *kind_of(yyjson_val *p)
{
    if (p != NULL && yyjson_is_obj(p)) {
        yyjson_val *v = yyjson_obj_get(p, "__kind");
        if (v != NULL && yyjson_is_str(v)) {
            return yyjson_get_str(v);
        }
    }
    return "invert";
}

static pc_color_mode doc_mode(const pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return PC_COLOR_MODE_RGB;
    }
    return pc_doc_arc_get(d->doc)->mode;
}

/* ---- enabled predicates (match the reference's wording) ----------------------------------- */

static pc_status not_pixel_err(pc_error *reason, const pc_layer *l)
{
    return pc_fail(reason, PC_ERR_INVALID, "active layer is %s %s layer, not a pixel layer",
                   pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
}

/* Image > Adjustments: a pixel layer, a smart object with a cache, or a targeted channel. */
static pc_status has_adjustable(const pc_engine_session *s, pc_error *reason)
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
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer);
    if (l == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    if (l->content.kind == PC_LAYER_RASTER) {
        return PC_OK;
    }
    if (l->content.kind == PC_LAYER_SMART && l->content.u.smart.has_cache) {
        return PC_OK;
    }
    return not_pixel_err(reason, l);
}

/* Replace/Match Color: a targeted channel or a pixel (raster) layer. */
static pc_status has_pixels(const pc_engine_session *s, pc_error *reason)
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
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer);
    if (l == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    if (l->content.kind == PC_LAYER_RASTER) {
        return PC_OK;
    }
    return not_pixel_err(reason, l);
}

/* Desaturate: a raster active layer (commands::has_pixel_layer, no channel-edit path). */
static pc_status has_pixel_layer(const pc_engine_session *s, pc_error *reason)
{
    const pc_layer *l = pc_engine_active_layer_of(s, reason);
    if (l == NULL) {
        return reason != NULL ? reason->code : PC_ERR_INVALID;
    }
    if (l->content.kind == PC_LAYER_RASTER) {
        return PC_OK;
    }
    return not_pixel_err(reason, l);
}

/* ---- mask-for-new-layer (commands::mask_new_layer, selection only; "path" deferred) -------- */

static pc_status mask_to_depth(pc_layer_mask *m, pc_sample_type depth, pc_error *err)
{
    pc_pixel_format cur = pc_surface_format(&m->surface);
    pc_pixel_format want = cur;
    want.sample = depth;
    if (!pc_pixel_format_eq(cur, want)) {
        pc_surface conv;
        if (pc_surface_convert(&m->surface, want, &conv, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        pc_surface_fini(&m->surface);
        m->surface = conv;
    }
    return PC_OK;
}

static pc_status mask_new_layer(pc_document *doc, pc_layer *l, yyjson_val *p, pc_error *err)
{
    yyjson_val *path = (p != NULL && yyjson_is_obj(p)) ? yyjson_obj_get(p, "path") : NULL;
    if (path != NULL && !yyjson_is_null(path)) {
        return pc_engine_err_other(
            err, "a path (vector) mask for a new fill/adjustment layer is not yet supported in this build");
    }
    pc_layer_mask m;
    pc_layer_mask_reveal_all(&m);
    if (doc->has_selection) {
        pc_surface sel;
        if (pc_surface_clone(&doc->selection, &sel, err) != PC_OK) {
            pc_layer_mask_fini(&m);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        pc_surface_fini(&m.surface);
        m.surface = sel;
    }
    if (mask_to_depth(&m, doc->depth, err) != PC_OK) {
        pc_layer_mask_fini(&m);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (l->has_mask) {
        pc_layer_mask_fini(&l->mask);
    }
    l->mask = m;
    l->has_mask = true;
    return PC_OK;
}

/* ---- new adjustment layer ----------------------------------------------------------------- */

struct new_adj_ctx {
    const pc_adjustment *adj; /* borrowed; cloned into the layer */
    yyjson_val *p;
    pc_layer_id id;
};

static pc_status edit_new_adjustment(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                                     pc_error *err)
{
    struct new_adj_ctx *c = vctx;
    pc_str nm;
    nm.ptr = NULL;
    nm.len = 0;
    const char *base = pc_adjustment_label(c->adj);
    if (pc_document_next_layer_name(doc, base, &nm, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer_content content;
    memset(&content, 0, sizeof content);
    content.kind = PC_LAYER_ADJUSTMENT;
    if (pc_adjustment_clone(c->adj, &content.u.adjustment, err) != PC_OK) {
        pc_str_fini(&nm);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer l;
    pc_status rc = pc_layer_init(&l, pc_str_c(&nm), &content, err); /* takes over content on success */
    pc_str_fini(&nm);
    if (rc != PC_OK) {
        pc_layer_content_fini(&content);
        return rc;
    }
    if ((rc = mask_new_layer(doc, &l, c->p, err)) != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    pc_layer_id id;
    const pc_layer_id *above = *has_active ? active : NULL;
    if ((rc = pc_document_insert_above(doc, above, &l, &id, err)) != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    c->id = id;
    *active = id;
    *has_active = true;
    return PC_OK;
}

static pc_status new_adjustment(pc_engine_session *s, const pc_adjustment *adj, yyjson_val *p,
                                pc_json_value *out, pc_error *err)
{
    char label[64];
    snprintf(label, sizeof label, "New %s Layer", pc_adjustment_label(adj));
    struct new_adj_ctx ctx = {adj, p, 0};
    pc_status rc = pc_engine_session_edit(s, label, edit_new_adjustment, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "layer", ctx.id);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- destructive adjustment --------------------------------------------------------------- */

enum adj_target { ADJ_PIXELS, ADJ_MASK };

struct dadj_ctx {
    const pc_adjustment *adj;
    enum adj_target target;
    pc_layer_id id;
    pc_color_mode mode;
};

static pc_status edit_destructive(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                                  pc_error *err)
{
    (void)has_active;
    (void)active;
    struct dadj_ctx *c = vctx;
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    if (c->target == ADJ_MASK) {
        pc_layer *l = pc_document_layer_mut(doc, c->id);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, c->id);
        }
        if (!l->has_mask) {
            return pc_engine_err_other(err, "layer has no mask");
        }
        pc_engine_adjust_mask(&l->mask.surface, c->adj, sel);
        return PC_OK;
    }
    pc_surface *surf = NULL;
    pc_status rc = pc_engine_paint_surface(doc, c->id, NULL, &surf, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_adjust_surface(surf, c->adj, sel, c->mode);
    return PC_OK;
}

static pc_status destructive_adjust(pc_engine_session *s, const char *label, const pc_adjustment *adj,
                                    yyjson_val *p, pc_json_value *out, pc_error *err)
{
    struct dadj_ctx ctx;
    ctx.adj = adj;
    ctx.mode = doc_mode(s);
    if (pc_engine_is_mask_target(p)) {
        ctx.target = ADJ_MASK;
    } else if (pc_engine_is_channel_target(p)) {
        return pc_engine_err_other(
            err,
            "adjusting a targeted alpha / Quick Mask channel is not yet supported in this build "
            "(E7 channel surface)");
    } else {
        ctx.target = ADJ_PIXELS;
    }
    if (pc_engine_layer_param(s, NULL, &ctx.id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_status rc = pc_engine_session_edit(s, label, edit_destructive, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

/* ---- generated run functions -------------------------------------------------------------- */

/* A smart object is active -> the adjustment would record as a smart filter (deferred). */
static bool active_is_smart(const pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL || !d->has_active_layer) {
        return false;
    }
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer);
    return l != NULL && l->content.kind == PC_LAYER_SMART;
}

static pc_status run_new_adjustment_layer(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                          pc_error *err)
{
    const char *kind = kind_of(p);
    if ((strcmp(kind, "levels") == 0 || strcmp(kind, "curves") == 0) && p != NULL &&
        yyjson_is_obj(p) && yyjson_obj_get(p, "eyedropper") != NULL) {
        return pc_engine_err_bad_params(err, "eyedropper",
                                        "the Levels/Curves eyedropper is not yet supported in this "
                                        "build (E10 sampler)");
    }
    pc_adjustment adj;
    pc_status rc = pc_engine_adjustment_from_params(kind, p, NULL, doc_mode(s), &adj, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = new_adjustment(s, &adj, p, out, err);
    pc_adjustment_fini(&adj);
    return rc;
}

static pc_status run_image_adjustment(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                      pc_error *err)
{
    const char *kind = kind_of(p);
    if ((strcmp(kind, "levels") == 0 || strcmp(kind, "curves") == 0) && p != NULL &&
        yyjson_is_obj(p) && yyjson_obj_get(p, "eyedropper") != NULL) {
        return pc_engine_err_bad_params(err, "eyedropper",
                                        "the Levels/Curves eyedropper is not yet supported in this "
                                        "build (E10 sampler)");
    }
    /* A smart object would record the adjustment as a smart filter (deferred: E9-full). Only a
     * mask/channel target falls through to a destructive edit there, so refuse the rest. */
    if (active_is_smart(s) && !pc_engine_is_mask_target(p) && !pc_engine_is_channel_target(p)) {
        return pc_engine_err_other(
            err, "recording an adjustment as a smart filter is not yet supported in this build "
                 "(E9 smart filters)");
    }
    pc_adjustment adj;
    pc_status rc = pc_engine_adjustment_from_params(kind, p, NULL, doc_mode(s), &adj, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = destructive_adjust(s, pc_adjustment_label(&adj), &adj, p, out, err);
    pc_adjustment_fini(&adj);
    return rc;
}

/* ---- layer.setAdjustment ------------------------------------------------------------------ */

struct set_adj_ctx {
    yyjson_val *p;
    pc_color_mode mode;
    pc_layer_id id;
};

static pc_status edit_set_adjustment(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                                     pc_error *err)
{
    (void)has_active;
    (void)active;
    struct set_adj_ctx *c = vctx;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    if (l->content.kind != PC_LAYER_ADJUSTMENT) {
        return pc_engine_err_other(err, "not an adjustment layer");
    }
    pc_adjustment *adj = &l->content.u.adjustment;
    if (adj->kind == PC_ADJ_UNSUPPORTED) {
        return pc_engine_err_other(err, "this adjustment can't be edited (kept as imported)");
    }
    const char *kind = pc_engine_adjustment_kind(adj);
    pc_adjustment next;
    pc_status rc;
    if (pc_engine_adjustment_user_keys_empty(c->p)) {
        rc = pc_engine_adjustment_default_for(kind, c->mode, &next, err);
    } else {
        rc = pc_engine_adjustment_from_params(kind, c->p, adj, c->mode, &next, err);
    }
    if (rc != PC_OK) {
        return rc;
    }
    pc_adjustment_fini(adj);
    *adj = next;
    return PC_OK;
}

static pc_status run_set_adjustment(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    struct set_adj_ctx ctx;
    ctx.p = p;
    ctx.mode = doc_mode(s);
    if (pc_engine_layer_param(s, p, &ctx.id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    /* The Levels/Curves eyedropper for layer.setAdjustment is deferred (E10 sampler). */
    if (p != NULL && yyjson_is_obj(p) && yyjson_obj_get(p, "eyedropper") != NULL) {
        return pc_engine_err_bad_params(err, "eyedropper",
                                        "the Levels/Curves eyedropper is not yet supported in this "
                                        "build (E10 sampler)");
    }
    pc_status rc = pc_engine_session_edit(s, "Modify Adjustment", edit_set_adjustment, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

/* ---- desaturate (Hue/Saturation with saturation = -100) ----------------------------------- */

static pc_status run_desaturate(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_adjustment adj;
    memset(&adj, 0, sizeof adj);
    adj.kind = PC_ADJ_HUE_SATURATION;
    adj.u.hue_saturation.hue = 0.0f;
    adj.u.hue_saturation.saturation = -100.0f;
    adj.u.hue_saturation.lightness = 0.0f;
    adj.u.hue_saturation.colorize = false;
    pc_hue_range_defaults(adj.u.hue_saturation.ranges);
    /* Desaturate always targets the active pixel layer (Value::Null target). */
    pc_status rc = destructive_adjust(s, "Desaturate", &adj, NULL, out, err);
    return rc;
}

/* ---- destructive-only tone adjustments (algo/tone) ---------------------------------------- */

/* Run `f` over the active layer's content as straight RGBA, blended through the selection. */
typedef void (*rgba_fn)(float (*px)[4], size_t n, size_t w, size_t h, pc_rect r, const pc_surface *sel,
                        void *ctx);

struct rgba_ctx {
    rgba_fn f;
    void *fctx;
    pc_layer_id id;
};

static pc_status process_surface(pc_surface *surf, const pc_surface *sel, rgba_fn f, void *fctx,
                                 pc_error *err)
{
    pc_rect r = pc_surface_content_bounds(surf);
    if (pc_rect_is_empty(r)) {
        return PC_OK;
    }
    pc_pixel_format fmt = pc_surface_format(surf);
    size_t n = pc_pixel_format_channels(fmt);
    float *raw = NULL;
    size_t len = 0;
    if (pc_surface_read_region(surf, r, &raw, &len, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    size_t count = (n == 0) ? 0 : len / n;
    if (count == 0) {
        free(raw);
        return PC_OK;
    }
    float(*orig)[4] = pc_malloc_array(count, sizeof *orig);
    float(*px)[4] = pc_malloc_array(count, sizeof *px);
    if (orig == NULL || px == NULL) {
        free(raw);
        free(orig);
        free(px);
        return pc_fail(err, PC_ERR_NOMEM, "adjust");
    }
    for (size_t i = 0; i < count; i++) {
        pc_to_rgba(fmt, raw + i * n, n, orig[i]);
        for (int cc = 0; cc < 4; cc++) {
            px[i][cc] = orig[i][cc];
        }
    }
    free(raw);
    size_t w = (size_t)pc_rect_width(r);
    size_t h = (size_t)pc_rect_height(r);
    f(px, count, w, h, r, sel, fctx);
    float *mixed = NULL;
    size_t mlen = 0;
    pc_status rc = pc_engine_mix_through_selection(fmt, r, (const float(*)[4])orig,
                                                   (const float(*)[4])px, count, sel, &mixed, &mlen, err);
    free(orig);
    free(px);
    if (rc == PC_OK && mixed != NULL) {
        rc = pc_surface_write_region(surf, r, mixed, mlen, err);
    }
    free(mixed);
    return rc;
}

static pc_status edit_rgba(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                           pc_error *err)
{
    (void)has_active;
    (void)active;
    struct rgba_ctx *c = vctx;
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    pc_surface *surf = NULL;
    pc_status rc = pc_engine_paint_surface(doc, c->id, NULL, &surf, err);
    if (rc != PC_OK) {
        return rc;
    }
    return process_surface(surf, sel, c->f, c->fctx, err);
}

static pc_status rgba_edit(pc_engine_session *s, const char *label, yyjson_val *p, rgba_fn f, void *fctx,
                           pc_json_value *out, pc_error *err)
{
    if (pc_engine_is_channel_target(p) || pc_engine_is_mask_target(p)) {
        return pc_engine_err_other(
            err, "this adjustment on a channel or mask is not yet supported in this build "
                 "(E7 channel surface)");
    }
    struct rgba_ctx ctx = {f, fctx, 0};
    if (pc_engine_layer_param(s, NULL, &ctx.id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_status rc = pc_engine_session_edit(s, label, edit_rgba, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

/* Shadows/Highlights */
static void sh_fn(float (*px)[4], size_t n, size_t w, size_t h, pc_rect r, const pc_surface *sel,
                  void *ctx)
{
    (void)r;
    (void)sel;
    (void)pc_algo_shadows_highlights(px, n, w, h, ctx, NULL);
}

static pc_status run_shadows_highlights(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                        pc_error *err)
{
    pc_algo_shadows_highlights_params sh = pc_algo_shadows_highlights_default();
    sh.shadow_amount = clampf(fnum(p, "shadowAmount", sh.shadow_amount), 0.0f, 100.0f);
    sh.shadow_tone = clampf(fnum(p, "shadowTone", sh.shadow_tone), 0.0f, 100.0f);
    sh.shadow_radius = clampf(fnum(p, "shadowRadius", sh.shadow_radius), 0.0f, 2500.0f);
    sh.highlight_amount = clampf(fnum(p, "highlightAmount", sh.highlight_amount), 0.0f, 100.0f);
    sh.highlight_tone = clampf(fnum(p, "highlightTone", sh.highlight_tone), 0.0f, 100.0f);
    sh.highlight_radius = clampf(fnum(p, "highlightRadius", sh.highlight_radius), 0.0f, 2500.0f);
    sh.color = clampf(fnum(p, "color", sh.color), -100.0f, 100.0f);
    sh.midtone = clampf(fnum(p, "midtone", sh.midtone), -100.0f, 100.0f);
    sh.black_clip = clampf(fnum(p, "blackClip", sh.black_clip), 0.0f, 50.0f);
    sh.white_clip = clampf(fnum(p, "whiteClip", sh.white_clip), 0.0f, 50.0f);
    return rgba_edit(s, "Shadows/Highlights", p, sh_fn, &sh, out, err);
}

/* Replace Color */
struct replace_ctx {
    float color[3];
    float fuzz, hue, sat, light;
};

static void replace_fn(float (*px)[4], size_t n, size_t w, size_t h, pc_rect r, const pc_surface *sel,
                       void *ctx)
{
    (void)w;
    (void)h;
    (void)r;
    (void)sel;
    struct replace_ctx *c = ctx;
    pc_algo_replace_color(px, n, c->color, c->fuzz, c->hue, c->sat, c->light, NULL);
}

static pc_status run_replace_color(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_tool_state *t = pc_engine_session_tools(s);
    float def[4] = {t->foreground[0], t->foreground[1], t->foreground[2], t->foreground[3]};
    float rgba[4];
    pc_engine_param_color(p, "color", def, rgba);
    struct replace_ctx c;
    c.color[0] = rgba[0];
    c.color[1] = rgba[1];
    c.color[2] = rgba[2];
    c.fuzz = clampf(fnum(p, "fuzziness", 40.0f), 0.0f, 200.0f);
    c.hue = clampf(fnum(p, "hue", 0.0f), -180.0f, 180.0f);
    c.sat = clampf(fnum(p, "saturation", 0.0f), -100.0f, 100.0f);
    c.light = clampf(fnum(p, "lightness", 0.0f), -100.0f, 100.0f);
    return rgba_edit(s, "Replace Color", p, replace_fn, &c, out, err);
}

/* Match Color */
struct match_ctx {
    pc_algo_match_color opts;
    bool has_source;
    pc_algo_lab_stats source;
    bool target_sel;
};

static void match_fn(float (*px)[4], size_t n, size_t w, size_t h, pc_rect r, const pc_surface *sel,
                     void *ctx)
{
    (void)w;
    (void)h;
    struct match_ctx *c = ctx;
    float *mask = NULL;
    size_t mlen = 0;
    if (c->target_sel && sel != NULL) {
        (void)pc_algo_selection_mask_from_surface(sel, r, &mask, &mlen, NULL);
    }
    pc_algo_lab_stats target;
    if (!pc_algo_lab_stats_compute((const float(*)[4])px, n, mask, mlen, &target)) {
        free(mask);
        return;
    }
    pc_algo_match_color_apply(px, n, &target, c->has_source ? &c->source : NULL, &c->opts);
    free(mask);
}

/* Lab stats of a source document's merged image (optionally through its selection). */
static bool source_stats(const pc_document *doc, bool use_selection, pc_algo_lab_stats *out)
{
    pc_compose_buffer buf;
    memset(&buf, 0, sizeof buf);
    if (pc_compose_flatten(doc, &buf, NULL) != PC_OK) {
        return false;
    }
    float *mask = NULL;
    size_t mlen = 0;
    if (use_selection && doc->has_selection) {
        (void)pc_algo_selection_mask_from_surface(&doc->selection, buf.rect, &mask, &mlen, NULL);
    }
    bool ok = pc_algo_lab_stats_compute((const float(*)[4])buf.px, buf.len, mask, mlen, out);
    free(mask);
    pc_compose_buffer_fini(&buf);
    return ok;
}

static pc_status run_match_color(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "image.adjustments.matchColor";
    struct match_ctx c;
    memset(&c, 0, sizeof c);
    c.opts.luminance = clampf(fnum(p, "luminance", 100.0f), 1.0f, 200.0f);
    c.opts.intensity = clampf(fnum(p, "intensity", 100.0f), 1.0f, 200.0f);
    c.opts.fade = clampf(fnum(p, "fade", 0.0f), 0.0f, 100.0f);
    c.opts.neutralize = bnum(p, "neutralize", false);
    c.target_sel = bnum(p, "useSelectionInTarget", false);
    /* A sourceLayer render is deferred; only the merged image of a source document is supported. */
    if (p != NULL && yyjson_is_obj(p)) {
        yyjson_val *sl = yyjson_obj_get(p, "sourceLayer");
        if (sl != NULL && !yyjson_is_null(sl)) {
            return pc_engine_err_bad_params(
                err, cmd, "a Match Color `sourceLayer` is not yet supported in this build");
        }
        yyjson_val *src = yyjson_obj_get(p, "source");
        if (src != NULL && yyjson_is_num(src) && yyjson_get_num(src) >= 0) {
            size_t i = (size_t)yyjson_get_num(src);
            size_t ndocs = 0;
            const pc_engine_doc_state *docs = pc_engine_session_documents(s, &ndocs);
            if (i >= ndocs) {
                return pc_engine_err_bad_params(err, cmd, "no document %zu", i);
            }
            bool use_src_sel = bnum(p, "useSelectionInSource", false);
            if (!source_stats(pc_doc_arc_get(docs[i].doc), use_src_sel, &c.source)) {
                return pc_engine_err_bad_params(err, cmd, "the source has no opaque pixels");
            }
            c.has_source = true;
        }
    }
    return rgba_edit(s, "Match Color", p, match_fn, &c, out, err);
}

/* HDR Toning (flattens the image to a single Background layer). */
struct hdr_ctx {
    pc_algo_hdr_toning_params h;
    float (*curve)[2];
    size_t curve_len;
};

static pc_status edit_hdr(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                          pc_error *err)
{
    struct hdr_ctx *c = vctx;
    pc_compose_buffer buf;
    memset(&buf, 0, sizeof buf);
    if (pc_compose_flatten(doc, &buf, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    float bg[3] = {1.0f, 1.0f, 1.0f};
    pc_compose_buffer_over_background(&buf, bg);
    size_t w = (size_t)pc_rect_width(buf.rect);
    size_t h = (size_t)pc_rect_height(buf.rect);
    pc_status rc = pc_algo_hdr_toning(buf.px, buf.len, w, h, &c->h, err);
    if (rc != PC_OK) {
        pc_compose_buffer_fini(&buf);
        return rc;
    }
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    size_t n = pc_pixel_format_channels(fmt);
    float *data = pc_malloc_array(buf.len * (n == 0 ? 1 : n), sizeof *data);
    if (data == NULL) {
        pc_compose_buffer_fini(&buf);
        return pc_fail(err, PC_ERR_NOMEM, "hdr toning");
    }
    size_t wi = 0;
    for (size_t i = 0; i < buf.len; i++) {
        float enc[PC_RASTER_MAX_CHANNELS];
        size_t m = pc_from_rgba(fmt, buf.px[i], enc);
        for (size_t cc = 0; cc < m; cc++) {
            data[wi++] = enc[cc];
        }
    }
    pc_rect bounds = pc_document_bounds(doc);
    pc_layer bgl;
    rc = pc_layer_raster(&bgl, "Background", fmt, err);
    if (rc != PC_OK) {
        free(data);
        pc_compose_buffer_fini(&buf);
        return rc;
    }
    bgl.locks.transparency = true;
    bgl.locks.position = true;
    pc_surface *surf = pc_layer_surface_mut(&bgl);
    if (surf != NULL) {
        (void)pc_surface_write_region(surf, bounds, data, wi, err);
    }
    free(data);
    pc_compose_buffer_fini(&buf);
    /* Replace the layer stack with the single flattened Background. */
    for (size_t i = 0; i < doc->nlayers; i++) {
        pc_layer_fini(&doc->layers[i]);
    }
    free(doc->layers);
    doc->layers = pc_malloc_array(1, sizeof *doc->layers);
    if (doc->layers == NULL) {
        doc->nlayers = 0;
        pc_layer_fini(&bgl);
        return pc_fail(err, PC_ERR_NOMEM, "hdr toning");
    }
    doc->layers[0] = bgl;
    doc->nlayers = 1;
    *active = bgl.id;
    *has_active = true;
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    return PC_OK;
}

static pc_status run_hdr_toning(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    struct hdr_ctx c;
    memset(&c, 0, sizeof c);
    pc_algo_hdr_toning_params d = pc_algo_hdr_toning_default();
    c.h = d;
    c.h.radius = clampf(fnum(p, "radius", d.radius), 1.0f, 500.0f);
    c.h.strength = clampf(fnum(p, "strength", d.strength), 0.1f, 4.0f);
    c.h.gamma = clampf(fnum(p, "gamma", d.gamma), 0.1f, 2.0f);
    c.h.exposure = clampf(fnum(p, "exposure", d.exposure), -5.0f, 5.0f);
    c.h.detail = clampf(fnum(p, "detail", d.detail), -100.0f, 300.0f);
    c.h.shadow = clampf(fnum(p, "shadow", d.shadow), -100.0f, 100.0f);
    c.h.highlight = clampf(fnum(p, "highlight", d.highlight), -100.0f, 100.0f);
    c.h.vibrance = clampf(fnum(p, "vibrance", d.vibrance), -100.0f, 100.0f);
    c.h.saturation = clampf(fnum(p, "saturation", d.saturation), -100.0f, 100.0f);
    /* curve: [[in, out], ...] in 0..255 (normalised to 0..1). */
    c.h.curve = NULL;
    c.h.curve_len = 0;
    if (p != NULL && yyjson_is_obj(p)) {
        yyjson_val *cv = yyjson_obj_get(p, "curve");
        if (cv != NULL && yyjson_is_arr(cv)) {
            size_t ncv = yyjson_arr_size(cv);
            if (ncv > 0) {
                c.curve = pc_malloc_array(ncv, sizeof *c.curve);
                if (c.curve == NULL) {
                    return pc_fail(err, PC_ERR_NOMEM, "hdr curve");
                }
                size_t k = 0;
                for (size_t i = 0; i < ncv; i++) {
                    yyjson_val *pair = yyjson_arr_get(cv, i);
                    if (!yyjson_is_arr(pair) || yyjson_arr_size(pair) < 2) {
                        continue;
                    }
                    yyjson_val *a = yyjson_arr_get(pair, 0), *b = yyjson_arr_get(pair, 1);
                    if (!yyjson_is_num(a) || !yyjson_is_num(b)) {
                        continue;
                    }
                    c.curve[k][0] = (float)yyjson_get_num(a) / 255.0f;
                    c.curve[k][1] = (float)yyjson_get_num(b) / 255.0f;
                    k++;
                }
                c.h.curve = (const float(*)[2])c.curve;
                c.h.curve_len = k;
            }
        }
    }
    pc_status rc = pc_engine_session_edit(s, "HDR Toning", edit_hdr, &c, err);
    free(c.curve);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

/* ---- Color Lookup look list --------------------------------------------------------------- */

static pc_status run_list_looks(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    (void)p;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < PC_LUTFILE_BUILTIN_COUNT; i++) {
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, o, "id", pc_lutfile_builtin_id(i));
        yyjson_mut_obj_add_strcpy(d, o, "label", pc_lutfile_builtin_label(i));
        yyjson_mut_arr_add_val(arr, o);
    }
    yyjson_mut_doc_set_root(d, arr);
    return PC_OK;
}

/* ---- the spec table ----------------------------------------------------------------------- */

static const char *const MENU_IMAGE_ADJ[2] = {"Image", "Adjustments"};
static const char *const MENU_LAYER_NEW_ADJ[2] = {"Layer", "New Adjustment Layer"};

#define NLP                                                                                             \
    "\"path\":\"work\"|saved path name|{…path}? (the active path: becomes the vector mask; else the "   \
    "selection is the layer mask, or white with no selection)"

/* per-kind image params */
#define P_BRIGHTNESS "{\"brightness\":-150..150=0,\"contrast\":-50..100=0,\"legacy\":bool=false}"
#define P_LEVELS                                                                                        \
    "{\"inBlack\":0..253=0,\"gamma\":0.01..9.99=1,\"inWhite\":2..255=255,\"outBlack\":0..255=0,"         \
    "\"outWhite\":0..255=255,\"red\":json,\"green\":json,\"blue\":json,\"eyedropper\":json} (top "      \
    "level = composite; per channel {\"inBlack\",\"gamma\",\"inWhite\",\"outBlack\",\"outWhite\"} "      \
    "under red/green/blue, gray, cyan/magenta/yellow/black or lightness/a/b; eyedropper = the "         \
    "dialog's Set Black/Neutral Gray/White Point picker "                                               \
    "{\"point\":\"black|gray|white\",\"at\":[x,y],\"size\":1..101=1} or {\"point\",…,\"color\":[r,g,b] " \
    "0..1}: black/white set each channel's input black/white to the sample, gray each channel's "       \
    "gamma so the sample turns neutral at its luma; RGB and Grayscale (no gray) only, applied over "    \
    "the given levels)"
#define P_CURVES                                                                                        \
    "{\"points\":json,\"red\":json,\"green\":json,\"blue\":json,\"pencil\":bool=false,\"smooth\":0..10" \
    "=0,\"eyedropper\":json} (curves as [[in,out],…] in 0..255, 2..19 points: points = composite; "     \
    "red/green/blue, gray, cyan/magenta/yellow/black or lightness/a/b per channel; pencil = drawn "     \
    "curves (Draw to modify the curve): each curve is 256 output levels in 0..255 (point curves "       \
    "given are drawn in, and turning pencil off fits points to the drawing); smooth = Smooth passes "   \
    "over the drawn curves; eyedropper = the dialog's Set Black/Neutral Gray/White Point picker "       \
    "{\"point\":\"black|gray|white\",\"at\":[x,y],\"size\":1..101=1} or {\"point\",…,\"color\":[r,g,b] " \
    "0..1}, RGB and Grayscale (no gray) only, applied over the given curves)"
#define P_EXPOSURE "{\"exposure\":-20..20=0,\"offset\":-0.5..0.5=0,\"gamma\":0.01..9.99=1}"
#define P_VIBRANCE "{\"vibrance\":-100..100=0,\"saturation\":-100..100=0}"
#define P_HUESAT                                                                                        \
    "{\"hue\":-180..180=0,\"saturation\":-100..100=0,\"lightness\":-100..100=0,\"colorize\":bool=false" \
    ",\"reds\":json,\"yellows\":json,\"greens\":json,\"cyans\":json,\"blues\":json,\"magentas\":json} " \
    "(per range {\"hue\",\"saturation\",\"lightness\",\"range\":[4 hue degrees]}; colorize: hue "       \
    "0..360, saturation 0..100)"
#define P_COLORBAL                                                                                      \
    "{\"shadows\":json,\"midtones\":json,\"highlights\":json,\"preserveLuminosity\":bool=true} (each "  \
    "tone [cyan-red, magenta-green, yellow-blue] in -100..100)"
#define P_BLACKWHITE                                                                                    \
    "{\"reds\":-200..300=40,\"yellows\":-200..300=60,\"greens\":-200..300=40,\"cyans\":-200..300=60,"   \
    "\"blues\":-200..300=20,\"magentas\":-200..300=80,\"tint\":bool=false,\"tintColor\":\"#rrggbb\"}"
#define P_PHOTOFILTER                                                                                   \
    "{\"filter\":\"warming85|warmingLBA|warming81|cooling80|coolingLBB|cooling82|red|orange|yellow|"    \
    "green|cyan|blue|violet|magenta|sepia|deepRed|deepBlue|deepEmerald|deepYellow|underwater\","        \
    "\"color\":\"#rrggbb\",\"density\":0..100=25,\"preserveLuminosity\":bool=true}"
#define P_CHANNELMIXER                                                                                  \
    "{\"red\":json,\"green\":json,\"blue\":json,\"gray\":json,\"monochrome\":bool=false} (per output "  \
    "channel [red %, green %, blue %, constant %] in -200..200; gray = the monochrome mix)"
#define P_INVERT "{}"
#define P_POSTERIZE "{\"levels\":2..255=4}"
#define P_THRESHOLD "{\"level\":1..255=128}"
#define P_GRADIENTMAP                                                                                   \
    "{\"stops\":json,\"reverse\":bool=false,\"dither\":bool=false} (stops [[location 0..1, "            \
    "\"#rrggbb\"], …], 2..64)"
#define P_SELECTIVE                                                                                     \
    "{\"method\":\"relative|absolute\"=\"relative\",\"colors\":\"reds|yellows|greens|cyans|blues|"      \
    "magentas|whites|neutrals|blacks\"=\"reds\",\"cyan\":-100..100=0,\"magenta\":-100..100=0,"           \
    "\"yellow\":-100..100=0,\"black\":-100..100=0,\"reds\":json} (per-range [c,m,y,k] arrays: reds "    \
    "yellows greens cyans blues magentas whites neutrals blacks)"
#define P_COLORLOOKUP                                                                                   \
    "{\"lut\":\"none|warm|cool|tealOrange|bleachBypass|fadedFilm|dayForNight|monoContrast|crossProcess" \
    "\"=\"none\",\"file\":text,\"interpolation\":\"trilinear|tetrahedral\"=\"trilinear\",\"dither\":"    \
    "bool=false,\"data\":json} (file: .cube/.3dl/.look path; data: file text + \"fileName\")"

#define LAYER_P(P) P " + {" NLP "}"

#define ADJ_PAIR(kind, label, params, shortcut)                                                         \
    {"layer.newAdjustmentLayer." kind, label, MENU_LAYER_NEW_ADJ, 2, NULL, LAYER_P(params),             \
     pc_engine_has_doc, run_new_adjustment_layer, true},                                                \
    {                                                                                                   \
        "image.adjustments." kind, label, MENU_IMAGE_ADJ, 2, shortcut, params, has_adjustable,          \
            run_image_adjustment, true                                                                  \
    }

static const pc_engine_command SPECS[] = {
    ADJ_PAIR("brightnessContrast", "Brightness/Contrast…", P_BRIGHTNESS, NULL),
    ADJ_PAIR("levels", "Levels…", P_LEVELS, "Cmd+L"),
    ADJ_PAIR("curves", "Curves…", P_CURVES, "Cmd+M"),
    ADJ_PAIR("exposure", "Exposure…", P_EXPOSURE, NULL),
    ADJ_PAIR("vibrance", "Vibrance…", P_VIBRANCE, NULL),
    ADJ_PAIR("hueSaturation", "Hue/Saturation…", P_HUESAT, "Cmd+U"),
    ADJ_PAIR("colorBalance", "Color Balance…", P_COLORBAL, "Cmd+B"),
    ADJ_PAIR("blackWhite", "Black & White…", P_BLACKWHITE, "Cmd+Alt+Shift+B"),
    ADJ_PAIR("photoFilter", "Photo Filter…", P_PHOTOFILTER, NULL),
    ADJ_PAIR("channelMixer", "Channel Mixer…", P_CHANNELMIXER, NULL),
    ADJ_PAIR("invert", "Invert", P_INVERT, "Cmd+I"),
    ADJ_PAIR("posterize", "Posterize…", P_POSTERIZE, NULL),
    ADJ_PAIR("threshold", "Threshold…", P_THRESHOLD, NULL),
    ADJ_PAIR("gradientMap", "Gradient Map…", P_GRADIENTMAP, NULL),
    ADJ_PAIR("selectiveColor", "Selective Color…", P_SELECTIVE, NULL),
    ADJ_PAIR("colorLookup", "Color Lookup…", P_COLORLOOKUP, NULL),
    /* layer.setAdjustment (no menu). */
    {"layer.setAdjustment", "Adjustment Properties", NULL, 0, NULL,
     "{\"layer\":id?, …params of that adjustment kind} (Levels, Curves: "
     "\"eyedropper\":{\"point\":\"black|gray|white\",\"at\":[x,y],\"size\":1..101=1} or "
     "{\"point\",…,\"color\":[r,g,b] 0..1} = the Properties panel's Set Black/Neutral Gray/White "
     "Point, sampling the image below the layer, RGB and Grayscale (no gray) only)",
     pc_engine_has_layer, run_set_adjustment, true},
    /* Desaturate (Image > Adjustments). */
    {"image.adjustments.desaturate", "Desaturate", MENU_IMAGE_ADJ, 2, "Cmd+Shift+U", "{}",
     has_pixel_layer, run_desaturate, true},
    /* Destructive-only adjustments (Image > Adjustments). */
    {"image.adjustments.shadowsHighlights", "Shadows/Highlights…", MENU_IMAGE_ADJ, 2, NULL,
     "{\"shadowAmount\":0..100=35,\"shadowTone\":0..100=50,\"shadowRadius\":0..2500=30,"
     "\"highlightAmount\":0..100=0,\"highlightTone\":0..100=50,\"highlightRadius\":0..2500=30,"
     "\"color\":-100..100=20,\"midtone\":-100..100=0,\"blackClip\":0..50=0.01,\"whiteClip\":0..50="
     "0.01}",
     has_adjustable, run_shadows_highlights, true},
    {"image.adjustments.replaceColor", "Replace Color…", MENU_IMAGE_ADJ, 2, NULL,
     "{\"color\":color,\"fuzziness\":0..200=40,\"hue\":-180..180=0,\"saturation\":-100..100=0,"
     "\"lightness\":-100..100=0} (color: \"#rrggbb\", default the foreground colour)",
     has_pixels, run_replace_color, true},
    {"image.adjustments.matchColor", "Match Color…", MENU_IMAGE_ADJ, 2, NULL,
     "{\"source\":doc,\"sourceLayer\":json,\"luminance\":1..200=100,\"intensity\":1..200=100,"
     "\"fade\":0..100=0,\"neutralize\":bool=false,\"useSelectionInSource\":bool=false,"
     "\"useSelectionInTarget\":bool=false} (source: document index; sourceLayer: layer id, default "
     "the merged image)",
     has_pixels, run_match_color, true},
    {"image.adjustments.hdrToning", "HDR Toning…", MENU_IMAGE_ADJ, 2, NULL,
     "{\"radius\":1..500=30,\"strength\":0.1..4=0.5,\"gamma\":0.1..2=1,\"exposure\":-5..5=0,"
     "\"detail\":-100..300=30,\"shadow\":-100..100=0,\"highlight\":-100..100=0,\"vibrance\":-100..100"
     "=0,\"saturation\":-100..100=20,\"curve\":[[in,out],…] 0..255} (flattens the image)",
     pc_engine_has_doc, run_hdr_toning, true},
    {"image.adjustments.colorLookup.list", "List Color Lookup Looks", NULL, 0, NULL, "{}",
     pc_engine_always, run_list_looks, false},
};

const pc_engine_command *pc_engine_adjust_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
