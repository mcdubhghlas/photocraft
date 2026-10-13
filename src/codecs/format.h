/* Format identification and capability declarations (port of crates/codecs/src/format.rs).
 * Thread-safe. */
#ifndef PC_CODECS_FORMAT_H
#define PC_CODECS_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codecs/image.h"

/* Supported (or known) raster file formats, in the reference's Format::ALL order. */
typedef enum pc_cformat {
    PC_CF_PNG,
    PC_CF_JPEG,
    PC_CF_TIFF,
    PC_CF_WEBP,
    PC_CF_GIF,
    PC_CF_BMP,
    PC_CF_TGA,
    PC_CF_ICO,
    PC_CF_PNM, /* Netpbm: PBM/PGM/PPM (P1-P6), PAM (P7), PFM (PF/Pf) */
    PC_CF_PCX, /* ZSoft Paintbrush: read-only (1/2/4-bit planar palettes, 8-bit indexed, 24-bit RGB) */
    PC_CF_QOI,
    PC_CF_EXR,
    PC_CF_HDR,  /* Radiance RGBE */
    PC_CF_AVIF, /* neither read nor written in this build */
    PC_CF_HEIF, /* read-only (libheif) when built with it */
    PC_CF_FITS  /* Flexible Image Transport System (astronomy): gray or RGB */
} pc_cformat;
#define PC_CF_COUNT 16

/* What a format can hold and what this build reads/writes for it. */
typedef struct pc_ccaps {
    bool read;
    bool write;
    const pc_csample *depths; /* sample types stored without conversion */
    size_t ndepths;
    const pc_clayout *layouts; /* layouts stored and read back without conversion */
    size_t nlayouts;
    bool alpha;
    bool icc;
    bool exif;
    bool xmp;
    bool dpi;
    bool text;
    bool animation; /* the container can animate; one frame is read and written */
    bool lossy;     /* our encoder is lossy */
} pc_ccaps;

pc_ccaps pc_cformat_caps(pc_cformat f);
bool pc_cformat_is_enabled(pc_cformat f); /* readable or writable */
const char *pc_cformat_name(pc_cformat f);  /* "PNG", "Netpbm", "Radiance HDR", ... */
const char *pc_cformat_debug(pc_cformat f); /* the reference's {:?}: "Png", "OpenExr", ... */
/* Lower-case extensions without the dot; the first is preferred for writing. NULL-ended. */
const char *const *pc_cformat_extensions(pc_cformat f);
const char *pc_cformat_mime(pc_cformat f);
/* Hard encode limits (JPEG/GIF 65535, WebP 16384, ICO 256); false when there are none. */
bool pc_cformat_max_dimensions(pc_cformat f, uint32_t *w, uint32_t *h);

/* Formats enabled but intentionally not symmetric, with the reason. */
typedef struct pc_casymmetric {
    pc_cformat format;
    const char *reason;
} pc_casymmetric;
extern const pc_casymmetric PC_ASYMMETRIC_EXCEPTIONS[];
extern const size_t PC_ASYMMETRIC_EXCEPTIONS_LEN;

/* Identifies a format from its leading bytes (TGA, which has no magic, last). */
bool pc_cformat_detect(const uint8_t *b, size_t n, pc_cformat *out);
/* By extension: case-insensitive, with or without a dot, or a whole file name or path. */
bool pc_cformat_from_extension(const char *ext, pc_cformat *out);

#endif
