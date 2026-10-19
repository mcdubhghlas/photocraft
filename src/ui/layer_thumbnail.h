#ifndef PC_UI_LAYER_THUMBNAIL_H
#define PC_UI_LAYER_THUMBNAIL_H
#include "doc/doc.h"
typedef enum pc_ui_layer_thumbnail_kind {
    PC_UI_THUMB_LAYER,
    PC_UI_THUMB_PIXEL_MASK,
    PC_UI_THUMB_VECTOR_MASK
} pc_ui_layer_thumbnail_kind;
/* Bounded point-sampled display image, letterboxed to document aspect. Samples honor the
 * surface's native depth/model and the document CMYK profile. Masks display their stored
 * shape even while disabled; enabled/target/link indicators belong to the row UI. */
pc_status pc_ui_layer_thumbnail(const pc_document *doc, const pc_layer *layer,
                                pc_ui_layer_thumbnail_kind kind, uint32_t side, pc_rgba8_image *out,
                                pc_error *err);
#endif
