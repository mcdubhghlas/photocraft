/* Image resolution (pixels per inch) recorded in metadata blocks (port of
 * crates/codecs/src/resolution.rs and exif_color.rs): EXIF / TIFF IFD0 (XResolution 282,
 * YResolution 283, ResolutionUnit 296), XMP (tiff:XResolution, tiff:YResolution,
 * tiff:ResolutionUnit) and Photoshop's ResolutionInfo image resource (8BIM 0x03ED); reading
 * them, and rewriting the EXIF and XMP copies on export so they agree with the resolution the
 * file is written at. Also the EXIF ColorSpace tag (0xA001 in the Exif sub-IFD).
 *
 * Precedence when a file carries several (as Photoshop resolves them): ResolutionInfo, then XMP,
 * then EXIF, and only then a JFIF density. Formats whose own field is deliberate (PNG pHYs, TIFF
 * tags) keep it first; the metadata blocks fill in when it is missing.
 *
 * Everything read here is untrusted: anything malformed reads as "no resolution". A resolution
 * is usable when both axes are finite, positive and at most 1,000,000 ppi. Thread-safe. */
#ifndef PC_CODECS_RESOLUTION_H
#define PC_CODECS_RESOLUTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"

/* IFD0's resolution of an EXIF block (with or without Exif\0\0) or TIFF structure: one
 * RATIONAL each (non-zero denominator), unit 2 (inch, also when absent) or 3 (centimetre). */
bool pc_exif_resolution(const uint8_t *exif, size_t n, float *x, float *y);

/* exif with its IFD0 resolution set to (x, y) ppi and the unit to inch. Entries that cannot be
 * rewritten in place are removed from IFD0 (nothing else moves). *out is NULL when nothing
 * changes (no resolution entries, already that resolution, unparseable), else a new buffer. */
pc_status pc_exif_with_resolution(const uint8_t *exif, size_t n, float x, float y, uint8_t **out,
                                  size_t *out_len, pc_error *err);

/* The ResolutionInfo of a Photoshop image resource block (APP13 or TIFF tag 34377). */
bool pc_photoshop_resolution(const uint8_t *res, size_t n, float *x, float *y);

/* The tiff: resolution of an XMP packet (UTF-8). */
bool pc_xmp_resolution(const char *xmp, size_t n, float *x, float *y);

/* xmp with its tiff: resolution set to (x, y) and the unit to 2. *out NULL when unchanged, else a
 * new NUL-terminated buffer of *out_len bytes. */
pc_status pc_xmp_with_resolution(const char *xmp, size_t n, float x, float y, char **out, size_t *out_len,
                                 pc_error *err);

/* The EXIF to write next to upright pixels at resolution dpi (NULL: unknown; else x, y):
 * Orientation 1, no IFD1 thumbnail of the source, and the resolution. *out NULL when unchanged. */
pc_status pc_export_exif(const uint8_t *exif, size_t n, const float *dpi, uint8_t **out, size_t *out_len,
                         pc_error *err);
/* The XMP to write next to upright pixels at resolution dpi. *out NULL when unchanged. */
pc_status pc_export_xmp(const char *xmp, size_t n, const float *dpi, char **out, size_t *out_len,
                        pc_error *err);

/* The EXIF ColorSpace value meaning sRGB (0xFFFF is uncalibrated). */
#define PC_EXIF_COLOR_SPACE_SRGB 1
/* The EXIF ColorSpace of a TIFF-structured block; false when absent or malformed. */
bool pc_exif_color_space(const uint8_t *exif, size_t n, uint16_t *out);

#endif
