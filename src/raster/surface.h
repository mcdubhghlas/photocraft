/* Sparse, copy-on-write tiled surfaces in any pixel format (port of crates/raster/src/lib.rs).
 *
 * A pc_surface is an infinite plane of PC_TILE_SIZE^2 tiles. Missing tiles read as the
 * surface's default pixel (transparent for layers, white for "reveal all" masks). Tiles are
 * reference counted, so cloning a surface is O(tiles) and mutation copies only the touched
 * tiles: undo snapshots, background jobs and autosave are cheap.
 *
 * Ownership: pc_surface is a plain struct for embedding (fields are private); pc_surface_init*
 * / pc_surface_fini. Tiles returned as `const pc_raster_tile *` are borrowed (valid until the
 * surface is mutated or finalised); retain them to keep them.
 *
 * Thread safety: a surface is not synchronised. Concurrent const calls are fine; a mutation
 * needs exclusive access. Tiles may be shared between surfaces on different threads (atomic
 * reference counts; a shared tile is copied before it is written).
 *
 * Failure: functions that may allocate return pc_status (PC_ERR_NOMEM; PC_ERR_LIMIT when a
 * rectangle spans more than PC_SURFACE_MAX_TILE_SPAN tiles or an output would exceed
 * PC_MAX_ALLOC; PC_ERR_INVALID for mismatched buffer lengths, where the reference panics). On
 * failure a surface stays valid; a multi-tile write may have been partly applied. */
#ifndef PC_RASTER_SURFACE_H
#define PC_RASTER_SURFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "color/color.h"
#include "geom/geom.h"
#include "raster/pixel.h"

/* Upper bound on the tiles one operation may touch (2^24 tiles = a 1M x 1M pixel area of
 * solid fill; a shared tile costs only a table entry). */
#define PC_SURFACE_MAX_TILE_SPAN ((uint64_t)1 << 24)

/* Pixel storage for one tile: PC_TILE_SIZE^2 * bytes_per_pixel bytes, row-major, interleaved
 * channels, native byte order. Reference counted and immutable while shared. */
typedef struct pc_raster_tile pc_raster_tile;

pc_raster_tile *pc_raster_tile_retain(pc_raster_tile *t); /* NULL-safe; returns t */
void pc_raster_tile_release(pc_raster_tile *t);           /* NULL-safe */
const uint8_t *pc_raster_tile_bytes(const pc_raster_tile *t);
size_t pc_raster_tile_len(const pc_raster_tile *t);
bool pc_raster_tile_eq(const pc_raster_tile *a, const pc_raster_tile *b); /* same bytes */

/* A tile and its coordinate (take_tiles / put_tiles / sorted listings). */
typedef struct pc_tile_entry {
    pc_tile coord;
    pc_raster_tile *tile; /* one reference, owned by whoever holds the entry */
} pc_tile_entry;
/* Releases every entry's tile and frees the array. NULL-safe. */
void pc_tile_entries_free(pc_tile_entry *entries, size_t n);

typedef struct pc_surface {
    /* private */
    pc_pixel_format format;
    uint8_t default_pixel[PC_RASTER_MAX_PIXEL_BYTES];
    size_t bpp;
    pc_tile_entry *slots; /* open-addressing hash table; tile == NULL marks an empty slot */
    size_t cap;           /* 0 or a power of two */
    size_t count;
} pc_surface;

/* A zero (transparent) surface. Never fails. */
void pc_surface_init(pc_surface *s, pc_pixel_format f);
/* A surface whose untouched pixels read as `px` (min(n, channels) normalised values). */
void pc_surface_init_default(pc_surface *s, pc_pixel_format f, const float *px, size_t n);
void pc_surface_fini(pc_surface *s); /* NULL-safe; the struct may be re-initialised */
/* Shares every tile (cheap). `out` is initialised on success only. */
pc_status pc_surface_clone(const pc_surface *s, pc_surface *out, pc_error *err);
/* Same format, default pixel and tile contents. */
bool pc_surface_eq(const pc_surface *a, const pc_surface *b);

pc_pixel_format pc_surface_format(const pc_surface *s);
size_t pc_surface_channels(const pc_surface *s);
size_t pc_surface_tile_count(const pc_surface *s);
/* The tile at c, borrowed, or NULL when not allocated. */
const pc_raster_tile *pc_surface_tile(const pc_surface *s, pc_tile c);
/* Unordered iteration: start with *cursor = 0; returns false when done. */
bool pc_surface_next_tile(const pc_surface *s, size_t *cursor, pc_tile *coord, const pc_raster_tile **tile);
/* Every tile in (tx, ty) order (the reference's BTreeMap order), each retained; free with
 * pc_tile_entries_free. *out is NULL when there are no tiles. */
pc_status pc_surface_tiles_sorted(const pc_surface *s, pc_tile_entry **out, size_t *n, pc_error *err);
/* The default pixel as `channels` floats, and as encoded bytes (bytes_per_pixel). */
void pc_surface_default_pixel(const pc_surface *s, float out[PC_RASTER_MAX_CHANNELS]);
const uint8_t *pc_surface_default_bytes(const pc_surface *s);

/* Mutable bytes of tile c, allocating it (filled with the default pixel) or un-sharing it.
 * The pointer is valid until the next mutation of the surface. */
pc_status pc_surface_tile_mut(pc_surface *s, pc_tile c, uint8_t **data, pc_error *err);

/* Coarse bounds: union of allocated tile rects. */
pc_rect pc_surface_tile_bounds(const pc_surface *s);
/* Exact bounds of pixels that differ from the default pixel. */
pc_rect pc_surface_content_bounds(const pc_surface *s);
/* Drops tiles that are entirely the default pixel. */
void pc_surface_prune(pc_surface *s);

/* One pixel as `channels` normalised floats. */
void pc_surface_read_pixel(const pc_surface *s, int32_t x, int32_t y, float *out);
/* Writes min(n, channels) channels of one pixel. */
pc_status pc_surface_write_pixel(pc_surface *s, int32_t x, int32_t y, const float *px, size_t n, pc_error *err);
/* Channel c of one pixel (0 when c >= channels). */
float pc_surface_sample_channel(const pc_surface *s, int32_t x, int32_t y, size_t c);
/* Channel c at (x0 + i * step, y) for i in [0, n) (step < 1 counts as 1), one tile lookup per
 * run. Coordinates wrap like the reference's release build. */
void pc_surface_sample_row_strided(const pc_surface *s, int32_t y, int32_t x0, int32_t step, size_t c, float *out,
                                   size_t n);
/* Composite-friendly straight RGBA of one pixel. */
void pc_surface_rgba(const pc_surface *s, int32_t x, int32_t y, float out[4]);

/* Is any allocated tile inside r's tile range? (else r reads as the default pixel) */
bool pc_surface_has_tiles_in(const pc_surface *s, pc_rect r);

/* r as interleaved normalised floats (w * h * channels), newly allocated (free()); *out is
 * NULL for an empty r. */
pc_status pc_surface_read_region(const pc_surface *s, pc_rect r, float **out, size_t *len, pc_error *err);
/* The same into a caller buffer of exactly w * h * channels floats (else PC_ERR_INVALID). */
pc_status pc_surface_read_region_into(const pc_surface *s, pc_rect r, float *out, size_t len, pc_error *err);
/* r as straight RGBA floats / RGBA8 into `out` of exactly w * h entries (else PC_ERR_INVALID),
 * converting from the surface's model. */
pc_status pc_surface_read_rgba_into(const pc_surface *s, pc_rect r, float (*out)[4], size_t len, pc_error *err);
pc_status pc_surface_read_rgba8_into(const pc_surface *s, pc_rect r, uint8_t (*out)[4], size_t len, pc_error *err);
/* Writes w * h * channels interleaved normalised floats into r (else PC_ERR_INVALID). */
pc_status pc_surface_write_region(pc_surface *s, pc_rect r, const float *data, size_t len, pc_error *err);

/* Takes the tiles covering r out of the surface, in r's tile order (missing ones are one shared
 * tile of the default pixel), so they can be edited elsewhere, e.g. in parallel. Each entry
 * owns one reference; un-share before writing (pc_raster_tile_make_mut). Give them back with
 * pc_surface_put_tiles. */
pc_status pc_surface_take_tiles(pc_surface *s, pc_rect r, pc_tile_entry **out, size_t *n, pc_error *err);
/* Puts tiles back, each replacing the tile at its coordinate; takes over every entry's
 * reference (also on failure) and frees nothing else: the caller frees the array. */
pc_status pc_surface_put_tiles(pc_surface *s, pc_tile_entry *entries, size_t n, pc_error *err);
/* Makes *t uniquely owned (copying it if shared) and returns its bytes, or NULL on NOMEM. */
uint8_t *pc_raster_tile_make_mut(pc_raster_tile **t);
/* A tile of this surface's format filled with `px`; sharing one across coordinates fills them
 * without copying. */
pc_status pc_surface_solid_tile(const pc_surface *s, const float *px, size_t n, pc_raster_tile **out,
                                pc_error *err);

/* Fills r with one pixel (px needs `channels` values, else PC_ERR_INVALID). Fully covered tiles
 * share one tile, so a solid fill costs one tile however large r is. */
pc_status pc_surface_fill_rect(pc_surface *s, pc_rect r, const float *px, size_t n, pc_error *err);

/* Interleaved *encoded* bytes of the surface's format covering r (w * h * bpp). */
pc_status pc_surface_write_interleaved(pc_surface *s, pc_rect r, const uint8_t *bytes, size_t len, pc_error *err);
pc_status pc_surface_from_interleaved(pc_pixel_format f, pc_rect r, const uint8_t *bytes, size_t len,
                                      pc_surface *out, pc_error *err);
/* r as encoded bytes, newly allocated (free()); *out is NULL for an empty r. */
pc_status pc_surface_to_interleaved(const pc_surface *s, pc_rect r, uint8_t **out, size_t *len, pc_error *err);

/* The surface moved by whole pixels (encoded samples copied tile to tile; moves by whole tiles
 * share the tiles). `content` must hold every non-default pixel (content_bounds, say): only
 * tiles it reaches are kept. `out` is initialised on success only. */
pc_status pc_surface_translated(const pc_surface *s, int32_t dx, int32_t dy, pc_rect content, pc_surface *out,
                                pc_error *err);
/* Converted to another format (depth and/or model, via pc_convert_pixel). */
pc_status pc_surface_convert(const pc_surface *s, pc_pixel_format to, pc_surface *out, pc_error *err);
/* A new tile filled with the default pixel (what a missing tile reads as); caller releases it. */
pc_status pc_surface_default_tile(const pc_surface *s, pc_raster_tile **out, pc_error *err);
/* Applies f to every colour sample (not alpha), the default pixel included: a tone-curve change
 * such as linearising when a document becomes 32-bit. On failure (PC_ERR_NOMEM) s is unchanged. */
pc_status pc_surface_map_color(pc_surface *s, float (*f)(void *ctx, float v), void *ctx, pc_error *err);

#endif
