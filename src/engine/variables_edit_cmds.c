/* The native Variables dialog commits definitions, sets and optional application atomically. */
#include "engine/engine_internal.h"
#include "engine/preview.h"
#include <string.h>

typedef struct variables_edit {
    const pc_document *document;
} variables_edit;
static pc_status adopt(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx, pc_error *err)
{
    (void)has_active;
    (void)active;
    const variables_edit *edit = ctx;
    pc_document copy;
    pc_status status = pc_document_clone(edit->document, &copy, err);
    if (status != PC_OK)
        return status;
    pc_document_fini(doc);
    *doc = copy;
    return PC_OK;
}
static pc_status dispatch(pc_engine_session *s, const char *command, yyjson_mut_val *params, pc_error *err)
{
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *parsed = NULL;
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status status = pc_json_write(params, 0, &text, err);
    if (status == PC_OK)
        status = pc_json_parse(pc_buf_cstr(&text), text.len, &parsed, err);
    if (status == PC_OK)
        status = pc_engine_session_execute(s, command, yyjson_doc_get_root(parsed), &out, err);
    pc_buf_fini(&text);
    yyjson_doc_free(parsed);
    pc_json_value_fini(&out);
    return status;
}
static pc_status run(pc_engine_session *s, yyjson_val *params, pc_json_value *out, pc_error *err)
{
    yyjson_val *defs = pc_json_get(params, "defs"), *sets = pc_json_get(params, "dataSets"),
               *apply = pc_json_get(params, "apply");
    if (!yyjson_is_arr(defs) || !yyjson_is_arr(sets) ||
        (apply && !yyjson_is_null(apply) && !yyjson_is_str(apply)))
        return pc_engine_err_bad_params(err, "image.variables.edit",
                                        "need defs/dataSets arrays and optional apply name");
    if (yyjson_arr_size(defs) > 4096 || yyjson_arr_size(sets) > 4096)
        return pc_fail(err, PC_ERR_LIMIT, "too many variable definitions or data sets");
    pc_engine_session *prepared = NULL;
    pc_status status = pc_engine_preview_clone(s, &prepared, err);
    if (status != PC_OK)
        return status;
    /* This adapter dispatches synchronously on the original owner thread. It must preserve
     * authorization of constituent commands, including every applied pixel-file path. */
    prepared->authorize = s->authorize;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *define = d ? yyjson_mut_obj(d) : NULL, *data = d ? yyjson_mut_obj(d) : NULL;
    if (!define || !data || !yyjson_mut_obj_add_val(d, define, "defs", yyjson_val_mut_copy(d, defs)) ||
        !yyjson_mut_obj_add_val(d, data, "dataSets", yyjson_val_mut_copy(d, sets)))
        status = pc_fail(err, PC_ERR_NOMEM, "variables draft");
    if (status == PC_OK)
        status = dispatch(prepared, "image.variables.define", define, err);
    if (status == PC_OK)
        status = dispatch(prepared, "image.variables.dataSets", data, err);
    if (status == PC_OK && yyjson_is_str(apply)) {
        yyjson_mut_val *p = yyjson_mut_obj(d);
        if (!p || !yyjson_mut_obj_add_val(d, p, "name", yyjson_val_mut_copy(d, apply)))
            status = pc_fail(err, PC_ERR_NOMEM, "apply data set");
        if (status == PC_OK)
            status = dispatch(prepared, "image.applyDataSet", p, err);
    }
    pc_json_value reply = PC_JSON_VALUE_INIT;
    if (status == PC_OK)
        status = pc_engine_session_execute(prepared, "variables.list", NULL, &reply, err);
    if (status == PC_OK) {
        variables_edit edit = {pc_doc_arc_get(pc_engine_session_active(prepared)->doc)};
        status = pc_engine_session_edit(s, "Variables", adopt, &edit, err);
    }
    if (status == PC_OK) {
        pc_json_value_fini(out);
        *out = reply;
        reply = (pc_json_value)PC_JSON_VALUE_INIT;
    }
    pc_json_value_fini(&reply);
    yyjson_mut_doc_free(d);
    pc_engine_session_free(prepared);
    return status;
}
const pc_engine_command *pc_engine_variables_edit_specs(size_t *n)
{
    static const pc_engine_command specs[] = {
        {"image.variables.edit", "Edit Variables", NULL, 0, NULL,
         "{\"defs\":[...],\"dataSets\":[...],\"apply\":name?} (one atomic history step)", pc_engine_has_doc,
         run, true}};
    *n = sizeof specs / sizeof specs[0];
    return specs;
}
