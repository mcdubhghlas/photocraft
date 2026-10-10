/* Web slices (port of crates/doc/src/slices.rs): stored user and layer-based slices, the
 * auto-slice partition and Photoshop-style numbering. Conventions: doc/adjust.h. */
#ifndef PC_DOC_SLICES_H
#define PC_DOC_SLICES_H

#include "doc/adjust.h"
#include "geom/geom.h"

typedef uint64_t pc_layer_id;
typedef struct pc_document pc_document;

typedef enum pc_slice_origin { PC_SLICE_AUTO, PC_SLICE_LAYER, PC_SLICE_USER } pc_slice_origin; /* default USER */

uint32_t pc_slice_origin_code(pc_slice_origin o);   /* PSD 1050: 0 auto, 1 layer, 2 user */
pc_slice_origin pc_slice_origin_from_code(uint32_t c); /* unknown -> user */
const char *pc_slice_origin_id(pc_slice_origin o);  /* "auto", "layer", "user" */

typedef enum pc_slice_kind { PC_SLICE_IMAGE, PC_SLICE_NO_IMAGE, PC_SLICE_TABLE } pc_slice_kind; /* default IMAGE */

uint32_t pc_slice_kind_code(pc_slice_kind k); /* 0 no image, 1 image, 2 table */
pc_slice_kind pc_slice_kind_from_code(uint32_t c);
const char *pc_slice_kind_id(pc_slice_kind k); /* "image", "noImage", "table" */
bool pc_slice_kind_from_id(const char *s, size_t len, pc_slice_kind *out);

typedef struct pc_slice {
    uint32_t id;
    uint32_t group_id;
    pc_slice_origin origin;
    bool has_layer;
    pc_layer_id layer;
    pc_str name;
    pc_rect rect;
    pc_slice_kind kind;
    pc_str url;
    pc_str target;
    pc_str message;
    pc_str alt;
    bool cell_text_is_html;
    pc_str cell_text;
    uint32_t horizontal_align;
    uint32_t vertical_align;
    bool has_background;
    uint8_t background[4]; /* ARGB */
    int32_t outsets[4];    /* top, left, bottom, right */
} pc_slice;

void pc_slice_default(pc_slice *out); /* all zero/empty, origin USER, kind IMAGE */
void pc_slice_fini(pc_slice *s);
pc_status pc_slice_clone(const pc_slice *s, pc_slice *out, pc_error *err);
bool pc_slice_eq(const pc_slice *a, const pc_slice *b);

typedef struct pc_slices {
    pc_str group_name;
    pc_slice *list; /* creation order; later slices are on top */
    size_t len;
} pc_slices;

void pc_slices_fini(pc_slices *s);
pc_status pc_slices_clone(const pc_slices *s, pc_slices *out, pc_error *err);
bool pc_slices_eq(const pc_slices *a, const pc_slices *b);
bool pc_slices_is_empty(const pc_slices *s);
bool pc_slices_next_id(const pc_slices *s, uint32_t *out); /* false when u32 is exhausted */
const pc_slice *pc_slices_get(const pc_slices *s, uint32_t id);
pc_slice *pc_slices_get_mut(pc_slices *s, uint32_t id);

typedef struct pc_resolved_slice {
    size_t number; /* 1-based display number */
    bool has_id;   /* stored slice id; false for auto slices */
    uint32_t id;
    pc_slice_origin origin;
    pc_rect rect;
    pc_str name;
    pc_slice_kind kind;
} pc_resolved_slice;

void pc_resolved_slices_free(pc_resolved_slice *s, size_t n);

/* Rectangles tiling canvas minus taken (malloc'd; free()). */
pc_status pc_auto_slices(pc_rect canvas, const pc_rect *taken, size_t ntaken, pc_rect **out, size_t *n,
                         pc_error *err);
/* Every slice of doc in display order. */
pc_status pc_slices_resolve(const pc_document *doc, pc_resolved_slice **out, size_t *n, pc_error *err);
/* The document name without extension, non-alphanumerics (Unicode) except - _ as '_';
 * "Untitled" when empty. */
pc_status pc_slices_base_name(const pc_document *doc, pc_str *out, pc_error *err);

#endif
