/* Warp primitives exported by the Filter/Transform chunk (E9), for the transform commands that
 * depend on them (E4 edit.transform / select.transformSelection, port of
 * crates/engine/src/warp_cmds.rs::warp_from_params).
 *
 * The warp COMMANDS themselves are deferred (they need transform_cmds E4, smart_cmds E6 and
 * vector_cmds/type_cmds E11); only the JSON -> geom::Warp builder is available now. */
#ifndef PC_ENGINE_WARP_CMDS_H
#define PC_ENGINE_WARP_CMDS_H

#include "engine_internal.h"
#include "geom/warp.h"

/* Build a pc_warp from a warp command's JSON params over the box `rect` [x0,y0,x1,y1]
 * (warp_cmds::warp_from_params). A full `warp` object is taken verbatim; otherwise `style`
 * (lenient name), `bend`/`hDistort`/`vDistort`/`vertical`, and for a custom style a `mesh` or a
 * `grid` identity mesh. `cmd` names the command in error messages. *out is written only on success
 * (caller frees with pc_warp_fini); on failure it is untouched and err is filled. Never crashes. */
pc_status pc_engine_warp_from_params(const char *cmd, yyjson_val *p, const double rect[4], pc_warp *out,
                                     pc_error *err);

#endif /* PC_ENGINE_WARP_CMDS_H */
