/* Patterns: the global Patt / Pat2 / Pat3 tagged blocks (8 / 16 / 32-bit documents) and
 * standalone .pat files (port of crates/psd/src/patterns.rs).
 *
 * Layout per the Photoshop File Formats Specification ("Patterns", "Virtual Memory Array List"):
 *   pattern  := version u32 (1), image mode u32, height i16, width i16, name (Unicode string),
 *               unique id (Pascal string, unpadded), [indexed: 768-byte RGB palette], VMA list
 *   VMA list := version u32 (3), length u32, rect (top, left, bottom, right i32),
 *               channel count u32, then count + 2 arrays (user mask, sheet mask)
 *   array    := written u32 (0 = absent), length u32 (0 = absent), depth u32, rect,
 *               depth u16, compression u8 (0 raw, 1 PackBits), data
 * In the tagged blocks each pattern is prefixed by its u32 length and padded to 4; a .pat file
 * is "8BPT", u16 version (1), u32 count, then the patterns without length prefixes. Photoshop
 * writes the channel count as 24 and marks only the real channels written; like ag-psd we take
 * the first written arrays as the colour channels and the next as transparency. Channels whose
 * rectangle is smaller than the pattern's are placed into zeroed full-size planes. All sizes
 * are validated before any plane is decoded. Thread-safe. */
#ifndef PC_PSD_PATTERNS_H
#define PC_PSD_PATTERNS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/compression.h"
#include "psd/types.h"

#define PC_PSD_PATTERN_MAX_EDGE 30000u /* largest pattern edge accepted */

/* One pattern tile with planar, big-endian samples. */
typedef struct pc_psd_pattern {
    uint32_t mode; /* PSD colour mode number (1 gray, 2 indexed, 3 RGB, 4 CMYK, 9 Lab, ...) */
    uint32_t width;
    uint32_t height;
    pc_psd_str name;          /* without the trailing NUL ("$$$/Patterns/...=Water" for built-ins) */
    pc_psd_str id;            /* unique id, usually a UUID */
    pc_psd_opt_bytes palette; /* indexed: 768 bytes of RGB triplets */
    uint16_t depth;           /* 1, 8, 16 or 32 */
    pc_psd_bytes *channels;   /* decoded colour planes, width x height samples each */
    size_t nchannels;
    pc_psd_opt_bytes alpha; /* decoded transparency plane */
} pc_psd_pattern;

/* Colour channels implied by a PSD colour mode (3 for RGB and Lab, 4 for CMYK, else 1). */
size_t pc_psd_pattern_mode_channels(uint32_t mode);

void pc_psd_pattern_fini(pc_psd_pattern *p);
void pc_psd_patterns_free(pc_psd_pattern *p, size_t n); /* fini each + free(p); NULL-safe */
bool pc_psd_pattern_eq(const pc_psd_pattern *a, const pc_psd_pattern *b);
pc_status pc_psd_pattern_clone(const pc_psd_pattern *p, pc_psd_pattern *out, pc_error *err);

/* Patt / Pat2 / Pat3 block data. Parsing stops at a zero length or when fewer than 4 bytes
 * remain; an empty block is an empty list (*out NULL, *n 0). */
pc_status pc_psd_parse_pattern_block(const uint8_t *data, size_t len, pc_psd_pattern **out, size_t *n,
                                     pc_error *err);
pc_status pc_psd_write_pattern_block(const pc_psd_pattern *p, size_t n, pc_psd_bytes *out, pc_error *err);
/* Tagged-block key for patterns of a document at `depth` bits: Pat2, Pat3, else Patt. */
void pc_psd_pattern_block_key(uint16_t depth, uint8_t out[4]);

/* .pat files (at most 100000 patterns are read). */
pc_status pc_psd_parse_pat_file(const uint8_t *data, size_t len, pc_psd_pattern **out, size_t *n, pc_error *err);
pc_status pc_psd_write_pat_file(const pc_psd_pattern *p, size_t n, pc_psd_bytes *out, pc_error *err);

/* Places a decoded channel plane of layout `src` at (x, y) inside a zeroed plane of layout
 * `dst` (one plane each, same depth). *plane is replaced on success; a full-size plane at the
 * origin is kept in place (only a 1-bit plane's unused row bits are cleared). Exposed for tests. */
pc_status pc_psd_pattern_place_plane(pc_psd_bytes *plane, const pc_psd_layout *src, size_t x, size_t y,
                                     const pc_psd_layout *dst, pc_error *err);

#endif
