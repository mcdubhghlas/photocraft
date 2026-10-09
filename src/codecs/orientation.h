/* EXIF / TIFF orientation (tag 274): reading it, applying it to the pixels, and rewriting it
 * to 1 once the pixels are upright (port of crates/codecs/src/orientation.rs).
 *
 * Decoders apply the orientation by default, so a document is always edited upright, and
 * encoders write Orientation = 1. Values: 1 upright, 2 mirrored, 3 rotated 180, 4 flipped
 * vertically, 5 transposed, 6 needs a 90 clockwise turn, 7 transverse, 8 needs a 90
 * counter-clockwise turn. Anything malformed, truncated or out of range reads as 1; the first
 * Orientation entry of IFD0 wins and must be exactly one SHORT or LONG. Thread-safe. */
#ifndef PC_CODECS_ORIENTATION_H
#define PC_CODECS_ORIENTATION_H

#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "codecs/image.h"

/* The orientation (1-8) of a TIFF-structured block: EXIF (with or without Exif\0\0) or a whole
 * TIFF / BigTIFF file. */
uint16_t pc_exif_orientation(const uint8_t *exif, size_t n);

/* exif with every well-formed Orientation entry set to 1 (in its own type and byte order; no
 * other byte changes). *out is NULL when there is nothing to change, else a new buffer. */
pc_status pc_upright_exif(const uint8_t *exif, size_t n, uint8_t **out, size_t *out_len, pc_error *err);

/* xmp with every tiff:Orientation (attribute or element form) set to 1; *out NULL when
 * unchanged, else a new NUL-terminated buffer of *out_len bytes. */
pc_status pc_upright_xmp(const char *xmp, size_t n, char **out, size_t *out_len, pc_error *err);

/* The image turned upright for orientation o (1-8; anything else returns it unchanged). Takes
 * ownership of *img (emptied, also on failure). Width/height and DPI swap for 5-8, and the
 * EXIF and XMP orientation are rewritten to 1. Bands of rows run on the global pool. */
pc_status pc_cimage_oriented(pc_cimage *img, uint16_t o, pc_cimage *out, pc_error *err);

#endif
