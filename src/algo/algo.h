/* photocraft algo: CPU image algorithms (port of crates/algo). Umbrella header.
 *
 * The Filter menu's filters are depth-agnostic (normalised f32 samples read from any surface, in
 * the surface's own colour model), tile/region aware (a filter declares its halo, so results are
 * identical whatever the tiling) and selection aware (output mixed with the original by the
 * selection's coverage). Other modules here: histograms, tone adjustments, resampling,
 * projective transforms, Laplacian pyramids, and (later chunks) selection, retouching, camera
 * raw, panoramas, Liquify and Puppet Warp.
 *
 * Layering: algo includes only base, geom, color and raster (tools/check-layers.sh).
 *
 * ---------------------------------------------------------------------------------------------
 * Conventions for the algo chunks (L5..L11) building on this core (L4):
 *
 * Files. Each Rust module becomes src/algo/<module>.c (+ <module>.h when other modules or later
 * layers call it), split past ~1,500 lines. Add one `#include "algo/<module>.h"` line below for
 * public headers; nothing else in the core needs editing. Public names are pc_algo_<module>_*;
 * enum constants PC_ALGO_*. Tests go to tests/algo/test_<module>.c, parity vectors to the
 * chunk's own tools/rustref module and tests/golden/algo-<x>-vectors.txt.
 *
 * Images. pc_algo_image (image.h) is Rust's `Image`: rect, ch, interleaved floats. Read with
 * pc_algo_image_read / _read_clamped, sample with pc_algo_image_get / _px / _get_edge / _sample;
 * pc_algo_image_crop_into is `src.crop(out)`. pc_algo_premultiply / _unpremultiply take the
 * buffer length.
 *
 * Filters (filter.h). One FilterParams variant = one `const pc_algo_filter_def`:
 *   - a params struct (enums as the C enums in params.h, f32 as float, u32/i32, bool, [f32; N]
 *     as float[N], Vec<[f32;2]> as pc_algo_points, Vec<f32> as pc_algo_floats, Vec<Light> /
 *     pins / paths as pc_algo_lights / ..._pins / pc_algo_blur_paths), described by a
 *     pc_algo_jfield table in Rust declaration order with the serde camelCase names
 *     (PC_ALGO_JF(...) required, PC_ALGO_JFD(...) / optional=true for #[serde(default)], whose
 *     default `init` writes first; PC_ALGO_JFSKIP for #[serde(skip)] caller data such as
 *     Displace's map, held as a borrowed `const pc_algo_image *` the caller keeps alive);
 *     JSON read/write, deep copy and release then come for free;
 *   - halo(params, bounds): `halo()` when bounds is NULL, `halo_for(bounds)` otherwise (only
 *     Relight differs); use pc_algo_halo_reach(v) for the reference's `r(v)` helper;
 *   - kernel(params, prep, src, out, ctx, dst, err): fill dst (zeroed, out.w * out.h * src->ch);
 *     return a status instead of panicking; never read outside src (use the accessors);
 *   - optional prepare/prep_free (shared per run, e.g. Shape Blur's kernel), label override
 *     (Filter Gallery), whole-area `apply` override (box-blur passes, FFT motion blur: set
 *     *handled), flags PC_ALGO_FILTER_CLAMP_TILE / _KEEP_ON_FAIL (Shape Blur).
 * List the defs in your chunk's table file (filters_l5.c, filters_l6.c, filters_l9.c). Other code
 * finds a filter by tag (pc_algo_filter_def_find) or builds one with pc_algo_filter_make; expose
 * a typed params struct + def in your header so callers (engine, camera raw, ...) need no JSON.
 * The 59 tags and labels are fixed in filter.c; JSON errors reproduce serde's texts.
 *
 * Shared helpers (fxutil.h), already ported here; use them instead of re-porting:
 *   fxutil.rs (ctx format, ncol, subtractive, via_rgb, xy, rgba/set_rgba/native, luma,
 *   premul_window, unpremul_px, box_blur_n, gauss_box_radii, gauss_blur_n, cell_point,
 *   smoothstep, add_sample_premul), noise::hash01, other2::{rgb_to_hsl, hsl_to_rgb, hsb_to_rgb},
 *   f32::rem_euclid, selection::color_range, photo_util::{par_rows, par_map} as pc_algo_par_for
 *   (the global pc_pool; per-index work only — combine reductions in index order yourself).
 *   PC_ALGO_MAXC (8) is fxutil's MAXC.
 *
 * Numerics (REWRITE_PLAN.md section 4): keep f32/f64 exactly as Rust; `a.min(b)` is pc_rs_minf,
 * `a.min(LITERAL)` pc_rs_minf_k; float `.sum()` starts at -0.0; `x.powf(2.0)` is x * x and
 * `2f32.powf(x)` exp2f(x); `powi(x, 3)` is (x * x) * x; `as` casts via pc_f32_to_i32 & co; i32
 * arithmetic that may overflow wraps (pc_i32_wrapping_*). Results must not depend on the thread
 * count: parallel loops write disjoint outputs, reductions combine in index order.
 *
 * Never crash: hostile params (NaN, inf, huge radii or sizes) return PC_ERR_INVALID /
 * PC_ERR_LIMIT or behave as the reference does; allocations sized by input are checked
 * (pc_algo_float_count, pc_size_mul*). Where the reference would loop for hours or abort on
 * memory, return PC_ERR_LIMIT and record it in REWRITE_PLAN.md section 7.
 * ------------------------------------------------------------------------------------------- */
#ifndef PC_ALGO_ALGO_H
#define PC_ALGO_ALGO_H

#include "algo/algo_paint.h"
#include "algo/artistic.h"
#include "algo/blur.h"
#include "algo/camera_raw.h"
#include "algo/content_aware.h"
#include "algo/erase.h"
#include "algo/exif.h"
#include "algo/features.h"
#include "algo/filter.h"
#include "algo/fx.h"
#include "algo/fxutil.h"
#include "algo/hdr.h"
#include "algo/histogram.h"
#include "algo/image.h"
#include "algo/inpaint.h"
#include "algo/lens.h"
#include "algo/liquify.h"
#include "algo/magnetic.h"
#include "algo/matting.h"
#include "algo/nonlocal.h"
#include "algo/panorama.h"
#include "algo/params.h"
#include "algo/perspective.h"
#include "algo/photo_util.h"
#include "algo/poisson.h"
#include "algo/puppet.h"
#include "algo/pyramid.h"
#include "algo/quantize.h"
#include "algo/redeye.h"
#include "algo/relight.h"
#include "algo/remove.h"
#include "algo/render2.h"
#include "algo/resample.h"
#include "algo/retouch.h"
#include "algo/scancrop.h"
#include "algo/seam.h"
#include "algo/segment.h"
#include "algo/selection.h"
#include "algo/selection_blur.h"
#include "algo/stack.h"
#include "algo/tone.h"
#include "algo/transform.h"
#include "algo/trap.h"
#include "algo/vanishing.h"
#include "algo/vectorscope.h"
#include "algo/warp.h"
#include "algo/wideangle.h"

#endif
