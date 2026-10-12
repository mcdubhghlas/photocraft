/* PSD import (chunk I2: crates/io/src/psd_import.rs, blocks.rs, adjust_map.rs, effects_map.rs).
 * Implemented in psd_import.c (document level) and psd_layers.c (layer records). Calls I1
 * (pixels.h, channel_map.h, core.h's pc_io_unequal_resolution_warning), I3 (smart_map.h) and I4
 * (vector_map.h, comps_map.h, pattern_map.h, annotations_map.h, slices_map.h,
 * text_styles_map.h, gradient_bake.h). Thread-safe (no shared state). */
#ifndef PC_IO_PSD_IMPORT_H
#define PC_IO_PSD_IMPORT_H

#include <stddef.h>
#include <stdint.h>

#include "doc/doc.h"
#include "io/core.h"
#include "psd/file.h"
#include "raster/interrupt.h"

/* Resources mapped to document fields or regenerated on export (psd_import::MAPPED_RESOURCES;
 * psd_import.c). */
extern const uint16_t PC_IO_MAPPED_RESOURCES[19];
/* Blocks regenerated from document fields on export, not kept in a layer's psd_blocks
 * (psd_import::REGENERATED; psd_layers.c). */
extern const char PC_IO_REGENERATED[9][5];

/* Deepest group nesting of the file's layer records (implemented in I1's lib.c: lib.rs needs it
 * before import; iterative). */
size_t pc_io_psd_group_depth(const pc_psd_file *file);

/* Converts a parsed PSD into a document; problems become warnings appended to *warnings (the
 * reference never fails; C fails only on out of memory). *out initialised on success. */
pc_status pc_io_psd_to_document(const pc_psd_file *file, pc_document *out, pc_io_warnings *warnings,
                                pc_error *err);
/* The same for a background open: checks ctl per layer record and reports progress (0.05 ..);
 * PC_ERR_CANCELLED "cancelled" when cancelled. ctl may be NULL. */
pc_status pc_io_psd_to_document_with(const pc_psd_file *file, const pc_interrupt *ctl, pc_document *out,
                                     pc_io_warnings *warnings, pc_error *err);

#endif
