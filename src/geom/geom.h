/* Geometry primitives shared by every PhotoCraft module (port of crates/geom/src/lib.rs).
 *
 * Document space is integer pixels with the origin at the canvas top-left; layers may extend
 * beyond the canvas, so negative coordinates are valid. All types are small plain values passed
 * by value; every function is pure and thread-safe, and none can fail. Integer edge arithmetic
 * saturates exactly where the reference does. */
#ifndef PC_GEOM_GEOM_H
#define PC_GEOM_GEOM_H

#include <stdbool.h>
#include <stdint.h>

/* Edge length of a raster tile in pixels. */
#define PC_TILE_SIZE 256

typedef struct pc_size {
    uint32_t width;
    uint32_t height;
} pc_size;

/* Integer rectangle, half-open: [x0, x1) x [y0, y1). Empty when x1 <= x0 or y1 <= y0. */
typedef struct pc_rect {
    int32_t x0, y0, x1, y1;
} pc_rect;

/* Index of a tile in the infinite tile grid. */
typedef struct pc_tile {
    int32_t tx, ty;
} pc_tile;

typedef struct pc_point {
    double x, y;
} pc_point;

/* 2D affine transform [a c e; b d f; 0 0 1] (column vectors), m = {a, b, c, d, e, f}. */
typedef struct pc_affine {
    double m[6];
} pc_affine;

#define PC_RECT_EMPTY ((pc_rect){0, 0, 0, 0})
#define PC_AFFINE_IDENTITY ((pc_affine){{1.0, 0.0, 0.0, 1.0, 0.0, 0.0}})

static inline pc_size pc_size_make(uint32_t w, uint32_t h) { return (pc_size){w, h}; }
static inline uint64_t pc_size_area(pc_size s) { return (uint64_t)s.width * s.height; }
static inline bool pc_size_is_empty(pc_size s) { return s.width == 0 || s.height == 0; }

static inline pc_rect pc_rect_make(int32_t x0, int32_t y0, int32_t x1, int32_t y1) { return (pc_rect){x0, y0, x1, y1}; }
static inline bool pc_rect_eq(pc_rect a, pc_rect b) { return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1; }
static inline bool pc_rect_is_empty(pc_rect r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }

/* (x, y, x + w, y + h); like the reference, w and h are reinterpreted as i32 and added with
 * saturation, so w > INT32_MAX behaves as a negative width. */
pc_rect pc_rect_from_xywh(int32_t x, int32_t y, uint32_t w, uint32_t h);
pc_rect pc_rect_from_size(pc_size s);

/* Extent; 0 when empty. Exact even for INT32-extreme edges. */
uint32_t pc_rect_width(pc_rect r);
uint32_t pc_rect_height(pc_rect r);
pc_size pc_rect_size(pc_rect r);

/* inner lies entirely inside outer (an empty inner is contained in anything). */
bool pc_rect_contains_rect(pc_rect outer, pc_rect inner);
bool pc_rect_contains(pc_rect r, int32_t x, int32_t y);
/* Empty results are normalised to PC_RECT_EMPTY. */
pc_rect pc_rect_intersect(pc_rect a, pc_rect b);
/* Bounding union; an empty side contributes nothing. */
pc_rect pc_rect_union(pc_rect a, pc_rect b);
pc_rect pc_rect_translate(pc_rect r, int32_t dx, int32_t dy); /* saturating */
pc_rect pc_rect_inflate(pc_rect r, int32_t d);                /* saturating */

/* The inclusive range of tiles overlapping r: iterate ty from first->ty to last->ty, then tx
 * from first->tx to last->tx (the reference's order). Returns false (outputs untouched) for an
 * empty rect. */
bool pc_rect_tile_range(pc_rect r, pc_tile *first, pc_tile *last);

/* The tile containing pixel (x, y) (floor division, so -1 is in tile -1). */
pc_tile pc_tile_containing(int32_t x, int32_t y);
/* The tile's pixel rect (coordinates wrap like the reference's release build for |t| > 2^23). */
pc_rect pc_tile_rect(pc_tile t);
/* Ordering by (tx, ty): negative, zero or positive. */
int pc_tile_cmp(pc_tile a, pc_tile b);

static inline pc_point pc_point_make(double x, double y) { return (pc_point){x, y}; }

pc_affine pc_affine_translate(double dx, double dy);
pc_affine pc_affine_scale(double s);
pc_affine pc_affine_rotate(double radians);
/* a then b: apply a first, then b (the reference's `a.then(&b)` = b * a). */
pc_affine pc_affine_then(pc_affine a, pc_affine b);
/* Matrix product a * b (apply b first). */
pc_affine pc_affine_mul(pc_affine a, pc_affine b);
pc_point pc_affine_apply(pc_affine t, pc_point p);
double pc_affine_determinant(pc_affine t);
/* False (out untouched) when |determinant| < 1e-12. */
bool pc_affine_inverse(pc_affine t, pc_affine *out);

#endif
