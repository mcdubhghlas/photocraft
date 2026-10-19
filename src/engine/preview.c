#include "engine/preview.h"
#include "engine/engine_internal.h"
#include "engine/prefs.h"
#include "engine/pattern_cmds.h"
#include "base/mem.h"
#include <stdlib.h>
#include <string.h>
static pc_status restricted_preview(const char *command, yyjson_val *params, pc_error *err)
{
    (void)command;
    (void)params;
    return pc_fail(err, PC_ERR_UNSUPPORTED, "previews are unavailable for restricted sessions");
}

pc_status pc_engine_preview_clone(const pc_engine_session *source, pc_engine_session **out, pc_error *err)
{
    if (!source || !out)
        return pc_fail(err, PC_ERR_INVALID, "missing preview session");
    pc_engine_session *next = NULL;
    pc_status status = pc_engine_session_new(&next, err);
    if (status != PC_OK)
        return status;
    char *prefs = pc_engine_session_prefs_to_json(source);
    if (!prefs)
        status = pc_fail(err, PC_ERR_NOMEM, "preview preferences");
    else
        status = pc_engine_session_load_prefs_json(next, prefs, strlen(prefs), err);
    free(prefs);
    if (status == PC_OK) {
        pc_color_settings settings = {0};
        status = pc_color_settings_clone(&source->color.settings, &settings, err);
        if (status == PC_OK) {
            pc_color_settings_fini(&next->color.settings);
            next->color.settings = settings;
        }
    }
    if (status == PC_OK && source->color.nproofs) {
        next->color.proofs = pc_calloc_array(source->color.nproofs, sizeof *next->color.proofs);
        if (!next->color.proofs)
            status = pc_fail(err, PC_ERR_NOMEM, "preview proof views");
        else
            next->color.cap_proofs = source->color.nproofs;
        for (size_t i = 0; status == PC_OK && i < source->color.nproofs; i++) {
            const pc_color_proof_entry *original = &source->color.proofs[i];
            pc_color_proof_entry *copy = &next->color.proofs[i];
            *copy = *original;
            copy->view.setup.profile = (pc_profile){0};
            copy->view.setup.name = NULL;
            next->color.nproofs++;
            const char *name = original->view.setup.name;
            if (name) {
                copy->view.setup.name = pc_strndup(name, strlen(name));
                if (!copy->view.setup.name)
                    status = pc_fail(err, PC_ERR_NOMEM, "preview proof name");
            }
            if (status == PC_OK)
                status = pc_profile_clone(&original->view.setup.profile, &copy->view.setup.profile, err);
        }
    }
    if (status == PC_OK && source->color.nhdr) {
        next->color.hdr = pc_malloc_array(source->color.nhdr, sizeof *next->color.hdr);
        if (!next->color.hdr)
            status = pc_fail(err, PC_ERR_NOMEM, "preview HDR views");
        else {
            memcpy(next->color.hdr, source->color.hdr, source->color.nhdr * sizeof *next->color.hdr);
            next->color.nhdr = next->color.cap_hdr = source->color.nhdr;
        }
    }
    if (status == PC_OK) {
        memcpy(next->tools.foreground, source->tools.foreground, sizeof next->tools.foreground);
        memcpy(next->tools.background, source->tools.background, sizeof next->tools.background);
        memcpy(next->tools.other_colors, source->tools.other_colors, sizeof next->tools.other_colors);
        memcpy(next->tools.cmyk_picks, source->tools.cmyk_picks, sizeof next->tools.cmyk_picks);
        next->tools.ncmyk_picks = source->tools.ncmyk_picks;
        next->tools.mask_targeted = source->tools.mask_targeted;
        pc_paint_brush_fini(&next->tools.brush);
        pc_paint_brush_copy(&next->tools.brush, &source->tools.brush);
        next->quick_mask_color = source->quick_mask_color;
        next->quick_mask_opacity = source->quick_mask_opacity;
        next->quick_mask_indicates = source->quick_mask_indicates;
    }
    if (status == PC_OK) {
        pc_engine_pattern_library_fini(next);
        if (source->npatterns) {
            next->patterns = pc_calloc_array(source->npatterns, sizeof *next->patterns);
            if (!next->patterns)
                status = pc_fail(err, PC_ERR_NOMEM, "preview pattern library");
        }
        for (size_t i = 0; status == PC_OK && i < source->npatterns; i++) {
            status = pc_pattern_clone(&source->patterns[i], &next->patterns[i], err);
            if (status == PC_OK)
                next->npatterns++;
        }
    }
    size_t count = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(source, &count);
    for (size_t i = 0; status == PC_OK && i < count; i++) {
        pc_document doc = {0};
        status = pc_document_clone(pc_doc_arc_get(docs[i].doc), &doc, err);
        if (status == PC_OK)
            status = pc_engine_session_add_document(next, &doc, NULL, NULL, err);
        pc_document_fini(&doc);
        if (status != PC_OK)
            break;
        pc_engine_doc_state *target = pc_engine_session_active_mut(next);
        target->has_active_layer = docs[i].has_active_layer;
        target->active_layer = docs[i].active_layer;
        target->has_layer_mask_view = docs[i].has_layer_mask_view;
        target->layer_mask_view_layer = docs[i].layer_mask_view_layer;
        target->layer_mask_view_mode = docs[i].layer_mask_view_mode;
        pc_layer_id *selected = NULL;
        if (docs[i].nselected) {
            selected = pc_malloc_array(docs[i].nselected, sizeof *selected);
            if (!selected) {
                status = pc_fail(err, PC_ERR_NOMEM, "preview layer selection");
                break;
            }
            memcpy(selected, docs[i].selected_layers, docs[i].nselected * sizeof *selected);
        }
        free(target->selected_layers);
        target->selected_layers = selected;
        target->nselected = docs[i].nselected;
        status = pc_engine_channel_view_clone(docs[i].channel_view, &target->channel_view, err);
    }
    size_t active = 0;
    if (status == PC_OK && pc_engine_session_active_index(source, &active))
        (void)pc_engine_session_set_active(next, active);
    if (status != PC_OK) {
        pc_engine_session_free(next);
        return status;
    }
    /* An arbitrary authorizer may depend on its owner thread. Never drop its policy or
     * invoke it from a preview worker. Synchronous adapters may explicitly retain it. */
    if (source->authorize)
        next->authorize = restricted_preview;
    *out = next;
    return PC_OK;
}

typedef struct preview_job {
    pc_engine_session *session;
    char *command;
    yyjson_doc *params;
    const char *label;
} preview_job;
static void preview_job_free(void *context)
{
    preview_job *job = context;
    if (!job)
        return;
    pc_engine_session_free(job->session);
    free(job->command);
    yyjson_doc_free(job->params);
    free(job);
}
typedef struct preview_job_result {
    pc_doc_arc *doc;
    const char *label;
    pc_json_value reply;
} preview_job_result;
static void preview_result_free(void *payload)
{
    preview_job_result *result = payload;
    if (!result)
        return;
    pc_doc_arc_release(result->doc);
    pc_json_value_fini(&result->reply);
    free(result);
}
static pc_status preview_job_work(const pc_engine_job_ctx *cancel, void *context, void **out, pc_error *err)
{
    preview_job *job = context;
    pc_engine_session_set_inline_job_ctx(job->session, cancel);
    pc_engine_job_ctx_progress(cancel, .02f, job->label);
    pc_status status = pc_engine_job_ctx_check(cancel, err);
    const pc_engine_doc_state *state = pc_engine_session_active(job->session);
    pc_doc_id original = state ? pc_doc_arc_get(state->doc)->id : 0;
    pc_json_value result = PC_JSON_VALUE_INIT;
    if (status == PC_OK)
        status = pc_engine_session_execute(
            job->session, job->command, job->params ? yyjson_doc_get_root(job->params) : NULL, &result, err);
    if (status == PC_OK)
        status = pc_engine_job_ctx_check(cancel, err);
    state = pc_engine_session_active(job->session);
    if (status == PC_OK && (!state || pc_doc_arc_get(state->doc)->id != original))
        status = pc_fail(err, PC_ERR_INVALID, "the background command changed its document target");
    if (status == PC_OK) {
        preview_job_result *ready = calloc(1, sizeof *ready);
        if (!ready)
            status = pc_fail(err, PC_ERR_NOMEM, "background result");
        else {
            ready->doc = pc_doc_arc_retain(state->doc);
            ready->label = job->label;
            ready->reply = result;
            result = (pc_json_value)PC_JSON_VALUE_INIT;
            *out = ready;
            pc_engine_job_ctx_progress(cancel, 1, "Complete");
        }
    }
    pc_json_value_fini(&result);
    return status;
}
static pc_status preview_replace(pc_document *doc, bool *has_layer, pc_layer_id *layer, void *payload,
                                 pc_error *err)
{
    (void)has_layer;
    (void)layer;
    const preview_job_result *ready = payload;
    const pc_document *result = ready ? pc_doc_arc_get(ready->doc) : NULL;
    if (!result || result->id != doc->id)
        return pc_fail(err, PC_ERR_INVALID, "background document target mismatch");
    pc_document copy = {0};
    pc_status status = pc_document_clone(result, &copy, err);
    if (status == PC_OK) {
        pc_document_fini(doc);
        *doc = copy;
    }
    return status;
}
static pc_status preview_job_apply(pc_engine_session *s, void *payload, pc_json_value *out, pc_error *err)
{
    preview_job_result *ready = payload;
    if (!ready)
        return pc_fail(err, PC_ERR_INVALID, "missing background result");
    pc_status status = pc_json_value_clone(&ready->reply, out, err);
    if (status == PC_OK)
        status = pc_engine_session_edit(s, ready->label, preview_replace, payload, err);
    if (status != PC_OK)
        pc_json_value_fini(out);
    return status;
}
bool pc_engine_preview_job_command(const char *command)
{
    if (!command)
        return false;
    return (!strncmp(command, "filter.", 7) && strcmp(command, "filter.lastFilter") &&
            strcmp(command, "filter.convertForSmartFilters")) ||
           !strncmp(command, "image.adjustments.", 18) || !strncmp(command, "select.modify.", 14) ||
           !strcmp(command, "select.colorRange") || !strcmp(command, "layer.layerStyle.edit") ||
           !strcmp(command, "edit.transform") || !strcmp(command, "select.transformSelection") ||
           !strcmp(command, "path.transform");
}
pc_status pc_engine_preview_start_job(pc_engine_session *s, const char *command, yyjson_val *params,
                                      bool *is_job, uint64_t *id, pc_json_value *out, pc_error *err)
{
    if (!s || !is_job || !id || !out || !pc_engine_preview_job_command(command))
        return pc_fail(err, PC_ERR_INVALID, "unsupported background document command");
    *is_job = false;
    *id = 0;
    if (s->authorize || (!strcmp(command, "edit.transform") && yyjson_get_bool(pc_json_get(params, "copy"))))
        return pc_engine_session_execute(s, command, params, out, err);
    const pc_engine_command *spec = pc_engine_command_find(command);
    if (!spec)
        return pc_fail(err, PC_ERR_NOT_FOUND, "unknown background command");
    pc_status status = spec->enabled(s, err);
    if (status != PC_OK)
        return status;
    if (pc_engine_job_conflict(s, command, true, err))
        return PC_ERR_INVALID;
    preview_job *job = calloc(1, sizeof *job);
    if (!job)
        return pc_fail(err, PC_ERR_NOMEM, "background document command");
    job->command = pc_strndup(command, strlen(command));
    job->label = spec->label;
    pc_buf text = PC_BUF_INIT;
    if (!job->command)
        status = pc_fail(err, PC_ERR_NOMEM, "background command parameters");
    if (status == PC_OK && params) {
        status = pc_json_write_val(params, 0, &text, err);
        if (status == PC_OK)
            status = pc_json_parse(pc_buf_cstr(&text), text.len, &job->params, err);
    }
    pc_buf_fini(&text);
    if (status == PC_OK)
        status = pc_engine_preview_clone(s, &job->session, err);
    if (status != PC_OK) {
        preview_job_free(job);
        return status;
    }
    status = pc_engine_session_start_job(s, command, params, spec->label, true, preview_job_work, job,
                                         preview_job_free, preview_result_free, preview_job_apply, is_job, id,
                                         out, err);
    if (status == PC_OK && *is_job && !pc_engine_job_record_command(s, *id)) {
        (void)pc_engine_session_cancel_job(s, *id);
        return pc_fail(err, PC_ERR_INVALID, "background command registration failed");
    }
    return status;
}

pc_status pc_engine_preview_sync(const pc_engine_session *source, const char *command, yyjson_val *params,
                                 pc_doc_arc **out, pc_error *err)
{
    if (!source || !out || !command || !pc_engine_preview_job_command(command))
        return pc_fail(err, PC_ERR_INVALID, "unsupported document preview command");
    pc_engine_session *prepared = NULL;
    pc_status status = pc_engine_preview_clone(source, &prepared, err);
    pc_json_value reply = PC_JSON_VALUE_INIT;
    if (status == PC_OK) {
        prepared->authorize = source->authorize;
        status = pc_engine_session_execute(prepared, command, params, &reply, err);
        if (status == PC_OK)
            *out = pc_doc_arc_retain(pc_engine_session_active(prepared)->doc);
    }
    pc_json_value_fini(&reply);
    pc_engine_session_free(prepared);
    return status;
}
