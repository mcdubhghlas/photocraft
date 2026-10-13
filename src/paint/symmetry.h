/* Straight painting symmetry axes (port of crates/paint/src/symmetry.rs): the geometry only,
 * independent of pixels and the UI. A stroke is painted once per pass: the original points plus
 * their reflections (one mirror, or three for Dual: both axes and the point reflection).
 *
 * Thread safety: plain data. */
#ifndef PC_PAINT_SYMMETRY_H
#define PC_PAINT_SYMMETRY_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"
#include "paint/brush.h"

#define PC_PAINT_SYMMETRY_MAX_COORDINATE 1000000.0

/* serde (lowercase) ids: "vertical", "horizontal", "dual", "diagonal". */
typedef enum pc_paint_symmetry_mode {
    PC_PAINT_SYMMETRY_VERTICAL,
    PC_PAINT_SYMMETRY_HORIZONTAL,
    PC_PAINT_SYMMETRY_DUAL,
    PC_PAINT_SYMMETRY_DIAGONAL
} pc_paint_symmetry_mode;
#define PC_PAINT_SYMMETRY_MODE_COUNT 4

typedef struct pc_paint_symmetry {
    pc_paint_symmetry_mode mode;
    double center[2];
    double rotation; /* clockwise degrees from the mode's initial direction (document y down) */
} pc_paint_symmetry;

const char *pc_paint_symmetry_mode_id(pc_paint_symmetry_mode m); /* NULL out of range */
bool pc_paint_symmetry_mode_from_id(const char *id, size_t len, pc_paint_symmetry_mode *out);

/* A symmetry with the rotation wrapped into 0..360; false (nothing written) when the rotation is
 * not finite or a centre coordinate is not finite or beyond +-PC_PAINT_SYMMETRY_MAX_COORDINATE. */
bool pc_paint_symmetry_new(pc_paint_symmetry_mode mode, const double center[2], double rotation,
                           pc_paint_symmetry *out);
/* The axis directions (unit vectors): 2 for Dual, else 1. Returns the count. */
size_t pc_paint_symmetry_directions(const pc_paint_symmetry *s, double out[2][2]);
/* Mirrored passes besides the original: 3 for Dual, else 1. */
size_t pc_paint_symmetry_mirror_count(const pc_paint_symmetry *s);
/* The reflected copies of pts (pressure, tilt and the rest kept): out[0..*count) each receive n
 * points (appended; on failure out keeps what was appended). *count = mirror_count. */
pc_status pc_paint_symmetry_reflected_passes(const pc_paint_symmetry *s, const pc_paint_point *pts, size_t n,
                                             pc_paint_point_vec out[3], size_t *count, pc_error *err);
/* The axes clipped to a size[0] x size[1] document (the centre may lie outside): out[i] is a
 * segment {{x0, y0}, {x1, y1}}; axes that miss the document are left out. Returns the count. */
size_t pc_paint_symmetry_segments(const pc_paint_symmetry *s, const double size[2], double out[2][2][2]);

#endif
