/* photocraft codecs: symmetric, depth- and metadata-preserving codecs for flat raster formats
 * (port of crates/codecs; heif is the optional libheif path).
 *
 *  - Every format written can also be read (PC_ASYMMETRIC_EXCEPTIONS lists the exceptions).
 *  - Bit depth (8/16-bit integer, 16/32-bit float), ICC profiles and basic metadata (EXIF, XMP,
 *    DPI, text) are kept where the format allows; pc_cfidelity_warnings says what an export
 *    will lose.
 *  - The API is I/O free (bytes in, bytes out). Decoders enforce pc_climits before allocating
 *    and never crash on malformed input.
 *
 * Errors follow the reference's CodecError and its messages:
 *   UnknownFormat -> PC_ERR_NOT_FOUND   "unrecognized image format"
 *   Unsupported   -> PC_ERR_UNSUPPORTED "<Fmt>: <reason>"
 *   Malformed     -> PC_ERR_FORMAT      "<Fmt>: malformed data: <message>"
 *   LimitExceeded -> PC_ERR_LIMIT       "limit exceeded: <message>"
 *   Encode        -> PC_ERR_UNSUPPORTED "<Fmt>: encode failed: <message>"
 *   InvalidImage  -> PC_ERR_INVALID     "invalid image: <message>"
 * where <Fmt> is pc_cformat_debug. Thread-safe. */
#ifndef PC_CODECS_CODECS_H
#define PC_CODECS_CODECS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "codecs/format.h"
#include "codecs/image.h"

/* ---- options ---- */

/* Decompression-bomb guards: dimension checks precede pixel allocation. PNG also applies
 * max_alloc to the decoded text metadata (keywords and values, as UTF-8). */
typedef struct pc_climits {
    uint32_t max_width;
    uint32_t max_height;
    uint64_t max_pixels; /* width * height */
    uint64_t max_alloc;  /* bytes of the decoded pixel buffer */
} pc_climits;

pc_climits pc_climits_default(void); /* 2^18 x 2^18, 2^30 pixels, 8 GiB (2 GiB on 32-bit) */
pc_climits pc_climits_none(void);
/* "invalid image: zero-sized image WxH", or limit errors. */
pc_status pc_climits_check(const pc_climits *l, uint32_t w, uint32_t h, pc_clayout layout, pc_csample s,
                           pc_error *err);

typedef struct pc_cdecode_opts {
    pc_climits limits;
    bool keep_orientation; /* keep the pixels as stored instead of applying EXIF/TIFF orientation */
} pc_cdecode_opts;

pc_cdecode_opts pc_cdecode_opts_default(void);

typedef enum pc_png_compression { PC_PNG_NONE, PC_PNG_FAST, PC_PNG_DEFAULT, PC_PNG_BEST } pc_png_compression;
typedef enum pc_tiff_compression { PC_TIFF_NONE, PC_TIFF_LZW, PC_TIFF_DEFLATE, PC_TIFF_PACKBITS } pc_tiff_compression;
typedef enum pc_exr_compression { PC_EXR_NONE, PC_EXR_RLE, PC_EXR_ZIP1, PC_EXR_ZIP16, PC_EXR_PIZ } pc_exr_compression;

typedef struct pc_cencode_opts {
    uint8_t jpeg_quality;          /* 1..=100 */
    bool jpeg_chroma_subsampling;  /* 4:2:0 (false = 4:4:4) */
    pc_png_compression png_compression;
    bool png_interlaced; /* Adam7 */
    bool webp_lossless;
    uint8_t webp_quality; /* lossy, 0..=100 */
    pc_tiff_compression tiff_compression;
    bool tiff_bigtiff; /* always BigTIFF (else only when the file could pass 4 GiB) */
    pc_exr_compression exr_compression;
    bool embed_icc;
    bool embed_metadata; /* EXIF/XMP/DPI/text */
} pc_cencode_opts;

pc_cencode_opts pc_cencode_opts_default(void);

/* ---- decoding and encoding ---- */

/* Detects the format and decodes (default limits / given options). */
pc_status pc_cdecode(const uint8_t *b, size_t n, pc_cimage *out, pc_error *err);
pc_status pc_cdecode_with(const uint8_t *b, size_t n, const pc_cdecode_opts *o, pc_cimage *out, pc_error *err);
pc_status pc_cdecode_as(pc_cformat f, const uint8_t *b, size_t n, pc_cimage *out, pc_error *err);
pc_status pc_cdecode_as_with(pc_cformat f, const uint8_t *b, size_t n, const pc_cdecode_opts *o, pc_cimage *out,
                             pc_error *err);
/* Encodes (conversions follow the plan pc_cfidelity_warnings_with reports); *out replaced. o may be
 * NULL for pc_cencode_opts_default(). */
pc_status pc_cencode(const pc_cimage *img, pc_cformat f, const pc_cencode_opts *o, pc_buf *out, pc_error *err);

/* Deep OpenEXR samples of the first deep part (PC_ERR_UNSUPPORTED when there is none). */
pc_status pc_cdecode_deep_exr(const uint8_t *b, size_t n, const pc_climits *l, pc_cdeep *out, pc_error *err);

/* ---- fidelity warnings ---- */

typedef enum pc_cfidelity_kind {
    PC_FW_WRITE_UNSUPPORTED, /* format */
    PC_FW_DEPTH_REDUCED,     /* from, to */
    PC_FW_RANGE_CLIPPED,
    PC_FW_ALPHA_DISCARDED,
    PC_FW_ALPHA_BINARIZED,
    PC_FW_CMYK_CONVERTED, /* to */
    PC_FW_COLOR_TO_GRAY,
    PC_FW_PALETTE_QUANTIZED,
    PC_FW_LOSSY_COMPRESSION,
    PC_FW_ICC_DROPPED,
    PC_FW_EXIF_DROPPED,
    PC_FW_XMP_DROPPED,
    PC_FW_DPI_DROPPED,
    PC_FW_TEXT_DROPPED,
    PC_FW_METADATA_TOO_LARGE, /* what ("EXIF"/"XMP"), bytes */
    PC_FW_DIMENSIONS_EXCEEDED /* max_width, max_height */
} pc_cfidelity_kind;

typedef struct pc_cfidelity {
    pc_cfidelity_kind kind;
    pc_cformat format;
    pc_csample from;
    pc_csample to;
    pc_clayout to_layout;
    const char *what;
    size_t bytes;
    uint32_t max_width;
    uint32_t max_height;
} pc_cfidelity;

bool pc_cfidelity_is_fatal(const pc_cfidelity *w);
bool pc_cfidelity_eq(const pc_cfidelity *a, const pc_cfidelity *b);
void pc_cfidelity_text(const pc_cfidelity *w, char *out, size_t cap);
/* Fills up to cap warnings into out (in the reference's order) and returns how many there are.
 * The _with form's o may be NULL for the default options. */
size_t pc_cfidelity_warnings(const pc_cimage *img, pc_cformat f, pc_cfidelity *out, size_t cap);
size_t pc_cfidelity_warnings_with(const pc_cimage *img, pc_cformat f, const pc_cencode_opts *o, pc_cfidelity *out,
                                  size_t cap);

/* ---- TIFF pages ---- */

typedef enum pc_tiff_page_kind { PC_TIFF_PAGE, PC_TIFF_REDUCED, PC_TIFF_MASK } pc_tiff_page_kind;

typedef struct pc_tiff_page {
    uint64_t ifd_offset;
    bool has_parent; /* a SubIFD: parent is the index of the directory listing it */
    size_t parent;
    pc_tiff_page_kind kind;
    uint32_t width; /* 0 when unreadable */
    uint32_t height;
    uint16_t samples_per_pixel;
    uint16_t bits_per_sample;
    uint16_t sample_format; /* 1 unsigned, 2 signed, 3 float */
    bool has_photometric;
    uint16_t photometric;
    uint16_t compression;
    bool tiled;
    bool planar;
} pc_tiff_page;

typedef struct pc_tiff_info {
    bool big_tiff;
    bool little_endian;
    pc_tiff_page *pages; /* main chain in file order, each followed by its SubIFDs */
    size_t npages;
    bool complete; /* false when the walk stopped early (damage, cycle, > 10000 directories) */
} pc_tiff_info;

void pc_tiff_info_fini(pc_tiff_info *i);
/* The page opened by default (first full-resolution main-chain page, else 0); false if none. */
bool pc_tiff_info_default_page(const pc_tiff_info *i, size_t *page);
size_t pc_tiff_info_page_count(const pc_tiff_info *i);
pc_status pc_ctiff_info(const uint8_t *b, size_t n, pc_tiff_info *out, pc_error *err);
pc_status pc_cdecode_tiff_page(const uint8_t *b, size_t n, size_t page, const pc_cdecode_opts *o, pc_cimage *out,
                               pc_error *err);
uint16_t pc_ctiff_orientation(const uint8_t *b, size_t n);
uint16_t pc_ctiff_page_orientation(const uint8_t *b, size_t n, size_t page);
/* The Photoshop private tags of the first directory (borrowed slices into b; data NULL when
 * absent). */
void pc_ctiff_photoshop_tags(const uint8_t *b, size_t n, const uint8_t **res, size_t *res_len,
                             const uint8_t **layers, size_t *layers_len);
bool pc_ctiff_writes_little_endian(void);

/* An 8-bit palette PNG: row-major indices, RGB palette (1..=256 entries), optional transparent
 * entry. *out replaced. */
pc_status pc_cencode_png_indexed(uint32_t w, uint32_t h, const uint8_t *indices, size_t n,
                                 const uint8_t (*palette)[3], size_t npal, int transparent, pc_buf *out,
                                 pc_error *err);

#endif
