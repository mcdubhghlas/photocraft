/* Layer > New Fill Layer: Solid Color and Gradient (port of the two newFillLayer entries in
 * crates/engine/src/commands.rs). A fill layer carries a Fill (solid colour or gradient) as its
 * content; the active selection becomes its layer mask (the "path" vector-mask case is deferred,
 * matching the new-adjustment-layer behaviour in adjust_cmds.c and documented in REWRITE_PLAN §7).
 *
 * Never crashes on hostile params (REWRITE_PLAN.md rule 9): colours fall back to the tool colours,
 * numbers to their defaults, and a missing document returns a graceful error. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "doc/fill.h"

#include "engine_internal.h"
#include "fill_layer_gap_cmds.h"
#include "layer_style.h" /* pc_engine_gradient_style */

/* ---- mask-for-new-layer (commands::mask_new_layer; selection only, "path" deferred) -------- */

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
            err, "a path (vector) mask for a new fill layer is not yet supported in this build");
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

/* ---- create the fill layer ----------------------------------------------------------------- */

struct fill_ctx {
    pc_fill fill;   /* moved into the new layer */
    const char *base; /* "Color Fill" / "Gradient Fill" */
    yyjson_val *p;
    pc_layer_id id; /* out */
};

static pc_status fill_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    struct fill_ctx *c = vctx;
    pc_fill_in_mode(&c->fill, doc->mode);
    pc_str nm;
    nm.ptr = NULL;
    nm.len = 0;
    if (pc_document_next_layer_name(doc, c->base, &nm, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer_content content;
    memset(&content, 0, sizeof content);
    content.kind = PC_LAYER_FILL;
    content.u.fill = c->fill;
    memset(&c->fill, 0, sizeof c->fill); /* moved into content */
    pc_layer l;
    pc_status rc = pc_layer_init(&l, pc_str_c(&nm), &content, err); /* takes over content on success */
    pc_str_fini(&nm);
    if (rc != PC_OK) {
        pc_layer_content_fini(&content);
        return rc;
    }
    rc = mask_new_layer(doc, &l, c->p, err);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    const pc_layer_id *above = *ha ? active : NULL;
    rc = pc_document_insert_above(doc, above, &l, &c->id, err);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    *active = c->id;
    *ha = true;
    return PC_OK;
}

static pc_status finish(pc_engine_session *s, struct fill_ctx *c, const char *label, pc_json_value *out,
                        pc_error *err)
{
    pc_status rc = pc_engine_session_edit(s, label, fill_edit, c, err);
    pc_fill_fini(&c->fill); /* no-op when moved */
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", c->id);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_solid(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const float *fg = pc_engine_session_tools(s)->foreground;
    float rgba[4];
    pc_engine_param_color(p, "color", fg, rgba);
    struct fill_ctx c;
    memset(&c, 0, sizeof c);
    c.fill = pc_fill_solid(pc_color_rgba(rgba[0], rgba[1], rgba[2], rgba[3]));
    c.base = "Color Fill";
    c.p = p;
    return finish(s, &c, "New Color Fill Layer", out, err);
}

static float f_or(yyjson_val *p, const char *key, float def)
{
    yyjson_val *v = p != NULL ? pc_json_get(p, key) : NULL;
    double d;
    if (v != NULL && pc_json_as_f64(v, &d)) {
        return (float)d;
    }
    return def;
}

static pc_status run_gradient(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_engine_tool_state *tools = pc_engine_session_tools(s);
    float a[4], b[4];
    pc_engine_param_color(p, "from", tools->foreground, a);
    pc_engine_param_color(p, "to", tools->background, b);
    float angle = f_or(p, "angle", 90.0f);
    size_t slen = 0;
    yyjson_val *sv = p != NULL ? pc_json_get(p, "style") : NULL;
    const char *style_name = sv != NULL ? pc_json_as_str(sv, &slen) : NULL;
    pc_gradient_style style = pc_engine_gradient_style(style_name != NULL ? style_name : "linear");
    bool reverse = false;
    yyjson_val *rv = p != NULL ? pc_json_get(p, "reverse") : NULL;
    bool rb;
    if (rv != NULL && pc_json_as_bool(rv, &rb)) {
        reverse = rb;
    }
    pc_color_stop stops[2] = {{0.0f, pc_color_rgba(a[0], a[1], a[2], a[3])},
                              {1.0f, pc_color_rgba(b[0], b[1], b[2], b[3])}};
    struct fill_ctx c;
    memset(&c, 0, sizeof c);
    if (pc_fill_gradient(stops, 2, angle, 1.0f, style, reverse, &c.fill, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    c.base = "Gradient Fill";
    c.p = p;
    return finish(s, &c, "New Gradient Fill Layer", out, err);
}

/* ---- registry ------------------------------------------------------------------------------ */

static const char *const MENU_NEWFILL[] = {"Layer", "New Fill Layer"};

static const pc_engine_command SPECS[] = {
    {"layer.newFillLayer.solidColor", "Solid Color…", MENU_NEWFILL, 2, NULL,
     "{\"color\":\"#rrggbb\"=foreground,\"path\":... (deferred)} (the selection becomes the layer mask)",
     pc_engine_has_doc, run_solid, true},
    {"layer.newFillLayer.gradient", "Gradient…", MENU_NEWFILL, 2, NULL,
     "{\"from\":\"#rrggbb\",\"to\":\"#rrggbb\",\"angle\":deg=90,"
     "\"style\":\"linear|radial|angle|reflected|diamond\",\"reverse\":bool} (selection = layer mask)",
     pc_engine_has_doc, run_gradient, true},
};

const pc_engine_command *pc_engine_fill_layer_gap_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
