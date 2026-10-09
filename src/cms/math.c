#include "math.h"

#include <math.h>
#include <string.h>

#include "base/rmath.h"

const double PC_D50[3] = {0.9642, 1.0, 0.8249};
const double PC_D65_XY[2] = {0.3127, 0.3290};
const pc_mat3 PC_MAT3_IDENTITY = {{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}};

pc_mat3 pc_mat3_mul(const pc_mat3 *a, const pc_mat3 *b)
{
    pc_mat3 o;
    int i;
    int j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            o.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j];
        }
    }
    return o;
}

void pc_mat3_apply(const pc_mat3 *m, const double v[3], double out[3])
{
    double r[3];
    r[0] = m->m[0][0] * v[0] + m->m[0][1] * v[1] + m->m[0][2] * v[2];
    r[1] = m->m[1][0] * v[0] + m->m[1][1] * v[1] + m->m[1][2] * v[2];
    r[2] = m->m[2][0] * v[0] + m->m[2][1] * v[1] + m->m[2][2] * v[2];
    memcpy(out, r, sizeof r); /* out may alias v */
}

double pc_mat3_det(const pc_mat3 *p)
{
    const double(*m)[3] = p->m;
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

bool pc_mat3_invert(const pc_mat3 *p, pc_mat3 *out)
{
    const double(*m)[3] = p->m;
    double d = pc_mat3_det(p);
    double inv;
    pc_mat3 o;
    if (!isfinite(d) || fabs(d) < 1e-12) {
        return false;
    }
    inv = 1.0 / d;
    o.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv;
    o.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv;
    o.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv;
    o.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv;
    o.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv;
    o.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv;
    o.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv;
    o.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv;
    o.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv;
    *out = o;
    return true;
}

void pc_xy_to_xyz(const double xy[2], double out[3])
{
    double x = xy[0];
    double y = xy[1];
    out[0] = x / y;
    out[1] = 1.0;
    out[2] = (1.0 - x - y) / y;
}

/* Bradford cone response matrix (Lam 1985). Determinant ~1.7, so always invertible. */
static const pc_mat3 BRADFORD = {{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};

pc_mat3 pc_bradford(const double src[3], const double dst[3])
{
    pc_mat3 inv = PC_MAT3_IDENTITY;
    pc_mat3 scale = PC_MAT3_IDENTITY;
    pc_mat3 t;
    double s[3];
    double d[3];
    (void)pc_mat3_invert(&BRADFORD, &inv);
    pc_mat3_apply(&BRADFORD, src, s);
    pc_mat3_apply(&BRADFORD, dst, d);
    scale.m[0][0] = d[0] / s[0];
    scale.m[1][1] = d[1] / s[1];
    scale.m[2][2] = d[2] / s[2];
    t = pc_mat3_mul(&scale, &BRADFORD);
    return pc_mat3_mul(&inv, &t);
}

bool pc_rgb_to_xyz_matrix(const double r[2], const double g[2], const double b[2], const double white[3],
                          pc_mat3 *out)
{
    double xr[3];
    double xg[3];
    double xb[3];
    double s[3];
    pc_mat3 m;
    pc_mat3 inv;
    int i;
    pc_xy_to_xyz(r, xr);
    pc_xy_to_xyz(g, xg);
    pc_xy_to_xyz(b, xb);
    for (i = 0; i < 3; i++) {
        m.m[i][0] = xr[i];
        m.m[i][1] = xg[i];
        m.m[i][2] = xb[i];
    }
    if (!pc_mat3_invert(&m, &inv)) {
        return false;
    }
    pc_mat3_apply(&inv, white, s);
    for (i = 0; i < 3; i++) {
        out->m[i][0] = m.m[i][0] * s[0];
        out->m[i][1] = m.m[i][1] * s[1];
        out->m[i][2] = m.m[i][2] * s[2];
    }
    return true;
}

static const double EPS = 216.0 / 24389.0;  /* (6/29)^3 */
static const double KAPPA = 24389.0 / 27.0; /* (29/3)^3 */

static double lab_f(double t)
{
    return t > EPS ? pc_cbrt(t) : (KAPPA * t + 16.0) / 116.0; /* Rust std's own cbrt */
}

static double lab_finv(double f)
{
    double f3 = f * f * f;
    return f3 > EPS ? f3 : (116.0 * f - 16.0) / KAPPA;
}

void pc_xyz_to_lab(const double xyz[3], const double white[3], double out[3])
{
    double fx = lab_f(xyz[0] / white[0]);
    double fy = lab_f(xyz[1] / white[1]);
    double fz = lab_f(xyz[2] / white[2]);
    out[0] = 116.0 * fy - 16.0;
    out[1] = 500.0 * (fx - fy);
    out[2] = 200.0 * (fy - fz);
}

void pc_lab_to_xyz(const double lab[3], const double white[3], double out[3])
{
    double fy = (lab[0] + 16.0) / 116.0;
    double fx = fy + lab[1] / 500.0;
    double fz = fy - lab[2] / 200.0;
    double w0 = white[0];
    double w1 = white[1];
    double w2 = white[2];
    out[0] = lab_finv(fx) * w0;
    out[1] = lab_finv(fy) * w1;
    out[2] = lab_finv(fz) * w2;
}

static const float W32[3] = {0.9642f, 1.0f, 0.8249f};
static const float E32 = 216.0f / 24389.0f;
static const float K32 = 24389.0f / 27.0f;

static float lab_f32(float t)
{
    return t > E32 ? pc_cbrtf(t) : (K32 * t + 16.0f) / 116.0f;
}

void pc_xyz_to_lab_f32(const float xyz[3], float out[3])
{
    float fx = lab_f32(xyz[0] / W32[0]);
    float fy = lab_f32(xyz[1] / W32[1]);
    float fz = lab_f32(xyz[2] / W32[2]);
    out[0] = 116.0f * fy - 16.0f;
    out[1] = 500.0f * (fx - fy);
    out[2] = 200.0f * (fy - fz);
}

static float lab_finv32(float f)
{
    float f3 = f * f * f;
    return f3 > E32 ? f3 : (116.0f * f - 16.0f) / K32;
}

void pc_lab_to_xyz_f32(const float lab[3], float out[3])
{
    float fy = (lab[0] + 16.0f) / 116.0f;
    float fx = fy + lab[1] / 500.0f;
    float fz = fy - lab[2] / 200.0f;
    out[0] = lab_finv32(fx) * W32[0];
    out[1] = lab_finv32(fy) * W32[1];
    out[2] = lab_finv32(fz) * W32[2];
}

double pc_delta_e76(const double a[3], const double b[3])
{
    double d0 = a[0] - b[0];
    double d1 = a[1] - b[1];
    double d2 = a[2] - b[2];
    return sqrt(d0 * d0 + d1 * d1 + d2 * d2);
}

double pc_s15f16_round(double v)
{
    return round(v * 65536.0) / 65536.0;
}

void pc_d50_quantized(double out[3])
{
    out[0] = pc_s15f16_round(PC_D50[0]);
    out[1] = pc_s15f16_round(PC_D50[1]);
    out[2] = pc_s15f16_round(PC_D50[2]);
}
