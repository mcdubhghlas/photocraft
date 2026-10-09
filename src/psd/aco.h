/* Photoshop Color Swatches files (.aco), versions 1 and 2 (port of crates/psd/src/aco.rs).
 *
 * Layout from Adobe's "Photoshop File Formats Specification" (Color Swatches): a version 1
 * section (u16 version = 1, u16 count, count colour records of a u16 colour space id and four
 * u16 values), usually followed by a version 2 section with the same colours plus a name each
 * (u32 UTF-16 unit count including the trailing NUL, then the units). Files that start with
 * the version 2 section are read too.
 *
 * Colours are kept at their 16-bit precision in the space they were stored in: RGB and HSB are
 * full 16-bit; CMYK is kept as ink (the file stores 65535 - ink); Lab is L* x 100 and signed
 * a*, b* x 100; gray is 0..10000 with 0 = black. Other spaces are kept verbatim. Converting
 * colours is the caller's job. Thread-safe. */
#ifndef PC_PSD_ACO_H
#define PC_PSD_ACO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/types.h"

#define PC_PSD_ACO_MAX_SWATCHES 65535u /* the counts are 16-bit */
#define PC_PSD_ACO_MAX_NAME_UNITS 1024u /* longest name read or written, in UTF-16 units */

/* Colour space ids of the colour record. */
enum { PC_PSD_ACO_SPACE_RGB = 0, PC_PSD_ACO_SPACE_HSB = 1, PC_PSD_ACO_SPACE_CMYK = 2, PC_PSD_ACO_SPACE_LAB = 7,
       PC_PSD_ACO_SPACE_GRAY = 8 };

typedef enum pc_psd_aco_kind {
    PC_PSD_ACO_RGB,  /* v[0..3]: red, green, blue */
    PC_PSD_ACO_HSB,  /* v[0..3]: hue (0..65535 for 0..360 degrees), saturation, brightness */
    PC_PSD_ACO_CMYK, /* v[0..4]: cyan, magenta, yellow, black ink (65535 = 100 %) */
    PC_PSD_ACO_LAB,  /* v[0] = L* x 100; (int16_t)v[1] = a* x 100, (int16_t)v[2] = b* x 100 */
    PC_PSD_ACO_GRAY, /* v[0]: 0..10000, 0 = black */
    PC_PSD_ACO_OTHER /* space and the four values as stored */
} pc_psd_aco_kind;

/* A swatch colour; unused values are 0. */
typedef struct pc_psd_aco_color {
    pc_psd_aco_kind kind;
    uint16_t space; /* OTHER only */
    uint16_t v[4];
} pc_psd_aco_color;

typedef struct pc_psd_aco_swatch {
    pc_psd_str name; /* empty in version 1 only files */
    pc_psd_aco_color color;
} pc_psd_aco_swatch;

typedef struct pc_psd_aco {
    pc_psd_aco_swatch *v; /* file order */
    size_t n;
    bool has_names; /* the file had a version 2 section */
} pc_psd_aco;

/* Decodes a colour record (space id + four stored values) and back. */
pc_psd_aco_color pc_psd_aco_from_record(uint16_t space, const uint16_t v[4]);
void pc_psd_aco_to_record(pc_psd_aco_color c, uint16_t *space, uint16_t v[4]);
bool pc_psd_aco_color_eq(pc_psd_aco_color a, pc_psd_aco_color b);
bool pc_psd_aco_swatch_eq(const pc_psd_aco_swatch *a, const pc_psd_aco_swatch *b);

void pc_psd_aco_fini(pc_psd_aco *f);
/* Truncated sections, unknown versions and impossible counts are errors. */
pc_status pc_psd_aco_parse(const uint8_t *data, size_t len, pc_psd_aco *out, pc_error *err);
/* A version 1 section followed by a version 2 section with the names, as Photoshop writes.
 * More than PC_PSD_ACO_MAX_SWATCHES swatches: PC_ERR_LIMIT. */
pc_status pc_psd_aco_write(const pc_psd_aco_swatch *s, size_t n, pc_psd_bytes *out, pc_error *err);

#endif
