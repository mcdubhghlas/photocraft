/* Layer > New > Background from Layer: layer.new.backgroundFromLayer (port of
 * crates/engine/src/background_cmds.rs).
 *
 * Turns the selected layer into the document's Background: it is rendered alone (mask, opacity and
 * effects applied) over the background colour, so it becomes an opaque, plain, locked raster layer
 * that drops to the bottom of the stack, keeping its original id and visibility. A document that
 * already has a Background, or whose active layer is a group or adjustment layer, refuses. The
 * rule that keeps the Background at the bottom lives in clip_cmds (keep_background_at_bottom).
 *
 * Never crashes (REWRITE_PLAN.md rule 9): params and layer kind are validated and the one history
 * step is all-or-nothing. */
#include <stddef.h>
#include <string.h>

#include "base/json.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "background_cmds.h"
#include "engine_internal.h"
#include "layer_cmds.h" /* pc_engine_layer_param */

#define CMD "layer.new.backgroundFromLayer"

static const char *const MENU_LAYER_NEW[] = {"Layer", "New"};

/* Groups and adjustment layers have no pixels of their own. */
static bool convertible(const pc_layer *l)
{
    return l->content.kind != PC_LAYER_GROUP && l->content.kind != PC_LAYER_ADJUSTMENT;
}

static pc_status bg_enabled(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (doc == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (pc_document_background(doc) != NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "the document already has a Background layer");
    }
    if (!d->has_active_layer) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    if (l == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    if (!convertible(l)) {
        return pc_fail(reason, PC_ERR_INVALID, "%s %s layer can't become the Background",
                       pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
    }
    return PC_OK;
}

struct bg_ctx {
    pc_layer_id id;
    float bg[4];
};

static pc_status bg_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    struct bg_ctx *c = vctx;
    if (pc_document_background(doc) != NULL) {
        return pc_engine_err_other(err, "the document already has a Background layer");
    }
    const pc_layer *l = pc_document_layer(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    if (!convertible(l)) {
        return pc_engine_err_other(err, "%s %s layer can't become the Background",
                                   pc_layer_content_article(&l->content),
                                   pc_layer_content_kind_name(&l->content));
    }
    bool visible = l->visible;
    pc_pixel_format fmt = pc_document_pixel_format(doc);

    /* Render the layer alone, as it looks, over the (opaque) background colour. */
    pc_layer only;
    pc_status rc = pc_layer_clone(l, &only, err);
    if (rc != PC_OK) {
        return rc;
    }
    only.visible = true;
    only.clipped = false;
    pc_layer *saved_layers = doc->layers;
    size_t saved_n = doc->nlayers;
    doc->layers = &only;
    doc->nlayers = 1;
    float bg3[3] = {c->bg[0], c->bg[1], c->bg[2]};
    pc_surface pixels;
    rc = pc_compose_flatten_to_surface(doc, fmt, bg3, &pixels, err);
    doc->layers = saved_layers;
    doc->nlayers = saved_n;
    pc_layer_fini(&only);
    if (rc != PC_OK) {
        return rc;
    }

    pc_layer out;
    rc = pc_layer_raster(&out, "Background", fmt, err);
    if (rc != PC_OK) {
        pc_surface_fini(&pixels);
        return rc;
    }
    out.id = c->id;
    out.visible = visible;
    out.locks.transparency = true;
    out.locks.position = true;
    pc_surface *su = pc_layer_surface_mut(&out);
    if (su == NULL) {
        pc_surface_fini(&pixels);
        pc_layer_fini(&out);
        return pc_engine_err_other(err, "the Background could not be built");
    }
    pc_surface_fini(su);
    *su = pixels; /* moved */

    /* Replace the original with the Background at the bottom of the stack. */
    pc_layer old;
    rc = pc_document_remove(doc, c->id, &old, err);
    if (rc != PC_OK) {
        pc_layer_fini(&out);
        return rc;
    }
    pc_layer_fini(&old);
    pc_layer_id newid = 0;
    if (doc->nlayers > 0) {
        pc_layer_id bottom = doc->layers[0].id;
        rc = pc_document_insert_above(doc, &bottom, &out, &newid, err);
        if (rc == PC_OK) {
            (void)pc_document_shift(doc, newid, -1); /* above-bottom -> bottom */
        }
    } else {
        rc = pc_document_insert_above(doc, NULL, &out, &newid, err);
    }
    if (rc != PC_OK) {
        pc_layer_fini(&out);
        return rc;
    }
    *active = c->id;
    return PC_OK;
}

static pc_status run_bg(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (doc != NULL && pc_document_background(doc) != NULL) {
        return pc_engine_err_other(err, "the document already has a Background layer");
    }
    struct bg_ctx c;
    memset(&c, 0, sizeof c);
    /* A present, non-null `layer` must be a layer id (as the reference requires); otherwise the
     * active layer. (pc_engine_layer_param falls back to the active layer for a bad value, so the
     * explicit type check below keeps parity with background_cmds.rs.) */
    yyjson_val *jl = p != NULL ? pc_json_get(p, "layer") : NULL;
    pc_status rc;
    if (jl != NULL && !yyjson_is_null(jl)) {
        uint64_t idv = 0;
        if (!pc_json_as_u64(jl, &idv)) {
            return pc_engine_err_bad_params(err, CMD, "`layer` must be a layer id");
        }
        c.id = (pc_layer_id)idv;
    } else {
        rc = pc_engine_layer_param(s, NULL, &c.id, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    memcpy(c.bg, s->tools.background, sizeof c.bg);

    rc = pc_engine_session_edit(s, "Background From Layer", bg_edit, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_engine_session_select_layer(s, c.id, err);
    if (rc != PC_OK) {
        return rc;
    }

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", c.id);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

#define BG_PARAMS                                                                                    \
    "{\"layer\":id?} (the active layer by default; transparent pixels take the background colour "    \
    "and the layer drops to the bottom as the locked Background)"

static const pc_engine_command SPECS[] = {
    {CMD, "Background From Layer", MENU_LAYER_NEW, 2, NULL, BG_PARAMS, bg_enabled, run_bg, true},
};

const pc_engine_command *pc_engine_background_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
