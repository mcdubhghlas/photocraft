/* Type Mask glyph coverage, including the temporary type layer used by inline editing. */
#include "engine_internal.h"
#include "type_cmds.h"
#include "layer_cmds.h"
#include "algo/selection.h"
#include "text/engine.h"
#include "base/mem.h"
#include <stdlib.h>
#include <string.h>
#define CMD "type.createSelection"
#define MASK_PIXELS ((size_t)64 * 1024 * 1024)
typedef struct type_mask_context {
    float *mask;
    size_t count;
    pc_rect area;
    pc_algo_selection_mode mode;
    bool remove;
    pc_layer_id layer;
} type_mask_context;
static pc_status apply_mask(pc_document *doc, bool *has_active, pc_layer_id *active, void *userdata,
                            pc_error *err)
{
    type_mask_context *c = userdata;
    pc_layer_id *neighbors = NULL;
    size_t count = 0;
    if (c->remove) {
        pc_status status = pc_engine_deletion_neighbours(doc, c->layer, &neighbors, &count, err);
        if (status != PC_OK) {
            return status;
        }
    }
    pc_surface mask = {0};
    bool some = false;
    pc_status status = pc_algo_selection_combine(doc->has_selection ? &doc->selection : NULL, c->mask,
                                                 c->count, c->area, c->mode, &mask, &some, err);
    if (status == PC_OK) {
        if (doc->has_selection) {
            pc_surface_fini(&doc->selection);
        }
        doc->has_selection = some;
        if (some) {
            doc->selection = mask;
            memset(&mask, 0, sizeof mask);
        }
        if (c->remove) {
            pc_layer removed = {0};
            status = pc_document_remove(doc, c->layer, &removed, err);
            pc_layer_fini(&removed);
            if (status == PC_OK && !doc->nlayers) {
                status = pc_fail(err, PC_ERR_INVALID, "a document must keep at least one layer");
            }
            if (status == PC_OK && *has_active && !pc_document_layer(doc, *active)) {
                *has_active = false;
                for (size_t i = 0; i < count; i++) {
                    if (pc_document_layer(doc, neighbors[i])) {
                        *active = neighbors[i];
                        *has_active = true;
                        break;
                    }
                }
            }
        }
    }
    pc_surface_fini(&mask);
    free(neighbors);
    return status;
}
static pc_status run(pc_engine_session *session, yyjson_val *params, pc_json_value *out, pc_error *err)
{
    pc_algo_selection_mode mode = PC_ALGO_SELECTION_REPLACE;
    yyjson_val *v = pc_json_get(params, "operation");
    if (v) {
        const char *operation = pc_json_as_str(v, NULL);
        if (operation && (!strcmp(operation, "new") || !strcmp(operation, "replace"))) {
            mode = PC_ALGO_SELECTION_REPLACE;
        } else if (operation && !strcmp(operation, "add")) {
            mode = PC_ALGO_SELECTION_ADD;
        } else if (operation && !strcmp(operation, "subtract")) {
            mode = PC_ALGO_SELECTION_SUBTRACT;
        } else if (operation && !strcmp(operation, "intersect")) {
            mode = PC_ALGO_SELECTION_INTERSECT;
        } else {
            return pc_engine_err_bad_params(err, CMD, "operation must be new, add, subtract or intersect");
        }
    }
    bool remove = false;
    v = pc_json_get(params, "removeLayer");
    if (v && !pc_json_as_bool(v, &remove)) {
        return pc_engine_err_bad_params(err, CMD, "removeLayer must be true or false");
    }
    pc_antialias aa = PC_AA_SMOOTH;
    bool set_aa = false;
    v = pc_json_get(params, "antialias");
    if (v) {
        const char *text = pc_json_as_str(v, NULL);
        set_aa = true;
        const char *const names[] = {"none", "sharp", "crisp", "strong", "smooth"};
        const pc_antialias modes[] = {PC_AA_NONE, PC_AA_SHARP, PC_AA_CRISP, PC_AA_STRONG, PC_AA_SMOOTH};
        bool found = false;
        for (size_t i = 0; text && i < 5; i++) {
            if (!strcmp(text, names[i])) {
                aa = modes[i];
                found = true;
                break;
            }
        }
        if (!found) {
            return pc_engine_err_bad_params(err, CMD,
                                            "antialias must be none, sharp, crisp, strong or smooth");
        }
    }
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(state->doc);
    bool has_layer = false;
    pc_layer_id layer = 0;
    v = pc_json_get(params, "layer");
    if (v && !yyjson_is_null(v)) {
        if (!pc_json_as_u64(v, &layer)) {
            return pc_engine_err_bad_params(err, CMD, "layer must be a layer id");
        }
        has_layer = true;
    }
    if (remove && !has_layer) {
        return pc_engine_err_bad_params(err, CMD, "removeLayer needs a layer");
    }
    pc_text_layer text = {0};
    pc_status status;
    if (has_layer) {
        const pc_layer *source = pc_document_layer(doc, layer);
        if (!source) {
            return pc_engine_err_no_layer(err, layer);
        }
        if (source->content.kind != PC_LAYER_TEXT) {
            return pc_engine_err_bad_params(err, CMD, "layer is not a type layer");
        }
        status = pc_text_layer_clone(&source->content.u.text, &text, err);
    } else {
        status = pc_engine_type_new_layer(session, params, CMD, &text, err);
    }
    if (status != PC_OK) {
        return status;
    }
    if (set_aa) {
        text.antialias = aa;
    }
    for (size_t i = 0; i < text.nruns; i++) {
        text.runs[i].style.color.alpha = 1;
    }
    pc_rendered rendered = {0};
    pc_text_engine *engine = NULL;
    status = pc_text_engine_shared_lock(&engine, err);
    if (status == PC_OK) {
        status = pc_text_engine_render(engine, &text, doc->resolution_dpi, pc_document_pixel_format(doc),
                                       NULL, &rendered, err);
        pc_text_engine_shared_unlock();
    }
    pc_text_layer_fini(&text);
    if (status != PC_OK) {
        pc_rendered_fini(&rendered);
        return status;
    }
    pc_rect text_area =
        pc_rect_intersect(pc_surface_content_bounds(&rendered.surface), pc_document_bounds(doc));
    pc_rect old = doc->has_selection
                      ? pc_rect_intersect(pc_surface_content_bounds(&doc->selection), pc_document_bounds(doc))
                      : PC_RECT_EMPTY;
    pc_rect area = text_area;
    if ((mode == PC_ALGO_SELECTION_ADD || mode == PC_ALGO_SELECTION_SUBTRACT) && !pc_rect_is_empty(old)) {
        area = pc_rect_is_empty(text_area) ? old : pc_rect_union(old, text_area);
    }
    size_t width = pc_rect_width(area), height = pc_rect_height(area);
    if (height && width > MASK_PIXELS / height) {
        pc_rendered_fini(&rendered);
        return pc_fail(err, PC_ERR_LIMIT, "Type Mask exceeds the coverage budget");
    }
    size_t count = width * height;
    float *mask = pc_calloc_array(count, sizeof *mask);
    if (!mask) {
        pc_rendered_fini(&rendered);
        return pc_fail(err, PC_ERR_NOMEM, "Type Mask coverage");
    }
    float pixels[1024][4];
    for (int32_t y = text_area.y0; status == PC_OK && y < text_area.y1; y++) {
        for (int32_t x = text_area.x0; status == PC_OK && x < text_area.x1;) {
            int32_t right = text_area.x1 - x > 1024 ? x + 1024 : text_area.x1;
            pc_rect row = {x, y, right, y + 1};
            size_t n = (size_t)(right - x);
            status = pc_surface_read_rgba_into(&rendered.surface, row, pixels, n, err);
            size_t offset = (size_t)(y - area.y0) * width + (size_t)(x - area.x0);
            if (status == PC_OK) {
                for (size_t i = 0; i < n; i++) {
                    mask[offset + i] = pixels[i][3];
                }
            }
            x = right;
        }
    }
    pc_rendered_fini(&rendered);
    if (status == PC_OK) {
        type_mask_context context = {mask, count, area, mode, remove, layer};
        status = pc_engine_session_edit(session, "Type Mask", apply_mask, &context, err);
    }
    free(mask);
    if (status != PC_OK) {
        return status;
    }
    pc_engine_doc_state *after = pc_engine_session_active_mut(session);
    (void)pc_history_set_current_label(after->history, "Type Mask", strlen("Type Mask"), NULL);
    yyjson_mut_doc *result = NULL;
    status = pc_engine_result_doc(out, &result, err);
    if (status != PC_OK) {
        return status;
    }
    yyjson_mut_val *root = yyjson_mut_obj(result), *bounds = yyjson_mut_null(result);
    const pc_document *changed = pc_doc_arc_get(after->doc);
    yyjson_mut_obj_add_bool(result, root, "selected", changed->has_selection);
    if (changed->has_selection) {
        pc_rect r = pc_surface_content_bounds(&changed->selection);
        bounds = yyjson_mut_arr(result);
        yyjson_mut_arr_add_int(result, bounds, r.x0);
        yyjson_mut_arr_add_int(result, bounds, r.y0);
        yyjson_mut_arr_add_int(result, bounds, r.x1);
        yyjson_mut_arr_add_int(result, bounds, r.y1);
    }
    yyjson_mut_obj_add_val(result, root, "bounds", bounds);
    yyjson_mut_doc_set_root(result, root);
    return PC_OK;
}
const pc_engine_command *pc_engine_type_mask_specs(size_t *count)
{
    static const pc_engine_command specs[] = {
        {CMD, "Type Mask", NULL, 0, NULL,
         "{\"layer\":id?,\"removeLayer\":bool=false,\"operation\":\"new|add|subtract|intersect\"=\"new\","
         "\"antialias\":\"none|sharp|crisp|strong|smooth\"?,...type.create keys}",
         pc_engine_has_doc, run, true}};
    *count = sizeof specs / sizeof specs[0];
    return specs;
}
