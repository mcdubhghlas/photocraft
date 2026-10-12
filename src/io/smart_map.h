/* Smart objects and smart filters <-> PSD placed-layer data (port of crates/io/src/smart_map.rs:
 * smart_map.c and smart_filters.c).
 *
 * Layouts follow the Adobe PSD spec ("Placed Layer", "Placed Layer Data") and Photoshop-authored
 * files: a soLD descriptor holds the linked-file id (Idnt, the uuid of the lnk2 item), the
 * instance id (placed, which keys the FEid filter cache), the transform quad (Trnf), the source
 * size (Sz), the warp and, when filters are applied, a filterFX object (filterFXStyle: enab,
 * validAtPosition, filterMaskEnable, filterMaskLinked, filterMaskExtendWithWhite, filterFXList of
 * filterFX items: Nm, blendOptions {Opct, Md}, enab, hasoptions, FrgC, BckC, Fltr, filterID).
 * The list is in application order (first = bottom). Filters PhotoCraft implements map to their
 * command id and parameters; any other filter becomes a PC_IO_UNSUPPORTED_FILTER entry that
 * keeps its descriptor verbatim (hex in params.psd) and is written back unchanged.
 *
 * Errors: descriptor problems in the reference's Option results are *found false; its
 * Result<_, String> errors are PC_ERR_UNSUPPORTED (item_for_filter) or PC_ERR_FORMAT
 * (mask_from_item) with the reference's text as the whole message. Out of memory is
 * PC_ERR_NOMEM. Thread-safe (no shared state). */
#ifndef PC_IO_SMART_MAP_H
#define PC_IO_SMART_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "doc/doc.h"
#include "io/core.h"
#include "psd/descriptor.h"
#include "psd/filter_effects.h"

/* The command id of a smart filter PhotoCraft can't run (kept for the round trip). */
#define PC_IO_UNSUPPORTED_FILTER "psd.unsupportedFilter"

/* The smart filter stack of a placed layer, as stored in filterFX. */
typedef struct pc_io_filter_stack {
    bool enabled; /* enab */
    pc_smart_filter *filters;
    size_t nfilters;
    bool mask_enabled; /* filterMaskEnable */
    bool mask_linked;  /* filterMaskLinked */
} pc_io_filter_stack;

/* The reference's Default: enabled, no filters, mask enabled, not linked. */
static inline void pc_io_filter_stack_init(pc_io_filter_stack *s)
{
    s->enabled = true;
    s->filters = NULL;
    s->nfilters = 0;
    s->mask_enabled = true;
    s->mask_linked = false;
}

static inline void pc_io_filter_stack_fini(pc_io_filter_stack *s)
{
    for (size_t i = 0; i < s->nfilters; i++) {
        pc_smart_filter_fini(&s->filters[i]);
    }
    free(s->filters);
    pc_io_filter_stack_init(s);
}

/* What a soLD descriptor says about a smart object. */
typedef struct pc_io_placed {
    pc_psd_desc descriptor;
    pc_str idnt;   /* uuid of the embedded or linked file (lnk2 item) */
    pc_str placed; /* id of this instance (keys the FEid filter cache) */
    pc_affine transform;
    bool has_stack; /* filterFX, if any filters were ever applied */
    pc_io_filter_stack stack;
} pc_io_placed;

void pc_io_placed_fini(pc_io_placed *p);
/* SoLd / SoLE block data; *found false when it is not a soLD structure. */
pc_status pc_io_parse_sold(const uint8_t *data, size_t len, bool *found, pc_io_placed *out, pc_error *err);
/* The filter mask of a placed layer from its FEid item; *has false when there is none. A bad
 * item is PC_ERR_FORMAT with the reference's message. */
pc_status pc_io_mask_from_item(const pc_psd_fx_item *item, pc_sample_type sample,
                               const pc_io_filter_stack *stack, bool *has, pc_layer_mask *out, pc_error *err);
/* One filterFX list item as a smart filter (unknown filters as PC_IO_UNSUPPORTED_FILTER). */
pc_status pc_io_filter_from_item(const pc_psd_desc *item, pc_smart_filter *out, pc_error *err);
/* A smart filter as a filterFX list item; PC_ERR_UNSUPPORTED with the reason when it has none. */
pc_status pc_io_item_for_filter(const pc_smart_filter *f, pc_psd_desc *out, pc_error *err);

/* ---- the rest of smart_map.rs (I3 and its tests) ---- */

bool pc_io_filter_stack_eq(const pc_io_filter_stack *a, const pc_io_filter_stack *b);
pc_status pc_io_filter_stack_clone(const pc_io_filter_stack *s, pc_io_filter_stack *out, pc_error *err);
/* Reads a filterFX object (filter_stack). *out initialised on success. */
pc_status pc_io_filter_stack_read(const pc_psd_desc *fx, pc_io_filter_stack *out, pc_error *err);
/* Builds a filterFX object; filters that can't be written are left out with a warning each
 * ('layer "<layer>": smart filter <command> was not written to PSD: <reason>'). */
pc_status pc_io_filter_fx(const pc_io_filter_stack *stack, const char *layer, pc_io_warnings *warnings,
                          pc_psd_desc *out, pc_error *err);
/* The soLD descriptor of SoLd / SoLE block data; *found false when it isn't one. */
pc_status pc_io_sold_descriptor(const uint8_t *data, size_t len, bool *found, pc_psd_desc *out,
                                pc_error *err);

/* Everything needed to write the placed-layer blocks of a smart object (PlacedSpec). */
typedef struct pc_io_placed_spec {
    const char *idnt; /* NUL-terminated */
    const char *placed;
    pc_affine transform; /* source pixels -> document pixels */
    bool has_perspective;
    double perspective[9]; /* the full projective map, overriding transform */
    double size[2];        /* source size in pixels */
    double dpi;
    const pc_warp *warp;          /* NULL = none */
    const pc_psd_desc *filter_fx; /* NULL = no filters */
} pc_io_placed_spec;

/* Whether the placement moved from the quad a template descriptor stores (Trnf). */
bool pc_io_quad_moved(const pc_psd_desc *tmpl, const pc_io_placed_spec *s);
/* SoLd block data. With tmpl (the imported descriptor, may be NULL) every key it has is kept and
 * only the transform (when it moved), warp and filter stack are rewritten; otherwise a fresh
 * descriptor in Photoshop's key order. Warnings appended. *out replaced. */
pc_status pc_io_sold_bytes(const pc_psd_desc *tmpl, const pc_io_placed_spec *s, pc_io_warnings *warnings,
                           pc_buf *out, pc_error *err);
/* PlLd block data (plcL version 3). */
pc_status pc_io_plld_bytes(const pc_io_placed_spec *s, pc_io_warnings *warnings, pc_buf *out, pc_error *err);
/* The source size a soLD descriptor stores (Sz), both sides > 0; false otherwise. */
bool pc_io_stored_size(const pc_psd_desc *d, double out[2]);
/* An FEid item for a smart object with filters: the unfiltered placed pixels (colour slots from
 * 0, transparency in slot 25, in doc_fmt, CMYK inverted) over content | bounds, and the filter
 * mask over bounds (white when mask is NULL). *has false when there are no unfiltered pixels. */
pc_status pc_io_feid_item(const char *placed, const pc_surface *unfiltered, const pc_layer_mask *mask,
                          pc_rect bounds, pc_pixel_format doc_fmt, bool *has, pc_psd_fx_item *out,
                          pc_error *err);
pc_status pc_io_fx_item_clone(const pc_psd_fx_item *it, pc_psd_fx_item *out, pc_error *err);
/* A uuid-shaped id (8-4-4-4-12 hex digits) from the BLAKE3 hash of seed. */
void pc_io_uuid_from(const void *seed, size_t len, char out[37]);

#endif
