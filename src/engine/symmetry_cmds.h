/* Per-document painting symmetry (symmetry_cmds.c, E10). The opaque pc_engine_symmetry lives on
 * DocState.symmetry; brush_cmds reflects stroke points through these accessors. */
#ifndef PC_ENGINE_SYMMETRY_CMDS_H
#define PC_ENGINE_SYMMETRY_CMDS_H

#include "engine_internal.h"
#include "paint/brush.h"

const pc_engine_command *pc_engine_symmetry_specs(size_t *n);

/* Mirror passes besides the original: a path axis = 1, a preset = pc_paint_symmetry_mirror_count. */
size_t pc_engine_symmetry_mirror_count(const struct pc_engine_symmetry *sym);
/* Reflect pts into out[0..*count) (each gets n points appended); *count = mirror_count. out must
 * be 3 zero-initialised pc_paint_point_vecs. On failure the vecs keep what was appended. */
pc_status pc_engine_symmetry_reflected_passes(const struct pc_engine_symmetry *sym,
                                              const pc_paint_point *pts, size_t n,
                                              pc_paint_point_vec out[3], size_t *count, pc_error *err);
/* A stroke lying on the axis needs no mirror pass (SymmetryAxis::has_distinct_mirror). */
bool pc_engine_symmetry_has_distinct_mirror(const pc_paint_point *a, const pc_paint_point *b, size_t n);

#endif /* PC_ENGINE_SYMMETRY_CMDS_H */
