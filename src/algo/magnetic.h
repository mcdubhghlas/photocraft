/* Magnetic Lasso edge tracing (port of crates/algo/src/magnetic.rs).
 *
 * Between two fastening points the border is the cheapest 8-connected pixel path where pixels on
 * a strong, well-localised edge are cheap (the "live wire" of Mortensen & Barrett, SIGGRAPH
 * 1995), searched in a corridor around the pointer's path with a pull towards it. Pixels are read
 * lazily in 128 px blocks through a caller-supplied fetch and cached (at most 1024 blocks before
 * the cache starts over), so tracing on a 36 MP image reads only the area around the border.
 *
 * Ownership: pc_algo_magnetic_tracer_new / _free. Point lists are returned newly allocated
 * (free()). A tracer is not thread-safe (it caches); distinct tracers are independent. */
#ifndef PC_ALGO_MAGNETIC_H
#define PC_ALGO_MAGNETIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "geom/geom.h"

/* Longest pointer path pc_algo_magnetic_trace follows (px); a longer one is returned as is. */
#define PC_ALGO_MAGNETIC_MAX_GUIDE_LENGTH 262144.0
/* Detection width limits (px), as in Photoshop's options bar. */
#define PC_ALGO_MAGNETIC_WIDTH_MIN 1.0
#define PC_ALGO_MAGNETIC_WIDTH_MAX 256.0

/* Detection settings (the options bar). */
typedef struct pc_algo_magnetic_settings {
    double width;   /* px: the border follows only edges this close to the pointer's path */
    float contrast; /* 0..1: steps weaker than this, in every channel, are not edges */
} pc_algo_magnetic_settings;

/* Settings::default(): width 10, contrast 0.1. */
pc_algo_magnetic_settings pc_algo_magnetic_settings_default(void);
/* Settings::new: clamped to width 1..256, contrast 0.01..1; non-finite values take the default. */
pc_algo_magnetic_settings pc_algo_magnetic_settings_new(double width, float contrast);

/* Fills out[0..n) with the straight-alpha RGBA8 pixels of r (inside the image, row-major).
 * Returning false reads the block as transparent (the reference's wrong-length result). */
typedef bool (*pc_algo_magnetic_fetch)(void *ctx, pc_rect r, uint8_t (*out)[4], size_t n);

typedef struct pc_algo_magnetic_tracer pc_algo_magnetic_tracer;

/* A tracer for an image covering `bounds`. */
pc_status pc_algo_magnetic_tracer_new(pc_rect bounds, pc_algo_magnetic_tracer **out, pc_error *err);
void pc_algo_magnetic_tracer_free(pc_algo_magnetic_tracer *t); /* NULL-safe */
pc_rect pc_algo_magnetic_tracer_bounds(const pc_algo_magnetic_tracer *t);
/* Pixels read so far (cached). */
size_t pc_algo_magnetic_cached_pixels(const pc_algo_magnetic_tracer *t);

/* Where a fastening point near p goes: the centre of the most prominent edge within the
 * detection width, or p itself (kept inside the image) when none reaches the contrast. *some is
 * false (out untouched) when p is not finite or the image is empty. */
pc_status pc_algo_magnetic_snap(pc_algo_magnetic_tracer *t, pc_algo_magnetic_fetch fetch, void *ctx,
                                const double p[2], pc_algo_magnetic_settings s, double out[2], bool *some,
                                pc_error *err);
/* The border from `from` to `to` along the edges near `guide` (the pointer's path between them).
 * Starts at from and ends at to exactly (both kept inside the image); *n == 0 (and *out NULL)
 * when either is not finite or the image is empty. *out is newly allocated (free()). */
pc_status pc_algo_magnetic_trace(pc_algo_magnetic_tracer *t, pc_algo_magnetic_fetch fetch, void *ctx,
                                 const double from[2], const double to[2], const double (*guide)[2],
                                 size_t nguide, pc_algo_magnetic_settings s, double (**out)[2], size_t *n,
                                 pc_error *err);

/* Ramer-Douglas-Peucker within tol, keeping both ends (out: newly allocated, free()). */
pc_status pc_algo_magnetic_simplify(const double (*p)[2], size_t n, double tol, double (**out)[2],
                                    size_t *nout, pc_error *err);
/* Length of a polyline. */
double pc_algo_magnetic_length(const double (*p)[2], size_t n);

/* The polyline cut into pieces at most `piece` long (each starts where the last ended). Pieces
 * are stored back to back in *pts; piece k is pts[starts[k] .. starts[k + 1]) with
 * starts[npieces] == total. Both arrays newly allocated (free()); npieces 0 for an empty g. */
pc_status pc_algo_magnetic_split(const double (*g)[2], size_t n, double piece, double (**pts)[2],
                                 size_t **starts, size_t *npieces, pc_error *err);

#endif
