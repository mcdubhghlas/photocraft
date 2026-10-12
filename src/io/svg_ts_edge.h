/* Private to svg_ts_edge.c / svg_ts_scan.c / svg_ts_hair.c: tiny-skia 0.11.4's fixed-point edges
 * (edge.rs) and edge building (edge_builder.rs with edge_clipper.rs, line_clipper.rs). */
#ifndef PC_IO_SVG_TS_EDGE_H
#define PC_IO_SVG_TS_EDGE_H

#include "io/svg_ts.h"

enum { TS_EDGE_LINE, TS_EDGE_QUAD, TS_EDGE_CUBIC };

/* Edge (LineEdge / QuadraticEdge / CubicEdge). x, dx are 16.16; first_y / last_y are rows of
 * the (supersampled) scan grid. prev / next link the active list (indices, -1 none). The c*
 * fields are the curve's forward-difference state (q* for quads). */
typedef struct ts_edge {
    int32_t prev, next;
    int32_t x, dx, first_y, last_y;
    int8_t winding;
    uint8_t kind;
    int8_t curve_count;
    uint8_t curve_shift, dshift;
    int32_t cx, cy, cdx, cdy, cddx, cddy, cdddx, cdddy, last_x, last_y_fixed;
} ts_edge;

bool ts_quad_update(ts_edge *e);
bool ts_cubic_update(ts_edge *e);

/* BasicEdgeBuilder::build_edges: the path's edges (clipped to clip when non-NULL) in *out
 * (malloc'd; NULL with *nout 0 when fewer than two edges or a non-finite clipped segment).
 * PC_ERR_NOMEM, PC_ERR_LIMIT past TS_MAX_EDGES. */
pc_status ts_build_edges(const ts_path *path, const sv_rect *clip, int32_t shift, ts_edge **out,
                         size_t *nout);

/* line_clipper::intersect */
bool ts_line_intersect(const sv_pt src[2], sv_rect clip, sv_pt dst[2]);

/* scan::fill_rect (aliased) */
void ts_scan_fill_rect_aliased(sv_rect r, ts_clip clip, const sv_spans *out);

int32_t ts_fdot16_div(int32_t numer, int32_t denom);
int32_t ts_fdot16_mul(int32_t a, int32_t b);
int32_t ts_fdot16_fast_div(int32_t a, int32_t b);

#endif
