/* Painting symmetry axes (symmetry.h). */
#include "paint/symmetry.h"

#include <math.h>
#include <string.h>

#include "base/rmath.h"

/* Rust's f64::to_radians factor. */
#define RAD_PER_DEG (3.14159265358979323846 / 180.0)

static const char *const IDS[PC_PAINT_SYMMETRY_MODE_COUNT] = {"vertical", "horizontal", "dual", "diagonal"};

const char *pc_paint_symmetry_mode_id(pc_paint_symmetry_mode m)
{
    return (unsigned)m < PC_PAINT_SYMMETRY_MODE_COUNT ? IDS[m] : NULL;
}

bool pc_paint_symmetry_mode_from_id(const char *id, size_t len, pc_paint_symmetry_mode *out)
{
    for (unsigned i = 0; i < PC_PAINT_SYMMETRY_MODE_COUNT; i++) {
        if (strlen(IDS[i]) == len && memcmp(IDS[i], id, len) == 0) {
            *out = (pc_paint_symmetry_mode)i;
            return true;
        }
    }
    return false;
}

bool pc_paint_symmetry_new(pc_paint_symmetry_mode mode, const double center[2], double rotation,
                           pc_paint_symmetry *out)
{
    double r;
    if (!isfinite(rotation)) {
        return false;
    }
    for (int i = 0; i < 2; i++) {
        if (!isfinite(center[i]) || fabs(center[i]) > PC_PAINT_SYMMETRY_MAX_COORDINATE) {
            return false;
        }
    }
    r = fmod(rotation, 360.0);
    out->mode = mode;
    out->center[0] = center[0];
    out->center[1] = center[1];
    out->rotation = r < 0.0 ? r + 360.0 : r;
    return true;
}

size_t pc_paint_symmetry_directions(const pc_paint_symmetry *s, double out[2][2])
{
    double initial = s->mode == PC_PAINT_SYMMETRY_HORIZONTAL ? 0.0
                     : s->mode == PC_PAINT_SYMMETRY_DIAGONAL ? 45.0
                                                             : 90.0;
    double a = (initial + s->rotation) * RAD_PER_DEG;
    out[0][0] = cos(a);
    out[0][1] = sin(a);
    if (s->mode != PC_PAINT_SYMMETRY_DUAL) {
        return 1;
    }
    out[1][0] = -out[0][1];
    out[1][1] = out[0][0];
    return 2;
}

size_t pc_paint_symmetry_mirror_count(const pc_paint_symmetry *s)
{
    return s->mode == PC_PAINT_SYMMETRY_DUAL ? 3 : 1;
}

pc_status pc_paint_symmetry_reflected_passes(const pc_paint_symmetry *s, const pc_paint_point *pts, size_t n,
                                             pc_paint_point_vec out[3], size_t *count, pc_error *err)
{
    double dirs[2][2];
    size_t nd = pc_paint_symmetry_directions(s, dirs);
    for (size_t k = 0; k < nd; k++) {
        for (size_t i = 0; i < n; i++) {
            pc_paint_point q = pts[i];
            double dx = pts[i].x - s->center[0], dy = pts[i].y - s->center[1];
            double along = dx * dirs[k][0] + dy * dirs[k][1];
            pc_status st;
            q.x = s->center[0] + 2.0 * along * dirs[k][0] - dx;
            q.y = s->center[1] + 2.0 * along * dirs[k][1] - dy;
            st = pc_paint_point_vec_push(&out[k], &q, err);
            if (st != PC_OK) {
                return st;
            }
        }
    }
    if (s->mode == PC_PAINT_SYMMETRY_DUAL) {
        for (size_t i = 0; i < n; i++) {
            pc_paint_point q = pts[i];
            pc_status st;
            q.x = 2.0 * s->center[0] - pts[i].x;
            q.y = 2.0 * s->center[1] - pts[i].y;
            st = pc_paint_point_vec_push(&out[2], &q, err);
            if (st != PC_OK) {
                return st;
            }
        }
    }
    *count = pc_paint_symmetry_mirror_count(s);
    return PC_OK;
}

size_t pc_paint_symmetry_segments(const pc_paint_symmetry *s, const double size[2], double out[2][2][2])
{
    double dirs[2][2];
    size_t nd = pc_paint_symmetry_directions(s, dirs), k = 0;
    for (size_t i = 0; i < nd; i++) {
        double lo = -INFINITY, hi = INFINITY;
        bool miss = false;
        for (int ax = 0; ax < 2; ax++) {
            double origin = s->center[ax], dir = dirs[i][ax], extent = size[ax];
            if (fabs(dir) < 1e-12) {
                if (origin < 0.0 || origin > extent) {
                    miss = true;
                    break;
                }
            } else {
                double a = -origin / dir, b = (extent - origin) / dir;
                lo = pc_rs_max(lo, pc_rs_min(a, b));
                hi = pc_rs_min(hi, pc_rs_max(a, b));
            }
        }
        if (miss || !isfinite(lo) || !isfinite(hi) || hi < lo) {
            continue;
        }
        out[k][0][0] = s->center[0] + lo * dirs[i][0];
        out[k][0][1] = s->center[1] + lo * dirs[i][1];
        out[k][1][0] = s->center[0] + hi * dirs[i][0];
        out[k][1][1] = s->center[1] + hi * dirs[i][1];
        k++;
    }
    return k;
}
