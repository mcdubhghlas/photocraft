/* Layer styles: Layer > Layer Style commands and the effect-building primitives (port of
 * crates/engine/src/layer_style.rs, E6).
 *
 * The commands add or replace a layer's effects (pc_effects). This header exposes the pure
 * primitives the reference keeps `pub`: building an effect from JSON params, overlaying params
 * onto an existing effect, the effect's kind name, the built-in contour presets and the
 * contour <-> param / .shc conversions. These are reused by the Layer Style dialog, the Contour
 * Editor (E12), smart filters and PSD mapping. None of this is the public engine API (engine.h);
 * it is internal to the engine crate.
 *
 * Never crashes on any params (REWRITE_PLAN.md rule 9): absent, null or malformed keys leave the
 * effect unchanged; unknown kinds return false. */
#ifndef PC_ENGINE_LAYER_STYLE_H
#define PC_ENGINE_LAYER_STYLE_H

#include "engine_internal.h"
#include "doc/effects.h"

/* The `layer.layerStyle.*` command specs (register_modules() in command.c). */
const pc_engine_command *pc_engine_layer_style_specs(size_t *n);

/* Parses a gradient style name ("radial", "angle", "reflected", "diamond"; else Linear). */
pc_gradient_style pc_engine_gradient_style(const char *name);

/* The effect's kind as the commands and dialog name it ("dropShadow", "innerShadow", ...). */
const char *pc_engine_effect_kind_of(const pc_effect *e);

/* Builds an effect of `kind` from JSON params (Photoshop units). Returns false (and leaves *out
 * untouched) for an unknown kind. p may be NULL ("no params" -> factory settings). On success the
 * caller owns *out (pc_effect_fini). */
bool pc_engine_effect_from_params(const char *kind, yyjson_val *p, pc_effect *out);

/* Applies the params present in `p` onto an existing effect, in the command's parameter units.
 * Absent or unparseable keys leave the effect unchanged. p may be NULL (no change). */
void pc_engine_overlay_effect(pc_effect *fx, yyjson_val *p);

/* Factory defaults for one effect kind as a {..} object (what a new effect starts from). Returns
 * false for an unknown kind. On success *out is a new value in doc (owned by doc). */
bool pc_engine_effect_defaults(const char *kind, yyjson_mut_doc *doc, yyjson_mut_val **out);

/* The built-in contour called `name` ("Linear" is the identity); false (out untouched) for names
 * not shipped, so imported contours are carried rather than clobbered. On success the caller owns
 * *out (pc_contour_fini). */
bool pc_engine_builtin_contour(const char *name, pc_contour *out);

/* The param naming a contour: "Linear", a preset name, or a {name, points, corners} curve. On
 * success *out is a new value in doc (owned by doc). */
pc_status pc_engine_contour_param(const pc_contour *c, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                  pc_error *err);

/* The contour a `contour` param names (a preset name or a curve object). Returns false (out
 * untouched) for an unknown name or a malformed curve, so the effect keeps its contour. On
 * success the caller owns *out (pc_contour_fini). */
bool pc_engine_contour_from_param(yyjson_val *v, pc_contour *out);

/* The contours of an .shc contour-preset file, each as a `contour` param value appended to `arr`
 * (a mutable array in `doc`). Unusable curves are skipped. */
pc_status pc_engine_contours_from_shc(const uint8_t *bytes, size_t len, yyjson_mut_doc *doc,
                                      yyjson_mut_val *arr, pc_error *err);

/* An .shc file holding `contour`. *out is a malloc'd buffer the caller frees (pc_buf_fini). */
pc_status pc_engine_contour_to_shc(const pc_contour *contour, pc_buf *out, pc_error *err);

#endif /* PC_ENGINE_LAYER_STYLE_H */
