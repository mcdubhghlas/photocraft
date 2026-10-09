/* Synthetic CMYK output profiles (port of crates/cms/src/synth.rs).
 *
 * Forward model (CMYK -> XYZ): Yule-Nielsen modified Neugebauer (n = 2) over the 16 overprints
 * with Demichel weights and parabolic dot gain; ISO 12647-2 coated-paper Lab aims for the K-free
 * primaries. AToB: lut16 of the model. BToA1: GCR black generation then a bounded least-squares
 * CMY solve under the total ink limit. BToA0/2: perceptual gamut compression first. A plausible
 * coated-offset profile, not a measured characterisation. Thread-safe; uses the global pool. */
#ifndef PC_CMS_SYNTH_H
#define PC_CMS_SYNTH_H

#include <stddef.h>

#include "base/status.h"
#include "profile.h"

#define PC_SYNTH_DESCRIPTION "Photocraft Coated CMYK (synthetic, 300% TAC, medium GCR)"

typedef struct pc_cmyk_params {
    const char *description; /* borrowed; copied into the profile */
    double tac;              /* total area coverage limit (3.0 = 300 %) */
    double gcr_start;        /* gray component where black generation starts */
    double k_max;            /* maximum black */
    double gcr_gamma;        /* exponent of the black curve above gcr_start */
    double tvi[4];           /* tone value increase at 50 % for C, M, Y, K */
    size_t grid_a2b;         /* AToB (4D) grid points */
    size_t grid_b2a;         /* BToA (3D) grid points */
} pc_cmyk_params;

/* The reference defaults (what the shipped profile was generated with). */
pc_cmyk_params pc_cmyk_params_default(void);

typedef struct pc_cmyk_model {
    double prim_n[16][3]; /* absolute XYZ of the overprints (bit mask C=1 M=2 Y=4 K=8), ^(1/n) */
    double paper[3];      /* absolute XYZ of the paper */
    double tvi[4];
} pc_cmyk_model;

void pc_cmyk_model_coated(const double tvi[4], pc_cmyk_model *out);
/* Absolute XYZ of an ink combination (each coverage in [0, 1]). */
void pc_cmyk_model_xyz(const pc_cmyk_model *m, const double cmyk[4], double out[3]);
/* ICC-relative Lab (paper = L* 100). */
void pc_cmyk_model_lab(const pc_cmyk_model *m, const double cmyk[4], double out[3]);

/* Colorimetric separation of a relative Lab target; warm (nullable) seeds the solve. Writes the
 * CMYK and returns the residual dE76. */
double pc_cmyk_separate(const pc_cmyk_model *m, const pc_cmyk_params *p, const double target[3], const double *warm,
                        double cmyk[4]);

/* Generate a synthetic CMYK profile (with encoded bytes). Slow at the default grid sizes. */
pc_status pc_synth_cmyk_profile(const pc_cmyk_params *p, pc_profile *out, pc_error *err);
pc_status pc_synth_coated_cmyk(pc_profile *out, pc_error *err);

#endif
