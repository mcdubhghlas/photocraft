/* Snapping (View > Snap / Snap To) and smart guides, as pure geometry
 * (port of crates/engine/src/snap.rs).
 *
 * Build a pc_snap_targets once per gesture from the document, then each pointer move asks it to
 * snap a point (pc_snap_targets_snap_point) or a moving rectangle (pc_snap_targets_snap_rect).
 * Distances are in document pixels; callers convert their screen threshold with the zoom
 * (8 px / zoom). Results carry pc_snap_lines so the UI can draw what was snapped to. */
#ifndef PC_ENGINE_SNAP_H
#define PC_ENGINE_SNAP_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"
#include "doc/doc.h"

typedef enum pc_snap_kind {
    PC_SNAP_GUIDE,
    PC_SNAP_GRID,
    PC_SNAP_DOCUMENT_EDGE,
    PC_SNAP_DOCUMENT_CENTER,
    PC_SNAP_LAYER_EDGE,
    PC_SNAP_LAYER_CENTER,
    PC_SNAP_SELECTION
} pc_snap_kind;

/* Layer-to-layer and document-centre alignments are what Photoshop shows as smart guides. */
bool pc_snap_kind_is_smart(pc_snap_kind k);

/* A line to snap to: pos on one axis (x for vertical lines), span its extent on the other. */
typedef struct pc_snap_target {
    double pos;
    pc_snap_kind kind;
    double span[2];
} pc_snap_target;

/* A snapped alignment, for drawing: a vertical line at x = pos from y = from to to (or horizontal
 * at y = pos). */
typedef struct pc_snap_line {
    bool vertical;
    double pos;
    double from;
    double to;
    pc_snap_kind kind;
} pc_snap_line;

/* Which target families are active (View > Snap To). grid_step is the subdivision spacing in
 * document pixels, independently for x and y. */
typedef struct pc_snap_options {
    bool guides;
    bool grid;
    bool layers;
    bool document;
    bool selection;
    double grid_step[2];
} pc_snap_options;

/* The default options (guides/layers/document/selection on, grid off, 18 px step). */
pc_snap_options pc_snap_options_default(void);

/* Snap targets on both axes. Owns its x/y arrays (free with pc_snap_targets_fini). */
typedef struct pc_snap_targets {
    pc_snap_target *x; /* vertical lines (x positions) */
    size_t nx, cap_x;
    pc_snap_target *y; /* horizontal lines (y positions) */
    size_t ny, cap_y;
    bool has_grid;
    double grid[2];
    double extent[4]; /* [x0, y0, x1, y1] for grid lines and guides */
} pc_snap_targets;

void pc_snap_targets_init(pc_snap_targets *t);
void pc_snap_targets_fini(pc_snap_targets *t);
bool pc_snap_targets_is_empty(const pc_snap_targets *t);

/* Targets of doc for the enabled families; layers in exclude[n] (the ones being moved) skipped. */
pc_status pc_snap_targets_from_document(const pc_document *doc, const pc_snap_options *opts,
                                        const pc_layer_id *exclude, size_t nexclude, pc_snap_targets *out,
                                        pc_error *err);

/* Add a rectangle's edges and centre lines (r = [x0, y0, x1, y1]). */
pc_status pc_snap_targets_add_rect(pc_snap_targets *t, const double r[4], pc_snap_kind edge,
                                   pc_snap_kind center, pc_error *err);

/* The closest target to v within tol on one axis (vertical = x positions); guides win ties.
 * Returns true and fills *out when one is within tolerance. */
bool pc_snap_targets_nearest(const pc_snap_targets *t, bool vertical, double v, double tol,
                             pc_snap_target *out);

/* Snap a point: each axis independently to its nearest target within tol. out_p gets the snapped
 * point; *lines (owned, free()) gets *nlines drawing lines. */
pc_status pc_snap_targets_snap_point(const pc_snap_targets *t, const double p[2], double tol,
                                     double out_p[2], pc_snap_line **lines, size_t *nlines, pc_error *err);

/* Snap a moving rectangle r = [x0, y0, x1, y1]: out_delta gets the offset to add; *lines (owned)
 * gets the alignments made. */
pc_status pc_snap_targets_snap_rect(const pc_snap_targets *t, const double r[4], double tol,
                                    double out_delta[2], pc_snap_line **lines, size_t *nlines, pc_error *err);

/* The Free-Transform bounds of a layer for snapping (None for empty layers, groups and maskless
 * adjustment layers). Returns true and fills out[4] = [x0, y0, x1, y1]. */
bool pc_snap_layer_rect(const pc_document *doc, pc_layer_id id, double out[4]);

/* Bounding union of n rectangles ([x0, y0, x1, y1]); false when n is 0. */
bool pc_snap_union(const double (*rects)[4], size_t n, double out[4]);

#endif /* PC_ENGINE_SNAP_H */
