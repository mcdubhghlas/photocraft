/* Small shared types of the PSD/PSB module (port of crates/psd: header.rs, compression.rs and
 * blend.rs enums, error.rs messages).
 *
 * The Rust enums that carry an `Unknown(v)` variant (ColorMode, Compression, SectionType,
 * BlendMode) are stored here as their raw on-disk value, with named constants for the known
 * ones: unknown values round-trip by construction.
 *
 * Errors (PsdError → pc_status; messages are the reference's Display text, which the psd-info
 * goldens record):
 *   UnexpectedEof     PC_ERR_FORMAT       "unexpected end of data at offset O (needed N more bytes)"
 *   InvalidSignature  PC_ERR_FORMAT       "invalid signature [a, b, c, d], expected X"
 *   UnsupportedVersion PC_ERR_UNSUPPORTED "unsupported file version V"
 *   LimitExceeded     PC_ERR_LIMIT        "limit exceeded: ..."
 *   Invalid           PC_ERR_FORMAT       "invalid data: ..." (PC_ERR_INVALID for caller misuse
 *                                         of the builder/encoders)
 *   Decompress        PC_ERR_FORMAT       "decompression failed: ..."
 *   Unsupported       PC_ERR_UNSUPPORTED  "unsupported: ..." */
#ifndef PC_PSD_TYPES_H
#define PC_PSD_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"

/* An owned byte vector (data may be NULL when len == 0). */
typedef struct pc_psd_bytes {
    uint8_t *data;
    size_t len;
} pc_psd_bytes;

/* Option<Vec<u8>>: has == false is None. */
typedef struct pc_psd_opt_bytes {
    bool has;
    pc_psd_bytes v;
} pc_psd_opt_bytes;

void pc_psd_bytes_fini(pc_psd_bytes *b);                                         /* NULL-safe */
pc_status pc_psd_bytes_set(pc_psd_bytes *b, const void *data, size_t len, pc_error *err); /* replaces */
pc_status pc_psd_bytes_clone(const pc_psd_bytes *b, pc_psd_bytes *out, pc_error *err);
bool pc_psd_bytes_eq(const pc_psd_bytes *a, const pc_psd_bytes *b);
bool pc_psd_opt_bytes_eq(const pc_psd_opt_bytes *a, const pc_psd_opt_bytes *b);
pc_status pc_psd_opt_bytes_clone(const pc_psd_opt_bytes *b, pc_psd_opt_bytes *out, pc_error *err);
void pc_psd_opt_bytes_fini(pc_psd_opt_bytes *b);

/* Human-readable warnings about data that was skipped, dropped or kept unconverted (owned
 * NUL-terminated strings). */
typedef struct pc_psd_warnings {
    char **v;
    size_t n;
} pc_psd_warnings;

void pc_psd_warnings_fini(pc_psd_warnings *w); /* NULL-safe */

/* An owned UTF-8 string with its length (Rust String): may hold U+0000; s is always
 * NUL-terminated after len bytes (s may be NULL when len == 0). */
typedef struct pc_psd_str {
    char *s;
    size_t len;
} pc_psd_str;

void pc_psd_str_fini(pc_psd_str *s); /* NULL-safe */
/* Copies len bytes (not validated). Replaces *out. */
pc_status pc_psd_str_set(pc_psd_str *out, const char *s, size_t len, pc_error *err);
pc_status pc_psd_str_clone(const pc_psd_str *s, pc_psd_str *out, pc_error *err);
bool pc_psd_str_eq(const pc_psd_str *a, const pc_psd_str *b);
/* The bytes, "" for an empty string. */
static inline const char *pc_psd_str_c(const pc_psd_str *s) { return s->s != NULL ? s->s : ""; }

/* File format version. */
typedef enum pc_psd_version { PC_PSD_V1 = 1, PC_PSD_V2 = 2 } pc_psd_version; /* PSD, PSB */

/* Color modes (raw u16). */
enum {
    PC_PSD_MODE_BITMAP = 0,
    PC_PSD_MODE_GRAYSCALE = 1,
    PC_PSD_MODE_INDEXED = 2,
    PC_PSD_MODE_RGB = 3,
    PC_PSD_MODE_CMYK = 4,
    PC_PSD_MODE_MULTICHANNEL = 7,
    PC_PSD_MODE_DUOTONE = 8,
    PC_PSD_MODE_LAB = 9
};
/* Colour (non-alpha) channels of a mode when fixed; 0 for Multichannel and unknown modes. */
uint16_t pc_psd_mode_color_channels(uint16_t mode);

/* Compression methods (raw u16): 0 raw, 1 PackBits RLE, 2 zip, 3 zip with prediction. */
enum { PC_PSD_RAW = 0, PC_PSD_RLE = 1, PC_PSD_ZIP = 2, PC_PSD_ZIP_PREDICT = 3 };
#define PC_PSD_COMPRESSION_KNOWN(c) ((c) <= 3)

/* Section divider types (raw u32). */
enum { PC_PSD_SECTION_OTHER = 0, PC_PSD_SECTION_OPEN = 1, PC_PSD_SECTION_CLOSED = 2, PC_PSD_SECTION_DIVIDER = 3 };

/* Blend modes: the stored 4-byte key ("norm", "mul ", ...). The 28 known keys in BlendMode::ALL
 * order (PassThrough first); unknown keys are kept verbatim by the model. */
#define PC_PSD_BLEND_COUNT 28
extern const char PC_PSD_BLEND_KEYS[PC_PSD_BLEND_COUNT][5];
/* Index of a known key in PC_PSD_BLEND_KEYS, or -1. */
int pc_psd_blend_index(const uint8_t key[4]);

/* A rectangle as stored in PSD files: top, left (inclusive), bottom, right (exclusive). */
typedef struct pc_psd_rect {
    int32_t top;
    int32_t left;
    int32_t bottom;
    int32_t right;
} pc_psd_rect;

/* From origin and size (saturating; sizes above INT32_MAX count as INT32_MAX). */
pc_psd_rect pc_psd_rect_from_xywh(int32_t left, int32_t top, uint32_t width, uint32_t height);
static inline int64_t pc_psd_rect_width(pc_psd_rect r) { return (int64_t)r.right - r.left; }
static inline int64_t pc_psd_rect_height(pc_psd_rect r) { return (int64_t)r.bottom - r.top; }
static inline bool pc_psd_rect_is_empty(pc_psd_rect r) { return pc_psd_rect_width(r) <= 0 || pc_psd_rect_height(r) <= 0; }
static inline bool pc_psd_rect_eq(pc_psd_rect a, pc_psd_rect b)
{
    return a.top == b.top && a.left == b.left && a.bottom == b.bottom && a.right == b.right;
}
/* Validated size: "rectangle has negative size" / "layer dimensions exceed 300000". */
pc_status pc_psd_rect_size(pc_psd_rect r, size_t *w, size_t *h, pc_error *err);

/* Limits (header.rs, layer.rs, compression.rs). */
#define PC_PSD_MAX_DIMENSION 300000u
#define PC_PSD_MAX_CHANNELS 56u
#define PC_PSD_MAX_LAYER_CHANNELS 64u
/* Most decoded bytes one decode call may produce (8 GiB on 64-bit, 2 GiB elsewhere). */
#define PC_PSD_MAX_DECODED_BYTES ((uint64_t)(SIZE_MAX > 0xFFFFFFFFu ? (8ull << 30) : (2ull << 30)))

#endif
