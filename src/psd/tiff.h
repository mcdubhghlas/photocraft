/* Photoshop layer data embedded in TIFF files, "layered TIFF" (port of crates/psd/src/tiff.rs).
 *
 * Photoshop keeps a layered TIFF's layers in two private tags (Adobe "Photoshop TIFF Technical
 * Notes", March 2002, plus the PSD specification):
 *   37724 ImageSourceData: "Adobe Photoshop Document Data Block\0" ("... V0002\0" for PSB-sized
 *         data) followed by tagged blocks padded to 4. Layr / Lr16 / Lr32 hold the layer info,
 *         LMsk the global layer mask, every other block is a global block.
 *   34377: the PSD image resources (8BIM blocks without a section length).
 *
 * Byte order: in an Intel-order (II) TIFF every integer and double in these structures is
 * little-endian and every four-character code is stored reversed (MIB8, ryaL), while pixel
 * samples inside channel data stay big-endian; RLE row counts follow the container. Reading
 * normalises to the big-endian PSD model and writing produces either order, by transcoding every
 * known structure field by field. A block of unknown layout from a little-endian file is kept
 * verbatim with the signature MIB8 (46B8 for 8B64) and written back exactly to a little-endian
 * file; a save to the other byte order has to drop it, with a warning.
 * Thread-safe (no shared state). */
#ifndef PC_PSD_TIFF_H
#define PC_PSD_TIFF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/file.h"

typedef enum pc_psd_byte_order { PC_PSD_BIG, PC_PSD_LITTLE } pc_psd_byte_order;

extern const uint8_t PC_PSD_ISD_SIGNATURE[36];     /* "Adobe Photoshop Document Data Block\0" */
extern const uint8_t PC_PSD_ISD_SIGNATURE_PSB[36]; /* "Adobe Photoshop Document Data V0002\0" */

/* The layer-info key Photoshop uses for a depth: Lr16, Lr32, else Layr. */
void pc_psd_layer_info_key(uint16_t depth, uint8_t out[4]);

/* The contents of an ImageSourceData tag in the big-endian PSD model. */
typedef struct pc_psd_isd {
    pc_psd_version version;       /* PSD (4-byte lengths) or PSB (8-byte for long keys) */
    pc_psd_byte_order byte_order; /* order the data was read in */
    bool has_layer_info;
    pc_psd_layer_info layer_info;
    uint8_t layer_info_key[4]; /* which key held it (Layr when there was none) */
    bool has_global_mask;
    pc_psd_bytes global_mask; /* the LMsk block */
    pc_psd_block *global_blocks; /* every other block, stored order */
    size_t nglobal;
    pc_psd_bytes trailing; /* bytes after the last block that form no block */
} pc_psd_isd;

void pc_psd_isd_init(pc_psd_isd *d); /* PSD, big, no layers, key Layr */
void pc_psd_isd_fini(pc_psd_isd *d);
bool pc_psd_isd_eq(const pc_psd_isd *a, const pc_psd_isd *b);
pc_status pc_psd_isd_clone(const pc_psd_isd *d, pc_psd_isd *out, pc_error *err);

/* Parses a 37724 tag. warnings may be NULL (it is initialised either way when non-NULL). */
pc_status pc_psd_isd_parse(const uint8_t *data, size_t len, pc_psd_isd *out, pc_psd_warnings *warnings,
                           pc_error *err);
/* Serialises in `order` (blocks padded to 4; the layer info padded to 4 as Photoshop writes). */
pc_status pc_psd_isd_to_bytes(const pc_psd_isd *d, pc_psd_byte_order order, pc_psd_bytes *out,
                              pc_psd_warnings *warnings, pc_error *err);
/* The layers, global mask and global blocks of a PSD (copied; byte order big). */
pc_status pc_psd_isd_from_psd(const pc_psd_file *f, pc_psd_isd *out, pc_error *err);
/* A PSD model from this data plus the parts a TIFF stores natively. Takes ownership of *d,
 * *resources (array of nres) and *image_data (all emptied), also on failure. */
pc_status pc_psd_isd_into_psd(pc_psd_isd *d, pc_psd_header header, pc_psd_resource **resources, size_t nres,
                              uint16_t image_compression, pc_psd_bytes *image_data, pc_psd_file *out, pc_error *err);

/* 34377: image resources (little-endian MIB8 blocks converted; a trailing pad tolerated). */
pc_status pc_psd_tiff_resources_parse(const uint8_t *data, size_t len, pc_psd_resource **out, size_t *n,
                                      pc_psd_warnings *warnings, pc_error *err);
/* Big-endian, as every reader expects. */
pc_status pc_psd_tiff_resources_to_bytes(const pc_psd_resource *r, size_t n, pc_psd_bytes *out, pc_error *err);

/* Field-by-field byte-order conversion (output lengths equal input lengths unless blocks are
 * dropped). warnings may be NULL. */
pc_status pc_psd_transcode_blocks(const uint8_t *data, size_t len, pc_psd_version v, pc_psd_byte_order from,
                                  pc_psd_byte_order to, pc_psd_bytes *out, pc_psd_warnings *warnings, pc_error *err);
pc_status pc_psd_transcode_layer_info(const uint8_t *data, size_t len, pc_psd_version v, pc_psd_byte_order from,
                                      pc_psd_byte_order to, pc_psd_bytes *out, pc_psd_warnings *warnings,
                                      pc_error *err);
pc_status pc_psd_transcode_resources(const uint8_t *data, size_t len, pc_psd_byte_order from, pc_psd_byte_order to,
                                     pc_psd_bytes *out, pc_psd_warnings *warnings, pc_error *err);

#endif
