/* Layer comps and artboards <-> PSD resource 1065 and the artb / artd / abdd and cmls blocks
 * (chunk I4: crates/io/src/comps_map.rs). The reference's Option results are *found / *has flags
 * (out-params written only when set); pc_buf outputs are replaced on success. Statuses other
 * than PC_OK are out of memory (or I2's blocks.h colour helpers failing). Thread-safe. */
#ifndef PC_IO_COMPS_MAP_H
#define PC_IO_COMPS_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "doc/doc.h"
#include "io/core.h"

#define PC_IO_LAYER_COMPS 1065
/* Per-layer artboard block keys, preferred first: artb, artd, abdd. */
extern const char PC_IO_ARTBOARD_KEYS[3][5];

/* An artboard block; *found false when unreadable. */
pc_status pc_io_parse_artboard(const uint8_t *data, size_t len, bool *found, pc_artboard *out, pc_error *err);
/* Appends the artboard block of a top-level artboard group, listing the guides inside it. */
pc_status pc_io_artboard_block(const pc_guides *guides, const pc_layer *l, pc_io_raw_blocks *raw,
                               pc_error *err);

/* Encodes an artb block (padded to even); guides are indices into the document's guide list
 * (resource 1032) that belong to the board. */
pc_status pc_io_write_artboard(const pc_artboard *a, const int32_t *guides, size_t n, pc_buf *out,
                               pc_error *err);
/* Indices of the guides (vertical first, then horizontal, as in resource 1032) strictly inside r
 * (*out malloc'd, free()). */
pc_status pc_io_guides_in(const pc_guides *g, pc_rect r, int32_t **out, size_t *n, pc_error *err);

/* Layer comps read from resource 1065. */
typedef struct pc_io_comps {
    pc_layer_comp *comps;
    size_t n;
    bool has_last_applied;
    uint32_t last_applied;
    bool has_last_state;
    pc_layer_comp last_state;
} pc_io_comps;

void pc_io_comps_fini(pc_io_comps *c);
/* Resource 1065 (res NULL = absent) against doc's layers' cmls items; notes (comp offsets that
 * don't fit an i32) appended to warnings (may be NULL). comps_from_psd is this without notes. */
pc_status pc_io_decode_comps(const uint8_t *res, size_t len, const pc_document *doc, pc_io_comps *out,
                             pc_io_warnings *warnings, pc_error *err);
/* Resource 1065 for doc's comps; *has false when it has none. */
pc_status pc_io_write_comps_resource(const pc_document *doc, bool *has, pc_buf *out, pc_error *err);
/* A layer's cmls block for the comps (last = the last document state, may be NULL); *has false
 * when no comp mentions the layer. */
pc_status pc_io_write_cmls(const pc_layer_comp *comps, size_t n, const pc_layer_comp *last, const pc_layer *l,
                           uint32_t psd_id, bool *has, pc_buf *out, pc_error *err);
/* The preserved comps data still matches the document. */
pc_status pc_io_comps_unchanged(const pc_document *doc, bool *out, pc_error *err);
/* Replaces (or, with cmls / *cmls NULL, removes) the cmls item of the shmd block in raw, keeping
 * the other items; *cmls is taken over (freed, set to NULL) in every case. */
pc_status pc_io_set_cmls(pc_io_raw_blocks *raw, uint8_t **cmls, size_t len, pc_error *err);

#endif
