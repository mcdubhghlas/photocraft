#ifndef PC_UI_SHELL_TYPE_PANELS_UI_H
#define PC_UI_SHELL_TYPE_PANELS_UI_H
#include "shell_internal.h"
typedef struct pc_ui_type_panels_runtime {
    bool shown[4];
    pc_doc_id spell_document, options_document;
    uint32_t selected[2], options_id;
    bool options_paragraph;
    char name[256], glyph_family[256], glyph_style[256], replacement[1024];
    pc_str charmap_family, charmap_style;
    uint32_t *charmap, *filtered;
    size_t ncharmap, nfiltered;
    pc_str category;
} pc_ui_type_panels_runtime;
void pc_ui_type_panels_runtime_fini(pc_ui_type_panels_runtime *runtime);
pc_ui_type_panels_runtime *pc_ui_shell_type_panels_runtime(pc_ui_shell_app *app);
pc_status pc_ui_shell_type_panels_menu(pc_ui_shell_app *app, const char *id, yyjson_val *params,
                                       pc_json_value *out, pc_error *err);
void pc_ui_shell_type_panels_draw(pc_ui_shell_app *app);

pc_status pc_ui_shell_type_style_color(pc_ui_shell_app *app, uint32_t id, pc_json_value *out, pc_error *err);
#endif
