/* Image > Trap: a CMYK prepress step (port of crates/engine/src/trap_cmds.rs, E9).
 *
 * Trapping spreads each ink slightly under already-inked pixels so internal colour edges get a
 * trap band. Photoshop requires a flattened CMYK image; we trap the active pixel layer's inks over
 * the canvas as one history step. The algorithm itself is pc_algo_trap (src/algo/trap.c). */
#include <math.h>
#include <stdlib.h>

#include "algo/trap.h"
#include "engine_internal.h"

#define CMD_ID "image.trap"

static pc_status trap_enabled(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document");
    }
    if (pc_doc_arc_get(d->doc)->mode != PC_COLOR_MODE_CMYK) {
        return pc_fail(reason, PC_ERR_INVALID, "Trap needs a CMYK document (Image \xE2\x80\xBA Mode "
                                               "\xE2\x80\xBA CMYK Color)");
    }
    return PC_OK;
}

/* round().max(0.0) as usize, with Rust's saturating/NaN->0 float->int cast. */
static size_t trap_width(yyjson_val *p)
{
    double w = 1.0;
    yyjson_val *v = pc_json_get(p, "width");
    if (v != NULL) {
        double got;
        if (pc_json_as_f64(v, &got)) {
            w = got;
        }
    }
    w = round(w);
    if (!(w >= 0.0)) { /* also catches NaN */
        return 0;
    }
    if (w >= (double)SIZE_MAX) {
        return SIZE_MAX;
    }
    return (size_t)w;
}

static pc_status edit_trap(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                           pc_error *err)
{
    size_t width = *(const size_t *)ctx;
    if (!*has_active) {
        return pc_engine_err_no_document(err);
    }
    pc_layer_id id = *active;
    pc_rect bounds = pc_document_bounds(doc);
    pc_layer *l = pc_document_layer_mut(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind != PC_LAYER_RASTER) {
        return pc_engine_err_bad_params(err, CMD_ID, "Trap needs a pixel layer (active layer is %s %s)",
                                        pc_layer_content_article(&l->content),
                                        pc_layer_content_kind_name(&l->content));
    }
    if (pc_rect_is_empty(bounds)) {
        return PC_OK;
    }
    pc_surface *surf = &l->content.u.raster;
    size_t ch = pc_surface_channels(surf);
    size_t w = pc_rect_width(bounds);
    size_t h = pc_rect_height(bounds);
    float *data = NULL;
    size_t len = 0;
    pc_status rc = pc_surface_read_region(surf, bounds, &data, &len, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_algo_trap(data, len, w, h, ch, width, err);
    if (rc != PC_OK) {
        free(data);
        return rc;
    }
    rc = pc_surface_write_region(surf, bounds, data, len, err);
    free(data);
    return rc;
}

static pc_status run_trap(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t width = trap_width(p);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (pc_doc_arc_get(d->doc)->mode != PC_COLOR_MODE_CMYK) {
        return pc_engine_err_bad_params(err, CMD_ID, "Trap needs a CMYK document");
    }
    pc_status rc = pc_engine_session_edit(s, "Trap", edit_trap, &width, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, root, "trapped", true);
    yyjson_mut_obj_add_uint(rd, root, "width", width);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const char *const MENU_IMAGE[] = {"Image"};

static const pc_engine_command SPECS[] = {
    {CMD_ID, "Trap\xE2\x80\xA6", MENU_IMAGE, 1, NULL,
     "{width:px>=1=1} \xE2\x86\x92 {trapped,width}: spread inks at colour edges (CMYK only)",
     trap_enabled, run_trap, true},
};

const pc_engine_command *pc_engine_trap_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
