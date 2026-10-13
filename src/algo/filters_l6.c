/* Filter registry of chunk L6 (distort, stylize, pixelate, render, other, gallery, video, Filter
 * Gallery): every pc_algo_filter_def this chunk ports, NULL-terminated (see filter_tables.h). The
 * defs live with their kernels (other.c, stylize.c, ...); their parameter structs are in fx.h. */
#include "algo/filter_tables.h"

#include <stddef.h>

#include "algo/artistic.h"
#include "algo/fx.h"

/* clang-format off */
const pc_algo_filter_def *const pc_algo_filters_l6[] = {
    &pc_algo_def_minimum,
    &pc_algo_def_maximum,
    &pc_algo_def_offset,
    &pc_algo_def_mosaic,
    &pc_algo_def_emboss,
    &pc_algo_def_find_edges,
    &pc_algo_def_solarize,
    &pc_algo_def_invert,
    &pc_algo_def_desaturate,
    &pc_algo_def_twirl,
    &pc_algo_def_pinch,
    &pc_algo_def_spherize,
    &pc_algo_def_wave,
    &pc_algo_def_ripple,
    &pc_algo_def_polar,
    &pc_algo_def_color_halftone,
    &pc_algo_def_crystallize,
    &pc_algo_def_facet,
    &pc_algo_def_fragment,
    &pc_algo_def_mezzotint,
    &pc_algo_def_pointillize,
    &pc_algo_def_diffuse,
    &pc_algo_def_extrude,
    &pc_algo_def_oil_paint,
    &pc_algo_def_tiles,
    &pc_algo_def_trace_contour,
    &pc_algo_def_wind,
    &pc_algo_def_displace,
    &pc_algo_def_shear,
    &pc_algo_def_zigzag,
    &pc_algo_def_fibers,
    &pc_algo_def_lens_flare,
    &pc_algo_def_lighting,
    &pc_algo_def_tilt_shift,
    &pc_algo_def_iris_blur,
    &pc_algo_def_field_blur,
    &pc_algo_def_spin_blur,
    &pc_algo_def_path_blur,
    &pc_algo_def_custom,
    &pc_algo_def_hsb_hsl,
    &pc_algo_def_color_to_alpha,
    &pc_algo_def_deinterlace,
    &pc_algo_def_ntsc,
    &pc_algo_def_filter_gallery,
    NULL,
};
/* clang-format on */
