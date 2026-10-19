#include <blake3.h>
#include "engine/preview.h"
#include "command_form.h"
#include "filter_ui.h"
#include "shell_internal.h"
#include "ui/filter_dialog.h"
#include "ui/i18n.h"
#include "ui/sizing.h"
#include "engine/prefs.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
bool pc_ui_shell_form_command(const char *command)
{
    return command &&
           (!strcmp(command, "file.export.exportAs") || !strcmp(command, "layer.new.artboard") ||
            !strcmp(command, "file.export.artboardsToFiles") ||
            !strcmp(command, "file.export.artboardsToPdf") || !strcmp(command, "image.imageSize") ||
            !strcmp(command, "image.canvasSize") || !strcmp(command, "image.rotation.arbitrary"));
}
pc_status pc_ui_shell_form_open(pc_ui_shell_app *app, const char *command, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    pc_engine_session *s = pc_ui_shell_app_session(app);
    const char *artboard_spec = "{\"name\":str=\"Artboard\",\"width\":1..300000=100,\"height\":1..300000=100,"
                                "\"background\":\"white|black|transparent\"=\"white\"}";
    const char *form_spec =
        !strcmp(command, "layer.new.artboard") ? artboard_spec
        : !strcmp(command, "file.export.artboardsToFiles")
            ? "{\"dir\":str,\"format\":\"png|jpg|psd|tiff\"=\"png\",\"prefix\":str=\"\",\"quality\":0..12=10}"
        : !strcmp(command, "file.export.artboardsToPdf") ? "{\"path\":str,\"quality\":0..12=10}"
                                                         : NULL;
    pc_status status = pc_ui_filter_dialog_fields_schema(s, command, form_spec, &d, err);
    if (status != PC_OK)
        return status;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d);
    (void)yyjson_mut_obj_remove_str(root, "__filter");
    (void)yyjson_mut_obj_remove_str(root, "preview");
    bool exporting = !strcmp(command, "file.export.exportAs");
    bool ok = yyjson_mut_obj_add_bool(d, root, "__form", true) &&
              yyjson_mut_obj_add_bool(d, root, "__export", exporting);
    if (!exporting && (!strcmp(command, "image.imageSize") || !strcmp(command, "image.canvasSize"))) {
        const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
        (void)yyjson_mut_obj_remove_str(root, "width");
        (void)yyjson_mut_obj_remove_str(root, "height");
        (void)yyjson_mut_obj_remove_str(root, "resolution");
        ok = ok && yyjson_mut_obj_add_uint(d, root, "width", doc->size.width) &&
             yyjson_mut_obj_add_uint(d, root, "height", doc->size.height);
        if (!strcmp(command, "image.imageSize")) {
            ok = ok && yyjson_mut_obj_add_real(d, root, "resolution", doc->resolution_dpi);
            pc_json_value interpolation = PC_JSON_VALUE_INIT;
            (void)pc_engine_prefs_get(s, "general.imageInterpolation", &interpolation, NULL);
            (void)yyjson_mut_obj_remove_str(root, "resample");
            ok = ok && yyjson_mut_obj_add_str(
                           d, root, "resample",
                           pc_ui_resample_preference(yyjson_mut_get_str(pc_json_value_root(&interpolation))));
            pc_json_value_fini(&interpolation);
        }
    }
    if (!strcmp(command, "layer.new.artboard")) {
        const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
        const char *keys[] = {"__spec", "preset", "rect", "color", "name",
                              "width",  "height", "x",    "y",     "background"};
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
            (void)yyjson_mut_obj_remove_str(root, keys[i]);
        ok = ok && yyjson_mut_obj_add_str(d, root, "__spec", artboard_spec) &&
             yyjson_mut_obj_add_str(d, root, "name", "Artboard") &&
             yyjson_mut_obj_add_uint(d, root, "width", doc->size.width) &&
             yyjson_mut_obj_add_uint(d, root, "height", doc->size.height) &&
             yyjson_mut_obj_add_str(d, root, "background", "white");
    }
    if (exporting) {
        (void)yyjson_mut_obj_remove_str(root, "estimate");
        ok = ok && yyjson_mut_obj_add_bool(d, root, "estimate", false);
    }
    if (!ok) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "command form");
    }
    return pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, d, root, out, err);
}
static void settings_hash(yyjson_mut_val *root, char hex[65])
{
    const char *keys[] = {"format", "quality", "lossless", "transparency", "scale", "metadata", "tgaBits"};
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        pc_buf json = PC_BUF_INIT;
        yyjson_mut_val *v = yyjson_mut_obj_get(root, keys[i]);
        blake3_hasher_update(&hasher, keys[i], strlen(keys[i]) + 1);
        if (v && pc_json_write(v, 0, &json, NULL) == PC_OK)
            blake3_hasher_update(&hasher, json.data, json.len);
        pc_buf_fini(&json);
    }
    uint8_t bytes[32];
    blake3_hasher_finalize(&hasher, bytes, sizeof bytes);
    const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof bytes; i++) {
        hex[2 * i] = digits[bytes[i] >> 4];
        hex[2 * i + 1] = digits[bytes[i] & 15];
    }
    hex[64] = 0;
}
void pc_ui_shell_form_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    if (r->schema_dialog != dialog->id) {
        pc_ui_parameter_schema_fini(&r->schema);
        const char *spec = yyjson_mut_get_str(yyjson_mut_obj_get(root, "__spec"));
        if (pc_ui_parameter_schema_parse(&r->schema, spec, NULL) == PC_OK)
            r->schema_dialog = dialog->id;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    bool exporting = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "__export"));
    if (exporting) {
        nk_layout_row_dynamic(ctx, 180, 1);
        struct nk_rect bounds;
        (void)nk_widget(&bounds, ctx);
        struct nk_image source;
        float aspect;
        if (pc_ui_shell_canvas_preview(app, &source, &aspect) && aspect > 0) {
            float width = fminf(bounds.w, bounds.h * aspect), height = width / aspect;
            struct nk_rect rect =
                nk_rect(bounds.x + (bounds.w - width) / 2, bounds.y + (bounds.h - height) / 2, width, height);
            nk_fill_rect(nk_window_get_canvas(ctx), rect, 0, nk_rgb(255, 255, 255));
            nk_draw_image(nk_window_get_canvas(ctx), rect, &source, nk_rgba(255, 255, 255, 255));
        }
        const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
        const pc_document *doc = state ? pc_doc_arc_get(state->doc) : NULL;
        yyjson_mut_val *scale = yyjson_mut_obj_get(root, "scale");
        double percent = scale ? yyjson_mut_get_num(scale) : 100;
        char label[128];
        if (doc && isfinite(percent) && percent >= 1 && percent <= 1000) {
            (void)snprintf(label, sizeof label, "%.0f × %.0f px",
                           fmax(1, round(doc->size.width * percent / 100)),
                           fmax(1, round(doc->size.height * percent / 100)));
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_label(ctx, label, NK_TEXT_CENTERED);
        }
    }
    pc_ui_shell_filter_fields(app, r, dialog, yyjson_mut_get_str(yyjson_mut_obj_get(root, "__command")));
    if (exporting) {
        char hash[65];
        settings_hash(root, hash);
        const char *previous = yyjson_mut_get_str(yyjson_mut_obj_get(root, "__estimateHash"));
        if (!previous || strcmp(previous, hash))
            (void)yyjson_mut_obj_remove_str(root, "__estimateLabel");
        const char *estimate = yyjson_mut_get_str(yyjson_mut_obj_get(root, "__estimateLabel"));
        nk_layout_row_dynamic(ctx, 26, 2);
        pc_ui_shell_widget_mapping(app, "export.estimate", nk_widget_bounds(ctx));
        if (nk_button_label(ctx, pc_i18n_t("Estimate file size"))) {
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *params = d ? yyjson_mut_val_mut_copy(d, root) : NULL;
            pc_buf json = PC_BUF_INIT;
            pc_engine_session *prepared = NULL;
            pc_json_value result = PC_JSON_VALUE_INIT;
            pc_error err = {0};
            pc_status status = params ? PC_OK : pc_fail(&err, PC_ERR_NOMEM, "export estimate");
            if (status == PC_OK) {
                (void)yyjson_mut_obj_remove_str(params, "path");
                (void)yyjson_mut_obj_remove_str(params, "estimate");
                status = pc_json_write(params, 0, &json, &err);
            }
            if (status == PC_OK)
                status = pc_engine_preview_clone(pc_ui_shell_app_session(app), &prepared, &err);
            if (status == PC_OK)
                status = pc_engine_session_execute_json(prepared, "file.export.exportAs", pc_buf_cstr(&json),
                                                        &result, &err);
            char label[128];
            if (status == PC_OK) {
                uint64_t bytes =
                    yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&result), "bytes"));
                (void)snprintf(label, sizeof label, "~%llu bytes", (unsigned long long)bytes);
                (void)yyjson_mut_obj_remove_str(root, "__estimateLabel");
                (void)yyjson_mut_obj_add_strcpy(dialog->fields, root, "__estimateLabel", label);
                (void)yyjson_mut_obj_remove_str(root, "__estimateHash");
                (void)yyjson_mut_obj_add_strcpy(dialog->fields, root, "__estimateHash", hash);
            } else
                (void)snprintf(r->error, sizeof r->error, "%s", err.msg);
            pc_engine_session_free(prepared);
            pc_json_value_fini(&result);
            pc_buf_fini(&json);
            yyjson_mut_doc_free(d);
        }
        nk_label(ctx, estimate ? estimate : "", NK_TEXT_LEFT);
    }
}
