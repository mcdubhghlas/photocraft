/* The encoder settings a lossy file was written with, read back from its own data, so a save
 * can write it again the way it came (port of crates/codecs/src/source_settings.rs):
 *  - JPEG: the quality (1-100, the IJG scale) whose scaled ITU-T T.81 Annex K tables match the
 *    file's quantisation tables best;
 *  - WebP: VP8L is lossless; VP8 is lossy, and its base quantiser index maps back to a quality
 *    on the scale our encoder (libwebp) uses.
 * Hostile bytes give "none", never a crash. Thread-safe. */
#ifndef PC_CODECS_SOURCE_SETTINGS_H
#define PC_CODECS_SOURCE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The quality a JPEG was most likely saved at; false without a usable luminance table. */
bool pc_jpeg_quality(const uint8_t *b, size_t n, uint8_t *quality);

typedef enum pc_webp_encoding_kind { PC_WEBP_LOSSLESS, PC_WEBP_LOSSY } pc_webp_encoding_kind;

typedef struct pc_webp_encoding {
    pc_webp_encoding_kind kind;
    bool has_quality; /* lossy: the frame header could be read */
    uint8_t quality;  /* 0-100 */
} pc_webp_encoding;

/* The compression of a WebP file's first image (also inside an animation frame); false when it
 * isn't a WebP or holds no image chunk. */
bool pc_webp_encoding_of(const uint8_t *b, size_t n, pc_webp_encoding *out);

/* The VP8 base quantiser index (0-127) our encoder writes for quality q (0-100). */
uint8_t pc_webp_quantizer_index(uint8_t quality);

#endif
