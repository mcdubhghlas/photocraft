/* OpenRaster (.ora) import and export (port of crates/io/src/ora.rs, R10): a ZIP whose first
 * entry is the uncompressed "mimetype" (image/openraster), stack.xml describing the layer tree and
 * one PNG per layer. Import keeps layers, groups, blend modes (SVG compositing ops and Krita's
 * ids), opacity and visibility; export writes the same plus mergedimage.png and a thumbnail.
 * Untrusted input: entry sizes, pixel budgets, XML size and node counts are capped; errors are
 * "OpenRaster: <reason>" (PC_IO_ERR_ORA). Thread-safe (no shared state). */
#ifndef PC_IO_ORA_H
#define PC_IO_ORA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "doc/doc.h"
#include "io/core.h"
#include "raster/interrupt.h"

/* Longest side accepted for a layer or the canvas (also Krita's merged image). */
#define PC_IO_ORA_MAX_SIDE 300000u

/* The stored first entry "mimetype" of a ZIP (zip_mimetype): its bytes, borrowed from `bytes`,
 * or NULL when the file isn't a ZIP starting with an uncompressed mimetype entry. */
const uint8_t *pc_io_zip_mimetype(const uint8_t *bytes, size_t len, size_t *mime_len);
/* An OpenRaster archive (mimetype image/openraster). */
bool pc_io_is_ora(const uint8_t *bytes, size_t len);
/* Imports an OpenRaster document; ctl (may be NULL) is checked per layer. *out initialised on
 * success. */
pc_status pc_io_ora_import(const char *name, const uint8_t *bytes, size_t len, const pc_interrupt *ctl,
                           pc_io_import_result *out, pc_error *err);
/* Exports doc as OpenRaster. *out initialised on success. */
pc_status pc_io_ora_export(const pc_document *doc, pc_io_export_result *out, pc_error *err);

#endif
