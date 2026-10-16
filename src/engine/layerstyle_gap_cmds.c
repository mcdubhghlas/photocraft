/* Layer > Layer Style: Blending Options, Global Light and Scale Effects (port of
 * crates/engine/src/layer_menu_cmds.rs: blending_options / global_light / scale_effects and the
 * Advanced-Blending and Blend-If parameter parsing they share).
 *
 *   - layer.layerStyle.blendingOptions: blend mode, opacity, fill opacity, Blend If and the
 *     Advanced Blending flags (knockout, interior/clipped grouping, transparency shapes, mask
 *     hides effects, excluded colour channels). Any subset of keys; the rest are kept.
 *   - layer.layerStyle.globalLight: the document's global light angle and altitude.
 *   - layer.layerStyle.scaleEffects: scale the active layer's effects (reuses Image Size's Scale
 *     Styles maths, pc_engine_scale_effects).
 *
 * Never crashes on hostile params (REWRITE_PLAN.md rule 9): every number is validated before use
 * (finite, in range, correct type) and a bad value returns a graceful error, never a panic. */
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/blend_if.h"
#include "doc/doc.h"
#include "doc/effects.h"

#include "brush_cmds.h" /* pc_engine_blend_from_str */
#include "engine_internal.h"
#include "layer_cmds.h" /* pc_engine_layer_param */
#include "layerstyle_gap_cmds.h"

#define CMD_BLEND "layer.layerStyle.blendingOptions"
#define CMD_LIGHT "layer.layerStyle.globalLight"

/* ---- enabled predicates -------------------------------------------------------------------- */

static pc_status has_effects(const pc_engine_session *s, pc_error *reason)
{
    const pc_layer *l = pc_engine_active_layer_of(s, reason);
    if (l == NULL) {
        return reason != NULL ? reason->code : PC_ERR_INVALID;
    }
    for (size_t i = 0; i < l->effects.nitems; i++) {
        if (pc_effect_enabled(&l->effects.items[i])) {
            return PC_OK;
        }
    }
    return pc_fail(reason, PC_ERR_INVALID, "the layer has no layer effects");
}

/* ---- Blending Options ---------------------------------------------------------------------- */

/* A parsed, validated Blend If entry: a range index, and the two optional ranges (This / Under). */
struct blendif_entry {
    size_t index;
    bool has_this, has_under;
    pc_blend_range this_r, under_r;
};

struct blend_params {
    pc_layer_id id;
    bool has_blend;
    pc_blend_mode blend;
    bool has_opacity;
    float opacity;
    bool has_fill;
    float fill;
    /* Blend If: none = no `blendIf` key; reset = `null`; else a list of entries. */
    enum { BI_NONE, BI_RESET, BI_ENTRIES } blend_if_mode;
    struct blendif_entry *entries;
    size_t nentries;
    /* Advanced Blending (each has_* false = keep). */
    bool has_knockout;
    pc_knockout knockout;
    bool has_blend_interior, blend_interior;
    bool has_blend_clipped, blend_clipped;
    bool has_transparency_shapes, transparency_shapes;
    bool has_mask_hides, mask_hides;
    bool has_vmask_hides, vmask_hides;
    bool has_excluded;
    uint32_t excluded;
};

/* Blend If channel -> range index: 0/1 Gray, then the mode's colour channels. Mirrors
 * blend_if_channel. Returns false for an unknown channel. */
static bool blend_if_channel(yyjson_val *v, pc_color_mode mode, size_t *out)
{
    size_t n = pc_color_mode_channels(mode);
    uint64_t u;
    if (v != NULL && pc_json_as_u64(v, &u)) {
        if (u <= (uint64_t)n) {
            *out = (size_t)u;
            return true;
        }
        return false;
    }
    size_t len = 0;
    const char *name = v != NULL ? pc_json_as_str(v, &len) : NULL;
    char lower[16];
    size_t k = 0;
    if (name != NULL) {
        for (; k < len && k < sizeof lower - 1; k++) {
            char c = name[k];
            lower[k] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
    }
    lower[k] = '\0';
    if (name == NULL || k == 0 || strcmp(lower, "gray") == 0 || strcmp(lower, "grey") == 0) {
        *out = (n == 1) ? 1 : 0;
        return true;
    }
    const char *const *names = NULL;
    static const char *const rgb[] = {"red", "green", "blue", NULL};
    static const char *const cmyk[] = {"cyan", "magenta", "yellow", "black", NULL};
    static const char *const lab[] = {"lightness", "a", "b", NULL};
    if (mode == PC_COLOR_MODE_RGB || mode == PC_COLOR_MODE_INDEXED) {
        names = rgb;
    } else if (mode == PC_COLOR_MODE_CMYK) {
        names = cmyk;
    } else if (mode == PC_COLOR_MODE_LAB) {
        names = lab;
    }
    if (names != NULL) {
        for (size_t i = 0; names[i] != NULL; i++) {
            if (strcmp(lower, names[i]) == 0) {
                *out = i + 1;
                return true;
            }
        }
    }
    return false;
}

/* Parse [black, white] or [blackLo, blackHi, whiteLo, whiteHi], each 0..=255 in increasing order. */
static bool parse_blend_range(yyjson_val *v, pc_blend_range *out)
{
    if (v == NULL || !yyjson_is_arr(v)) {
        return false;
    }
    size_t len = yyjson_arr_size(v);
    if (len != 2 && len != 4) {
        return false;
    }
    uint8_t q[4];
    double vals[4];
    for (size_t i = 0; i < len; i++) {
        yyjson_val *x = yyjson_arr_get(v, i);
        double d;
        if (x == NULL || !pc_json_as_f64(x, &d) || !(d >= 0.0 && d <= 255.0)) {
            return false;
        }
        vals[i] = d;
    }
    if (len == 2) {
        q[0] = q[1] = (uint8_t)(vals[0] + 0.5);
        q[2] = q[3] = (uint8_t)(vals[1] + 0.5);
    } else {
        for (int i = 0; i < 4; i++) {
            q[i] = (uint8_t)(vals[i] + 0.5);
        }
    }
    if (q[0] > q[1] || q[1] > q[2] || q[2] > q[3]) {
        return false;
    }
    *out = pc_blend_range_from_bytes(q);
    return true;
}

/* Parse one blendIf object into *e. Returns PC_OK or a graceful error. */
static pc_status parse_blendif_item(yyjson_val *it, pc_color_mode mode, struct blendif_entry *e,
                                    pc_error *err)
{
    if (it == NULL || !yyjson_is_obj(it)) {
        return pc_engine_err_bad_params(err, CMD_BLEND, "blendIf: expected an object");
    }
    yyjson_val *ch = pc_json_get(it, "channel");
    if (!blend_if_channel(ch, mode, &e->index)) {
        return pc_engine_err_bad_params(err, CMD_BLEND, "blendIf: unknown channel");
    }
    e->has_this = false;
    e->has_under = false;
    yyjson_val *t = pc_json_get(it, "thisLayer");
    if (t != NULL) {
        if (!parse_blend_range(t, &e->this_r)) {
            return pc_engine_err_bad_params(err, CMD_BLEND,
                                            "blendIf.thisLayer: expected [black,white] or "
                                            "[blackLo,blackHi,whiteLo,whiteHi], 0..255, increasing");
        }
        e->has_this = true;
    }
    yyjson_val *u = pc_json_get(it, "underlying");
    if (u != NULL) {
        if (!parse_blend_range(u, &e->under_r)) {
            return pc_engine_err_bad_params(err, CMD_BLEND,
                                            "blendIf.underlying: expected [black,white] or "
                                            "[blackLo,blackHi,whiteLo,whiteHi], 0..255, increasing");
        }
        e->has_under = true;
    }
    return PC_OK;
}

/* Parse an optional boolean flag. *set stays false when the key is missing or null. */
static pc_status parse_flag(yyjson_val *p, const char *key, bool *set, bool *val, pc_error *err)
{
    *set = false;
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    bool b;
    if (!pc_json_as_bool(v, &b)) {
        return pc_engine_err_bad_params(err, CMD_BLEND, "%s: expected true or false", key);
    }
    *set = true;
    *val = b;
    return PC_OK;
}

static pc_status parse_blend_params(const pc_engine_session *s, yyjson_val *p, struct blend_params *bp,
                                    pc_error *err)
{
    memset(bp, 0, sizeof *bp);
    if (pc_engine_layer_param(s, p, &bp->id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_color_mode mode = pc_doc_arc_get(d->doc)->mode;

    yyjson_val *bv = pc_json_get(p, "blend");
    if (bv != NULL && yyjson_is_str(bv)) {
        size_t len = 0;
        const char *bs = pc_json_as_str(bv, &len);
        if (bs == NULL || !pc_engine_blend_from_str(bs, len, &bp->blend)) {
            return pc_engine_err_bad_params(err, CMD_BLEND, "unknown blend mode");
        }
        bp->has_blend = true;
    }
    yyjson_val *ov = pc_json_get(p, "opacity");
    double od;
    if (ov != NULL && pc_json_as_f64(ov, &od)) {
        float o = (float)(od / 100.0);
        bp->opacity = o < 0.0f ? 0.0f : (o > 1.0f ? 1.0f : o);
        bp->has_opacity = true;
    }
    yyjson_val *fv = pc_json_get(p, "fillOpacity");
    double fd;
    if (fv != NULL && pc_json_as_f64(fv, &fd)) {
        float f = (float)(fd / 100.0);
        bp->fill = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
        bp->has_fill = true;
    }

    yyjson_val *bi = pc_json_get(p, "blendIf");
    if (bi == NULL) {
        bp->blend_if_mode = BI_NONE;
    } else if (yyjson_is_null(bi)) {
        bp->blend_if_mode = BI_RESET;
    } else {
        size_t count = yyjson_is_arr(bi) ? yyjson_arr_size(bi) : (yyjson_is_obj(bi) ? 1 : 0);
        if (count == 0 && !yyjson_is_arr(bi)) {
            return pc_engine_err_bad_params(err, CMD_BLEND,
                                            "blendIf: expected an object, a list of objects or null");
        }
        bp->blend_if_mode = BI_ENTRIES;
        if (count > 0) {
            bp->entries = pc_malloc_array(count, sizeof *bp->entries);
            if (bp->entries == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "blendIf");
            }
        }
        bp->nentries = count;
        if (yyjson_is_obj(bi)) {
            pc_status rc = parse_blendif_item(bi, mode, &bp->entries[0], err);
            if (rc != PC_OK) {
                return rc;
            }
        } else {
            for (size_t i = 0; i < count; i++) {
                pc_status rc = parse_blendif_item(yyjson_arr_get(bi, i), mode, &bp->entries[i], err);
                if (rc != PC_OK) {
                    return rc;
                }
            }
        }
    }

    /* Advanced Blending. */
    yyjson_val *kv = pc_json_get(p, "knockout");
    if (kv != NULL && !yyjson_is_null(kv)) {
        size_t len = 0;
        const char *ks = yyjson_is_str(kv) ? pc_json_as_str(kv, &len) : NULL;
        if (ks == NULL || !pc_knockout_from_name(ks, len, &bp->knockout)) {
            return pc_engine_err_bad_params(err, CMD_BLEND,
                                            "knockout: expected none, shallow or deep");
        }
        bp->has_knockout = true;
    }
    yyjson_val *cv = pc_json_get(p, "channels");
    if (cv != NULL && !yyjson_is_null(cv)) {
        if (!yyjson_is_arr(cv)) {
            return pc_engine_err_bad_params(err, CMD_BLEND,
                                            "channels: expected a list of true/false");
        }
        size_t n = pc_color_mode_channels(mode);
        if (n == 0) {
            n = 1;
        }
        size_t len = yyjson_arr_size(cv);
        if (len > n) {
            return pc_engine_err_bad_params(err, CMD_BLEND, "channels: too many for this mode");
        }
        uint32_t mask = 0;
        for (size_t i = 0; i < len; i++) {
            yyjson_val *x = yyjson_arr_get(cv, i);
            bool b;
            if (x == NULL || !pc_json_as_bool(x, &b)) {
                return pc_engine_err_bad_params(err, CMD_BLEND,
                                                "channels[%zu]: expected true or false", i);
            }
            if (!b) {
                mask |= (uint32_t)1 << i;
            }
        }
        bp->has_excluded = true;
        bp->excluded = mask;
    }
    pc_status rc;
    if ((rc = parse_flag(p, "blendInteriorEffectsAsGroup", &bp->has_blend_interior, &bp->blend_interior,
                         err)) != PC_OK ||
        (rc = parse_flag(p, "blendClippedLayersAsGroup", &bp->has_blend_clipped, &bp->blend_clipped,
                         err)) != PC_OK ||
        (rc = parse_flag(p, "transparencyShapesLayer", &bp->has_transparency_shapes,
                         &bp->transparency_shapes, err)) != PC_OK ||
        (rc = parse_flag(p, "layerMaskHidesEffects", &bp->has_mask_hides, &bp->mask_hides, err)) !=
            PC_OK ||
        (rc = parse_flag(p, "vectorMaskHidesEffects", &bp->has_vmask_hides, &bp->vmask_hides, err)) !=
            PC_OK) {
        return rc;
    }
    return PC_OK;
}

static pc_status blending_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct blend_params *bp = vctx;
    pc_layer *l = pc_document_layer_mut(doc, bp->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, bp->id);
    }
    if (bp->has_blend) {
        if (bp->blend == PC_BLEND_PASS_THROUGH && l->content.kind != PC_LAYER_GROUP) {
            return pc_engine_err_bad_params(err, CMD_BLEND, "Pass Through is for groups");
        }
        l->blend = bp->blend;
    }
    if (bp->has_opacity) {
        l->opacity = bp->opacity;
    }
    if (bp->has_fill) {
        l->fill_opacity = bp->fill;
    }
    if (bp->blend_if_mode == BI_RESET) {
        pc_blend_if_fini(&l->blend_if);
        memset(&l->blend_if, 0, sizeof l->blend_if);
    } else if (bp->blend_if_mode == BI_ENTRIES) {
        for (size_t i = 0; i < bp->nentries; i++) {
            struct blendif_entry *e = &bp->entries[i];
            pc_blend_range cur[2];
            pc_blend_if_get(&l->blend_if, e->index, cur);
            pc_blend_range pair[2] = {e->has_this ? e->this_r : cur[0],
                                      e->has_under ? e->under_r : cur[1]};
            if (pc_blend_if_set(&l->blend_if, e->index, pair, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
        }
    }
    if (bp->has_knockout) {
        l->advanced.knockout = bp->knockout;
    }
    if (bp->has_blend_interior) {
        l->advanced.blend_interior = bp->blend_interior;
    }
    if (bp->has_blend_clipped) {
        l->advanced.blend_clipped = bp->blend_clipped;
    }
    if (bp->has_transparency_shapes) {
        l->advanced.transparency_shapes = bp->transparency_shapes;
    }
    if (bp->has_mask_hides) {
        l->advanced.layer_mask_hides_effects = bp->mask_hides;
    }
    if (bp->has_vmask_hides) {
        l->advanced.vector_mask_hides_effects = bp->vmask_hides;
    }
    if (bp->has_excluded) {
        l->excluded_channels = bp->excluded;
    }
    return PC_OK;
}

static pc_status run_blending_options(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                      pc_error *err)
{
    struct blend_params bp;
    pc_status rc = parse_blend_params(s, p, &bp, err);
    if (rc != PC_OK) {
        free(bp.entries);
        return rc;
    }
    rc = pc_engine_session_edit(s, "Blending Options", blending_edit, &bp, err);
    free(bp.entries);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

/* ---- Global Light -------------------------------------------------------------------------- */

struct light_ctx {
    float angle, altitude;
};

static pc_status light_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    (void)err;
    struct light_ctx *c = vctx;
    doc->global_light.angle = c->angle;
    doc->global_light.altitude = c->altitude;
    return PC_OK;
}

/* Read key as f64, reject what f32 cannot hold (1e39 -> inf). */
static pc_status read_f32(yyjson_val *p, const char *key, float def, float *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, key);
    double d;
    if (v == NULL || !pc_json_as_f64(v, &d)) {
        *out = def;
        return PC_OK;
    }
    if (!isfinite(d) || fabs(d) > (double)FLT_MAX) {
        return pc_engine_err_bad_params(err, CMD_LIGHT, "%s must be a finite number", key);
    }
    *out = (float)d;
    return PC_OK;
}

static pc_status run_global_light(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_global_light cur = pc_doc_arc_get(d->doc)->global_light;
    struct light_ctx c = {cur.angle, cur.altitude};
    pc_status rc = read_f32(p, "angle", cur.angle, &c.angle, err);
    if (rc != PC_OK) {
        return rc;
    }
    float alt = cur.altitude;
    rc = read_f32(p, "altitude", cur.altitude, &alt, err);
    if (rc != PC_OK) {
        return rc;
    }
    c.altitude = alt < 0.0f ? 0.0f : (alt > 90.0f ? 90.0f : alt);
    /* Wrap angle into (-180, 180] via rem_euclid(360) - 180. */
    float a = fmodf(c.angle + 180.0f, 360.0f);
    if (a < 0.0f) {
        a += 360.0f;
    }
    c.angle = a - 180.0f;
    rc = pc_engine_session_edit(s, "Global Light", light_edit, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_real(rd, root, "angle", (double)c.angle);
    yyjson_mut_obj_add_real(rd, root, "altitude", (double)c.altitude);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- Scale Effects ------------------------------------------------------------------------- */

struct scale_ctx {
    pc_layer_id id;
    float k;
};

static pc_status scale_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct scale_ctx *c = vctx;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    pc_engine_scale_effects(&l->effects, c->k);
    pc_blob_release(l->effects.psd_raw);
    l->effects.psd_raw = NULL;
    return PC_OK;
}

static pc_status run_scale_effects(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    double scale = 100.0;
    yyjson_val *v = p != NULL ? pc_json_get(p, "scale") : NULL;
    double d;
    if (v != NULL && pc_json_as_f64(v, &d)) {
        scale = d;
    }
    if (!(scale >= 1.0)) { /* NaN too */
        scale = 1.0;
    }
    if (scale > 1000.0) {
        scale = 1000.0;
    }
    struct scale_ctx c = {0, (float)(scale / 100.0)};
    if (pc_engine_layer_param(s, p, &c.id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (pc_engine_session_edit(s, "Scale Effects", scale_edit, &c, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

/* ---- registry ------------------------------------------------------------------------------ */

static const char *const MENU_STYLE[] = {"Layer", "Layer Style"};

static const pc_engine_command SPECS[] = {
    {"layer.layerStyle.blendingOptions", "Blending Options…", MENU_STYLE, 2, NULL,
     "{\"layer\":id?,\"blend\":\"normal|multiply|…\"?,\"opacity\":0..100?,\"fillOpacity\":0..100?,"
     "\"blendIf\":{\"channel\":\"gray|red|…\"|index,\"thisLayer\":[…]?,\"underlying\":[…]?}|[…]|null?,"
     "\"channels\":[bool,…]?,\"knockout\":\"none|shallow|deep\"?,\"blendInteriorEffectsAsGroup\":bool?,"
     "\"blendClippedLayersAsGroup\":bool?,\"transparencyShapesLayer\":bool?,"
     "\"layerMaskHidesEffects\":bool?,\"vectorMaskHidesEffects\":bool?} (Blend If 0..255; null resets)",
     pc_engine_has_layer, run_blending_options, true},
    {"layer.layerStyle.globalLight", "Global Light…", MENU_STYLE, 2, NULL,
     "{\"angle\":-180..180=120,\"altitude\":0..90=30}", pc_engine_has_doc, run_global_light, true},
    {"layer.layerStyle.scaleEffects", "Scale Effects…", MENU_STYLE, 2, NULL, "{\"scale\":1..1000=100}",
     has_effects, run_scale_effects, true},
};

const pc_engine_command *pc_engine_layerstyle_gap_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
