/* Colour-science helpers: 3x3 matrices, CIE XYZ <-> L*a*b*, Bradford adaptation (port of
 * crates/cms/src/math.rs). Formulas from CIE 15, ICC.1:2010 Annex A and Lam (1985). Pure. */
#ifndef PC_CMS_MATH_H
#define PC_CMS_MATH_H

#include <stdbool.h>

/* Row-major 3x3 matrix. */
typedef struct pc_mat3 {
    double m[3][3];
} pc_mat3;

/* The ICC PCS illuminant D50 (as encoded in every header). */
extern const double PC_D50[3];
/* CIE D65 chromaticity used by sRGB, Display P3 and Adobe RGB-compatible spaces. */
extern const double PC_D65_XY[2];
extern const pc_mat3 PC_MAT3_IDENTITY;

pc_mat3 pc_mat3_mul(const pc_mat3 *a, const pc_mat3 *b);
void pc_mat3_apply(const pc_mat3 *m, const double v[3], double out[3]);
double pc_mat3_det(const pc_mat3 *m);
/* False (out untouched) when (nearly) singular or non-finite. */
bool pc_mat3_invert(const pc_mat3 *m, pc_mat3 *out);

void pc_xy_to_xyz(const double xy[2], double out[3]);
/* Bradford chromatic adaptation matrix from white src to white dst (both XYZ). */
pc_mat3 pc_bradford(const double src[3], const double dst[3]);
/* RGB -> XYZ for chromaticity primaries relative to white; false if not independent. */
bool pc_rgb_to_xyz_matrix(const double r[2], const double g[2], const double b[2], const double white[3],
                          pc_mat3 *out);

void pc_xyz_to_lab(const double xyz[3], const double white[3], double out[3]);
void pc_lab_to_xyz(const double lab[3], const double white[3], double out[3]);
/* f32 versions (D50 only) for the per-pixel paths. */
void pc_xyz_to_lab_f32(const float xyz[3], float out[3]);
void pc_lab_to_xyz_f32(const float lab[3], float out[3]);

double pc_delta_e76(const double a[3], const double b[3]);
/* Quantize to ICC s15Fixed16Number and back. */
double pc_s15f16_round(double v);
/* D50 exactly as an ICC header stores it. */
void pc_d50_quantized(double out[3]);

#endif
