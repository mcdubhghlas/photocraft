/* Camera Raw-style continuous RGB histogram ribbons (port of crates/ui-egui/src/rgb_histogram.rs).
 *
 * Raw bins stay exact; a local five-bin binomial display kernel softens quantization combs without
 * bridging wide empty tonal ranges. One shared vertical scale covers R, G and B; edge clipping
 * spikes are pinned at the edges and excluded from smoothing the interior.
 *
 * The filled ribbon is built as a list of coloured trapezoids ("quads"): every subspan is split
 * where two channel curves cross, so each subspan has a stable channel stacking order and exact
 * overlap colours without draw-order-dependent alpha stacking or cracks between bars. This mirrors
 * the egui `Mesh` the Rust reference emits; the shell converts the quads to Nuklear fills.
 *
 * Pure geometry: no SDL/GL/Nuklear here (model layer, headless-testable). Never allocates past a
 * hard cap; bad or empty rectangles produce an empty mesh. */
#ifndef PC_UI_RGB_HISTOGRAM_H
#define PC_UI_RGB_HISTOGRAM_H

#include <stddef.h>

#include "algo/histogram.h"
#include "ui/theme.h"

#define PC_UI_HIST_BINS PC_ALGO_HIST_BINS /* 256 display bins. */

/* A coloured trapezoid with vertical left/right sides. Corner order matches the reference:
 * p[0]=(x0,bottom0) p[1]=(x0,top0) p[2]=(x1,top1) p[3]=(x1,bottom1); triangles (0,1,2) and
 * (0,2,3). Positions are in the same pixel space as the rect passed to the builder. */
typedef struct pc_ui_hist_quad {
    float p[4][2];
    pc_ui_color color;
} pc_ui_hist_quad;

/* A growable mesh of coloured quads. Init to zero or with pc_ui_hist_mesh_init before use. */
typedef struct pc_ui_hist_mesh {
    pc_ui_hist_quad *quads;
    size_t count;
    size_t cap;
    int overflow; /* set when the hard cap was hit and further quads were dropped */
} pc_ui_hist_mesh;

void pc_ui_hist_mesh_init(pc_ui_hist_mesh *mesh);
void pc_ui_hist_mesh_free(pc_ui_hist_mesh *mesh);
void pc_ui_hist_mesh_clear(pc_ui_hist_mesh *mesh);

/* Normalised display curves for the three channels (R, G, B), each 0..1. Smoothing + shared
 * scaling exactly as the reference. `out` is fully written; never reads past the histogram. */
void pc_ui_rgb_histogram_curves(const pc_algo_rgb_histogram *h, float out[3][PC_UI_HIST_BINS]);

/* Build the filled ribbon mesh for histogram `h` inside rect (x0,y0)-(x1,y1) (y grows downward;
 * bottom = y1). A non-finite or non-positive rect clears the mesh and adds nothing. */
void pc_ui_rgb_histogram_build(pc_ui_hist_mesh *mesh, float x0, float y0, float x1, float y1,
                               const pc_algo_rgb_histogram *h, const pc_ui_tokens *t);

/* One subspan of the ribbon between two sets of channel heights a[3] and b[3] over the x range
 * x[2], split at curve crossings. Exposed for tests; append to `mesh`. */
void pc_ui_rgb_histogram_span(pc_ui_hist_mesh *mesh, float x0_rect, float y0_rect, float x1_rect,
                              float y1_rect, const float x[2], const float a[3], const float b[3],
                              const pc_ui_tokens *t);

/* Outline polyline for one channel (0=R,1=G,2=B) of the display curves, as (x,y) points inside
 * the rect. Writes at most `max` points and returns how many were written (PC_UI_HIST_BINS + 2
 * when `max` allows). Returns 0 for an empty channel or a bad rect. */
size_t pc_ui_rgb_histogram_outline(const float curves[3][PC_UI_HIST_BINS], int channel, float x0,
                                   float y0, float x1, float y1, float (*pts)[2], size_t max);

/* Neutral tone marker x position (Camera Raw tone weights) inside the rect, or a negative value
 * when the rect or rgb is not finite. */
float pc_ui_rgb_histogram_sample_x(float x0, float x1, const float rgb[3]);

#endif
