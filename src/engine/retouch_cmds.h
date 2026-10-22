/* Retouch command helpers exposed for the UI (port of the public helpers of
 * crates/engine/src/retouch_cmds.rs). See retouch_cmds.c / retouch_jobs.inc.
 *
 * Patch Tool live preview: pc_engine_patch_preview returns the active document as paint.patch would
 * leave it (no history step) plus the rectangle it changed, the membrane solved coarser for speed.
 * The Spot-Healing / Clone / Dodge / … trail coverage the UI draws is the paint layer's
 * pc_paint_stroke_coverage (paint/retouch.h); no engine wrapper is needed. */
#ifndef PC_ENGINE_RETOUCH_CMDS_H
#define PC_ENGINE_RETOUCH_CMDS_H

#include "doc/doc.h"     /* pc_doc_arc, pc_rect */
#include "engine.h"      /* pc_engine_session */

/* Patch live-heal preview: the active document as paint.patch with `p` would leave it (no history
 * step), and the rectangle it changed in *out_dmg. *out_doc is an owned doc arc (pc_doc_arc_release).
 * max_cells caps the coarse membrane grid, min_step floors the coarsening (the view's zoom-out).
 * Bad params / no selection / no document are graceful errors; never panics. */
pc_status pc_engine_patch_preview(pc_engine_session *s, yyjson_val *p, uint64_t max_cells,
                                  uint32_t min_step, pc_doc_arc **out_doc, pc_rect *out_dmg, pc_error *err);

#endif /* PC_ENGINE_RETOUCH_CMDS_H */
