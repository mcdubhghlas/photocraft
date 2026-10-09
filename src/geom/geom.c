#include "geom.h"

#include <math.h>

#include "base/num.h"

pc_rect pc_rect_from_xywh(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    return pc_rect_make(x, y, pc_i32_sat_add(x, pc_u32_as_i32(w)), pc_i32_sat_add(y, pc_u32_as_i32(h)));
}

pc_rect pc_rect_from_size(pc_size s)
{
    return pc_rect_from_xywh(0, 0, s.width, s.height);
}

uint32_t pc_rect_width(pc_rect r)
{
    return r.x1 > r.x0 ? (uint32_t)((int64_t)r.x1 - r.x0) : 0;
}

uint32_t pc_rect_height(pc_rect r)
{
    return r.y1 > r.y0 ? (uint32_t)((int64_t)r.y1 - r.y0) : 0;
}

pc_size pc_rect_size(pc_rect r)
{
    return pc_size_make(pc_rect_width(r), pc_rect_height(r));
}

bool pc_rect_contains_rect(pc_rect o, pc_rect i)
{
    return pc_rect_is_empty(i) || (i.x0 >= o.x0 && i.y0 >= o.y0 && i.x1 <= o.x1 && i.y1 <= o.y1);
}

bool pc_rect_contains(pc_rect r, int32_t x, int32_t y)
{
    return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
}

static int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }
static int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }

pc_rect pc_rect_intersect(pc_rect a, pc_rect b)
{
    pc_rect r = pc_rect_make(imax(a.x0, b.x0), imax(a.y0, b.y0), imin(a.x1, b.x1), imin(a.y1, b.y1));
    return pc_rect_is_empty(r) ? PC_RECT_EMPTY : r;
}

pc_rect pc_rect_union(pc_rect a, pc_rect b)
{
    if (pc_rect_is_empty(a)) {
        return b;
    }
    if (pc_rect_is_empty(b)) {
        return a;
    }
    return pc_rect_make(imin(a.x0, b.x0), imin(a.y0, b.y0), imax(a.x1, b.x1), imax(a.y1, b.y1));
}

pc_rect pc_rect_translate(pc_rect r, int32_t dx, int32_t dy)
{
    return pc_rect_make(pc_i32_sat_add(r.x0, dx), pc_i32_sat_add(r.y0, dy), pc_i32_sat_add(r.x1, dx),
                        pc_i32_sat_add(r.y1, dy));
}

pc_rect pc_rect_inflate(pc_rect r, int32_t d)
{
    return pc_rect_make(pc_i32_sat_sub(r.x0, d), pc_i32_sat_sub(r.y0, d), pc_i32_sat_add(r.x1, d),
                        pc_i32_sat_add(r.y1, d));
}

/* i32::div_euclid by a positive divisor: floor division, no overflow. */
static int32_t div_floor(int32_t x, int32_t d)
{
    int32_t q = x / d;
    return (x % d < 0) ? q - 1 : q;
}

pc_tile pc_tile_containing(int32_t x, int32_t y)
{
    pc_tile t;
    t.tx = div_floor(x, PC_TILE_SIZE);
    t.ty = div_floor(y, PC_TILE_SIZE);
    return t;
}

pc_rect pc_tile_rect(pc_tile t)
{
    return pc_rect_from_xywh(pc_i32_wrapping_mul(t.tx, PC_TILE_SIZE), pc_i32_wrapping_mul(t.ty, PC_TILE_SIZE),
                             PC_TILE_SIZE, PC_TILE_SIZE);
}

int pc_tile_cmp(pc_tile a, pc_tile b)
{
    if (a.tx != b.tx) {
        return a.tx < b.tx ? -1 : 1;
    }
    return a.ty < b.ty ? -1 : (a.ty > b.ty ? 1 : 0);
}

bool pc_rect_tile_range(pc_rect r, pc_tile *first, pc_tile *last)
{
    if (pc_rect_is_empty(r)) {
        return false;
    }
    /* Non-empty, so x1 > x0 >= INT32_MIN and x1 - 1 cannot overflow. */
    *first = pc_tile_containing(r.x0, r.y0);
    *last = pc_tile_containing(r.x1 - 1, r.y1 - 1);
    return true;
}

pc_affine pc_affine_translate(double dx, double dy)
{
    pc_affine t = {{1.0, 0.0, 0.0, 1.0, dx, dy}};
    return t;
}

pc_affine pc_affine_scale(double s)
{
    pc_affine t = {{s, 0.0, 0.0, s, 0.0, 0.0}};
    return t;
}

pc_affine pc_affine_rotate(double radians)
{
    double s = sin(radians);
    double c = cos(radians);
    pc_affine t = {{c, s, -s, c, 0.0, 0.0}};
    return t;
}

pc_affine pc_affine_mul(pc_affine p, pc_affine o)
{
    const double *m = p.m;
    const double *n = o.m;
    pc_affine r = {{m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1], m[0] * n[2] + m[2] * n[3],
                    m[1] * n[2] + m[3] * n[3], m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]}};
    return r;
}

pc_affine pc_affine_then(pc_affine a, pc_affine b)
{
    return pc_affine_mul(b, a);
}

pc_point pc_affine_apply(pc_affine t, pc_point p)
{
    const double *m = t.m;
    return pc_point_make(m[0] * p.x + m[2] * p.y + m[4], m[1] * p.x + m[3] * p.y + m[5]);
}

double pc_affine_determinant(pc_affine t)
{
    return t.m[0] * t.m[3] - t.m[1] * t.m[2];
}

bool pc_affine_inverse(pc_affine t, pc_affine *out)
{
    double det = pc_affine_determinant(t);
    double inv;
    const double *m = t.m;
    if (fabs(det) < 1e-12) {
        return false;
    }
    inv = 1.0 / det;
    out->m[0] = m[3] * inv;
    out->m[1] = -m[1] * inv;
    out->m[2] = -m[2] * inv;
    out->m[3] = m[0] * inv;
    out->m[4] = (m[2] * m[5] - m[3] * m[4]) * inv;
    out->m[5] = (m[1] * m[4] - m[0] * m[5]) * inv;
    return true;
}
