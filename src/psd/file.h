/* The whole PSD/PSB file: model, parser and writer (port of crates/psd/src/file.rs and
 * image_data.rs).
 *
 * parse(b) → to_bytes gives b back for unmodified well-formed files: channel data stays in its
 * stored encoding, unknown blocks and resources stay raw, padding is preserved; and
 * parse(to_bytes(f)) == f for any model f. The parser never crashes on malformed input and
 * enforces the limits in types.h. Thread-safe (no shared state). */
#ifndef PC_PSD_FILE_H
#define PC_PSD_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/compression.h"
#include "psd/header.h"
#include "psd/layer.h"
#include "psd/resources.h"
#include "psd/tagged.h"
#include "psd/types.h"

/* Where the layer info is stored. */
typedef enum pc_psd_li_where {
    PC_PSD_LI_SECTION,     /* the layer info field of the layer and mask section (8-bit files) */
    PC_PSD_LI_GLOBAL_BLOCK /* a global Lr16 / Lr32 / Layr block (16/32-bit files) */
} pc_psd_li_where;

typedef struct pc_psd_li_placement {
    pc_psd_li_where where;
    size_t index;              /* GLOBAL_BLOCK: position among the global blocks (clamped on write) */
    uint8_t signature[4];      /* 8BIM / 8B64 */
    uint8_t key[4];            /* Lr16 / Lr32 / Layr */
    pc_psd_opt_bytes padding;  /* as pc_psd_block.padding */
} pc_psd_li_placement;

typedef struct pc_psd_file {
    pc_psd_header header;
    pc_psd_bytes color_mode_data; /* palette, duotone spec, HDR toning, ... */
    pc_psd_resource *resources;   /* stored order */
    size_t nresources;
    bool has_layer_info; /* false when the layer info length is 0 and no Lr16/Lr32 holds layers */
    pc_psd_layer_info layer_info;
    pc_psd_li_placement placement;
    bool has_global_mask; /* false when the field is absent entirely */
    pc_psd_bytes global_mask; /* raw, without the length field */
    pc_psd_block *global_blocks;
    size_t nglobal;
    pc_psd_bytes layer_mask_trailing; /* bytes at the end of the layer/mask section that form no block */
    uint16_t image_compression;       /* merged image */
    pc_psd_bytes image_data;          /* encoded, verbatim to the end of the file */
} pc_psd_file;

/* An empty model (zeroed header, no anything). */
void pc_psd_file_init(pc_psd_file *f);
void pc_psd_file_fini(pc_psd_file *f);
pc_status pc_psd_file_clone(const pc_psd_file *f, pc_psd_file *out, pc_error *err);
bool pc_psd_file_eq(const pc_psd_file *a, const pc_psd_file *b);

pc_status pc_psd_file_parse(const uint8_t *data, size_t len, pc_psd_file *out, pc_error *err);
pc_status pc_psd_file_to_bytes(const pc_psd_file *f, pc_psd_bytes *out, pc_error *err);
/* Reads / writes a file on disk (PC_ERR_IO with errno, or the parse error). */
pc_status pc_psd_file_open(const char *path, pc_psd_file *out, pc_error *err);
pc_status pc_psd_file_save(const pc_psd_file *f, const char *path, pc_error *err);

/* Structural invariants the writer relies on (PSD ≤ 30000 px, indexed needs a 768-byte palette). */
pc_status pc_psd_file_validate(const pc_psd_file *f, pc_error *err);

/* Layer records in file order (bottom-most first); NULL/0 when there is no layer info. */
const pc_psd_layer *pc_psd_file_layers(const pc_psd_file *f, size_t *n);
/* First resource with the id / first global block with the key, borrowed, or NULL. */
const pc_psd_resource *pc_psd_file_resource(const pc_psd_file *f, uint16_t id);
const pc_psd_block *pc_psd_file_global_block(const pc_psd_file *f, const char key[4]);
/* The global Lr16 / Lr32 / Layr block that should have held the layers but could not be parsed
 * (borrowed), with the parse error in *why (may be NULL); NULL when the file has layer info or no
 * such block parses badly. Parsing keeps such a block verbatim and leaves has_layer_info false, so
 * the file reads as flattened; this tells a reader its layers were lost rather than absent. */
const pc_psd_block *pc_psd_file_unreadable_layer_block(const pc_psd_file *f, pc_error *why);
/* ICC profile bytes (1039), borrowed, or NULL. */
const uint8_t *pc_psd_file_icc(const pc_psd_file *f, size_t *len);
bool pc_psd_file_resolution(const pc_psd_file *f, pc_psd_resolution *out);
/* The 1057 "has real merged data" flag when present and valid. */
bool pc_psd_file_has_real_merged_data(const pc_psd_file *f, bool *out);
/* Whether the merged image carries a transparency channel. */
bool pc_psd_file_merged_has_alpha(const pc_psd_file *f);

/* Global layer mask fields (absent when too short). */
bool pc_psd_global_mask_be16(const pc_psd_file *f, size_t at, uint16_t *out); /* 0 color space, 2..8 components, 10 opacity */
bool pc_psd_global_mask_kind(const pc_psd_file *f, uint8_t *out);           /* 0 color selected, 1 protected, 128 per layer */

/* ---- merged image data ---- */

pc_psd_layout pc_psd_image_layout(const pc_psd_header *h);
/* Planar data (channels × height × row_bytes) → encoded. */
pc_status pc_psd_image_encode(uint16_t compression, const uint8_t *decoded, size_t len, const pc_psd_header *h,
                              pc_psd_bytes *out, pc_error *err);
/* The merged image as planar big-endian samples. */
pc_status pc_psd_file_decode_merged(const pc_psd_file *f, pc_psd_bytes *out, pc_error *err);
pc_status pc_psd_file_decode_merged_channel(const pc_psd_file *f, size_t index, pc_psd_bytes *out, pc_error *err);

#endif
