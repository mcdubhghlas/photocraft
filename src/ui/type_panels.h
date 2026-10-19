#ifndef PC_UI_TYPE_PANELS_H
#define PC_UI_TYPE_PANELS_H
#include "base/json.h"
#include "base/str.h"
#include "text/fonts.h"
typedef struct pc_ui_spell_dialog {
    pc_json_value items;
    size_t index;
    pc_strlist ignore;
    pc_str change_to, message;
    bool all_layers;
} pc_ui_spell_dialog;
typedef struct pc_ui_type_panels {
    bool styles, glyphs, has_options, options_paragraph;
    size_t styles_tab;
    uint32_t options_id;
    pc_str glyph_family, glyph_style, glyph_category;
    float glyph_size;
    pc_strlist glyph_recent;
    bool has_glyph_selected, has_spell;
    pc_str glyph_selected;
    pc_ui_spell_dialog spell;
} pc_ui_type_panels;
void pc_ui_type_panels_fini(pc_ui_type_panels *state);
pc_status pc_ui_type_panels_to_json(const pc_ui_type_panels *state, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                    pc_error *err);
pc_status pc_ui_type_panels_from_json(pc_ui_type_panels *state, yyjson_val *object, pc_error *err);
pc_status pc_ui_type_glyph_used(pc_ui_type_panels *state, const char *text, size_t length, pc_error *err);
#endif
