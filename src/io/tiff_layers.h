/* Layered TIFF: Photoshop layer data in the TIFF tags 37724 (ImageSourceData) and 34377 (image
 * resources), see psd/tiff.h (port of crates/io/src/tiff_layers.rs).
 *
 * Import rebuilds a PSD model from the TIFF's pixels plus those two tags and hands it to the PSD
 * importer (I2), so everything the PSD path understands opens from a TIFF too. Export runs the
 * PSD exporter (I3) and stores its layer section and resources in the tags, in the byte order
 * the TIFF encoder writes, next to the flattened composite the TIFF itself carries.
 * Thread-safe (no shared state). */
#ifndef PC_IO_TIFF_LAYERS_H
#define PC_IO_TIFF_LAYERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codecs/codecs.h"
#include "doc/doc.h"
#include "io/core.h"
#include "psd/file.h"

/* Saving doc as TIFF writes layer data: anything beyond a single Background layer (one plain,
 * transparency-locked raster layer). */
bool pc_io_would_write_layers(const pc_document *doc);
/* Removes the blocks kept verbatim from a little-endian TIFF (foreign byte order) before file is
 * written as a PSD, with a warning for each. */
pc_status pc_io_strip_foreign_order_blocks(pc_psd_file *file, pc_io_warnings *warnings, pc_error *err);

/* Internal: opens a TIFF whose decoded image carries Photoshop layer data (its 37724 tag). Layer
 * data that can't be read opens flattened with a warning. *out initialised on success. */
pc_status pc_io_import_layered(const char *name, const pc_cimage *img, const uint8_t *layers,
                               size_t layers_len, pc_io_import_result *out, pc_error *err);
/* Internal: saves doc as a layered TIFF (Lab and layerless documents flat, with a warning). */
pc_status pc_io_export_layered(const pc_document *doc, const pc_io_export_opts *opts,
                               pc_io_export_result *out, pc_error *err);

#endif
