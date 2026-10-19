#include "rasterize_ui.h"
#include "command_form.h"
#include "variables_ui.h"
/* Modal data lives in UiState. Native fields and control-channel edits share the same values. */
#include "dialogs.h"
#include "credits_ui.h"
#include "workspace_ui.h"
#include "jobs_ui.h"
#include "system_info.h"
#include "filter_ui.h"
#include "layer_style_ui.h"
#include "ui/filter_dialog.h"
#include "preferences.h"
#include "discard.h"
#include "shell_internal.h"
#include "base/mem.h"
#include "ui/brand.h"
#include "ui/color_picker.h"
#include "ui/new_document.h"
#include "ui/i18n.h"
#include "engine/prefs.h"
#include "ui/widgets.h"
#include "color/convert.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* SDL can complete on a worker after the app has closed. The callback owns only this mailbox. */
struct pc_ui_file_reply {
    atomic_uint refs, ready;
    uint64_t dialog;
    char *path;
    char error[256];
};
static void file_reply_release(struct pc_ui_file_reply *reply)
{
    if (reply && atomic_fetch_sub_explicit(&reply->refs, 1, memory_order_acq_rel) == 1) {
        free(reply->path);
        free(reply);
    }
}
static void SDLCALL file_chosen(void *userdata, const char *const *files, int filter)
{
    (void)filter;
    struct pc_ui_file_reply *reply = userdata;
    if (!files) {
        (void)snprintf(reply->error, sizeof reply->error, "%s", SDL_GetError());
    } else if (files[0]) {
        size_t len = strlen(files[0]);
        if (len > 65536) {
            (void)snprintf(reply->error, sizeof reply->error, "File path is too long");
        } else {
            reply->path = pc_strndup(files[0], len);
            if (!reply->path) {
                (void)snprintf(reply->error, sizeof reply->error, "Cannot allocate file path");
            }
        }
    }
    atomic_store_explicit(&reply->ready, 1, memory_order_release);
    file_reply_release(reply);
}
static void choose_file(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog)
{
    if (runtime->file_reply) {
        return;
    }
    struct pc_ui_file_reply *reply = calloc(1, sizeof *reply);
    if (!reply) {
        (void)snprintf(runtime->error, sizeof runtime->error, "Cannot allocate file chooser");
        return;
    }
    atomic_init(&reply->refs, 2);
    atomic_init(&reply->ready, 0);
    reply->dialog = dialog->id;
    runtime->file_reply = reply;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    const char *command = yyjson_mut_get_str(yyjson_mut_obj_get(root, "__command"));
    const char *path = yyjson_mut_get_str(yyjson_mut_obj_get(root, "path"));
    static const SDL_DialogFileFilter open_filters[] = {
        {"Supported images",
         "pcraft;psd;psb;png;jpg;jpeg;tif;tiff;webp;bmp;gif;tga;exr;hdr;heic;heif;avif;qoi;dpx"},
        {"All files", "*"}};
    static const SDL_DialogFileFilter save_filters[] = {{"PhotoCraft document", "pcraft"},
                                                        {"Photoshop document", "psd;psb"},
                                                        {"PNG image", "png"},
                                                        {"JPEG image", "jpg;jpeg"},
                                                        {"TIFF image", "tif;tiff"},
                                                        {"OpenEXR image", "exr"},
                                                        {"All files", "*"}};
    bool saving = command && (!strcmp(command, "file.saveAs") || !strcmp(command, "file.saveACopy") ||
                              !strcmp(command, "file.export.exportAs"));
    if (saving) {
        SDL_ShowSaveFileDialog(file_chosen, reply, pc_ui_shell_app_sdl_window(app), save_filters,
                               (int)(sizeof save_filters / sizeof *save_filters), path);
    } else {
        SDL_ShowOpenFileDialog(file_chosen, reply, pc_ui_shell_app_sdl_window(app), open_filters,
                               (int)(sizeof open_filters / sizeof *open_filters), path, false);
    }
}
static pc_status null_result(pc_json_value *out, pc_error *err)
{
    (void)err;
    pc_json_value_fini(out);
    *out = (pc_json_value)PC_JSON_VALUE_INIT;
    return PC_OK;
}
pc_status pc_ui_shell_dialog_store(pc_ui_shell_app *app, pc_ui_dialog_kind kind, yyjson_mut_doc *fields,
                                   yyjson_mut_val *root, pc_json_value *out, pc_error *err)
{
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *parsed = NULL;
    pc_status rc = pc_json_write(root, 0, &text, err);
    if (rc == PC_OK) {
        rc = pc_json_parse(pc_buf_cstr(&text), text.len, &parsed, err);
    }
    yyjson_mut_doc *result = rc == PC_OK ? yyjson_mut_doc_new(NULL) : NULL;
    if (rc == PC_OK && !result) {
        rc = pc_fail(err, PC_ERR_NOMEM, "dialog result");
    }
    if (rc == PC_OK) {
        pc_ui_state *ui = pc_ui_shell_app_state(app);
        uint64_t id = pc_ui_state_open_dialog(ui, kind, yyjson_doc_get_root(parsed));
        if (!id) {
            rc = pc_fail(err, PC_ERR_LIMIT, "cannot allocate another dialog");
        } else {
            yyjson_mut_val *object = yyjson_mut_obj(result);
            yyjson_mut_obj_add_uint(result, object, "dialog", id);
            rc = pc_json_value_from_mut(object, out, err);
            if (rc != PC_OK) {
                pc_ui_state_close_dialog(ui, id);
            } else {
                pc_ui_shell_modal_changed(app);
            }
        }
    }
    yyjson_mut_doc_free(result);
    yyjson_doc_free(parsed);
    pc_buf_fini(&text);
    yyjson_mut_doc_free(fields);
    return rc;
}
static bool put(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *key, yyjson_mut_val *value)
{
    yyjson_mut_val *k = yyjson_mut_strcpy(doc, key);
    return k && value && yyjson_mut_obj_put(object, k, value);
}
static bool protected_field(const char *field)
{
    return field && field[0] == '_' && field[1] == '_';
}
static pc_status open_dialog(pc_ui_shell_app *app, yyjson_val *params, pc_json_value *out, pc_error *err)
{
    const char *kind = pc_json_as_str(pc_json_get(params, "kind"), NULL);
    if (!kind) {
        return pc_fail(err, PC_ERR_INVALID, "missing dialog kind");
    }
    if (!strcmp(kind, "exportAs"))
        return pc_ui_shell_form_open(app, "file.export.exportAs", out, err);
    if (!strcmp(kind, "variables"))
        return pc_ui_shell_variables_open(app, "image.variables.define", out, err);
    if (!strcmp(kind, "layerStyle")) {
        yyjson_val *select = pc_json_get(params, "select");
        if (select && !yyjson_is_str(select))
            return pc_fail(err, PC_ERR_INVALID, "style selection must be text");
        return pc_ui_shell_style_open(app, pc_json_as_str(select, NULL), out, err);
    }
    if (!strcmp(kind, "preferences") || !strcmp(kind, "Preferences")) {
        yyjson_val *section = pc_json_get(params, "section");
        if (section && !yyjson_is_str(section)) {
            return pc_fail(err, PC_ERR_INVALID, "Preferences section must be a string");
        }
        return pc_ui_shell_preferences_open(app, yyjson_get_str(section), out, err);
    }
    pc_ui_dialog_kind type;
    bool new_document = !strcmp(kind, "newDocument") || !strcmp(kind, "NewDocument");
    bool color = !strcmp(kind, "colorPicker") || !strcmp(kind, "ColorPicker");
    bool file = !strcmp(kind, "fileOpen") || !strcmp(kind, "fileSave") || !strcmp(kind, "filePlace");
    if (new_document) {
        type = PC_DIALOG_NEW_DOCUMENT;
    } else if (!strcmp(kind, "about") || !strcmp(kind, "About")) {
        type = PC_DIALOG_ABOUT;
    } else if (!strcmp(kind, "command") || !strcmp(kind, "Command") || color || file) {
        type = PC_DIALOG_COMMAND;
    } else {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "dialog kind `%s` is not available yet", kind);
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return pc_fail(err, PC_ERR_NOMEM, "dialog fields");
    }
    yyjson_mut_val *root = new_document ? pc_ui_state_new_document_fields(doc) : yyjson_mut_obj(doc);
    if (!root) {
        yyjson_mut_doc_free(doc);
        return pc_fail(err, PC_ERR_NOMEM, "dialog fields");
    }
    if (type == PC_DIALOG_COMMAND && !color) {
        const char *command = pc_json_as_str(pc_json_get(params, "command"), NULL);
        if (file && !command) {
            command = !strcmp(kind, "fileSave")    ? "file.saveAs"
                      : !strcmp(kind, "filePlace") ? "file.placeEmbedded"
                                                   : "file.open";
        }
        if (!command || !*command || strlen(command) > 255) {
            yyjson_mut_doc_free(doc);
            return pc_fail(err, PC_ERR_INVALID, "command dialogs need `command`");
        }
        const pc_engine_command *spec = pc_engine_command_find(command);
        if (file && strcmp(command, "file.open") && strcmp(command, "file.openAs") &&
            strcmp(command, "file.saveAs") && strcmp(command, "file.saveACopy") &&
            strcmp(command, "file.placeEmbedded") && strcmp(command, "file.placeLinked")) {
            yyjson_mut_doc_free(doc);
            return pc_fail(err, PC_ERR_INVALID, "invalid file dialog command");
        }
        if (file) {
            yyjson_mut_obj_add_bool(doc, root, "__fileDialog", true);
            yyjson_mut_obj_add_bool(doc, root, "__choose", true);
            yyjson_mut_obj_add_str(doc, root, "path", "");
        }
        yyjson_mut_obj_add_strcpy(doc, root, "__command", command);
        yyjson_mut_obj_add_strcpy(doc, root, "__label", spec ? spec->label : command);
    }
    if (color) {
        const char *target = pc_json_as_str(pc_json_get(params, "target"), NULL);
        if (pc_json_get(params, "parent") || pc_json_get(params, "textStyle") ||
            pc_json_get(params, "effect") || pc_json_get(params, "field")) {
            yyjson_mut_doc_free(doc);
            return pc_fail(err, PC_ERR_UNSUPPORTED, "nested color picker targets require their panel editor");
        }
        if(target && strcmp(target,"foreground") && strcmp(target,"background") && strcmp(target,"pasteboard")) {
            yyjson_mut_doc_free(doc);return pc_fail(err,PC_ERR_INVALID,"unknown color target");
        }
        bool pasteboard=target && !strcmp(target,"pasteboard");
        bool background = target && !strcmp(target, "background");
        const pc_engine_tool_state *tools = pc_engine_session_tools(pc_ui_shell_app_session(app));
        const float *rgba = background ? tools->background : tools->foreground;
        root = pc_ui_picker_initial(
            doc, background ? "background" : "foreground",
            background ? "Color Picker (Background Color)" : "Color Picker (Foreground Color)", rgba);
        if(pasteboard) {
            float custom[4]={.5f,.5f,.5f,1};pc_json_value preference=PC_JSON_VALUE_INIT;
            if(pc_engine_prefs_get(pc_ui_shell_app_session(app),"interface.canvasCustomColor",&preference,NULL)==PC_OK) {
                const char *hex=yyjson_mut_get_str(pc_json_value_root(&preference));
                if(hex)(void)pc_ui_picker_parse_hex(hex,strlen(hex),custom);
            }
            pc_json_value_fini(&preference);
            root=pc_ui_picker_initial(doc,"pasteboard","Color Picker (Custom Canvas Color)",custom);
        }
        float cmyk[4];
        if (!pasteboard && pc_engine_tool_state_picked_cmyk(tools, rgba, cmyk)) {
            yyjson_mut_val *a = yyjson_mut_arr(doc);
            for (size_t i = 0; i < 4; i++) {
                yyjson_mut_arr_add_float(doc, a, cmyk[i]);
            }
            yyjson_mut_obj_add_val(doc, root, "__cmyk", a);
        }
    }
    if (new_document) {
        pc_size clipboard;
        if (pc_engine_session_clipboard_size(pc_ui_shell_app_session(app), &clipboard)) {
            pc_status status = pc_ui_new_offer_clipboard(doc, root, clipboard.width, clipboard.height, err);
            if (status != PC_OK) {
                yyjson_mut_doc_free(doc);
                return status;
            }
        }
    }
    yyjson_val *patch = pc_json_get(params, "fields");
    if (patch) {
        if (!yyjson_is_obj(patch)) {
            yyjson_mut_doc_free(doc);
            return pc_fail(err, PC_ERR_INVALID, "dialog fields must be an object");
        }
        size_t i, n;
        yyjson_val *key, *value;
        yyjson_obj_foreach(patch, i, n, key, value)
        {
            if (protected_field(yyjson_get_str(key))) {
                yyjson_mut_doc_free(doc);
                return pc_fail(err, PC_ERR_INVALID, "dialog routing fields are read-only");
            }
            if (!put(doc, root, yyjson_get_str(key), yyjson_val_mut_copy(doc, value))) {
                yyjson_mut_doc_free(doc);
                return pc_fail(err, PC_ERR_NOMEM, "dialog fields");
            }
        }
    }
    if (file) {
        const char *command = yyjson_mut_get_str(yyjson_mut_obj_get(root, "__command"));
        if (command && strcmp(command, "file.open") && strcmp(command, "file.openAs")) {
            const pc_engine_doc_state *active = pc_engine_session_active(pc_ui_shell_app_session(app));
            if (!active) {
                yyjson_mut_doc_free(doc);
                return pc_fail(err, PC_ERR_INVALID, "no document open");
            }
            if (!yyjson_mut_obj_add_uint(doc, root, "__document", pc_doc_arc_get(active->doc)->id)) {
                yyjson_mut_doc_free(doc);
                return pc_fail(err, PC_ERR_NOMEM, "file dialog target");
            }
        }
    }
    return pc_ui_shell_dialog_store(app, type, doc, root, out, err);
}
pc_status pc_ui_shell_dialog_color_command(pc_ui_shell_app *app, const char *label, const float rgba[4],
                                           const char *command, const yyjson_mut_val *params,
                                           const char *field, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state || !command || !pc_engine_command_find(command) || !yyjson_mut_is_obj(params) || !field ||
        (strcmp(field, "color") && strcmp(field, "attrs.color"))) {
        return pc_fail(err, PC_ERR_INVALID, "missing color target");
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? pc_ui_picker_initial(doc, "command", label, rgba) : NULL;
    if (!root || !yyjson_mut_obj_add_strcpy(doc, root, "__colorCommand", command) ||
        !yyjson_mut_obj_add_val(doc, root, "__colorParams", yyjson_mut_val_mut_copy(doc, params)) ||
        !yyjson_mut_obj_add_strcpy(doc, root, "__colorField", field) ||
        !yyjson_mut_obj_add_uint(doc, root, "__document", pc_doc_arc_get(state->doc)->id) ||
        !yyjson_mut_obj_add_uint(doc, root, "__revision", state->revision)) {
        yyjson_mut_doc_free(doc);
        return pc_fail(err, PC_ERR_NOMEM, "color target");
    }
    return pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, doc, root, out, err);
}
pc_status pc_ui_shell_dialog_file(pc_ui_shell_app *app, const char *command, pc_json_value *out,
                                  pc_error *err)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return pc_fail(err, PC_ERR_NOMEM, "file dialog");
    }
    yyjson_mut_val *params = yyjson_mut_obj(doc);
    bool saving = !strcmp(command, "file.saveAs") || !strcmp(command, "file.saveACopy");
    yyjson_mut_obj_add_str(doc, params, "kind", saving ? "fileSave" : "fileOpen");
    yyjson_mut_obj_add_strcpy(doc, params, "command", command);
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *read = NULL;
    pc_status status = pc_json_write(params, 0, &text, err);
    if (status == PC_OK) {
        status = pc_json_parse(pc_buf_cstr(&text), text.len, &read, err);
    }
    if (status == PC_OK) {
        status = open_dialog(app, yyjson_doc_get_root(read), out, err);
    }
    yyjson_doc_free(read);
    pc_buf_fini(&text);
    yyjson_mut_doc_free(doc);
    return status;
}
pc_status pc_ui_shell_dialog_new(pc_ui_shell_app *app, pc_json_value *out, pc_error *err)
{
    yyjson_doc *params = NULL;
    pc_status rc =
        pc_json_parse("{\"kind\":\"newDocument\"}", sizeof "{\"kind\":\"newDocument\"}" - 1, &params, err);
    if (rc == PC_OK) {
        rc = open_dialog(app, yyjson_doc_get_root(params), out, err);
    }
    yyjson_doc_free(params);
    return rc;
}
static void prune_children(pc_ui_state *ui, uint64_t parent)
{
    for (size_t i = ui->n_dialogs; i-- > 0;) {
        yyjson_mut_val *root = yyjson_mut_doc_get_root(ui->dialogs[i].fields);
        if (yyjson_mut_get_uint(yyjson_mut_obj_get(root, "__dialog")) == parent) {
            pc_ui_state_close_dialog(ui, ui->dialogs[i].id);
        }
    }
}
static pc_status confirm(pc_ui_shell_app *app, uint64_t id, bool apply, pc_json_value *out, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_dialog *dialog = pc_ui_state_dialog_mut(ui, id);
    if (!dialog || !dialog->fields) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "no dialog %llu", (unsigned long long)id);
    }
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(dialog->fields);
    if (!apply && yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "__rasterize")))
        return pc_ui_shell_rasterize_confirm(app, id, out, err);
    if (pc_ui_shell_is_discard(fields) && !apply) {
        return pc_ui_shell_discard_confirm(app, id, out, err);
    }
    if (pc_ui_shell_is_preferences(fields)) {
        return pc_ui_shell_preferences_commit(app, id, !apply, out, err);
    }
    if (yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "__variables"))) {
        const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
        if (!state ||
            pc_doc_arc_get(state->doc)->id != yyjson_mut_get_uint(yyjson_mut_obj_get(fields, "__document")) ||
            state->revision != yyjson_mut_get_uint(yyjson_mut_obj_get(fields, "__revision")))
            return pc_fail(err, PC_ERR_INVALID, "the document changed while Variables was open");
    }
    if (apply) {
        return pc_fail(err, PC_ERR_INVALID, "Apply is only available for Preferences");
    }
    if (yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "__filter")) ||
        yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "__form"))) {
        pc_status guard = pc_ui_filter_dialog_guard(pc_ui_shell_app_session(app), fields, err);
        if (guard != PC_OK)
            return guard;
    }
    if (yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "__export"))) {
        const char *path = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "path"));
        if (!path || !*path)
            return pc_fail(err, PC_ERR_INVALID, "choose an export destination");
    }
    const char *command = NULL;
    const char *raw = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__command"));
    if (dialog->kind == PC_DIALOG_NEW_DOCUMENT) {
        command = "file.new";
    } else if (dialog->kind == PC_DIALOG_COMMAND) {
        command = raw;
    } else if (dialog->kind == PC_DIALOG_ABOUT || dialog->kind == PC_DIALOG_ERROR) {
        pc_ui_state_close_dialog(ui, id);
        return null_result(out, err);
    } else {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "dialog editor is not available yet");
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d) {
        return pc_fail(err, PC_ERR_NOMEM, "dialog parameters");
    }
    yyjson_mut_val *params = yyjson_mut_obj(d);
    const char *picker_target = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__colorPicker"));
    bool picker = picker_target != NULL;
    if (picker_target && !strcmp(picker_target, "dialogField")) {
        yyjson_mut_doc_free(d);
        return pc_ui_shell_dialog_color_field_confirm(app, id, out, err);
    }
    if (picker) {
        command = "tools.setColors";
        const char *target = !strcmp(picker_target, "background") ? "background" : "foreground";
        const char *hex = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "color"));
        float rgb[3];
        if (!hex || !pc_ui_picker_parse_hex(hex, strlen(hex), rgb)) {
            yyjson_mut_doc_free(d);
            return pc_fail(err, PC_ERR_INVALID, "not a colour");
        }
        const char *color_command = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__colorCommand"));
        if (color_command) {
            command = color_command;
            params = yyjson_mut_val_mut_copy(d, yyjson_mut_obj_get(fields, "__colorParams"));
            const char *field = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__colorField"));
            yyjson_mut_val *color_target = params;
            if (field && !strcmp(field, "attrs.color") && params) {
                color_target = yyjson_mut_obj_get(params, "attrs");
                if (!color_target) {
                    color_target = yyjson_mut_obj(d);
                    if (!color_target || !yyjson_mut_obj_add_val(d, params, "attrs", color_target)) {
                        color_target = NULL;
                    }
                }
            }
            if (!params || !yyjson_mut_is_obj(color_target) ||
                !put(d, color_target, "color", yyjson_mut_strcpy(d, hex))) {
                yyjson_mut_doc_free(d);
                return pc_fail(err, PC_ERR_NOMEM, "color target");
            }
        } else {
            yyjson_mut_obj_add_strcpy(d, params, target, hex);
        }
        if(!strcmp(picker_target,"pasteboard")) {
            command="prefs.set";params=yyjson_mut_obj(d);yyjson_mut_val *values=yyjson_mut_obj(d);
            if(!params || !values || !yyjson_mut_obj_add_strcpy(d,values,"interface.canvasCustomColor",hex) ||
               !yyjson_mut_obj_add_str(d,values,"interface.canvasColor","custom") ||
               !yyjson_mut_obj_add_val(d,params,"values",values)) {
                yyjson_mut_doc_free(d);return pc_fail(err,PC_ERR_NOMEM,"pasteboard color");
            }
        }
        yyjson_mut_val *stored = yyjson_mut_obj_get(fields, "__cmyk");
        if (yyjson_mut_is_arr(stored) && yyjson_mut_arr_size(stored) == 4) {
            float inks[4], back[3];
            bool valid = true;
            for (size_t i = 0; i < 4; i++) {
                yyjson_mut_val *v = yyjson_mut_arr_get(stored, i);
                inks[i] = (float)yyjson_mut_get_num(v);
                valid = valid && yyjson_mut_is_num(v) && isfinite(inks[i]);
            }
            pc_cmyk_to_rgb(inks, back);
            char selected[8], converted[8];
            pc_ui_picker_hex(rgb, selected);
            pc_ui_picker_hex(back, converted);
            if (valid && !strcmp(selected, converted)) {
                yyjson_mut_obj_add_val(d, params,
                                       !strcmp(target, "background") ? "backgroundCmyk" : "foregroundCmyk",
                                       pc_json_normalized_copy(d, stored));
            }
        }
    } else if (apply) {
        yyjson_mut_val *values = yyjson_mut_obj_get(fields, "values");
        if (!yyjson_mut_is_obj(values)) {
            yyjson_mut_doc_free(d);
            return pc_fail(err, PC_ERR_INVALID, "ui.dialog.apply is for Preferences");
        }
        command = "prefs.set";
        yyjson_mut_obj_add_str(d, params, "path", "");
        yyjson_mut_obj_add_val(d, params, "value", pc_json_normalized_copy(d, values));
    } else {
        size_t i, max;
        yyjson_mut_val *key, *value;
        yyjson_mut_obj_foreach(fields, i, max, key, value)
        {
            if (!key || !value) {
                yyjson_mut_doc_free(d);
                return pc_fail(err, PC_ERR_INVALID, "invalid dialog fields");
            }
            const char *name = yyjson_mut_get_str(key);
            if (name && strncmp(name, "__", 2) && strcmp(name, "preview")) {
                yyjson_mut_obj_add_val(d, params, name, pc_json_normalized_copy(d, value));
            }
        }
    }
    pc_str name = PC_STR_INIT;
    pc_status rc = command ? pc_str_set(&name, command, strlen(command), err)
                           : pc_fail(err, PC_ERR_INVALID, "dialog has no command");
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *read = NULL;
    if (rc == PC_OK) {
        rc = pc_json_write(params, 0, &text, err);
    }
    if (rc == PC_OK) {
        rc = pc_json_parse(pc_buf_cstr(&text), text.len, &read, err);
    }
    bool creating = dialog->kind == PC_DIALOG_NEW_DOCUMENT;
    pc_buf recent = PC_BUF_INIT;
    if (rc == PC_OK && creating) {
        yyjson_mut_val *settings = pc_ui_new_settings(d, fields), *request = yyjson_mut_obj(d);
        if (settings && request && yyjson_mut_obj_add_val(d, request, "settings", settings)) {
            (void)pc_json_write(request, 0, &recent, NULL);
        }
    }
    bool file = yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "__fileDialog"));
    if (rc == PC_OK && (file || picker) && yyjson_mut_obj_get(fields, "__document")) {
        pc_doc_id target = yyjson_mut_get_uint(yyjson_mut_obj_get(fields, "__document"));
        size_t count = 0, index = 0;
        const pc_engine_doc_state *docs = pc_engine_session_documents(pc_ui_shell_app_session(app), &count);
        while (index < count && pc_doc_arc_get(docs[index].doc)->id != target) {
            index++;
        }
        if (index == count) {
            rc = pc_fail(err, PC_ERR_NOT_FOUND, "the document for this file dialog was closed");
        } else if (picker && yyjson_mut_obj_get(fields, "__revision") &&
                   docs[index].revision != yyjson_mut_get_uint(yyjson_mut_obj_get(fields, "__revision"))) {
            rc = pc_fail(err, PC_ERR_INVALID, "the color target changed while the picker was open");
        } else {
            (void)pc_engine_session_set_active(pc_ui_shell_app_session(app), index);
        }
    }
    if (rc == PC_OK) {
        pc_ui_shell_app_changed(app);
        float foreground[4];
        memcpy(foreground, pc_engine_session_tools(pc_ui_shell_app_session(app))->foreground,
               sizeof foreground);
        rc = file ? pc_ui_shell_file_execute(app, pc_str_c(&name), yyjson_doc_get_root(read), out, err)
             : pc_ui_shell_workspace_handles(pc_str_c(&name))
                 ? pc_ui_shell_workspace_command(app, pc_str_c(&name), yyjson_doc_get_root(read), out, err)
                 : pc_ui_shell_jobs_execute(app, pc_str_c(&name), yyjson_doc_get_root(read), out, err);
        if (rc == PC_OK && !strcmp(pc_str_c(&name), "tools.setColors")) {
            rc = pc_ui_type_foreground_changed(pc_ui_shell_app_session(app), ui, foreground, err);
        }
        if (rc == PC_OK && picker && ui->has_text_edit && yyjson_mut_obj_get(fields, "__colorCommand")) {
            const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
            if (state && pc_doc_arc_get(state->doc)->id == ui->text_edit.document) {
                ui->text_edit.revision = state->revision;
            }
        }
        if (rc == PC_OK && !apply) {
            pc_ui_state_close_dialog(ui, id);
            prune_children(ui, id);
        }
        if (rc == PC_OK && creating && recent.len) {
            pc_json_value recorded = PC_JSON_VALUE_INIT;
            (void)pc_engine_session_execute_json(pc_ui_shell_app_session(app), "document.recent.add",
                                                 pc_buf_cstr(&recent), &recorded, NULL);
            pc_json_value_fini(&recorded);
        }
        if (rc == PC_OK && (creating || file)) {
            rc = pc_ui_shell_app_sync_views(app, err);
        }
    }
    pc_buf_fini(&recent);
    pc_str_fini(&name);
    pc_buf_fini(&text);
    yyjson_doc_free(read);
    yyjson_mut_doc_free(d);
    return rc;
}
pc_status pc_ui_shell_dialog_control(pc_ui_shell_app *app, const char *method, yyjson_val *p,
                                     pc_json_value *out, pc_error *err)
{
    if (!app || !method || !out) {
        return pc_fail(err, PC_ERR_INVALID, "missing dialog request");
    }
    if (!strcmp(method, "ui.dialog.open")) {
        return open_dialog(app, p, out, err);
    }
    uint64_t id = 0;
    if (!pc_json_as_u64(pc_json_get(p, "dialog"), &id)) {
        return pc_fail(err, PC_ERR_INVALID, "missing `dialog`");
    }
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_dialog *dialog = pc_ui_state_dialog_mut(ui, id);
    if (!dialog) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "no dialog %llu", (unsigned long long)id);
    }
    if (!strcmp(method, "ui.dialog.cancel")) {
        pc_ui_state_close_dialog(ui, id);
        prune_children(ui, id);
        return null_result(out, err);
    }
    if (!strcmp(method, "ui.dialog.confirm") || !strcmp(method, "ui.dialog.apply")) {
        pc_status status = confirm(app, id, !strcmp(method, "ui.dialog.apply"), out, err);
        pc_ui_shell_discard_poll(app);
        return status;
    }
    if (!strcmp(method, "ui.dialog.set")) {
        size_t len = 0;
        const char *field = pc_json_as_str(pc_json_get(p, "field"), &len);
        if (!field || !len || len > 255) {
            return pc_fail(err, PC_ERR_INVALID, "missing or invalid `field`");
        }
        if (protected_field(field)) {
            return pc_fail(err, PC_ERR_INVALID, "dialog routing fields are read-only");
        }
        if (pc_ui_shell_is_discard(yyjson_mut_doc_get_root(dialog->fields)) && strcmp(field, "choice") &&
            strcmp(field, "path")) {
            return pc_fail(err, PC_ERR_INVALID, "unknown discard field");
        }
        yyjson_val *value = pc_json_get(p, "value");
        yyjson_mut_doc *replacement = yyjson_mut_doc_new(NULL);
        if (!replacement) {
            return pc_fail(err, PC_ERR_NOMEM, "dialog field");
        }
        yyjson_mut_val *root = yyjson_mut_val_mut_copy(replacement, yyjson_mut_doc_get_root(dialog->fields));
        yyjson_mut_val *copy = value ? yyjson_val_mut_copy(replacement, value) : yyjson_mut_null(replacement);
        if (!root || !put(replacement, root, field, copy)) {
            yyjson_mut_doc_free(replacement);
            return pc_fail(err, PC_ERR_NOMEM, "dialog field");
        }
        yyjson_mut_doc_set_root(replacement, root);
        yyjson_mut_doc_free(dialog->fields);
        dialog->fields = replacement;
        return null_result(out, err);
    }
    return pc_fail(err, PC_ERR_NOT_FOUND, "unknown dialog method");
}
void pc_ui_shell_dialogs_fini(pc_ui_dialog_runtime *runtime)
{
    if (!runtime) {
        return;
    }
    pc_ui_credits_fini(&runtime->credits);
    pc_ui_filter_preview_fini(&runtime->preview);
    pc_ui_parameter_schema_fini(&runtime->schema);
    file_reply_release(runtime->file_reply);
    for (size_t i = 0; i < runtime->ntexts; i++) {
        pc_buf_fini(&runtime->texts[i].text);
    }
    *runtime = (pc_ui_dialog_runtime){0};
}
pc_ui_dialog_text *pc_ui_dialog_text_buffer(pc_ui_dialog_runtime *r, uint64_t id, const char *key,
                                            const char *value)
{
    pc_ui_dialog_text *item = NULL;
    for (size_t i = 0; i < r->ntexts; i++) {
        if (r->texts[i].id == id && !strcmp(r->texts[i].key, key)) {
            item = &r->texts[i];
            break;
        }
    }
    if (!item) {
        if (r->ntexts == 32) {
            for (size_t i = 0; i < r->ntexts; i++) {
                if (r->texts[i].touched != r->frame) {
                    item = &r->texts[i];
                    pc_buf_fini(&item->text);
                    *item = (pc_ui_dialog_text){0};
                    break;
                }
            }
            if (!item) {
                return NULL;
            }
        } else {
            item = &r->texts[r->ntexts++];
        }
        item->id = id;
        (void)snprintf(item->key, sizeof item->key, "%s", key);
    }
    item->touched = r->frame;
    if (strlen(value) > ((size_t)1 << 20)) {
        return NULL;
    }
    if (!item->active && strcmp(pc_buf_cstr(&item->text), value)) {
        pc_buf_truncate(&item->text, 0);
        if (pc_buf_append_str(&item->text, value, NULL) != PC_OK) {
            return NULL;
        }
    }
    if (pc_buf_reserve(&item->text, 1024, NULL) != PC_OK) {
        return NULL;
    }
    return item;
}
static void text_field(struct nk_context *ctx, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog, const char *key,
                       const char *label)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    const char *value = yyjson_mut_get_str(yyjson_mut_obj_get(root, key));
    if (!value) {
        value = "";
    }
    pc_ui_dialog_text *buffer = pc_ui_dialog_text_buffer(r, dialog->id, key, value);
    nk_layout_row_dynamic(ctx, 24, 2);
    nk_label(ctx, label, NK_TEXT_LEFT);
    if (!buffer) {
        nk_label(ctx, pc_i18n_t("Text is too large to edit"), NK_TEXT_LEFT);
        return;
    }
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, (char *)buffer->text.data, (int)buffer->text.cap,
                                   nk_filter_default);
    size_t len = strlen((char *)buffer->text.data);
    buffer->text.len = len;
    if (strcmp(value, (char *)buffer->text.data)) {
        (void)put(dialog->fields, root, key,
                  yyjson_mut_strncpy(dialog->fields, (char *)buffer->text.data, len));
    }
}
static void choice(struct nk_context *ctx, pc_ui_dialog *dialog, const char *key, const char *label,
                   const char *const *names, const char *const *ids, size_t count)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    const char *value = yyjson_mut_get_str(yyjson_mut_obj_get(root, key));
    size_t chosen = 0;
    for (size_t i = 0; value && i < count; i++) {
        if (!strcmp(ids[i], value)) {
            chosen = i;
            break;
        }
    }
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_label(ctx, label, NK_TEXT_LEFT);
    if (nk_combo_begin_label(ctx, names[chosen], nk_vec2(190, 160))) {
        nk_layout_row_dynamic(ctx, 24, 1);
        for (size_t i = 0; i < count; i++) {
            if (nk_combo_item_label(ctx, names[i], NK_TEXT_LEFT)) {
                (void)put(dialog->fields, root, key, yyjson_mut_str(dialog->fields, ids[i]));
            }
        }
        nk_combo_end(ctx);
    }
}
static float new_number(yyjson_mut_val *fields, const char *key, float fallback)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(fields, key);
    float number = (float)yyjson_mut_get_num(v);
    return yyjson_mut_is_num(v) && isfinite(number) ? number : fallback;
}
static const char *new_string(yyjson_mut_val *fields, const char *key, const char *fallback)
{
    const char *s = yyjson_mut_get_str(yyjson_mut_obj_get(fields, key));
    return s ? s : fallback;
}
static void new_error(pc_ui_dialog_runtime *r, pc_status status, const pc_error *err)
{
    if (status != PC_OK) {
        (void)snprintf(r->error, sizeof r->error, "%s", err->msg);
    }
}
static pc_status new_command(pc_ui_shell_app *app, const char *command, yyjson_mut_val *params,
                             pc_json_value *out, pc_error *err)
{
    pc_buf text = PC_BUF_INIT;
    pc_status status = pc_json_write(params, 0, &text, err);
    if (status == PC_OK) {
        status = pc_engine_session_execute_json(pc_ui_shell_app_session(app), command, pc_buf_cstr(&text),
                                                out, err);
    }
    pc_buf_fini(&text);
    return status;
}
static float new_preference(pc_ui_shell_app *app, const char *path, float fallback)
{
    pc_json_value value = PC_JSON_VALUE_INIT;
    if (pc_engine_prefs_get(pc_ui_shell_app_session(app), path, &value, NULL) == PC_OK) {
        yyjson_mut_val *root = pc_json_value_root(&value);
        float number = (float)yyjson_mut_get_num(root);
        if (yyjson_mut_is_num(root) && isfinite(number) && number > 0) {
            fallback = number;
        }
    }
    pc_json_value_fini(&value);
    return fallback;
}
static void new_size_field(struct nk_context *ctx, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog,
                           const char *key, const char *label)
{
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(dialog->fields);
    const char *unit = new_string(fields, "__unit", "px");
    float ppi = new_number(fields, "resolution", 72);
    float value = pc_ui_new_shown_size(fields, key, !strcmp(key, "width") ? 1920 : 1080, unit, ppi),
          before = value;
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_property_float(ctx, label, .01f, &value, 300000, !strcmp(unit, "px") ? 1 : .1f, .1f);
    if (value != before) {
        pc_error err;
        pc_status status = pc_ui_new_set_size(dialog->fields, fields, key, value, unit, ppi, &err);
        new_error(r, status, &err);
    }
}
static void new_document_body(pc_ui_shell_app *app, struct nk_context *ctx, pc_ui_dialog_runtime *r,
                              pc_ui_dialog *dialog)
{
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(dialog->fields);
    const char *const categories[] = {"Recent", "Photo",  "Print",        "Art & Illustration",
                                      "Web",    "Mobile", "Film & Video", "Saved"};
    choice(ctx, dialog, "__category", "Category", categories, categories, 8);
    const char *category = new_string(fields, "__category", "Recent");
    float height = fmaxf(220, nk_window_get_content_region(ctx).h - 82);
    nk_layout_row_dynamic(ctx, height, 2);
    if (nk_group_begin(ctx, "blank-presets", 0)) {
        pc_json_value data = PC_JSON_VALUE_INIT;
        const char *query = !strcmp(category, "Saved") ? "document.presets.list" : "document.recent.list";
        bool saved = !strcmp(category, "Saved"), recent = !strcmp(category, "Recent");
        yyjson_mut_val *clipboard = yyjson_mut_obj_get(fields, "__clipboard");
        if (recent && yyjson_mut_arr_size(clipboard) == 2) {
            uint32_t width = (uint32_t)yyjson_mut_get_uint(yyjson_mut_arr_get(clipboard, 0)),
                     clipboard_height = (uint32_t)yyjson_mut_get_uint(yyjson_mut_arr_get(clipboard, 1));
            char label[160];
            (void)snprintf(label, sizeof label, "%s  %u x %u", pc_i18n_t("Clipboard"), width,
                           clipboard_height);
            nk_layout_row_dynamic(ctx, 32, 1);
            pc_ui_shell_widget_mapping(app, "new.clipboard", nk_widget_bounds(ctx));
            if (nk_select_label(ctx, label, NK_TEXT_LEFT,
                                !strcmp(new_string(fields, "__preset", ""), "Clipboard"))) {
                pc_error err;
                new_error(r, pc_ui_new_offer_clipboard(dialog->fields, fields, width, clipboard_height, &err),
                          &err);
            }
        }
        if ((saved || recent) &&
            pc_engine_session_execute(pc_ui_shell_app_session(app), query, NULL, &data, NULL) == PC_OK) {
            yyjson_mut_val *items =
                yyjson_mut_obj_get(pc_json_value_root(&data), saved ? "presets" : "recent");
            size_t count = yyjson_mut_arr_size(items);
            for (size_t i = 0; i < count; i++) {
                yyjson_mut_val *item = yyjson_mut_arr_get(items, i),
                               *settings = saved ? yyjson_mut_obj_get(item, "settings") : item;
                const char *name = saved ? new_string(item, "name", "Preset") : "Custom";
                char label[320];
                (void)snprintf(label, sizeof label, "%s  %.0f x %.0f", name,
                               (double)new_number(settings, "width", 0),
                               (double)new_number(settings, "height", 0));
                nk_layout_row_dynamic(ctx, 32, saved ? 2 : 1);
                if (nk_button_label(ctx, label)) {
                    pc_error err;
                    pc_status status = pc_ui_new_apply_settings(dialog->fields, fields, settings, &err);
                    new_error(r, status, &err);
                    if (status == PC_OK && saved) {
                        (void)put(dialog->fields, fields, "__savedPreset",
                                  yyjson_mut_strcpy(dialog->fields, name));
                    }
                }
                if (saved && nk_button_label(ctx, pc_i18n_t("Delete"))) {
                    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
                    pc_json_value result = PC_JSON_VALUE_INIT;
                    pc_error err;
                    if (d) {
                        yyjson_mut_val *p = yyjson_mut_obj(d);
                        yyjson_mut_obj_add_strcpy(d, p, "name", name);
                        new_error(r, new_command(app, "document.presets.delete", p, &result, &err), &err);
                    }
                    pc_json_value_fini(&result);
                    yyjson_mut_doc_free(d);
                }
            }
            if (saved && !count) {
                nk_layout_row_dynamic(ctx, 30, 1);
                nk_label_wrap(ctx, pc_i18n_t("Save a preset from Preset Details."));
            }
        }
        pc_json_value_fini(&data);
        if (!saved) {
            size_t count = 0;
            const pc_ui_new_preset *presets = pc_ui_new_presets(&count);
            float print_ppi = new_preference(app, "unitsAndRulers.printResolution", 300),
                  screen_ppi = new_preference(app, "unitsAndRulers.screenResolution", 72);
            for (size_t i = 0; i < count; i++) {
                if (strcmp(presets[i].category, category)) {
                    continue;
                }
                nk_layout_row_dynamic(ctx, 40, 1);
                if (nk_select_label(ctx, presets[i].name, NK_TEXT_LEFT,
                                    !strcmp(new_string(fields, "__preset", ""), presets[i].name))) {
                    pc_error err;
                    new_error(r,
                              pc_ui_new_apply_preset(dialog->fields, fields, &presets[i], print_ppi,
                                                     screen_ppi, &err),
                              &err);
                }
            }
        }
        nk_group_end(ctx);
    }
    if (nk_group_begin(ctx, "preset-details", 0)) {
        text_field(ctx, r, dialog, "name", "Name");
        const char *const unit_ids[] = {"px", "in", "cm", "mm", "pt", "pica"};
        const char *const unit_labels[] = {"Pixels",      "Inches", "Centimeters",
                                           "Millimeters", "Points", "Picas"};
        choice(ctx, dialog, "__unit", "Units", unit_labels, unit_ids, 6);
        new_size_field(ctx, r, dialog, "width", "Width");
        new_size_field(ctx, r, dialog, "height", "Height");
        nk_layout_row_dynamic(ctx, 26, 2);
        bool portrait = new_number(fields, "height", 1080) > new_number(fields, "width", 1920);
        bool turn_portrait = nk_button_label(ctx, pc_i18n_t("Portrait")),
             turn_landscape = nk_button_label(ctx, pc_i18n_t("Landscape"));
        if (turn_portrait || turn_landscape) {
            (void)put(dialog->fields, fields, "__orientation",
                      yyjson_mut_str(dialog->fields, turn_portrait ? "portrait" : "landscape"));
            if (portrait != turn_portrait) {
                pc_error err;
                new_error(r, pc_ui_new_swap_size(dialog->fields, fields, &err), &err);
            }
        }
        bool per_cm = !strcmp(new_string(fields, "__resUnit", "in"), "cm");
        float ppi = new_number(fields, "resolution", 72), resolution = per_cm ? ppi / 2.54f : ppi,
              before = resolution;
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_property_float(ctx, "Resolution", 1, &resolution, 30000, 1, 1);
        if (resolution != before) {
            pc_error err;
            new_error(r,
                      pc_ui_new_set_resolution(dialog->fields, fields,
                                               per_cm ? resolution * 2.54f : resolution, &err),
                      &err);
        }
        const char *const res_ids[] = {"in", "cm"}, *const res_labels[] = {"Pixels/Inch",
                                                                           "Pixels/Centimeter"};
        choice(ctx, dialog, "__resUnit", "Resolution units", res_labels, res_ids, 2);
        const char *const modes[] = {"RGB Color", "Grayscale", "CMYK Color", "Lab Color"},
                          *const mode_ids[] = {"rgb", "gray", "cmyk", "lab"};
        choice(ctx, dialog, "mode", "Color mode", modes, mode_ids, 4);
        int depths[] = {8, 16, 32};
        const char *const names[] = {"8 bit", "16 bit", "32 bit (float)"};
        uint64_t depth = yyjson_mut_get_uint(yyjson_mut_obj_get(fields, "depth"));
        size_t selected = depth == 16 ? 1 : depth == 32 ? 2 : 0;
        nk_layout_row_dynamic(ctx, 26, 2);
        nk_label(ctx, pc_i18n_t("Bit depth"), NK_TEXT_LEFT);
        if (nk_combo_begin_label(ctx, names[selected], nk_vec2(190, 120))) {
            nk_layout_row_dynamic(ctx, 24, 1);
            for (size_t i = 0; i < 3; i++) {
                if (nk_combo_item_label(ctx, names[i], NK_TEXT_LEFT)) {
                    (void)put(dialog->fields, fields, "depth",
                              yyjson_mut_uint(dialog->fields, (uint64_t)depths[i]));
                }
            }
            nk_combo_end(ctx);
        }
        const char *const backgrounds[] = {"White", "Black", "Background Color", "Transparent"},
                          *const background_ids[] = {"white", "black", "backgroundColor", "transparent"};
        const char *previous = new_string(fields, "background", "white");
        choice(ctx, dialog, "background", "Background", backgrounds, background_ids, 4);
        if (strcmp(previous, new_string(fields, "background", "white"))) {
            (void)yyjson_mut_obj_remove_key(fields, "backgroundColor");
        }
        text_field(ctx, r, dialog, "__saveName", "Preset name");
        nk_layout_row_dynamic(ctx, 26, 1);
        if (nk_button_label(ctx, pc_i18n_t("Save Preset"))) {
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            pc_json_value result = PC_JSON_VALUE_INIT;
            pc_error err;
            if (d) {
                yyjson_mut_val *p = yyjson_mut_obj(d), *settings = pc_ui_new_settings(d, fields);
                const char *name = new_string(fields, "__saveName", "Custom");
                if (!*name) {
                    name = "Custom";
                }
                if (p && settings && yyjson_mut_obj_add_strcpy(d, p, "name", name) &&
                    yyjson_mut_obj_add_val(d, p, "settings", settings)) {
                    pc_status status = new_command(app, "document.presets.save", p, &result, &err);
                    new_error(r, status, &err);
                    if (status == PC_OK) {
                        (void)put(dialog->fields, fields, "__savedPreset",
                                  yyjson_mut_strcpy(dialog->fields, name));
                    }
                }
            }
            pc_json_value_fini(&result);
            yyjson_mut_doc_free(d);
        }
        nk_group_end(ctx);
    }
}
static struct nk_color picker_color(const float rgb[3])
{
    char hex[8];
    pc_ui_picker_hex(rgb, hex);
    return nk_rgb_hex(hex);
}
static bool picker_drag(const struct nk_context *ctx, struct nk_rect rect)
{
    struct nk_vec2 origin = ctx->input.mouse.buttons[NK_BUTTON_LEFT].clicked_pos;
    return nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT) && origin.x >= rect.x && origin.y >= rect.y &&
           origin.x < rect.x + rect.w && origin.y < rect.y + rect.h;
}
static void picker_component(struct nk_context *ctx, pc_ui_dialog *dialog, const char *label,
                             pc_ui_picker_keep model, size_t channel, float low, float high)
{
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(dialog->fields);
    pc_ui_picker_components c;
    pc_ui_picker_current(fields, &c);
    float *values = model == PC_UI_PICKER_HSV    ? c.hsv
                    : model == PC_UI_PICKER_LAB  ? c.lab
                    : model == PC_UI_PICKER_CMYK ? c.cmyk
                                                 : c.rgb;
    float scale = model == PC_UI_PICKER_NOTHING                                          ? 255
                  : model == PC_UI_PICKER_CMYK || (model == PC_UI_PICKER_HSV && channel) ? 100
                                                                                         : 1;
    float value = roundf(values[channel] * scale), before = value;
    nk_property_float(ctx, label, low, &value, high, 1, 1);
    if (value != before) {
        values[channel] = value / scale;
        float rgb[3];
        if (model == PC_UI_PICKER_HSV) {
            pc_ui_picker_hsv_to_rgb(values, rgb);
        } else if (model == PC_UI_PICKER_LAB) {
            pc_lab_to_srgb(values, rgb);
        } else if (model == PC_UI_PICKER_CMYK) {
            pc_cmyk_to_rgb(values, rgb);
        } else {
            memcpy(rgb, values, sizeof rgb);
        }
        (void)pc_ui_picker_set_rgb(dialog->fields, fields, rgb, model, values, NULL);
    }
}
static void picker_body(struct nk_context *ctx, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
{
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(dialog->fields);
    pc_ui_picker_components c;
    pc_ui_picker_current(fields, &c);
    const char *const modes[] = {"h", "s", "v", "r", "g", "b", "lab_l", "lab_a", "lab_b"};
    const char *const labels[] = {"Hue",  "Saturation", "Brightness", "Red", "Green",
                                  "Blue", "Lightness",  "a",          "b"};
    const char *mode = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__mode"));
    size_t selected = 0;
    for (size_t i = 0; mode && i < 9; i++) {
        if (!strcmp(mode, modes[i])) {
            selected = i;
            break;
        }
    }
    mode = modes[selected];
    nk_layout_row_dynamic(ctx, 24, 1);
    if (nk_combo_begin_label(ctx, labels[selected], nk_vec2(200, 220))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        for (size_t i = 0; i < 9; i++) {
            if (nk_combo_item_label(ctx, labels[i], NK_TEXT_LEFT)) {
                (void)put(dialog->fields, fields, "__mode", yyjson_mut_str(dialog->fields, modes[i]));
                mode = modes[i];
                selected = i;
            }
        }
        nk_combo_end(ctx);
    }
    float axes[3];
    pc_ui_picker_axes(mode, &c, axes);
    bool web = yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "__webOnly"));
    float field_size = fminf(200, fmaxf(80, nk_window_get_content_region(ctx).w - 120));
    nk_layout_row_begin(ctx, NK_STATIC, field_size, 3);
    nk_layout_row_push(ctx, field_size);
    struct nk_rect field;
    (void)nk_widget(&field, ctx);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    for (unsigned y = 0; y < 32; y++) {
        for (unsigned x = 0; x < 32; x++) {
            float x0 = (float)x / 32, y0 = (float)y / 32, x1 = (float)(x + 1) / 32, y1 = (float)(y + 1) / 32;
            float corners[4][3];
            pc_ui_picker_field_color(mode, axes[2], x0, y0, corners[0]);
            pc_ui_picker_field_color(mode, axes[2], x1, y0, corners[1]);
            pc_ui_picker_field_color(mode, axes[2], x1, y1, corners[2]);
            pc_ui_picker_field_color(mode, axes[2], x0, y1, corners[3]);
            /* Nuklear stores these command bounds as integer shorts. Round outward so adjacent
             * cells cover fractional UI coordinates without dark seams. */
            float left = floorf(field.x + x0 * field.w), top = floorf(field.y + y0 * field.h);
            struct nk_rect cell =
                nk_rect(left, top, ceilf(field.x + x1 * field.w) - left, ceilf(field.y + y1 * field.h) - top);
            if (web) {
                float rgb[3];
                pc_ui_picker_field_color(mode, axes[2], (x0 + x1) / 2, (y0 + y1) / 2, rgb);
                for (size_t i = 0; i < 3; i++) {
                    rgb[i] = roundf(rgb[i] * 5) / 5;
                }
                nk_fill_rect(canvas, cell, 0, picker_color(rgb));
            } else {
                nk_fill_rect_multi_color(canvas, cell, picker_color(corners[0]), picker_color(corners[1]),
                                         picker_color(corners[2]), picker_color(corners[3]));
            }
        }
    }
    nk_stroke_rect(canvas, field, 0, 1, ctx->style.text.color);
    float mx = field.x + axes[0] * field.w, my = field.y + axes[1] * field.h;
    nk_stroke_circle(canvas, nk_rect(mx - 4, my - 4, 8, 8), 1.5f,
                     c.hsv[2] > .6f && c.hsv[1] < .4f ? nk_rgb(0, 0, 0) : nk_rgb(255, 255, 255));
    if (picker_drag(ctx, field)) {
        float x = fminf(1, fmaxf(0, (ctx->input.mouse.pos.x - field.x) / field.w));
        float y = fminf(1, fmaxf(0, (ctx->input.mouse.pos.y - field.y) / field.h));
        float rgb[3], keep[3];
        pc_ui_picker_field_color(mode, axes[2], x, y, rgb);
        pc_ui_picker_keep model = PC_UI_PICKER_NOTHING;
        if (selected < 3) {
            model = PC_UI_PICKER_HSV;
            keep[0] = selected == 0 ? axes[2] * 360 : x * 360;
            keep[1] = selected == 0 ? x : selected == 1 ? axes[2] : 1 - y;
            keep[2] = selected == 2 ? axes[2] : 1 - y;
        } else if (pc_ui_picker_field_lab(mode, axes[2], x, y, keep)) {
            model = PC_UI_PICKER_LAB;
        }
        (void)pc_ui_picker_set_rgb(dialog->fields, fields, rgb, model, keep, NULL);
    }
    nk_layout_row_push(ctx, 24);
    struct nk_rect slider;
    (void)nk_widget(&slider, ctx);
    for (unsigned y = 0; y < 36; y++) {
        float rgb0[3], rgb1[3];
        pc_ui_picker_slider_color(mode, 1 - (float)y / 36, &c, rgb0);
        pc_ui_picker_slider_color(mode, 1 - (float)(y + 1) / 36, &c, rgb1);
        nk_fill_rect_multi_color(canvas,
                                 nk_rect(slider.x, floorf(slider.y + (float)y * slider.h / 36), slider.w,
                                         ceilf(slider.y + (float)(y + 1) * slider.h / 36) -
                                             floorf(slider.y + (float)y * slider.h / 36)),
                                 picker_color(rgb0), picker_color(rgb0), picker_color(rgb1),
                                 picker_color(rgb1));
    }
    float sy = slider.y + (1 - axes[2]) * slider.h;
    nk_stroke_line(canvas, slider.x - 3, sy, slider.x + slider.w + 3, sy, 2, ctx->style.text.color);
    if (picker_drag(ctx, slider)) {
        float z = 1 - fminf(1, fmaxf(0, (ctx->input.mouse.pos.y - slider.y) / slider.h));
        float rgb[3];
        pc_ui_picker_slider_color(mode, z, &c, rgb);
        pc_ui_picker_keep model = PC_UI_PICKER_NOTHING;
        float *keep = NULL;
        if (selected < 3) {
            c.hsv[selected] = selected ? z : z * 360;
            pc_ui_picker_hsv_to_rgb(c.hsv, rgb);
            keep = c.hsv;
            model = PC_UI_PICKER_HSV;
        } else if (selected >= 6) {
            size_t axis = selected - 6;
            c.lab[axis] = axis == 0 ? z * 100 : z * 255 - 128;
            keep = c.lab;
            model = PC_UI_PICKER_LAB;
        }
        (void)pc_ui_picker_set_rgb(dialog->fields, fields, rgb, model, keep, NULL);
    }
    nk_layout_row_push(ctx, 80);
    struct nk_rect chips;
    (void)nk_widget(&chips, ctx);
    float original[3];
    const char *old = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__orig"));
    if (!old || !pc_ui_picker_parse_hex(old, strlen(old), original)) {
        memcpy(original, c.rgb, sizeof original);
    }
    struct nk_rect now_rect = nk_rect(chips.x + 8, chips.y + 20, 48, 34),
                   old_rect = nk_rect(chips.x + 8, chips.y + 54, 48, 34);
    nk_fill_rect(canvas, now_rect, 0, picker_color(c.rgb));
    nk_fill_rect(canvas, old_rect, 0, picker_color(original));
    if (picker_drag(ctx, old_rect)) {
        (void)pc_ui_picker_set_rgb(dialog->fields, fields, original, PC_UI_PICKER_NOTHING, NULL, NULL);
    }
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_bool web_only = web;
    if (nk_checkbox_label(ctx, pc_i18n_t("Only Web Colors"), &web_only)) {
        (void)put(dialog->fields, fields, "__webOnly", yyjson_mut_bool(dialog->fields, web_only));
        (void)pc_ui_picker_set_rgb(dialog->fields, fields, c.rgb, PC_UI_PICKER_NOTHING, NULL, NULL);
    }
    nk_layout_row_dynamic(ctx, 22, 2);
    picker_component(ctx, dialog, "H", PC_UI_PICKER_HSV, 0, 0, 360);
    picker_component(ctx, dialog, "L", PC_UI_PICKER_LAB, 0, 0, 100);
    picker_component(ctx, dialog, "S %", PC_UI_PICKER_HSV, 1, 0, 100);
    picker_component(ctx, dialog, "a", PC_UI_PICKER_LAB, 1, -128, 127);
    picker_component(ctx, dialog, "B %", PC_UI_PICKER_HSV, 2, 0, 100);
    picker_component(ctx, dialog, "b", PC_UI_PICKER_LAB, 2, -128, 127);
    picker_component(ctx, dialog, "R", PC_UI_PICKER_NOTHING, 0, 0, 255);
    picker_component(ctx, dialog, "C %", PC_UI_PICKER_CMYK, 0, 0, 100);
    picker_component(ctx, dialog, "G", PC_UI_PICKER_NOTHING, 1, 0, 255);
    picker_component(ctx, dialog, "M %", PC_UI_PICKER_CMYK, 1, 0, 100);
    picker_component(ctx, dialog, "B", PC_UI_PICKER_NOTHING, 2, 0, 255);
    picker_component(ctx, dialog, "Y %", PC_UI_PICKER_CMYK, 2, 0, 100);
    nk_spacing(ctx, 1);
    picker_component(ctx, dialog, "K %", PC_UI_PICKER_CMYK, 3, 0, 100);
    const char *hex = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "color"));
    if (!hex) {
        hex = "#000000";
    }
    pc_ui_dialog_text *buffer = pc_ui_dialog_text_buffer(r, dialog->id, "pickerHex", hex);
    nk_layout_row_dynamic(ctx, 24, 2);
    nk_label(ctx, pc_i18n_t("Hex"), NK_TEXT_LEFT);
    if (buffer) {
        nk_flags edited = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, (char *)buffer->text.data,
                                                         (int)buffer->text.cap, nk_filter_default);
        buffer->text.len = strlen((char *)buffer->text.data);
        buffer->active = (edited & NK_EDIT_ACTIVE) != 0;
        float rgb[3];
        if (pc_ui_picker_parse_hex(pc_buf_cstr(&buffer->text), buffer->text.len, rgb) &&
            strcmp(pc_buf_cstr(&buffer->text), hex)) {
            (void)pc_ui_picker_set_rgb(dialog->fields, fields, rgb, PC_UI_PICKER_NOTHING, NULL, NULL);
        }
    } else {
        nk_label(ctx, pc_i18n_t(""), NK_TEXT_LEFT);
    }
    float near[3];
    nk_layout_row_dynamic(ctx, 24, 2);
    if (pc_ui_picker_out_of_gamut(c.rgb, near)) {
        if (nk_button_label(ctx, pc_i18n_t("Use printable color"))) {
            (void)pc_ui_picker_set_rgb(dialog->fields, fields, near, PC_UI_PICKER_NOTHING, NULL, NULL);
        }
    } else {
        nk_spacing(ctx, 1);
    }
    if (pc_ui_picker_not_web_safe(c.rgb, near)) {
        if (nk_button_label(ctx, pc_i18n_t("Use web color"))) {
            (void)pc_ui_picker_set_rgb(dialog->fields, fields, near, PC_UI_PICKER_NOTHING, NULL, NULL);
        }
    } else {
        nk_spacing(ctx, 1);
    }
}
void pc_ui_shell_dialogs_draw(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, float width, float height)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    r->frame++;
    if (r->file_reply && atomic_load_explicit(&r->file_reply->ready, memory_order_acquire)) {
        struct pc_ui_file_reply *reply = r->file_reply;
        pc_ui_dialog *target = pc_ui_state_dialog_mut(ui, reply->dialog);
        if (target && reply->path) {
            yyjson_mut_val *root = yyjson_mut_doc_get_root(target->fields);
            if (put(target->fields, root, "path", yyjson_mut_strcpy(target->fields, reply->path))) {
                pc_json_value result = PC_JSON_VALUE_INIT;
                pc_error error;
                pc_status status = confirm(app, reply->dialog, false, &result, &error);
                if (status != PC_OK) {
                    (void)snprintf(r->error, sizeof r->error, "%s", error.msg);
                }
                pc_json_value_fini(&result);
            } else {
                (void)snprintf(r->error, sizeof r->error, "Cannot allocate file path");
            }
        } else if (target && reply->error[0]) {
            (void)snprintf(r->error, sizeof r->error, "%s", reply->error);
        } else if (target) {
            pc_ui_state_close_dialog(ui, reply->dialog);
        }
        r->file_reply = NULL;
        file_reply_release(reply);
    }
    r->laid_out = false;
    r->graph_blocked = false;
    memset(r->apply, 0, sizeof r->apply);
    for (size_t i = r->ntexts; i-- > 0;) {
        if (!pc_ui_state_dialog_mut(ui, r->texts[i].id)) {
            pc_buf_fini(&r->texts[i].text);
            size_t last = --r->ntexts;
            if (i != last) {
                r->texts[i] = r->texts[last];
            }
            r->texts[last] = (pc_ui_dialog_text){0};
        }
    }
    if (!ui->n_dialogs) {
        r->error[0] = 0;
        r->confirm_requested = r->cancel_requested = false;
        return;
    }
    pc_ui_dialog *dialog = &ui->dialogs[ui->n_dialogs - 1];
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    bool is_discard = pc_ui_shell_is_discard(yyjson_mut_doc_get_root(dialog->fields));
    const char *discard_request =
        yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_doc_get_root(dialog->fields), "__request"));
    bool is_revert = is_discard && discard_request && !strcmp(discard_request, "file.revert");
    bool is_preferences = pc_ui_shell_is_preferences(yyjson_mut_doc_get_root(dialog->fields));
    bool is_picker =
        yyjson_mut_is_str(yyjson_mut_obj_get(yyjson_mut_doc_get_root(dialog->fields), "__colorPicker"));
    bool is_filter =
        yyjson_mut_get_bool(yyjson_mut_obj_get(yyjson_mut_doc_get_root(dialog->fields), "__filter"));
    bool is_style =
        yyjson_mut_get_bool(yyjson_mut_obj_get(yyjson_mut_doc_get_root(dialog->fields), "__style"));
    bool is_form = yyjson_mut_get_bool(yyjson_mut_obj_get(yyjson_mut_doc_get_root(dialog->fields), "__form"));
    bool is_variables =
        yyjson_mut_get_bool(yyjson_mut_obj_get(yyjson_mut_doc_get_root(dialog->fields), "__variables"));
    bool is_gallery =
        yyjson_mut_get_bool(yyjson_mut_obj_get(yyjson_mut_doc_get_root(dialog->fields), "__gallery"));
    bool is_camera = !strcmp(
        yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_doc_get_root(dialog->fields), "__command"))
            ? yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_doc_get_root(dialog->fields), "__command"))
            : "",
        "filter.cameraRaw");
    float w = fminf(dialog->kind == PC_DIALOG_ABOUT          ? 980
                    : is_form                                ? 700
                    : is_variables                           ? 760
                    : is_camera                              ? 1000
                    : is_gallery                             ? 1100
                    : is_style                               ? 820
                    : is_filter                              ? 460
                    : is_picker                              ? 560
                    : is_preferences                         ? 820
                    : dialog->kind == PC_DIALOG_NEW_DOCUMENT ? 960
                                                             : 440,
                    width - 24),
          h = fminf(dialog->kind == PC_DIALOG_ABOUT ? 650
                    : is_form                       ? 720
                    : is_variables                  ? 650
                    : is_camera                     ? 720
                    : is_gallery                    ? 700
                    : is_style                      ? 650
                    : is_filter
                        ? fminf(650, 192 + 30 * (float)yyjson_mut_get_uint(yyjson_mut_obj_get(
                                                    yyjson_mut_doc_get_root(dialog->fields), "__rows")))
                    : is_picker                                                ? 560
                    : dialog->kind == PC_DIALOG_NEW_DOCUMENT || is_preferences ? 650
                                                                               : 250,
                    height - 24);
    if (w < 100 || h < 100) {
        return;
    }
    const char *title = dialog->kind == PC_DIALOG_NEW_DOCUMENT ? "New Document"
                        : dialog->kind == PC_DIALOG_ABOUT      ? "About PhotoCraft"
                                                               : "PhotoCraft";
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    const char *label = yyjson_mut_get_str(yyjson_mut_obj_get(root, "__label"));
    if (label) {
        title = label;
    }
    char name[64];
    (void)snprintf(name, sizeof name, "dialog-%llu", (unsigned long long)dialog->id);
    bool accepted = false, cancelled = false, applied = false;
    uint64_t id = dialog->id;
    if (nk_begin_titled(
            ctx, name, pc_i18n_t(title),
            nk_rect(is_filter ? fmaxf(12, width - w - 24) : (width - w) / 2, (height - h) / 2, w, h),
            NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_NO_SCROLLBAR)) {
        struct nk_rect rect = nk_window_get_bounds(ctx);
        r->rect[0] = rect.x;
        r->rect[1] = rect.y;
        r->rect[2] = rect.w;
        r->rect[3] = rect.h;
        r->laid_out = true;
        struct nk_rect content = nk_window_get_content_region(ctx);
        nk_layout_row_dynamic(ctx, fmaxf(40, content.h - (r->error[0] ? 78 : 40)), 1);
        if (nk_group_begin(ctx, "dialog-body", 0)) {
            if (yyjson_mut_get_bool(yyjson_mut_obj_get(root, "__rasterize"))) {
                pc_ui_shell_rasterize_body(app, dialog);
            } else if (is_discard) {
                const char *message = "Save changes before closing?";
                const char *request = yyjson_mut_get_str(yyjson_mut_obj_get(root, "__request"));
                if (request && !strcmp(request, "file.revert")) {
                    message = "Revert to the saved version? Your changes will be lost.";
                }
                pc_doc_id document =
                    yyjson_mut_get_uint(yyjson_mut_arr_get_first(yyjson_mut_obj_get(root, "__pending")));
                size_t count = 0;
                const pc_engine_doc_state *docs =
                    pc_engine_session_documents(pc_ui_shell_app_session(app), &count);
                nk_layout_row_dynamic(ctx, 28, 1);
                for (size_t i = 0; i < count; i++) {
                    if (pc_doc_arc_get(docs[i].doc)->id == document) {
                        nk_label(ctx, pc_str_c(&pc_doc_arc_get(docs[i].doc)->name), NK_TEXT_LEFT);
                        break;
                    }
                }
                nk_layout_row_dynamic(ctx, 60, 1);
                nk_label_wrap(ctx, message);
            } else if (dialog->kind == PC_DIALOG_NEW_DOCUMENT) {
                new_document_body(app, ctx, r, dialog);
            } else if (is_preferences) {
                pc_ui_shell_preferences_draw(app, r, dialog);
            } else if (yyjson_mut_get_bool(yyjson_mut_obj_get(root, "__fileDialog"))) {
                text_field(ctx, r, dialog, "path", "File path");
                nk_layout_row_dynamic(ctx, 28, 1);
                bool choose =
                    nk_button_label(ctx, r->file_reply ? "Waiting for file chooser..." : "Browse...");
                bool automatic = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "__choose"));
                if (automatic) {
                    (void)put(dialog->fields, root, "__choose", yyjson_mut_bool(dialog->fields, false));
                }
                if ((choose || automatic) && !r->file_reply) {
                    choose_file(app, r, dialog);
                }
            } else if (is_form) {
                pc_ui_shell_form_body(app, r, dialog);
                if (yyjson_mut_get_bool(yyjson_mut_obj_get(root, "__export"))) {
                    text_field(ctx, r, dialog, "path", "File path");
                    nk_layout_row_dynamic(ctx, 26, 1);
                    if (nk_button_label(ctx, pc_i18n_t("Browse…")))
                        choose_file(app, r, dialog);
                }
            } else if (is_variables) {
                pc_ui_shell_variables_body(app, r, dialog);
            } else if (is_style) {
                pc_ui_shell_style_body(app, r, dialog);
            } else if (is_filter) {
                pc_ui_shell_filter_body(app, r, dialog);
            } else if (yyjson_mut_get_bool(yyjson_mut_obj_get(root, "__workspace"))) {
                pc_ui_shell_workspace_body(app, r, dialog);
            } else if (dialog->kind == PC_DIALOG_ABOUT) {
                if (yyjson_mut_get_bool(yyjson_mut_obj_get(root, "systemInfo")))
                    pc_ui_shell_system_info_body(app, r, dialog);
                else
                    pc_ui_shell_credits_body(app, r, dialog);
            } else if (yyjson_mut_is_str(yyjson_mut_obj_get(root, "__colorPicker"))) {
                picker_body(ctx, r, dialog);
            } else {
                nk_layout_row_dynamic(ctx, 40, 1);
                nk_label_wrap(ctx, pc_i18n_t("Apply this command to the current document?"));
            }
            nk_group_end(ctx);
        }
        if (r->error[0]) {
            nk_layout_row_dynamic(ctx, 34, 1);
            nk_label_wrap(ctx, r->error);
        }
        nk_layout_row_dynamic(ctx, 28, is_preferences || (is_discard && !is_revert) ? 3 : 2);
        struct nk_rect button = nk_widget_bounds(ctx);
        r->confirm[0] = button.x;
        r->confirm[1] = button.y;
        r->confirm[2] = button.w;
        r->confirm[3] = button.h;
        accepted = nk_button_label(
            ctx, pc_i18n_t(is_discard ? ((yyjson_mut_get_str(yyjson_mut_obj_get(root, "__request")) &&
                                          !strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(root, "__request")),
                                                  "file.revert"))
                                             ? "Revert"
                                             : "Save")
                           : dialog->kind == PC_DIALOG_NEW_DOCUMENT ? "Create"
                                                                    : "OK"));
        button = nk_widget_bounds(ctx);
        r->cancel[0] = button.x;
        r->cancel[1] = button.y;
        r->cancel[2] = button.w;
        r->cancel[3] = button.h;
        cancelled = nk_button_label(ctx, pc_i18n_t("Cancel"));
        if (is_preferences || (is_discard && !is_revert)) {
            button = nk_widget_bounds(ctx);
            r->apply[0] = button.x;
            r->apply[1] = button.y;
            r->apply[2] = button.w;
            r->apply[3] = button.h;
            applied = nk_button_label(ctx, pc_i18n_t(is_discard ? "Don't Save" : "Apply"));
        }
    }
    nk_end(ctx);
    if (applied && is_discard) {
        (void)put(dialog->fields, root, "choice", yyjson_mut_str(dialog->fields, "discard"));
        accepted = true;
        applied = false;
    }
    /* Replacing an object field leaves the old value in yyjson's arena. Compact periodically
     * so prolonged typing and picker drags have bounded retained storage. */
    if (++r->compact >= 64) {
        r->compact = 0;
        yyjson_mut_doc *copy = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *value =
            copy ? yyjson_mut_val_mut_copy(copy, yyjson_mut_doc_get_root(dialog->fields)) : NULL;
        if (value) {
            yyjson_mut_doc_set_root(copy, value);
            yyjson_mut_doc_free(dialog->fields);
            dialog->fields = copy;
        } else {
            yyjson_mut_doc_free(copy);
        }
    }
    accepted = accepted || r->confirm_requested;
    cancelled = cancelled || r->cancel_requested;
    r->confirm_requested = r->cancel_requested = false;
    if (applied && !accepted && !cancelled) {
        pc_json_value result = PC_JSON_VALUE_INIT;
        pc_error error;
        pc_status status = pc_ui_shell_preferences_commit(app, id, false, &result, &error);
        if (status != PC_OK) {
            (void)snprintf(r->error, sizeof r->error, "%s", error.msg);
        } else {
            r->error[0] = 0;
        }
        pc_json_value_fini(&result);
    }
    if (accepted || cancelled) {
        pc_json_value out = PC_JSON_VALUE_INIT;
        pc_error err;
        pc_status rc = accepted && !cancelled ? confirm(app, id, false, &out, &err)
                                              : (pc_ui_state_close_dialog(ui, id) ? PC_OK : PC_ERR_NOT_FOUND);
        if (rc != PC_OK) {
            (void)snprintf(r->error, sizeof r->error, "%s", err.msg);
            free(ui->status);
            ui->status = pc_strndup(err.msg, strlen(err.msg));
            ui->status_error = true;
        } else {
            r->error[0] = 0;
        }
        pc_json_value_fini(&out);
    }
}

pc_doc_arc *pc_ui_shell_dialog_preview(pc_ui_shell_app *app, uint64_t *generation)
{
    pc_ui_dialog_runtime *r = pc_ui_shell_dialog_runtime(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    yyjson_mut_val *fields =
        ui->n_dialogs ? yyjson_mut_doc_get_root(ui->dialogs[ui->n_dialogs - 1].fields) : NULL;
    yyjson_mut_doc *color_preview = NULL;
    const char *picker = fields ? yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__colorPicker")) : NULL;
    pc_error error = {0};
    pc_status status = PC_OK;
    if (picker && !strcmp(picker, "dialogField")) {
        status = pc_ui_shell_dialog_color_field_preview(app, fields, &color_preview, &error);
        fields = status == PC_OK ? yyjson_mut_doc_get_root(color_preview) : NULL;
    }
    if (status == PC_OK)
        status = pc_ui_filter_preview_update(&r->preview, pc_ui_shell_app_session(app), fields, &error);
    else
        (void)pc_ui_filter_preview_update(&r->preview, pc_ui_shell_app_session(app), NULL, NULL);
    yyjson_mut_doc_free(color_preview);
    if (status != PC_OK)
        return NULL;
    *generation = r->preview.generation;
    return r->preview.result;
}
