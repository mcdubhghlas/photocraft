/* One-dimensional tone curves (ICC curv/para) and their inverses (port of crates/cms/src/curve.rs).
 *
 * Gamma and parametric curves are evaluated analytically, also outside [0, 1] (negatives
 * mirror, values above 1 extend the formula) so unbounded float transforms keep HDR values;
 * sampled tables clamp their input to [0, 1]. A curve owns its table (pc_cms_curve_fini). */
#ifndef PC_CMS_CURVE_H
#define PC_CMS_CURVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"

typedef enum pc_cms_curve_kind {
    PC_CURVE_IDENTITY,
    PC_CURVE_GAMMA,      /* y = x^gamma */
    PC_CURVE_PARAMETRIC, /* ICC parametricCurveType function ptype (0..4), p = {g,a,b,c,d,e,f} */
    PC_CURVE_TABLE       /* n samples at evenly spaced inputs over [0, 1], normalised outputs */
} pc_cms_curve_kind;

typedef struct pc_cms_curve {
    pc_cms_curve_kind kind;
    double gamma;
    uint16_t ptype;
    double p[7];
    float *table; /* owned, n entries (TABLE only) */
    size_t n;
} pc_cms_curve;

pc_cms_curve pc_cms_curve_identity(void);
pc_cms_curve pc_cms_curve_gamma(double g);
pc_cms_curve pc_cms_curve_parametric(uint16_t ptype, const double p[7]);
/* Takes ownership of table (malloc'd, n entries). */
pc_cms_curve pc_cms_curve_table(float *table, size_t n);
/* A two-point identity table {0, 1} (what lut16 tables use for pass-through). */
pc_status pc_cms_curve_table01(pc_cms_curve *out, pc_error *err);
/* The sRGB transfer function as parametric type 3. */
pc_cms_curve pc_cms_srgb_trc(void);

void pc_cms_curve_fini(pc_cms_curve *c);
pc_status pc_cms_curve_clone(const pc_cms_curve *c, pc_cms_curve *out, pc_error *err);
/* Value equality (kind, parameters, table contents). */
bool pc_cms_curve_eq(const pc_cms_curve *a, const pc_cms_curve *b);

/* Parameters stored for a parametric function type. */
size_t pc_cms_curve_param_count(uint16_t ptype);
bool pc_cms_curve_is_identity(const pc_cms_curve *c);

double pc_cms_curve_eval64(const pc_cms_curve *c, double x);
float pc_cms_curve_eval(const pc_cms_curve *c, float x);
double pc_cms_curve_eval_inverse64(const pc_cms_curve *c, double y);
float pc_cms_curve_eval_inverse(const pc_cms_curve *c, float y);

/* n >= 2 evenly spaced samples over [0, 1] into out (caller-sized). */
void pc_cms_curve_sample(const pc_cms_curve *c, size_t n, float *out);
/* Never decreasing (or never increasing) over [0, 1], within 1e-6. */
bool pc_cms_curve_is_monotonic(const pc_cms_curve *c);

#endif
