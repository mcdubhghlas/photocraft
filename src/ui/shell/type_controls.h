#ifndef PC_UI_SHELL_TYPE_CONTROLS_H
#define PC_UI_SHELL_TYPE_CONTROLS_H
#include "app.h"
typedef struct pc_ui_type_controls {
    pc_json_value families, faces;
    pc_str face_family;
    char search[256];
    bool editing_style, style_paragraph;
    uint32_t style_id;
    const pc_text_layer *override_text;
    pc_str filter_query;
    size_t *filtered, nfiltered;
} pc_ui_type_controls;
void pc_ui_type_controls_fini(pc_ui_type_controls *controls);
pc_ui_type_controls *pc_ui_shell_type_controls(pc_ui_shell_app *app);
void pc_ui_shell_type_options(pc_ui_shell_app *app);
void pc_ui_shell_type_panel(pc_ui_shell_app *app, bool paragraph);
pc_status pc_ui_shell_type_color(pc_ui_shell_app *app, pc_json_value *out, pc_error *err);
void pc_ui_shell_type_style_editor(pc_ui_shell_app *app, bool paragraph, uint32_t id,
                                   const pc_char_style *character, const pc_paragraph_style *para);
#endif
