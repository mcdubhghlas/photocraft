#ifndef PC_UI_SHELL_DIALOGS_H
#define PC_UI_SHELL_DIALOGS_H
#include "app.h"
#include "ui/parameter_schema.h"
#include "ui/filter_preview.h"
#include "ui/point_curve.h"
#include "ui/credits.h"
#include "photocraft_nuklear.h"
typedef struct pc_ui_dialog_text {
    uint64_t id;
    char key[256];
    uint64_t touched;
    pc_buf text;
    bool active;
} pc_ui_dialog_text;
typedef struct pc_ui_dialog_runtime {
    pc_ui_credits credits;
    bool has_credits;
    pc_ui_point_curve curve;
    uint64_t curve_dialog;
    size_t tone_channel, adjustment_range, gradient_stop, style_kind, style_blend_channel;
    uint64_t style_dialog;
    bool gradient_dragging;
    uint8_t curve_hash[32];
    char curve_key[32];
    bool curve_valid, curve_focus, map_dragging, graph_blocked;
    float curve_map[256], map_base[256];
    double map_start[2], map_last[2];
    pc_ui_filter_preview preview;
    pc_ui_parameter_schema schema;
    uint64_t schema_dialog;
    pc_ui_dialog_text texts[32];
    size_t ntexts;
    unsigned compact;
    uint64_t frame;
    struct pc_ui_file_reply *file_reply;
    bool laid_out, confirm_requested, cancel_requested;
    float rect[4], confirm[4], cancel[4], apply[4];
    char error[256];
} pc_ui_dialog_runtime;
pc_ui_dialog_text *pc_ui_dialog_text_buffer(pc_ui_dialog_runtime *runtime, uint64_t id, const char *key,
                                            const char *value);
pc_doc_arc *pc_ui_shell_dialog_preview(pc_ui_shell_app *app, uint64_t *generation);
pc_ui_dialog_runtime *pc_ui_shell_dialog_runtime(pc_ui_shell_app *app);
void pc_ui_shell_dialogs_fini(pc_ui_dialog_runtime *runtime);
void pc_ui_shell_dialogs_draw(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, float width, float height);
pc_status pc_ui_shell_dialog_control(pc_ui_shell_app *app, const char *method, yyjson_val *params,
                                     pc_json_value *out, pc_error *err);
pc_status pc_ui_shell_dialog_file(pc_ui_shell_app *app, const char *command, pc_json_value *out,
                                  pc_error *err);
pc_status pc_ui_shell_dialog_new(pc_ui_shell_app *app, pc_json_value *out, pc_error *err);
pc_status pc_ui_shell_dialog_color_field(pc_ui_shell_app *app, yyjson_val *params, pc_json_value *out,
                                         pc_error *err);
pc_status pc_ui_shell_dialog_color_field_confirm(pc_ui_shell_app *app, uint64_t id, pc_json_value *out,
                                                 pc_error *err);
pc_status pc_ui_shell_dialog_color_field_preview(pc_ui_shell_app *app, const yyjson_mut_val *picker,
                                                 yyjson_mut_doc **out, pc_error *err);
pc_status pc_ui_shell_dialog_color_command(pc_ui_shell_app *app, const char *label, const float rgba[4],
                                           const char *command, const yyjson_mut_val *params,
                                           const char *field, pc_json_value *out, pc_error *err);
/* Consumes fields, including on failure. Only native editors may set internal metadata. */
pc_status pc_ui_shell_dialog_store(pc_ui_shell_app *app, pc_ui_dialog_kind kind, yyjson_mut_doc *fields,
                                   yyjson_mut_val *root, pc_json_value *out, pc_error *err);
#endif
