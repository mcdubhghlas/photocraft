#ifndef PC_UI_TYPE_STATE_H
#define PC_UI_TYPE_STATE_H
#include "base/json.h"
#include "base/str.h"
typedef struct pc_ui_text_edit {
    uint64_t layer;
    size_t caret, anchor;
    pc_str session;
    bool created, mask, dragging, has_resize, has_preedit;
    uint8_t resize;
    size_t preedit_start, preedit_length;
    /* Runtime guards are deliberately absent from persisted UI state. */
    uint64_t document, revision;
} pc_ui_text_edit;
void pc_ui_text_edit_fini(pc_ui_text_edit *edit);
pc_status pc_ui_text_edit_to_json(const pc_ui_text_edit *edit, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                  pc_error *err);
pc_status pc_ui_text_edit_from_json(pc_ui_text_edit *out, yyjson_val *object, pc_error *err);
#endif
