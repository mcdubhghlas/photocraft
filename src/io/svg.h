/* SVG import. Detection is I1's (lib.c); the import and the smart-object rasteriser are chunk
 * I5's (crates/io/src/svg.rs: usvg parsing, resvg rendering; see svg_internal.h). Errors of a drawing that
 * doesn't parse are PC_ERR_FORMAT "SVG: ..." (pc_io_svg_error). Thread-safe. */
#ifndef PC_IO_SVG_H
#define PC_IO_SVG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compose/compose.h"
#include "io/core.h"

/* Largest canvas side an SVG opens at; a larger drawing is scaled down to fit. */
#define PC_IO_SVG_MAX_SIDE 16384u

/* Looks like an SVG document: plain XML starting (after BOMs and white space) with <svg, or an
 * XML declaration / doctype / comment followed by <svg in the first 4096 bytes (I1). */
bool pc_io_is_svg(const uint8_t *bytes, size_t len);

/* Opens an SVG (or .svgz) as a document of shape layers (I5). *out initialised on success. */
pc_status pc_io_svg_import(const char *name, const uint8_t *bytes, size_t len, pc_io_import_result *out,
                           pc_error *err);
/* The whole drawing rendered at scale (1 = its own size) as straight RGBA at the origin (I5). */
pc_status pc_io_svg_rasterize(const uint8_t *bytes, size_t len, float scale, pc_compose_buffer *out,
                              pc_error *err);

#endif
