/* Photoshop contour preset files (.shc), the Layer Style dialog's Contour Editor Load/Save (port of
 * crates/psd/src/shc.rs).
 *
 * Layout from Adobe's public "Photoshop File Formats Specification" (Additional File Formats ›
 * Contours): a 4-byte type `8BFS`, a u16 version (1) and a u32 count of contours. Each contour is
 * a u32 version (1 or 2), a Unicode name, a u16 point count and the points as two u16s each
 * (vertical first, then horizontal). Version 2 then has one boolean byte per point ("whether the
 * point is continuous"); both versions end with a 4-byte minimum and maximum input range. All
 * values are big-endian.
 *
 * The spec leaves two details open, handled defensively:
 * - The name is read like the spec's other Unicode strings (a u32 UTF-16 count that includes a
 *   trailing null, as in .aco); a name whose count leaves out the null and is followed by one is
 *   read too. Names are written with the null counted.
 * - Coordinates are read as 0..=255 levels (the layer-style contour descriptors' scale); the
 *   input range is written as 0..=255.
 * Thread-safe. */
#ifndef PC_PSD_SHC_H
#define PC_PSD_SHC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/types.h"

#define PC_PSD_SHC_MAX_CONTOURS 4096u   /* most contours read from one file */
#define PC_PSD_SHC_MAX_POINTS 256u      /* most points in one contour */
#define PC_PSD_SHC_MAX_NAME_UNITS 1024u /* longest name read or written, in UTF-16 units */

/* One point: input (horizontal) and output (vertical) levels 0..=255; continuous = smooth
 * (false = a corner). */
typedef struct pc_psd_shc_point {
    float input;
    float output;
    bool continuous;
} pc_psd_shc_point;

typedef struct pc_psd_shc_contour {
    pc_psd_str name;
    pc_psd_shc_point *points; /* file order */
    size_t npoints;
} pc_psd_shc_contour;

void pc_psd_shc_contour_fini(pc_psd_shc_contour *c);
void pc_psd_shc_free(pc_psd_shc_contour *c, size_t n); /* NULL-safe */
bool pc_psd_shc_contour_eq(const pc_psd_shc_contour *a, const pc_psd_shc_contour *b);

/* Bad signatures, unknown versions, truncation and impossible counts are errors. *out is a new
 * array (pc_psd_shc_free). */
pc_status pc_psd_shc_parse(const uint8_t *data, size_t len, pc_psd_shc_contour **out, size_t *n,
                           pc_error *err);
/* Every contour as version 2, with its corner flags. More than PC_PSD_SHC_MAX_POINTS points in a
 * contour or more than PC_PSD_SHC_MAX_CONTOURS contours: PC_ERR_LIMIT. */
pc_status pc_psd_shc_write(const pc_psd_shc_contour *c, size_t n, pc_psd_bytes *out, pc_error *err);

#endif
