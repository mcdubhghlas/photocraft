/* Slices image resource, id 1050 (port of crates/psd/src/slices.rs).
 *
 * Version 6 is a fixed binary layout optionally followed by a versioned descriptor with extra
 * per-slice data (layer-slice outsets); versions 7 and 8 are one versioned descriptor. Writing
 * produces version 6 (or the version 7 descriptor form on request). Unknown fields are not
 * kept: callers that need a byte-exact round trip keep the raw resource. Thread-safe. */
#ifndef PC_PSD_SLICES_H
#define PC_PSD_SLICES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/types.h"

typedef struct pc_psd_slice {
    uint32_t id;
    uint32_t group_id;
    uint32_t origin; /* 0 auto-generated, 1 layer-based, 2 user-drawn */
    bool has_layer_id;
    uint32_t layer_id; /* layer-based slices: the layer's lyid */
    char *name;        /* owned UTF-8 strings (never NULL after parse/init) */
    uint32_t kind;     /* 0 no image, 1 image, 2 table */
    int32_t rect[4];   /* left, top, right, bottom */
    char *url;
    char *target;
    char *message;
    char *alt;
    bool cell_text_is_html;
    char *cell_text;
    uint32_t horizontal_align;
    uint32_t vertical_align;
    uint8_t color[4];   /* alpha, red, green, blue */
    int32_t outsets[4]; /* top, left, bottom, right */
} pc_psd_slice;

typedef struct pc_psd_slices {
    uint32_t version;  /* read: 6, 7 or 8 */
    int32_t bounds[4]; /* top, left, bottom, right */
    char *group_name;  /* owned */
    pc_psd_slice *slices;
    size_t n;
} pc_psd_slices;

/* A slice with empty strings and zeroes. */
pc_status pc_psd_slice_init(pc_psd_slice *s, pc_error *err);
void pc_psd_slice_fini(pc_psd_slice *s);
void pc_psd_slices_fini(pc_psd_slices *s);
bool pc_psd_slices_eq(const pc_psd_slices *a, const pc_psd_slices *b);

pc_status pc_psd_slices_parse(const uint8_t *data, size_t len, pc_psd_slices *out, pc_error *err);
/* Version 6, with a trailing outsets descriptor when any slice has outsets. */
pc_status pc_psd_slices_to_bytes(const pc_psd_slices *s, pc_psd_bytes *out, pc_error *err);
/* Version 7 descriptor form. */
pc_status pc_psd_slices_to_descriptor_bytes(const pc_psd_slices *s, pc_psd_bytes *out, pc_error *err);

#endif
