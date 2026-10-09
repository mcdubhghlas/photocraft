/* Photoshop brush files (.abr): the legacy v1/v2 layout and the sectioned v6+ layout (port of
 * crates/psd/src/abr.rs). Clean-room from public descriptions of the format. Big-endian.
 *
 *   v1 / v2  := version u16 (1|2), count u16, count x brush
 *   brush    := type u16 (1 computed, 2 sampled), length u32, body[length]
 *   computed := misc u32, spacing u16 (%), diameter u16, roundness u16 (%), angle i16, hardness u16
 *   sampled  := misc u32, spacing u16, [v2: name (u32 length + UTF-16 incl. NUL)], anti-alias u8,
 *               short bounds 4 x i16, bounds 4 x i32 (top, left, bottom, right), depth u16 (8|16),
 *               compression u8 (0 raw, 1 PackBits with a u16 row-length table)
 *   v6+      := version u16 (6..=10), subversion u16 (1|2), sections
 *   section  := "8BIM", key[4], length u32, data (padded to 4)
 *   "samp"   := entries: length u32 (padded to 4), Pascal uuid, skip (10 if subversion 1, 264 if
 *               2), bounds 4 x i32, depth i16, compression u8, data
 *   "patt"   := patterns as in the Patt global block
 *   "desc"   := versioned ActionDescriptor; key Brsh lists brushPreset objects
 *
 * Parsing never fails hard on one damaged brush: it is skipped with a warning while the rest of
 * the file is readable. Sizes are capped (PC_PSD_ABR_MAX_EDGE, PC_PSD_ABR_MAX_TOTAL_BYTES).
 * Thread-safe. */
#ifndef PC_PSD_ABR_H
#define PC_PSD_ABR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/descriptor.h"
#include "psd/patterns.h"
#include "psd/types.h"

#define PC_PSD_ABR_MAX_EDGE 8192u                       /* largest sampled tip edge */
#define PC_PSD_ABR_MAX_BRUSHES 20000u                   /* most brushes read from one file */
#define PC_PSD_ABR_MAX_TOTAL_BYTES ((size_t)512 << 20) /* decoded sample bytes over a file */

/* A sampled tip: one grayscale plane where the maximum value is full paint. */
typedef struct pc_psd_abr_sample {
    pc_psd_str id; /* the uuid a preset's sampledData refers to (v6+), or empty */
    uint32_t width;
    uint32_t height;
    uint16_t depth;    /* 8 or 16 */
    pc_psd_bytes data; /* row-major, width x height x depth / 8 bytes, 16-bit big-endian */
} pc_psd_abr_sample;

typedef enum pc_psd_abr_tip { PC_PSD_ABR_COMPUTED, PC_PSD_ABR_SAMPLED } pc_psd_abr_tip;

/* A legacy (v1/v2) brush. */
typedef struct pc_psd_abr_legacy {
    pc_psd_str name;  /* v2 sampled brushes only */
    uint16_t spacing; /* percent of the diameter (0 = off) */
    bool anti_alias;  /* sampled tips */
    pc_psd_abr_tip tip;
    uint16_t diameter;  /* COMPUTED: pixels */
    uint16_t hardness;  /* COMPUTED: 0..100 % */
    int16_t angle;      /* COMPUTED: degrees */
    uint16_t roundness; /* COMPUTED: 0..100 % */
    pc_psd_abr_sample sample; /* SAMPLED */
} pc_psd_abr_legacy;

typedef struct pc_psd_abr {
    uint16_t version;    /* 1, 2, 6, 7, 9 or 10 */
    uint16_t subversion; /* v6+: 1 or 2; 0 for v1/v2 */
    pc_psd_abr_legacy *legacy;
    size_t nlegacy;
    pc_psd_abr_sample *samples; /* v6+ samp, file order */
    size_t nsamples;
    pc_psd_pattern *patterns; /* v6+ patt */
    size_t npatterns;
    pc_psd_desc *presets; /* v6+ brushPreset objects of desc, file order */
    size_t npresets;
    pc_psd_warnings warnings; /* problems that did not stop the parse */
} pc_psd_abr;

void pc_psd_abr_sample_fini(pc_psd_abr_sample *s);
bool pc_psd_abr_sample_eq(const pc_psd_abr_sample *a, const pc_psd_abr_sample *b);
/* Samples as 0..1 paint amounts (new array of width x height floats). */
pc_status pc_psd_abr_sample_to_unit(const pc_psd_abr_sample *s, float **out, size_t *n, pc_error *err);
void pc_psd_abr_legacy_fini(pc_psd_abr_legacy *b);
bool pc_psd_abr_legacy_eq(const pc_psd_abr_legacy *a, const pc_psd_abr_legacy *b);
void pc_psd_abr_fini(pc_psd_abr *f);

pc_status pc_psd_abr_parse(const uint8_t *data, size_t len, pc_psd_abr *out, pc_error *err);
/* A v1 or v2 file; sampled tips PackBits-compressed when rle. */
pc_status pc_psd_abr_write_v12(uint16_t version, const pc_psd_abr_legacy *b, size_t n, bool rle, pc_psd_bytes *out,
                               pc_error *err);
/* A v6 file (subversion 1 or 2): sampled tips, embedded patterns and preset descriptors (each a
 * brushPreset object listed under Brsh). */
pc_status pc_psd_abr_write_v6(uint16_t subversion, const pc_psd_abr_sample *s, size_t ns, const pc_psd_pattern *p,
                              size_t np, const pc_psd_desc *presets, size_t npresets, bool rle, pc_psd_bytes *out,
                              pc_error *err);

#endif
