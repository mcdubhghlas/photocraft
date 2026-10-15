/* Smart Objects: rendering, placement and source primitives (port of
 * crates/engine/src/smart_cmds.rs, E6).
 *
 * This chunk is a PRIMITIVE PROVIDER. Smart objects keep a source (embedded bytes or a linked
 * path) plus a placement (affine transform, or a projective map for Distort/Perspective) and a
 * stack of smart filters; `cache` is the rendered appearance the compositors draw. The engine
 * owns re-rendering. The functions here are what the sibling command modules reuse:
 *
 *   - E4 (copy/stamp, free transform): source_bytes, refresh_layer, placement/set/transform.
 *   - E5 (layer menu): refresh_layer.
 *   - E9 (distort/warp geometry): placement/set_placement/transform_placement.
 *   - Edit > Purge: purge_stack_cache / stack_cache_bytes.
 *
 * DEFERRED (reported to the coordinator, see smart_cmds.c §7): the smart-FILTER subsystem
 * (apply_smart_filters, add_smart_filter, the layer.smartFilter.* commands and filtered
 * re-rendering) needs the engine-level filter dispatcher crate::filters::apply_filter_to_surface
 * (distort/lens/adjust/plugin/filters_ext + the id->algo mapping), which E9 did not export.
 * Smart objects with NO visible smart filters render fully; a filtered smart object keeps its
 * existing cache. The Layer > Smart Objects commands (convert, contents, relink, ...) need file
 * I/O (E3 write), image_cmds/transform_cmds (E4) and the pattern library (E10); they are deferred
 * too. None of this is the public engine API (engine.h); it is internal to the engine crate. */
#ifndef PC_ENGINE_SMART_CMDS_H
#define PC_ENGINE_SMART_CMDS_H

#include "engine_internal.h"
#include "algo/transform.h" /* pc_algo_homography */

/* The source file of a smart object: embedded bytes, the PSD's embedded linked-layer data (found
 * by uuid), or a linked file on disk. *found=false (name/bytes untouched) when unavailable. On
 * success the caller owns *name (pc_str_fini) and holds a retained reference in *bytes
 * (pc_blob_release). */
pc_status pc_engine_smart_source_bytes(const pc_metadata *meta, const pc_smart_source *src, bool *found,
                                       pc_str *name, pc_blob **bytes, pc_error *err);

/* Renders a smart object from its source into *out (the caller owns it, pc_surface_fini).
 * *available is false (out untouched) when the source can't be read, OR when the smart object has
 * visible smart filters (the filter subsystem is deferred -> callers keep the existing cache). A
 * smart object with no visible filters renders fully (its placed source). */
pc_status pc_engine_smart_render(const pc_document *doc, const pc_smart_object *sm, bool *available,
                                 pc_surface *out, pc_error *err);

/* Re-renders smart layer `l` (which lives in `doc`) in place: on success *updated reports whether
 * the cache was refreshed (false = source unavailable or a filtered smart object, cache kept). A
 * non-smart layer is *updated=false, PC_OK. */
pc_status pc_engine_smart_refresh_layer(const pc_document *doc, pc_layer *l, bool *updated, pc_error *err);
/* Re-renders the smart object `id` of `doc`. PC_ERR_NOT_FOUND for no such layer; PC_ERR_INVALID
 * when the layer is not a smart object. */
pc_status pc_engine_smart_refresh(pc_document *doc, pc_layer_id id, bool *updated, pc_error *err);

/* Where the source lands in the document: its projective map (Distort/Perspective) or its affine
 * transform as a homography. */
void pc_engine_smart_placement(const pc_smart_object *sm, pc_algo_homography *out);
/* An affine transform as a homography (source -> document). */
pc_algo_homography pc_engine_smart_affine_homography(pc_affine a);
/* Sets where the source lands: an affine map is stored exactly in `transform`; a projective one in
 * `perspective`, with `transform` its affine approximation at the source origin. */
void pc_engine_smart_set_placement(pc_smart_object *sm, const pc_algo_homography *h);
/* Composes `a` (document -> document) onto the smart object's placement. */
void pc_engine_smart_transform_placement(pc_smart_object *sm, pc_affine a);
/* Rounds float noise off a composed transform (snap_affine). */
pc_affine pc_engine_smart_snap_affine(pc_affine a);
/* Moves a smart object by whole pixels without re-rendering (shifts the placement, cache and
 * filter mask). */
pc_status pc_engine_smart_shift(pc_smart_object *sm, int32_t dx, int32_t dy, pc_error *err);

/* Bytes held by rendered smart filter stages (0 until the filter subsystem lands). */
size_t pc_engine_smart_stack_cache_bytes(void);
/* Drop every rendered smart filter stage (Edit > Purge). Returns the bytes released. */
size_t pc_engine_smart_purge_stack_cache(void);
/* How many times these contents were decoded at this pixel format (0 = never / evicted). */
uint32_t pc_engine_smart_source_decode_count(const uint8_t *bytes, size_t len, pc_pixel_format fmt);

/* The Layer > Smart Objects / Layer > Smart Filter command specs (smart_cmds_run.c,
 * register_modules() in command.c). */
const pc_engine_command *pc_engine_smart_specs(size_t *n);

#endif /* PC_ENGINE_SMART_CMDS_H */
