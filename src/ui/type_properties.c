#include "type_properties.h"
#include <math.h>
#include <string.h>
const pc_char_style *pc_ui_type_representative(const pc_text_layer *text, size_t caret, size_t anchor)
{
    if (!text || !text->nruns) {
        return NULL;
    }
    size_t at = pc_text_byte_index(pc_str_c(&text->text), text->text.len, caret < anchor ? caret : anchor),
           start = 0;
    for (size_t i = 0; i < text->nruns; i++) {
        size_t length = text->runs[i].len;
        if (length > text->text.len - start) {
            return &text->runs[i].style;
        }
        size_t end = start + length;
        if (at < end || i == text->nruns - 1) {
            return &text->runs[i].style;
        }
        start = end;
    }
    return NULL;
}
float pc_ui_type_shown_scale(const pc_text_layer *text)
{
    double scale = text ? sqrt(fabs(pc_affine_determinant(text->transform))) : 1;
    return isfinite(scale) && scale > .001 && scale < 100000 ? (float)scale : 1;
}
float pc_ui_type_unit_factor(const char *unit, float dpi)
{
    if (!isfinite(dpi) || dpi <= 0) {
        dpi = 72;
    }
    return unit && !strcmp(unit, "pixels") ? dpi / 72 : unit && !strcmp(unit, "mm") ? 25.4f / 72 : 1;
}
