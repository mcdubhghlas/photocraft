/* photocraft io: import and export between documents and files (port of crates/io).
 *
 *  - PSD / PSB (8BPS signature) through src/psd, fidelity levels 1-3 (I2 import, I3 export).
 *  - Camera raws through src/raw, developed into a 16-bit ProPhoto RGB "Background" layer;
 *    unsupported variants fall back to the embedded JPEG preview (raw_import.h).
 *  - Layered TIFFs (tags 37724 / 34377) open with their layers through the PSD path and are
 *    written back the same way (tiff_layers.h).
 *  - Affinity documents open natively, read-only (I5), SVG drawings as shape layers (I5).
 *  - .pcraft bundles through src/format.
 *  - Every other format through src/codecs as a single "Background" layer (flat.h).
 * Exports to flat formats report what is lost; errors and results are described in core.h.
 *
 * Headers: core.h (warnings, errors, results, options), pixels.h, flat.h, raw_import.h,
 * linked.h, tiff_layers.h, channel_map.h (DisplayInfo + Multichannel), psd_common.h, and one
 * header per later module (below). Thread safety: every function is reentrant; pixel work
 * runs on pc_pool_global() and gives the same result with any thread count.
 *
 * ---- Chunk plan (REWRITE_PLAN.md I1-I5) ----
 *
 * I1 (done): lib.c, core.c, pixels.c, flat_import.c, flat_export.c, raw_import.c, linked.c,
 * tiff_layers.c, channel_map.c, multichannel_map.c, psd_common.c. Also ported here because
 * lib.rs needs them before dispatching: psd_import::group_depth (pc_io_psd_group_depth),
 * affinity::{EXTENSIONS, has_extension, is_affinity} and svg::is_svg (lib.c); and
 * psd_export::{psd_version, estimate_psd_size, psd_size_exceeds_limit, unicode_names_resource,
 * pascal_names_resource} (psd_common.c), which the Multichannel writer needs.
 *
 * Each later chunk owns the headers listed for it: it implements every declared function,
 * deletes the matching stub from stub_iN.c (and the file when empty), and may add
 * declarations. Keep declared signatures: other chunks code against them in parallel.
 *
 * I2 (done): psd_import.c, psd_layers.c, blocks.c, blocks_fill.c, adjust_map.c,
 *    adjust_write.c, effects_map.c -> psd_import.h, blocks.h, adjust_map.h, effects_map.h. Calls I1 (pixels, channel_map incl.
 *    pc_io_multichannel_import, core), I3 (smart_map.h: pc_io_parse_sold, pc_io_mask_from_item,
 *    pc_io_placed_fini) and I4 (vector_map.h, comps_map.h, pattern_map.h, annotations_map.h,
 *    slices_map.h, text_styles_map.h, gradient_bake.h).
 * I3 (done) psd_export.c, psd_export_file.c, psd_export_smart.c, smart_map.c, smart_filters.c ->
 *    psd_export.h, smart_map.h. Calls I1
 *    (psd_common, channel_map incl. pc_io_multichannel_to_psd, pixels, linked, pc_io_import),
 *    I2 (adjust_map.h, blocks.h, effects_map.h, psd_import.h's PC_IO_REGENERATED and
 *    pc_io_psd_to_document) and I4 (vector_map.h, comps_map.h, annotations_map.h,
 *    pattern_map.h, slices_map.h, text_styles_map.h).
 * I4 abr_map.c, annotations_map.c, comps_map.c, gradient_bake.c, pattern_map.c, slices_map.c,
 *    text_import.c, text_styles_map.c, vector_map.c -> the headers of those names (stubs:
 *    stub_i4.c; abr_map.h is I4's own). Calls I1 (pixels: pattern_map) and I2 (blocks.h).
 * I5 affinity.c, camera_raw_map.c, svg.c -> affinity.h, svg.h (stubs: stub_i5.c;
 *    camera_raw_map.h is I5's own). Calls I1 (pc_io_image_to_document, pc_io_import), I3
 *    (smart_map.h: PC_IO_UNSUPPORTED_FILTER, pc_io_filter_from_item, pc_io_item_for_filter)
 *    and I4 (text_import.h). I5 also ports the rest of crates/io/tests (see tests/io/).
 *
 * Dispatch from lib.c into later chunks: PSD import -> pc_io_psd_to_document_with (I2) then
 * pc_io_text_prepare (I4); PSD / layered-TIFF export -> pc_io_document_to_psd_with (I3);
 * layered-TIFF import -> pc_io_psd_to_document (I2); Affinity -> pc_io_affinity_import (I5);
 * SVG -> pc_io_svg_import (I5); merged_composite's generic path -> pc_io_psd_to_document. */
#ifndef PC_IO_IO_H
#define PC_IO_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compose/compose.h"
#include "doc/doc.h"
#include "io/affinity.h"
#include "io/channel_map.h"
#include "io/core.h"
#include "io/flat.h"
#include "io/linked.h"
#include "io/pixels.h"
#include "io/psd_common.h"
#include "io/psd_export.h"
#include "io/psd_import.h"
#include "io/raw_import.h"
#include "io/svg.h"
#include "io/text_import.h"
#include "io/tiff_layers.h"
#include "psd/file.h"
#include "raster/interrupt.h"

/* true if bytes start with the PSD/PSB signature 8BPS. */
bool pc_io_is_psd(const uint8_t *bytes, size_t len);

/* Imports a file: .pcraft (by extension or content), PSD/PSB, Affinity and camera raws by
 * magic, SVG by extension or content, everything else through the codecs. name is the file
 * name (used for the document name and extension checks). *out initialised on success. */
pc_status pc_io_import(const char *name, const uint8_t *bytes, size_t len, pc_io_import_result *out,
                       pc_error *err);
/* pc_io_import for a background open: checks ctl between stages (and per layer for PSD/PSB)
 * and reports progress, ending at 1.0. A cancelled import fails with PC_ERR_CANCELLED
 * "cancelled". ctl may be NULL. */
pc_status pc_io_import_with(const char *name, const uint8_t *bytes, size_t len, const pc_interrupt *ctl,
                            pc_io_import_result *out, pc_error *err);
/* Exports doc to the format named by name_or_ext (a file name, path or bare extension):
 * .pcraft, PSD/PSB, layered or flat TIFF, any writable codec format. Affinity is
 * "unsupported: ..."; an unknown extension is PC_ERR_NOT_FOUND "unknown file format: <name>".
 * opts may be NULL (defaults). *out initialised on success. */
pc_status pc_io_export(const pc_document *doc, const char *name_or_ext, const pc_io_export_opts *opts,
                       pc_io_export_result *out, pc_error *err);
/* The PSD's merged composite as straight RGBA floats (row-major, *n = width * height; caller
 * frees *px), un-matted from white when the file has merged alpha. Lab, CMYK and other modes go
 * through the document conversion (I2). The compositing oracle. */
pc_status pc_io_merged_composite(const pc_psd_file *file, float (**px)[4], size_t *n, pc_error *err);

#endif
