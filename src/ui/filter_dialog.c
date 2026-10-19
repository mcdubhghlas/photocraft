#include "ui/filter_dialog.h"
#include "engine/channel_display.h"
#include <string.h>
static bool prefix(const char *text, const char *p)
{
    return !strncmp(text, p, strlen(p));
}
bool pc_ui_filter_dialog_command(const char *command)
{
    if (!command)
        return false;
    if (!prefix(command, "filter.") && !prefix(command, "image.adjustments.") &&
        !prefix(command, "select.modify.") && strcmp(command, "select.colorRange"))
        return false;
    const char *special[] = {"filter.liquify", "filter.vanishingPoint", "filter.adaptiveWideAngle",
                             "filter.cameraRawFilter"};
    for (size_t i = 0; i < sizeof special / sizeof *special; i++)
        if (!strcmp(command, special[i]))
            return false;
    const pc_engine_command *spec = pc_engine_command_find(command);
    if (!spec)
        return false;
    pc_ui_parameter_schema schema = {0};
    bool result = pc_ui_parameter_schema_parse(&schema, spec->params, NULL) == PC_OK && schema.count > 0;
    pc_ui_parameter_schema_fini(&schema);
    return result;
}
pc_status pc_ui_filter_dialog_fields_schema(const pc_engine_session *session, const char *command,
                                            const char *schema_text, yyjson_mut_doc **out, pc_error *err)
{
    const pc_engine_command *spec = pc_engine_command_find(command);
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!spec || !state)
        return pc_fail(err, PC_ERR_INVALID, "the editor needs an open document");
    pc_status status = spec->enabled ? spec->enabled(session, err) : PC_OK;
    if (status != PC_OK)
        return status;
    pc_ui_parameter_schema schema = {0};
    status = pc_ui_parameter_schema_parse(&schema, schema_text ? schema_text : spec->params, err);
    if (status != PC_OK)
        return status;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = d ? yyjson_mut_obj(d) : NULL;
    bool quick = false;
    size_t alpha = 0;
    bool channel = pc_engine_targeted_channel(session, &quick, &alpha);
    bool ok =
        o && yyjson_mut_obj_add_strcpy(d, o, "__command", command) &&
        yyjson_mut_obj_add_strcpy(d, o, "__label", spec->label) &&
        yyjson_mut_obj_add_strcpy(d, o, "__spec",
                                  schema_text ? schema_text : (spec->params ? spec->params : "{}")) &&
        yyjson_mut_obj_add_bool(d, o, "__filter", true) && yyjson_mut_obj_add_bool(d, o, "preview", true) &&
        yyjson_mut_obj_add_uint(d, o, "__document", pc_doc_arc_get(state->doc)->id) &&
        yyjson_mut_obj_add_uint(d, o, "__revision", state->revision) &&
        yyjson_mut_obj_add_uint(d, o, "__layer", state->has_active_layer ? state->active_layer : 0) &&
        yyjson_mut_obj_add_uint(d, o, "__color", pc_engine_targeted_color(session)) &&
        yyjson_mut_obj_add_bool(d, o, "__mask", pc_engine_targeted_mask(session)) &&
        yyjson_mut_obj_add_bool(d, o, "__channel", channel) &&
        yyjson_mut_obj_add_bool(d, o, "__quick", quick) && yyjson_mut_obj_add_uint(d, o, "__alpha", alpha);
    yyjson_mut_val *layers = d ? yyjson_mut_arr(d) : NULL;
    for (size_t i = 0; ok && layers && i < state->nselected; i++)
        ok = yyjson_mut_arr_add_uint(d, layers, state->selected_layers[i]);
    ok = ok && layers && yyjson_mut_obj_add_val(d, o, "__layers", layers);
    if (!ok)
        status = pc_fail(err, PC_ERR_NOMEM, "filter editor");
    if (status == PC_OK &&
        !yyjson_mut_obj_add_uint(d, o, "__rows",
                                 schema.count + ((!strcmp(command, "image.adjustments.curves") ||
                                                  !strcmp(command, "image.adjustments.levels"))
                                                     ? 10
                                                     : 0)))
        status = pc_fail(err, PC_ERR_NOMEM, "editor dimensions");
    if (status == PC_OK)
        status = pc_ui_parameter_schema_defaults(&schema, d, o, err);
    pc_ui_parameter_schema_fini(&schema);
    if (status != PC_OK) {
        yyjson_mut_doc_free(d);
        return status;
    }
    yyjson_mut_doc_set_root(d, o);
    *out = d;
    return PC_OK;
}
pc_status pc_ui_filter_dialog_fields(const pc_engine_session *session, const char *command,
                                     yyjson_mut_doc **out, pc_error *err)
{
    return pc_ui_filter_dialog_fields_schema(session, command, NULL, out, err);
}
pc_status pc_ui_filter_dialog_guard(const pc_engine_session *session, const yyjson_mut_val *f, pc_error *err)
{
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    bool quick = false;
    size_t alpha = 0;
    bool channel = pc_engine_targeted_channel(session, &quick, &alpha);
    if (!state ||
        pc_doc_arc_get(state->doc)->id != yyjson_mut_get_uint(yyjson_mut_obj_get(f, "__document")) ||
        state->revision != yyjson_mut_get_uint(yyjson_mut_obj_get(f, "__revision")) ||
        (state->has_active_layer ? state->active_layer : 0) !=
            yyjson_mut_get_uint(yyjson_mut_obj_get(f, "__layer")) ||
        pc_engine_targeted_color(session) != yyjson_mut_get_uint(yyjson_mut_obj_get(f, "__color")) ||
        pc_engine_targeted_mask(session) != yyjson_mut_get_bool(yyjson_mut_obj_get(f, "__mask")) ||
        channel != yyjson_mut_get_bool(yyjson_mut_obj_get(f, "__channel")) ||
        quick != yyjson_mut_get_bool(yyjson_mut_obj_get(f, "__quick")) ||
        alpha != yyjson_mut_get_uint(yyjson_mut_obj_get(f, "__alpha")))
        return pc_fail(err, PC_ERR_INVALID, "the document or edit target changed while the editor was open");
    if (yyjson_mut_get_bool(yyjson_mut_obj_get(f, "__style")) &&
        yyjson_mut_get_uint(yyjson_mut_obj_get(f, "layer")) !=
            yyjson_mut_get_uint(yyjson_mut_obj_get(f, "__layer")))
        return pc_fail(err, PC_ERR_INVALID, "the style editor is bound to its original layer");
    yyjson_mut_val *layers = yyjson_mut_obj_get(f, "__layers");
    if (yyjson_mut_arr_size(layers) != state->nselected)
        return pc_fail(err, PC_ERR_INVALID, "the layer selection changed while the editor was open");
    for (size_t i = 0; i < state->nselected; i++)
        if (yyjson_mut_get_uint(yyjson_mut_arr_get(layers, i)) != state->selected_layers[i])
            return pc_fail(err, PC_ERR_INVALID, "the layer selection changed while the editor was open");
    return PC_OK;
}
pc_status pc_ui_filter_dialog_params(const yyjson_mut_val *fields, pc_buf *out, pc_error *err)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = doc ? yyjson_mut_val_mut_copy(doc, fields) : NULL;
    if (!yyjson_mut_is_obj(p)) {
        yyjson_mut_doc_free(doc);
        return pc_fail(err, PC_ERR_NOMEM, "filter parameters");
    }
    yyjson_mut_obj_iter iter = yyjson_mut_obj_iter_with(p);
    yyjson_mut_val *key;
    while ((key = yyjson_mut_obj_iter_next(&iter))) {
        const char *name = yyjson_mut_get_str(key);
        if (name && (!strncmp(name, "__", 2) || !strcmp(name, "preview")))
            (void)yyjson_mut_obj_iter_remove(&iter);
    }
    if (yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "__style"))) {
        (void)yyjson_mut_obj_remove_str(p, "selected");
        (void)yyjson_mut_obj_remove_str(p, "blendingValues");
        (void)yyjson_mut_obj_remove_str(p, "globalLightValues");
        yyjson_mut_val *effects = yyjson_mut_obj_get(p, "effects");
        size_t count = yyjson_mut_arr_size(effects);
        for (size_t i = 0; i < count; i++) {
            yyjson_mut_val *entry = yyjson_mut_arr_get(effects, i);
            if (yyjson_mut_is_obj(entry)) {
                (void)yyjson_mut_obj_remove_str(entry, "id");
                (void)yyjson_mut_obj_remove_str(entry, "values");
            }
        }
    }
    pc_status status = pc_json_write(p, 0, out, err);
    yyjson_mut_doc_free(doc);
    return status;
}
