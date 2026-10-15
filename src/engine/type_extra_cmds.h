/* The Type menu (port of crates/engine/src/type_extra_cmds.rs, E11): anti-aliasing, orientation,
 * point <-> paragraph conversion, Create Work Path, Convert to Shape, Rasterize, Warp Text, the
 * OpenType toggles, Paste Lorem Ipsum, Update All Text Layers, missing-font replacement and the
 * default type styles.
 *
 * type.attachToPath / type.detachFromPath are PERMANENTLY DEFERRED: Type-on-a-Path needs a
 * doc-model TextShape::Path variant that src/doc does not have (only Point/Box). Owed to a doc
 * forward-port (REWRITE_PLAN.md §8). */
#ifndef PC_ENGINE_TYPE_EXTRA_CMDS_H
#define PC_ENGINE_TYPE_EXTRA_CMDS_H

#include "doc/doc.h"
#include "engine.h"
#include "text/fonts.h" /* pc_strlist */

/* The command specs for register_modules() (command.c). */
const pc_engine_command *pc_engine_type_extra_specs(size_t *n);

/* Warnings for the font families a type layer uses that the font database cannot supply (one
 * "font '<name>' is not installed; ..." string each). Appends owned strings to *out. Referenced
 * by type.create / type.edit / type.setStyle (type_cmds.c). */
pc_status pc_engine_missing_font_warnings_for_layer(const pc_text_layer *t, pc_strlist *out, pc_error *err);

#endif /* PC_ENGINE_TYPE_EXTRA_CMDS_H */
