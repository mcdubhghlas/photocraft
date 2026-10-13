/* Colour science for raw development (port of crates/raw/src/color.rs): 3x3 matrices,
 * chromaticities, correlated colour temperature (McCamy 1992), Bradford adaptation and the
 * camera -> XYZ (D50) mapping of the Adobe DNG Specification 1.7, chapter 6. */
#include <math.h>

#include "base/num.h"
#include "base/rmath.h"
#include "raw/internal.h"

const pc_raw_mat3 RX_IDENTITY = {{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}};
const double RX_D50[3] = {0.9642, 1.0, 0.8249};
const double RX_D50_XY[2] = {0.3457, 0.3585};
const double RX_D65_XY[2] = {0.3127, 0.3290};

const double RX_ROMM[3][2] = {{0.7347, 0.2653}, {0.1596, 0.8404}, {0.0366, 0.0001}};
static const double REC709[3][2] = {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}};

pc_raw_mat3 rx_mul(const pc_raw_mat3 *a, const pc_raw_mat3 *b)
{
    pc_raw_mat3 o;
    int i;
    int j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            /* Iterator::sum starts at -0.0. */
            double s = -0.0;
            int k;
            for (k = 0; k < 3; k++) {
                s += a->m[i][k] * b->m[k][j];
            }
            o.m[i][j] = s;
        }
    }
    return o;
}

void rx_apply(const pc_raw_mat3 *m, const double v[3], double out[3])
{
    double r[3];
    int i;
    for (i = 0; i < 3; i++) {
        r[i] = m->m[i][0] * v[0] + m->m[i][1] * v[1] + m->m[i][2] * v[2];
    }
    out[0] = r[0];
    out[1] = r[1];
    out[2] = r[2];
}

pc_raw_mat3 rx_diag(const double v[3])
{
    pc_raw_mat3 o = {{{v[0], 0.0, 0.0}, {0.0, v[1], 0.0}, {0.0, 0.0, v[2]}}};
    return o;
}

pc_raw_mat3 rx_scale(const pc_raw_mat3 *m, double k)
{
    pc_raw_mat3 o;
    int i;
    int j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            o.m[i][j] = m->m[i][j] * k;
        }
    }
    return o;
}

bool rx_invert(const pc_raw_mat3 *m, pc_raw_mat3 *out)
{
    double a = m->m[0][0], b = m->m[0][1], c = m->m[0][2];
    double d = m->m[1][0], e = m->m[1][1], f = m->m[1][2];
    double g = m->m[2][0], h = m->m[2][1], i = m->m[2][2];
    double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    double r;
    pc_raw_mat3 o;
    int x;
    int y;
    if (!isfinite(det) || fabs(det) < 1e-12) {
        return false;
    }
    r = 1.0 / det;
    o.m[0][0] = (e * i - f * h) * r;
    o.m[0][1] = (c * h - b * i) * r;
    o.m[0][2] = (b * f - c * e) * r;
    o.m[1][0] = (f * g - d * i) * r;
    o.m[1][1] = (a * i - c * g) * r;
    o.m[1][2] = (c * d - a * f) * r;
    o.m[2][0] = (d * h - e * g) * r;
    o.m[2][1] = (b * g - a * h) * r;
    o.m[2][2] = (a * e - b * d) * r;
    for (x = 0; x < 3; x++) {
        for (y = 0; y < 3; y++) {
            if (!isfinite(o.m[x][y])) {
                return false;
            }
        }
    }
    *out = o;
    return true;
}

bool rx_from_slice(const double *v, size_t n, pc_raw_mat3 *out)
{
    int i;
    if (n < 9) {
        return false;
    }
    for (i = 0; i < 9; i++) {
        if (!isfinite(v[i])) {
            return false;
        }
    }
    for (i = 0; i < 9; i++) {
        out->m[i / 3][i % 3] = v[i];
    }
    return true;
}

void rx_xy_to_xyz(const double xy[2], double out[3])
{
    double y = pc_rs_max_k(xy[1], 1e-6);
    out[0] = xy[0] / y;
    out[1] = 1.0;
    out[2] = (1.0 - xy[0] - xy[1]) / y;
}

static bool xyz_to_xy(const double v[3], double out[2])
{
    double s = v[0] + v[1] + v[2];
    if (!(isfinite(s) && s > 1e-9)) {
        return false;
    }
    out[0] = v[0] / s;
    out[1] = v[1] / s;
    return true;
}

/* LLVM's powi for an integer exponent: repeated squaring (compiler-rt __powidf2). */
static double powi(double a, int b)
{
    bool recip = b < 0;
    double r = 1.0;
    for (;;) {
        if (b & 1) {
            r *= a;
        }
        b /= 2;
        if (b == 0) {
            break;
        }
        a *= a;
    }
    return recip ? 1.0 / r : r;
}

double rx_cct(const double xy[2])
{
    double n = (xy[0] - 0.3320) / (0.1858 - xy[1]);
    double t = 449.0 * powi(n, 3) + 3525.0 * powi(n, 2) + 6823.3 * n + 5520.33;
    return isfinite(t) ? pc_clampd(t, 2000.0, 50000.0) : 5000.0;
}

bool rx_illuminant_temperature(uint32_t code, double *t)
{
    double v;
    switch (code) {
    case 17:
        v = 2856.0;
        break;
    case 18:
        v = 4874.0;
        break;
    case 19:
        v = 6774.0;
        break;
    case 20:
        v = 5503.0;
        break;
    case 21:
        v = 6504.0;
        break;
    case 22:
        v = 7504.0;
        break;
    case 23:
        v = 5003.0;
        break;
    case 24:
        v = 3200.0;
        break;
    case 3:
        v = 2850.0;
        break;
    case 1:
    case 4:
    case 9:
        v = 5500.0;
        break;
    case 10:
        v = 6500.0;
        break;
    case 11:
        v = 7500.0;
        break;
    case 12:
        v = 6430.0;
        break;
    case 13:
        v = 5000.0;
        break;
    case 14:
        v = 4150.0;
        break;
    case 15:
        v = 3525.0;
        break;
    case 16:
        v = 2925.0;
        break;
    case 2:
        v = 4200.0;
        break;
    default:
        return false;
    }
    *t = v;
    return true;
}

pc_raw_mat3 rx_bradford(const double src[3], const double dst[3])
{
    static const pc_raw_mat3 M = {{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};
    pc_raw_mat3 mi;
    double s[3];
    double d[3];
    double q[3];
    pc_raw_mat3 dg;
    pc_raw_mat3 t;
    if (!rx_invert(&M, &mi)) {
        return RX_IDENTITY;
    }
    rx_apply(&M, src, s);
    rx_apply(&M, dst, d);
    if (fabs(s[0]) < 1e-9 || fabs(s[1]) < 1e-9 || fabs(s[2]) < 1e-9) {
        return RX_IDENTITY;
    }
    q[0] = d[0] / s[0];
    q[1] = d[1] / s[1];
    q[2] = d[2] / s[2];
    dg = rx_diag(q);
    t = rx_mul(&dg, &M);
    return rx_mul(&mi, &t);
}

static pc_raw_mat3 rgb_to_xyz(const double prim[3][2], const double white[2])
{
    double cols[3][3];
    pc_raw_mat3 p;
    pc_raw_mat3 pi;
    double w[3];
    double s[3];
    pc_raw_mat3 ds;
    int i;
    for (i = 0; i < 3; i++) {
        rx_xy_to_xyz(prim[i], cols[i]);
    }
    for (i = 0; i < 3; i++) {
        p.m[i][0] = cols[0][i];
        p.m[i][1] = cols[1][i];
        p.m[i][2] = cols[2][i];
    }
    if (!rx_invert(&p, &pi)) {
        return RX_IDENTITY;
    }
    rx_xy_to_xyz(white, w);
    rx_apply(&pi, w, s);
    ds = rx_diag(s);
    return rx_mul(&p, &ds);
}

pc_raw_mat3 rx_rgb_to_xyz(const double prim[3][2], const double white[2]) { return rgb_to_xyz(prim, white); }

pc_raw_mat3 rx_xyz_d50_to_prophoto(void)
{
    pc_raw_mat3 m = rgb_to_xyz(RX_ROMM, RX_D50_XY);
    pc_raw_mat3 o;
    return rx_invert(&m, &o) ? o : RX_IDENTITY;
}

pc_raw_mat3 rx_srgb_to_xyz_d50(void)
{
    double a[3];
    double b[3];
    pc_raw_mat3 br;
    pc_raw_mat3 r;
    rx_xy_to_xyz(RX_D65_XY, a);
    rx_xy_to_xyz(RX_D50_XY, b);
    br = rx_bradford(a, b);
    r = rgb_to_xyz(REC709, RX_D65_XY);
    return rx_mul(&br, &r);
}

static pc_raw_mat3 lerp_mat(const pc_raw_mat3 *a, const pc_raw_mat3 *b, double g)
{
    pc_raw_mat3 o = *a;
    int i;
    int j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            o.m[i][j] = a->m[i][j] * g + b->m[i][j] * (1.0 - g);
        }
    }
    return o;
}

/* Weight of the first calibration at temperature t (inverse-temperature interpolation). */
static double weight(const pc_raw_color_info *c, double t)
{
    if (c->ncalibrations == 2 && fabs(c->calibrations[0].temperature - c->calibrations[1].temperature) > 1.0) {
        double t1 = c->calibrations[0].temperature;
        double t2 = c->calibrations[1].temperature;
        if (t <= t1) {
            return 1.0;
        }
        if (t >= t2) {
            return 0.0;
        }
        return pc_clampd((1.0 / t - 1.0 / t2) / (1.0 / t1 - 1.0 / t2), 0.0, 1.0);
    }
    return 1.0;
}

/* The interpolated (AB * CC * CM, ForwardMatrix, AB * CC) at temperature t. */
static bool at_temp(const pc_raw_color_info *c, double t, pc_raw_mat3 *xyz_to_cam, bool *has_fm, pc_raw_mat3 *fm,
                    pc_raw_mat3 *abcc)
{
    const pc_raw_calibration *first;
    double g;
    pc_raw_mat3 cm;
    pc_raw_mat3 cc;
    pc_raw_mat3 ab;
    double one[3] = {1.0, 1.0, 1.0};
    if (c->ncalibrations == 0) {
        return false;
    }
    first = &c->calibrations[0];
    g = weight(c, t);
    if (c->ncalibrations > 1) {
        const pc_raw_calibration *b = &c->calibrations[1];
        cm = lerp_mat(&first->color_matrix, &b->color_matrix, g);
        cc = lerp_mat(&first->camera_calibration, &b->camera_calibration, g);
        if (first->has_forward_matrix && b->has_forward_matrix) {
            *has_fm = true;
            *fm = lerp_mat(&first->forward_matrix, &b->forward_matrix, g);
        } else {
            *has_fm = false;
        }
    } else {
        cm = first->color_matrix;
        cc = first->camera_calibration;
        *has_fm = first->has_forward_matrix;
        if (*has_fm) {
            *fm = first->forward_matrix;
        }
    }
    ab = rx_diag(c->has_analog_balance ? c->analog_balance : one);
    *abcc = rx_mul(&ab, &cc);
    *xyz_to_cam = rx_mul(abcc, &cm);
    return true;
}

void rx_neutral_to_xy(const pc_raw_color_info *c, const double neutral[3], double out[2])
{
    double xy[2] = {RX_D50_XY[0], RX_D50_XY[1]};
    int it;
    for (it = 0; it < 30; it++) {
        pc_raw_mat3 xyz_to_cam;
        pc_raw_mat3 cam_to_xyz;
        pc_raw_mat3 fm;
        pc_raw_mat3 abcc;
        bool has_fm;
        double v[3];
        double next[2];
        bool done;
        if (!at_temp(c, rx_cct(xy), &xyz_to_cam, &has_fm, &fm, &abcc) || !rx_invert(&xyz_to_cam, &cam_to_xyz)) {
            break;
        }
        rx_apply(&cam_to_xyz, neutral, v);
        if (!xyz_to_xy(v, next) || !(next[0] >= 0.0 && next[0] < 1.0) || !(next[1] >= 0.0 && next[1] < 1.0)) {
            break;
        }
        done = fabs(next[0] - xy[0]) + fabs(next[1] - xy[1]) < 1e-7;
        xy[0] = next[0];
        xy[1] = next[1];
        if (done) {
            break;
        }
    }
    out[0] = xy[0];
    out[1] = xy[1];
}

bool rx_xy_to_neutral(const pc_raw_color_info *c, const double xy[2], double out[3])
{
    pc_raw_mat3 xyz_to_cam;
    pc_raw_mat3 fm;
    pc_raw_mat3 abcc;
    bool has_fm;
    double w[3];
    double n[3];
    double m = -1.7976931348623157e308;
    int i;
    if (!at_temp(c, rx_cct(xy), &xyz_to_cam, &has_fm, &fm, &abcc)) {
        return false;
    }
    rx_xy_to_xyz(xy, w);
    rx_apply(&xyz_to_cam, w, n);
    for (i = 0; i < 3; i++) {
        m = pc_rs_max(m, n[i]);
    }
    if (!(m > 1e-9)) {
        return false;
    }
    for (i = 0; i < 3; i++) {
        if (!(isfinite(n[i]) && n[i] > 0.0)) {
            return false;
        }
    }
    for (i = 0; i < 3; i++) {
        out[i] = n[i] / m;
    }
    return true;
}

bool pc_raw_balanced_to_xyz_d50(const pc_raw_color_info *c, const double neutral[3], pc_raw_mat3 *out)
{
    double nmax = -1.7976931348623157e308;
    double xy[2];
    pc_raw_mat3 xyz_to_cam;
    pc_raw_mat3 fm;
    pc_raw_mat3 abcc;
    bool has_fm;
    double un[3];
    pc_raw_mat3 unbalance;
    pc_raw_mat3 m;
    double one[3] = {1.0, 1.0, 1.0};
    double w[3];
    double y;
    int i;
    for (i = 0; i < 3; i++) {
        nmax = pc_rs_max(nmax, neutral[i]);
    }
    if (isnan(nmax) || nmax <= 0.0) {
        return false;
    }
    for (i = 0; i < 3; i++) {
        if (isnan(neutral[i]) || neutral[i] <= 0.0) {
            return false;
        }
    }
    rx_neutral_to_xy(c, neutral, xy);
    if (!at_temp(c, rx_cct(xy), &xyz_to_cam, &has_fm, &fm, &abcc)) {
        return false;
    }
    for (i = 0; i < 3; i++) {
        un[i] = neutral[i] / nmax;
    }
    unbalance = rx_diag(un);
    if (has_fm) {
        /* CameraToXYZ_D50 = FM * D * Inverse(AB * CC), D = Invert(diag(ReferenceNeutral)). */
        pc_raw_mat3 abcc_inv;
        double reference[3];
        double inv[3];
        pc_raw_mat3 d;
        pc_raw_mat3 t1;
        pc_raw_mat3 t2;
        if (!rx_invert(&abcc, &abcc_inv)) {
            return false;
        }
        rx_apply(&abcc_inv, neutral, reference);
        for (i = 0; i < 3; i++) {
            if (isnan(reference[i]) || reference[i] <= 0.0) {
                return false;
            }
            inv[i] = 1.0 / reference[i];
        }
        d = rx_diag(inv);
        t1 = rx_mul(&d, &abcc_inv);
        t2 = rx_mul(&fm, &t1);
        m = rx_mul(&t2, &unbalance);
    } else {
        pc_raw_mat3 cam_to_xyz;
        pc_raw_mat3 adapt;
        pc_raw_mat3 t;
        double src[3];
        if (!rx_invert(&xyz_to_cam, &cam_to_xyz)) {
            return false;
        }
        rx_xy_to_xyz(xy, src);
        adapt = rx_bradford(src, RX_D50);
        t = rx_mul(&adapt, &cam_to_xyz);
        m = rx_mul(&t, &unbalance);
    }
    rx_apply(&m, one, w);
    y = w[1];
    if (!(isfinite(y) && y > 1e-9)) {
        return false;
    }
    *out = rx_scale(&m, 1.0 / y);
    return true;
}
