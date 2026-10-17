/* Filter › Liquify, Edit › Puppet Warp and Edit › Perspective Warp, plus their Layer › Smart
 * Objects twins (port of crates/engine/src/distort_cmds.rs).
 *
 * All three take their whole edit as data, so each is one replayable command and one history step.
 * On a smart object each becomes a smart filter re-applied to a fresh render of the source; on a
 * pixel layer it edits the pixels in place. */
#ifndef PC_ENGINE_DISTORT_CMDS_H
#define PC_ENGINE_DISTORT_CMDS_H

#include <stdbool.h>
#include <stddef.h>

#include <yyjson.h>

#include "base/status.h"
#include "engine.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* The command specs for register_modules() (command.c). */
const pc_engine_command *pc_engine_distort_specs(size_t *n);

/* Smart-filter re-apply hook (distort_cmds::apply_to_surface): re-runs a stored Liquify / Puppet /
 * Perspective Warp onto a rendered smart-object surface. On a match *applied=true and *out holds the
 * result; otherwise *applied=false and *out is untouched. Called from pc_engine_hook_filter_subtype
 * (plugin_cmds.c). Never crashes. */
pc_status pc_engine_distort_apply_to_surface(const char *id, yyjson_val *params, const pc_surface *surf,
                                             pc_rect canvas, pc_surface *out, bool *applied,
                                             pc_error *err);

#endif /* PC_ENGINE_DISTORT_CMDS_H */
