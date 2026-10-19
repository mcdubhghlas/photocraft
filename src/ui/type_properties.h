#ifndef PC_UI_TYPE_PROPERTIES_H
#define PC_UI_TYPE_PROPERTIES_H
#include "ui/type_tool.h"
/* Representative character is the selection's first scalar, or the collapsed caret. */
const pc_char_style *pc_ui_type_representative(const pc_text_layer *text, size_t caret, size_t anchor);
float pc_ui_type_shown_scale(const pc_text_layer *text);
float pc_ui_type_unit_factor(const char *unit, float dpi);
#endif
