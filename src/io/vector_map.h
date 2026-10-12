/* Paths, vector masks and shape layers <-> PSD path records and blocks (chunk I4:
 * crates/io/src/vector_map.rs).
 *
 * The reference's Option results are a *found flag (written on success; the other out-params
 * only when *found). Statuses other than PC_OK are out of memory (or, for the fills, I2's
 * blocks.h errors). pc_buf outputs are replaced on success. Thread-safe. */
#ifndef PC_IO_VECTOR_MAP_H
#define PC_IO_VECTOR_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "doc/doc.h"
#include "io/core.h"
#include "psd/misc.h"

/* Saved paths are resources 2000..=2997; the work path 1025; the clipping path name 2999. */
#define PC_IO_SAVED_PATHS_FIRST 2000
#define PC_IO_SAVED_PATHS_LAST 2997
#define PC_IO_WORK_PATH 1025
#define PC_IO_CLIPPING_PATH 2999

/* A document path as PSD path records for a w x h canvas. */
pc_status pc_io_path_to_records(const pc_path *p, uint32_t w, uint32_t h, pc_psd_path *out, pc_error *err);
/* A path resource; *found false when unreadable. */
pc_status pc_io_path_from_resource(const uint8_t *data, size_t len, uint32_t w, uint32_t h, bool *found,
                                   pc_path *out, pc_error *err);
/* vmsk / vsms block data -> path and flags; *found false when unreadable. */
pc_status pc_io_path_from_vmsk(const uint8_t *data, size_t len, uint32_t w, uint32_t h, bool *found,
                               pc_path *out, uint32_t *flags, pc_error *err);
pc_status pc_io_vector_mask_from_block(const uint8_t *data, size_t len, uint32_t w, uint32_t h, bool *found,
                                       pc_vector_mask *out, pc_error *err);
pc_status pc_io_vector_mask_bytes(const pc_vector_mask *m, uint32_t w, uint32_t h, pc_buf *out,
                                  pc_error *err);
/* The preserved block still describes m (so it can be written back unchanged). */
pc_status pc_io_vector_mask_block_matches(const uint8_t *data, size_t len, const pc_vector_mask *m,
                                          uint32_t w, uint32_t h, bool *out, pc_error *err);

/* ---- added by I4 (the reference's other public items) ---- */

/* Path records -> model path in document pixels (w x h canvas). */
pc_status pc_io_path_from_records(const pc_psd_path *p, uint32_t w, uint32_t h, pc_path *out, pc_error *err);
/* A vmsk block for path with extra flags (2 not linked, 4 disabled). An empty path is written as
 * the initial-fill record (Photoshop's form); otherwise inversion uses the invert flag. */
pc_status pc_io_vmsk_bytes(const pc_path *path, uint32_t flags, uint32_t w, uint32_t h, pc_buf *out,
                           pc_error *err);
/* vscg: content key (SoCo / GdFl / PtFl) + version-16 descriptor; *found false when unreadable. */
pc_status pc_io_fill_from_vscg(const uint8_t *data, size_t len, bool *found, pc_fill *out, pc_error *err);
pc_status pc_io_vscg_bytes(const pc_fill *f, pc_buf *out, pc_error *err);

/* Decoded vstk: the stroke (has_stroke false when disabled) and whether the fill is enabled. */
typedef struct pc_io_vstk {
    bool has_stroke;
    pc_shape_stroke stroke;
    bool fill_enabled;
} pc_io_vstk;

void pc_io_vstk_fini(pc_io_vstk *v);
pc_status pc_io_parse_vstk(const uint8_t *data, size_t len, float dpi, bool *found, pc_io_vstk *out,
                           pc_error *err);
/* stroke NULL = disabled (the defaults are written). */
pc_status pc_io_vstk_bytes(const pc_shape_stroke *stroke, bool fill_enabled, float dpi, pc_buf *out,
                           pc_error *err);
/* Live shape from vogk (u32 version + versioned descriptor): only a single, valid, untransformed
 * origination maps to a shape. */
pc_status pc_io_live_from_vogk(const uint8_t *data, size_t len, bool *found, pc_live_shape *out,
                               pc_error *err);
/* vogk for a live shape; *has false for polygons (no origination type for them). */
pc_status pc_io_vogk_bytes(const pc_live_shape *s, float dpi, bool *has, pc_buf *out, pc_error *err);

/* A layer's block by key (borrowed data, *len set) or NULL when it has none. */
typedef const uint8_t *(*pc_io_block_lookup)(void *ctx, const uint8_t key[4], size_t *len);
/* Fills a shape layer's path, fill and stroke from its blocks. */
pc_status pc_io_shape_from_blocks(pc_shape_layer *sh, pc_io_block_lookup block, void *ctx, uint32_t w,
                                  uint32_t h, float dpi, pc_error *err);
/* Appends (or updates) the blocks that store a shape layer. */
pc_status pc_io_shape_blocks(const pc_shape_layer *sh, pc_io_raw_blocks *raw, uint32_t w, uint32_t h,
                             float dpi, pc_error *err);

#endif
