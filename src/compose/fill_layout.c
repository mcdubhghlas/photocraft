/* Gradient fill geometry (port of crates/compose/src/fill_layout.rs) and the gradient
 * parameter functions of effects.rs it is built on. */
#include <math.h>

#include "base/rmath.h"
#include "compose/fill_layout.h"

#define PI_F64 3.14159265358979323846
#define PI_F32 3.14159265358979323846f

/* Rust's f32::to_radians and the f64 conversions (same constants, same rounding). */
static float rad_f32(float deg)
{
    const float k = PI_F32 / 180.0f;
    return deg * k;
}

static double rad_f64(double deg)
{
    const double k = PI_F64 / 180.0;
    return deg * k;
}

static double deg_f64(double rad)
{
    const double k = 180.0 / PI_F64;
    return rad * k;
}

void pc_compose_gradient_units(float angle, float w, float h, float *chord, float *len)
{
    float a = rad_f32(angle);
    float s = sinf(a);
    float c = cosf(a);
    float cw = c * w;
    float sh = s * h;
    *len = pc_rs_maxf_k(sqrtf(cw * cw + sh * sh), 1.0f);
    *chord =
        pc_rs_maxf_k(pc_rs_minf(w / pc_rs_maxf_k(fabsf(c), 1e-6f), h / pc_rs_maxf_k(fabsf(s), 1e-6f)), 1.0f);
}

float pc_compose_gradient_t(pc_gradient_style style, float angle, float scale, bool reverse,
                            const float offset[2], pc_rect bounds, float x, float y)
{
    uint32_t bw = pc_rect_width(bounds);
    uint32_t bh = pc_rect_height(bounds);
    float w = (float)(bw > 1 ? bw : 1);
    float h = (float)(bh > 1 ? bh : 1);
    float cx = (float)bounds.x0 + w / 2.0f + offset[0] * w;
    float cy = (float)bounds.y0 + h / 2.0f + offset[1] * h;
    float a = rad_f32(angle);
    float s = sinf(a);
    float c = cosf(a);
    float dx = x - cx;
    float dy = y - cy;
    float along = dx * c - dy * s;
    float across = dx * s + dy * c;
    float chord;
    float len;
    float corner;
    float t;
    pc_compose_gradient_units(angle, w, h, &chord, &len);
    chord = chord * pc_rs_maxf_k(scale, 1e-3f);
    len = len * pc_rs_maxf_k(scale, 1e-3f);
    corner = 0.5f * (c - s);
    switch (style) {
    case PC_GRADIENT_LINEAR:
        t = (along - corner) / chord + 0.5f;
        break;
    case PC_GRADIENT_REFLECTED:
        t = fabsf((along - corner) / (chord / 2.0f));
        break;
    case PC_GRADIENT_RADIAL:
        t = sqrtf(dx * dx + dy * dy) / (len / 2.0f);
        break;
    case PC_GRADIENT_DIAMOND:
        t = (fabsf(along) + fabsf(across)) / (len / 2.0f);
        break;
    default: { /* Angle: clockwise sweep starting at the gradient angle. */
        const float tau = 6.28318530717958647692f;
        float r = fmodf((rad_f32(angle) - atan2f(-dy, dx)) / tau, 1.0f);
        t = r < 0.0f ? r + 1.0f : r;
        break;
    }
    }
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return reverse ? 1.0f - t : t;
}

/* Unscaled chord length along a (radians). */
static double chord_of(double a, double w, double h)
{
    double s = sin(a);
    double c = cos(a);
    return pc_rs_max_k(pc_rs_min(w / pc_rs_max_k(fabs(c), 1e-6), h / pc_rs_max_k(fabs(s), 1e-6)), 1.0);
}

static double snap(double v)
{
    return floor(v + 1e-4);
}

pc_compose_gradient_geom pc_compose_gradient_layout(pc_gradient_style style, float angle, float scale,
                                                    const float offset[2], pc_rect frame)
{
    pc_compose_gradient_geom unchanged = {angle, scale, {offset[0], offset[1]}};
    pc_compose_gradient_geom out;
    uint32_t fw = pc_rect_width(frame);
    uint32_t fh = pc_rect_height(frame);
    double w;
    double h;
    double cx;
    double cy;
    double a;
    double half;
    double s;
    double c;
    double dx;
    double dy;
    double end[2];
    double start[2];
    double mid[2];
    double v[2];
    double len;
    double a2;
    double span;
    double scale2;
    if ((style != PC_GRADIENT_LINEAR && style != PC_GRADIENT_REFLECTED) || !isfinite(angle) ||
        !isfinite(scale) || !isfinite(offset[0]) || !isfinite(offset[1])) {
        return unchanged;
    }
    w = (double)(fw > 1 ? fw : 1);
    h = (double)(fh > 1 ? fh : 1);
    cx = (double)frame.x0 + w / 2.0 + (double)offset[0] * w;
    cy = (double)frame.y0 + h / 2.0 + (double)offset[1] * h;
    a = rad_f64((double)angle);
    half = chord_of(a, w, h) * (double)pc_rs_maxf_k(scale, 1e-3f) / 2.0;
    s = sin(a);
    c = cos(a);
    dx = c * half;
    dy = -s * half;
    end[0] = snap(cx + dx);
    end[1] = snap(cy + dy);
    if (style == PC_GRADIENT_REFLECTED) {
        start[0] = cx;
        start[1] = cy;
        mid[0] = cx;
        mid[1] = cy;
    } else {
        start[0] = snap(cx - dx);
        start[1] = snap(cy - dy);
        mid[0] = (start[0] + end[0]) / 2.0;
        mid[1] = (start[1] + end[1]) / 2.0;
    }
    v[0] = end[0] - start[0];
    v[1] = end[1] - start[1];
    len = hypot(v[0], v[1]);
    if (len < 0.5) {
        return unchanged;
    }
    a2 = atan2(-v[1], v[0]);
    span = style == PC_GRADIENT_REFLECTED ? 2.0 * len : len;
    scale2 = span / chord_of(a2, w, h);
    out.angle = (float)deg_f64(a2);
    out.scale = (float)scale2;
    out.offset[0] = offset[0] + (float)((mid[0] - cx) / w);
    out.offset[1] = offset[1] + (float)((mid[1] - cy) / h);
    return out;
}

pc_compose_gradient_geom pc_compose_fill_gradient_layout(pc_gradient_style style, float angle, float scale,
                                                         pc_rect frame)
{
    const float zero[2] = {0.0f, 0.0f};
    return pc_compose_gradient_layout(style, angle, scale, zero, frame);
}

float pc_compose_fill_gradient_t(pc_gradient_style style, float angle, float scale, bool reverse,
                                 pc_rect frame, float x, float y)
{
    pc_compose_gradient_geom l = pc_compose_fill_gradient_layout(style, angle, scale, frame);
    return pc_compose_gradient_t(style, l.angle, l.scale, reverse, l.offset, frame, x, y);
}
