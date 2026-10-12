/* Document -> PSD/PSB (port of crates/io/src/psd_export.rs: psd_export.c for the layer records
 * and their tagged blocks, psd_export_file.c for the merged image, resources and global blocks,
 * psd_export_smart.c for smart objects; the state they share is psd_export_internal.h).
 *
 * Preserved PSD data (layer blocks, resources, global blocks, placed-layer and type data) is
 * written back verbatim while it still decodes to the document; anything edited is regenerated.
 * Layer and mask channels are RLE (zip with prediction at 32 bits; empty channels raw), the
 * merged image RLE (raw at 32 bits), matted against white like Photoshop's. Multichannel
 * documents go through pc_io_multichannel_to_psd. The reference cannot fail; here out of memory
 * is PC_ERR_NOMEM and documents past the compositor's limits (groups nested deeper than 128, a
 * merged image over PC_MAX_ALLOC) are PC_ERR_LIMIT. Calls I1 (psd_common, channel_map, pixels,
 * linked, pc_io_import), I2 (adjust_map, blocks, effects_map) and I4 (vector_map, comps_map,
 * annotations_map, pattern_map, slices_map, text_styles_map). Thread-safe (no shared state). */
#ifndef PC_IO_PSD_EXPORT_H
#define PC_IO_PSD_EXPORT_H

#include "doc/doc.h"
#include "io/core.h"
#include "psd/file.h"

/* A document as a PSD/PSB model with the default options (warnings dropped). */
pc_status pc_io_document_to_psd(const pc_document *doc, pc_psd_file *out, pc_error *err);
/* A document as a PSD/PSB model; what could not be represented is appended to *warnings. The
 * merged composite is rendered by the compositor. *out initialised on success. */
pc_status pc_io_document_to_psd_with(const pc_document *doc, const pc_io_psd_export_opts *opts,
                                     pc_psd_file *out, pc_io_warnings *warnings, pc_error *err);

#endif
