/* Filter > Lens Correction (port of crates/algo/src/lens.rs): Brown-Conrady distortion, lateral
 * chromatic aberration, vignetting and perspective / rotation / scale, with Photoshop's edge
 * modes. The geometry runs as an inverse map per output pixel (bicubic, premultiplied when there
 * is alpha), per 128 px output tile in parallel; results do not depend on the thread count.
 * Generic profiles interpolate typical distortion and vignetting over the 35 mm-equivalent focal
 * length (manufacturer data is proprietary).
 *
 * Plain-data structs; functions are reentrant. */
#ifndef PC_ALGO_LENS_H
#define PC_ALGO_LENS_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* What fills the output where the corrected image doesn't reach. */
typedef enum pc_algo_lens_edge_kind {
    PC_ALGO_LENS_EDGE_EXTENSION, /* repeat the edge pixels */
    PC_ALGO_LENS_EDGE_TRANSPARENCY,
    PC_ALGO_LENS_EDGE_COLOR /* straight RGBA in `color` */
} pc_algo_lens_edge_kind;

typedef struct pc_algo_lens_edge {
    pc_algo_lens_edge_kind kind;
    float color[4];
} pc_algo_lens_edge;

/* Room for "Generic Lens (<any f64 with no decimals> mm equivalent)". */
#define PC_ALGO_LENS_NAME_MAX 352

/* Distortion, vignetting and lateral CA coefficients. */
typedef struct pc_algo_lens_profile {
    char name[PC_ALGO_LENS_NAME_MAX];
    double k1, k2, p1, p2;
    double vignette[3];     /* falloff 1 + v1 r^2 + v2 r^4 + v3 r^6 */
    double ca_red, ca_blue; /* radial scale of red / blue relative to green (1 + ca) */
} pc_algo_lens_profile;

/* Generic profile for a 35 mm-equivalent focal length. */
void pc_algo_lens_generic_profile(double focal_35mm, pc_algo_lens_profile *out);

/* Lens Correction parameters in Photoshop's dialog units. */
typedef struct pc_algo_lens_correction {
    bool has_profile;
    pc_algo_lens_profile profile; /* applied first when has_profile */
    bool correct_distortion, correct_vignette, correct_ca;
    double distortion;           /* Remove Distortion -100..=100 */
    double red_cyan;             /* Fix Red/Cyan Fringe -100..=100 */
    double blue_yellow;          /* Fix Blue/Yellow Fringe -100..=100 */
    double vignette_amount;      /* -100..=100 (positive lightens the corners) */
    double vignette_midpoint;    /* 0..=100 */
    double vertical, horizontal; /* perspective -100..=100 */
    double angle;                /* degrees, positive turns clockwise */
    double scale;                /* percent (50..=150) */
    pc_algo_lens_edge edge;
} pc_algo_lens_correction;

void pc_algo_lens_correction_default(pc_algo_lens_correction *lc);

/* Where an output pixel samples the source: green (and alpha / non-RGB channels) at p, red at
 * pr, blue at pb; the colour is then multiplied by gain. */
typedef struct pc_algo_lens_sample {
    double p[2], pr[2], pb[2];
    float gain;
} pc_algo_lens_sample;

/* The inverse map of a correction over a frame. */
typedef struct pc_algo_lens_map {
    double c[2], r, k1, k2, p1, p2, ca_r, ca_b, vig_profile[3], vig_amount, vig_mid, hinv[9], rot_s, rot_c,
        scale;
} pc_algo_lens_map;

void pc_algo_lens_map_init(pc_algo_lens_map *m, const pc_algo_lens_correction *lc, pc_rect frame);
/* Output px -> where to sample; false when the keystone sends it to infinity. */
bool pc_algo_lens_map_sample(const pc_algo_lens_map *m, double x, double y, pc_algo_lens_sample *out);

/* Smallest scale (percent) at which the corrected image covers the whole frame (Auto Scale). */
double pc_algo_lens_auto_scale(const pc_algo_lens_correction *lc, pc_rect frame);
/* Angle (degrees) that makes the line a -> b horizontal or vertical (the Straighten tool). */
double pc_algo_lens_straighten_angle(const double a[2], const double b[2]);

/* Inverse map callback: where output pixel centre (x, y) samples; false = outside. Called from
 * several threads at once. */
typedef bool (*pc_algo_lens_map_fn)(void *ctx, double x, double y, pc_algo_lens_sample *out);

/* Resamples src through an inverse map over out_area. `frame` is the image the map refers to:
 * samples clamp to it, and pixels mapping outside it take the edge mode. `out` is initialised on
 * success (src's format, with alpha added for transparent edges). */
pc_status pc_algo_lens_remap(const pc_surface *src, pc_rect frame, pc_rect out_area, pc_algo_lens_edge edge,
                             pc_algo_lens_map_fn map, void *ctx, pc_surface *out, pc_error *err);
/* Applies a lens correction to src over frame (normally the canvas). */
pc_status pc_algo_lens_correct(const pc_surface *src, pc_rect frame, const pc_algo_lens_correction *lc,
                               pc_surface *out, pc_error *err);

#endif
