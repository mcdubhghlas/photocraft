/* Patterns: Edit > Define Pattern, the pattern library (pattern.*), the Patterns panel
 * (pattern.presets.*), Layer > New Fill Layer > Pattern and brush textures from library patterns
 * (port of crates/engine/src/pattern_cmds.rs and crates/engine/src/presets/patterns.rs, E10).
 *
 * Two places hold patterns, as in Photoshop: the app-level library (pc_engine_session.patterns:
 * built-in procedural patterns plus defined/imported ones) and each document's own patterns
 * (pc_document.patterns, saved in files). Commands that apply a pattern copy it into the document
 * in the same history step. Patterns are referenced by "pattern": id | name (document first, then
 * library). This header exports the resolution helpers the Paint Bucket (paint_cmds) and the
 * Pattern Stamp (retouch_cmds) share, plus the library lifecycle session.c drives. */
#ifndef PC_ENGINE_PATTERN_CMDS_H
#define PC_ENGINE_PATTERN_CMDS_H

#include <stdbool.h>
#include <stddef.h>

#include <yyjson.h>

#include "base/status.h"
#include "doc/doc.h"
#include "doc/pattern.h"

#include "engine_internal.h"

/* The built-in procedural pattern library (our own, deterministic ids). *out is an owned array
 * (pc_io_patterns_free or fini each + free) of *n patterns. On OOM *n may be short (never
 * crashes). */
pc_status pc_engine_patterns_builtin(pc_pattern **out, size_t *n, pc_error *err);

/* Build the session's pattern library from the built-ins and fill the Patterns panel's default
 * folders (presets.pattern_groups) with their ids. Called from session_new; leaves a valid but
 * possibly empty library on OOM. */
void pc_engine_pattern_library_init(pc_engine_session *s);
/* Release the session's pattern library. NULL-safe. */
void pc_engine_pattern_library_fini(pc_engine_session *s);

/* Find a pattern by id or name: the active document's first, then the library. An empty key picks
 * the Patterns-panel selection, else the first library pattern. Writes an owned clone into *out
 * and sets *found; *found = false (nothing written) when there is none. */
pc_status pc_engine_pattern_resolve(const pc_engine_session *s, const char *key, size_t keylen,
                                    pc_pattern *out, bool *found, pc_error *err);
/* resolve the "pattern" param (default the selection), erroring when none is found. Writes an
 * owned clone into *out (pc_pattern_fini it). */
pc_status pc_engine_pattern_resolve_param(const pc_engine_session *s, const char *cmd, yyjson_val *p,
                                          pc_pattern *out, pc_error *err);
/* The placement a pattern command asks for: scale (fraction, clamped 0.01..10), angle (deg CCW),
 * link (default true) and phase (pixels). */
void pc_engine_pattern_placement(yyjson_val *p, float *scale, float *angle, bool *link, float phase[2]);
/* Copy pat into the document's patterns unless already there (by id). */
pc_status pc_engine_pattern_ensure_in_doc(pc_document *doc, const pc_pattern *pat, pc_error *err);

#endif /* PC_ENGINE_PATTERN_CMDS_H */
