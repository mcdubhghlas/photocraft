/* Grayscale bitmaps for sampled tips and texture patterns (port of crates/paint/src/tile.rs).
 *
 * pc_paint_gray_tile: 16-bit samples (0..65535 <-> 0..1), row-major. The samples live in an
 * immutable, atomically reference-counted block, so copying a tile (and therefore a whole brush)
 * never allocates and never fails; a tile is never modified after it is built. A tile whose
 * sample count isn't width * height is representable (the reference allows it) but "invalid":
 * renderers ignore it.
 *
 * pc_paint_mips (a float mip pyramid of a tile) and pc_paint_pattern_image (a tiling float
 * texture) are heap objects with atomic reference counts, shared by brush contexts.
 *
 * Also the tile serde form's own base64 (lenient about whitespace, unlike src/base/base64.h),
 * which commands use for file bytes too.
 *
 * Thread safety: tiles, mips and pattern images are immutable once built; copies/retains and
 * releases may happen on any thread. Failure: constructors return PC_ERR_NOMEM, or PC_ERR_LIMIT
 * when a size overflows or exceeds PC_MAX_ALLOC; out-params are written only on success. */
#ifndef PC_PAINT_TILE_H
#define PC_PAINT_TILE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"

/* Opaque shared sample block. */
typedef struct pc_paint_tile_data pc_paint_tile_data;

typedef struct pc_paint_gray_tile {
    uint32_t width;
    uint32_t height;
    pc_paint_tile_data *data; /* NULL = no samples; one reference owned by this tile */
} pc_paint_gray_tile;

#define PC_PAINT_GRAY_TILE_INIT {0, 0, NULL}

/* q16 of the reference: clamp to 0..1, scale, round half up (NaN -> 0). */
uint16_t pc_paint_q16(float v);

typedef float (*pc_paint_tile_fn)(void *ctx, uint32_t x, uint32_t y);
/* Build from a function returning 0..1 values (called row by row). */
pc_status pc_paint_gray_tile_from_fn(uint32_t width, uint32_t height, pc_paint_tile_fn f, void *ctx,
                                     pc_paint_gray_tile *out, pc_error *err);
/* Build from width * height floats; another count is PC_ERR_INVALID (the reference panics). */
pc_status pc_paint_gray_tile_from_f32(uint32_t width, uint32_t height, const float *v, size_t n,
                                      pc_paint_gray_tile *out, pc_error *err);
/* Build from raw samples; any count is accepted (a mismatch makes the tile invalid). */
pc_status pc_paint_gray_tile_from_u16(uint32_t width, uint32_t height, const uint16_t *v, size_t n,
                                      pc_paint_gray_tile *out, pc_error *err);
/* *dst = a copy sharing src's samples (dst is overwritten, not finalised). Never fails. */
void pc_paint_gray_tile_copy(pc_paint_gray_tile *dst, const pc_paint_gray_tile *src);
void pc_paint_gray_tile_fini(pc_paint_gray_tile *t); /* NULL-safe; leaves an empty tile */
size_t pc_paint_gray_tile_len(const pc_paint_gray_tile *t);
/* The samples (borrowed while t holds them), NULL when there are none. */
const uint16_t *pc_paint_gray_tile_samples(const pc_paint_gray_tile *t);
/* Sample (x, y) as 0..1; 0 outside the samples (the reference panics). */
float pc_paint_gray_tile_get(const pc_paint_gray_tile *t, uint32_t x, uint32_t y);
/* All samples as 0..1 floats, newly allocated (free()); *out is NULL when empty. */
pc_status pc_paint_gray_tile_to_f32(const pc_paint_gray_tile *t, float **out, size_t *n, pc_error *err);
bool pc_paint_gray_tile_is_valid(const pc_paint_gray_tile *t);
bool pc_paint_gray_tile_eq(const pc_paint_gray_tile *a, const pc_paint_gray_tile *b);

/* Standard base64 with padding: appends to *out. */
pc_status pc_paint_b64_encode(const uint8_t *b, size_t n, pc_buf *out, pc_error *err);
/* Decodes standard base64, ignoring ASCII whitespace, into *out (replacing its contents).
 * Invalid input is PC_ERR_FORMAT ("invalid base64"); *out is unchanged on failure. Like the
 * reference it accepts any run of trailing '=' in the last quantum. */
pc_status pc_paint_b64_decode(const char *s, size_t n, pc_buf *out, pc_error *err);

/* ---- sampling ---- */

typedef struct pc_paint_mip_level {
    size_t width;
    size_t height;
    float *data; /* width * height values */
} pc_paint_mip_level;

/* A float mip pyramid for sampling a tile at any scale without aliasing; level 0 = full size. */
typedef struct pc_paint_mips {
    pc_paint_mip_level *levels;
    size_t count; /* >= 1 */
    atomic_size_t refs;
} pc_paint_mips;

/* Builds the pyramid of a tile with at least one sample (else PC_ERR_INVALID). One reference. */
pc_status pc_paint_mips_new(const pc_paint_gray_tile *t, pc_paint_mips **out, pc_error *err);
pc_paint_mips *pc_paint_mips_retain(pc_paint_mips *m); /* NULL-safe; returns m */
void pc_paint_mips_release(pc_paint_mips *m);          /* NULL-safe */
/* Mip level for drawing the tile at `px` pixels across its larger side. */
size_t pc_paint_mips_level_for(const pc_paint_mips *m, float px);
/* Bilinear sample at normalised (u, v) in 0..1; outside (and an out-of-range level) = 0. */
float pc_paint_mips_sample_clamped(const pc_paint_mips *m, size_t level, float u, float v);

/* A tiling float pattern (texture), sampled with wrap-around. */
typedef struct pc_paint_pattern_image {
    size_t width;  /* >= 1 */
    size_t height; /* >= 1 */
    float *data;   /* width * height values, owned */
    atomic_size_t refs;
} pc_paint_pattern_image;

/* A new image of width * height zeroes (both >= 1, else PC_ERR_INVALID). One reference. */
pc_status pc_paint_pattern_image_new(size_t width, size_t height, pc_paint_pattern_image **out,
                                     pc_error *err);
pc_paint_pattern_image *pc_paint_pattern_image_retain(pc_paint_pattern_image *p); /* NULL-safe */
void pc_paint_pattern_image_release(pc_paint_pattern_image *p);                   /* NULL-safe */
float pc_paint_pattern_sample_wrap(const pc_paint_pattern_image *p, float x, float y);

#endif
