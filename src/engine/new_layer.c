/* Layer > New > Layer: Photoshop's New Layer options (name, Use Previous Layer to Create Clipping
 * Mask, color label, blend mode, opacity, Fill with neutral color), in one history step. Port of
 * crates/engine/src/new_layer.rs. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "brush_cmds.h" /* pc_engine_blend_from_str */
#include "layer_base_cmds.h"
#include "raster/pixel.h"

#define CMD_ID "layer.new.layer"

/* The RGB color that leaves everything below unchanged in `mode` (Photoshop's "Fill with neutral
 * color"); false for modes that have none. */
static bool neutral_color(pc_blend_mode mode, float rgb[3])
{
    float v;
    switch (mode) {
    case PC_BLEND_DARKEN:
    case PC_BLEND_MULTIPLY:
    case PC_BLEND_COLOR_BURN:
    case PC_BLEND_LINEAR_BURN:
    case PC_BLEND_DARKER_COLOR:
    case PC_BLEND_DIVIDE:
        v = 1.0f;
        break;
    case PC_BLEND_LIGHTEN:
    case PC_BLEND_SCREEN:
    case PC_BLEND_COLOR_DODGE:
    case PC_BLEND_LINEAR_DODGE:
    case PC_BLEND_LIGHTER_COLOR:
    case PC_BLEND_DIFFERENCE:
    case PC_BLEND_EXCLUSION:
    case PC_BLEND_SUBTRACT:
        v = 0.0f;
        break;
    case PC_BLEND_OVERLAY:
    case PC_BLEND_SOFT_LIGHT:
    case PC_BLEND_HARD_LIGHT:
    case PC_BLEND_VIVID_LIGHT:
    case PC_BLEND_LINEAR_LIGHT:
    case PC_BLEND_PIN_LIGHT:
        v = 0.5f;
        break;
    default:
        return false;
    }
    rgb[0] = rgb[1] = rgb[2] = v;
    return true;
}

/* A boolean param: absent/null = false; a non-bool value is an error. */
static pc_status flag(yyjson_val *p, const char *key, bool *out, pc_error *err)
{
    *out = false;
    yyjson_val *v = (p != NULL) ? pc_json_get(p, key) : NULL;
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    if (!pc_json_as_bool(v, out)) {
        return pc_engine_err_bad_params(err, CMD_ID, "`%s` must be true or false", key);
    }
    return PC_OK;
}

typedef struct {
    bool have_name;
    char *name; /* owned */
    pc_blend_mode mode;
    float opacity;
    pc_label_color label;
    bool fill_neutral;
    float neutral[3];
    bool clip;
    pc_layer_id out_id;
} new_layer_ctx;

static pc_status edit_new_layer(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    new_layer_ctx *ctx = vctx;
    pc_str nm = {0};
    if (ctx->have_name) {
        if (pc_str_set_c(&nm, ctx->name, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    } else if (pc_document_next_layer_name(doc, "Layer", &nm, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer layer;
    pc_status rc = pc_layer_raster(&layer, pc_str_c(&nm), pc_document_pixel_format(doc), err);
    pc_str_fini(&nm);
    if (rc != PC_OK) {
        return rc;
    }
    layer.blend = ctx->mode;
    layer.opacity = ctx->opacity;
    layer.label = ctx->label;
    if (ctx->fill_neutral) {
        pc_surface *surf = pc_layer_surface_mut(&layer);
        if (surf == NULL) {
            pc_layer_fini(&layer);
            return pc_engine_err_bad_params(err, CMD_ID, "a new pixel layer has no pixels");
        }
        float rgba[4] = {ctx->neutral[0], ctx->neutral[1], ctx->neutral[2], 1.0f};
        float px[PC_RASTER_MAX_CHANNELS];
        size_t m = pc_from_rgba(pc_surface_format(surf), rgba, px);
        if (pc_surface_fill_rect(surf, pc_document_bounds(doc), px, m, err) != PC_OK) {
            pc_layer_fini(&layer);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }
    const pc_layer_id *above = *ha ? active : NULL;
    pc_layer_id id;
    if (pc_document_insert_above(doc, above, &layer, &id, err) != PC_OK) {
        pc_layer_fini(&layer);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (pc_engine_clip_settle(doc, &id, 1, NULL, ctx->clip, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    ctx->out_id = id;
    *ha = true;
    *active = id;
    return PC_OK;
}

static pc_status run_new_layer(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    if (p != NULL && !yyjson_is_obj(p) && !yyjson_is_null(p)) {
        return pc_engine_err_bad_params(err, CMD_ID, "expected an object");
    }
    new_layer_ctx ctx = {0};
    ctx.mode = PC_BLEND_NORMAL;
    ctx.opacity = 1.0f;
    ctx.label = PC_LABEL_NONE;
    if (flag(p, "clipToPrevious", &ctx.clip, err) != PC_OK) {
        return err->code;
    }
    if (flag(p, "fillNeutral", &ctx.fill_neutral, err) != PC_OK) {
        return err->code;
    }
    yyjson_val *mv = (p != NULL) ? pc_json_get(p, "mode") : NULL;
    if (mv != NULL && !yyjson_is_null(mv)) {
        size_t len = 0;
        const char *name = pc_json_as_str(mv, &len);
        if (name == NULL) {
            return pc_engine_err_bad_params(err, CMD_ID, "`mode` must be a blend mode name");
        }
        if (!pc_engine_blend_from_str(name, len, &ctx.mode) || ctx.mode == PC_BLEND_PASS_THROUGH) {
            return pc_engine_err_bad_params(err, CMD_ID, "unknown layer blend mode `%.*s`", (int)len, name);
        }
    }
    yyjson_val *ov = (p != NULL) ? pc_json_get(p, "opacity") : NULL;
    if (ov != NULL && !yyjson_is_null(ov)) {
        double o;
        if (!pc_json_as_f64(ov, &o) || !isfinite(o)) {
            return pc_engine_err_bad_params(err, CMD_ID, "`opacity` must be a number from 0 to 1");
        }
        if (o < 0.0) {
            o = 0.0;
        } else if (o > 1.0) {
            o = 1.0;
        }
        ctx.opacity = (float)o;
    }
    yyjson_val *cv = (p != NULL) ? pc_json_get(p, "color") : NULL;
    if (cv != NULL && !yyjson_is_null(cv)) {
        size_t len = 0;
        const char *id = pc_json_as_str(cv, &len);
        if (id == NULL || !pc_label_color_from_id(id, len, &ctx.label)) {
            return pc_engine_err_bad_params(err, CMD_ID, "`color` must be a layer color id");
        }
    }
    if (ctx.fill_neutral && !neutral_color(ctx.mode, ctx.neutral)) {
        return pc_engine_err_bad_params(err, CMD_ID, "%s has no neutral color to fill with",
                                        pc_blend_label(ctx.mode));
    }
    yyjson_val *nv = (p != NULL) ? pc_json_get(p, "name") : NULL;
    if (nv != NULL && !yyjson_is_null(nv)) {
        size_t len = 0;
        const char *name = pc_json_as_str(nv, &len);
        if (name == NULL) {
            return pc_engine_err_bad_params(err, CMD_ID, "`name` must be a string");
        }
        ctx.name = malloc(len + 1);
        if (ctx.name == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "new layer");
        }
        memcpy(ctx.name, name, len);
        ctx.name[len] = '\0';
        ctx.have_name = true;
    }
    pc_status rc = pc_engine_session_edit(s, "New Layer", edit_new_layer, &ctx, err);
    free(ctx.name);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", ctx.out_id);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const char *const MENU_NEW[] = {"Layer", "New"};

static const pc_engine_command SPECS[] = {
    {"layer.new.layer", "Layer\xe2\x80\xa6", MENU_NEW, 2, "Cmd+Shift+N",
     "{\"name\":str?,\"clipToPrevious\":bool=false,\"color\":\"none|red|orange|yellow|green|seafoam|"
     "blue|indigo|magenta|fuchsia|violet|gray\"=\"none\",\"mode\":\"Normal|Multiply|\xe2\x80\xa6\"="
     "\"Normal\",\"opacity\":0..1=1,\"fillNeutral\":bool=false}",
     pc_engine_has_doc, run_new_layer, true},
};

const pc_engine_command *pc_engine_new_layer_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
