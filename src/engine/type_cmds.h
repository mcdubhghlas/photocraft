/* Type (text) commands (port of crates/engine/src/type_cmds.rs, E11) and the cross-module type
 * primitives other engine chunks reference.
 *
 * This file also hosts the canvas-geometry content hooks for type layers
 * (pc_engine_hook_transform_text / pc_engine_hook_refresh_text), relocated from hooks.c.
 *
 * Text offsets exchanged here are UTF-8 BYTE offsets (the Rust pub primitives take byte offsets;
 * the JSON command layer converts character indices to bytes).
 */
#ifndef PC_ENGINE_TYPE_CMDS_H
#define PC_ENGINE_TYPE_CMDS_H

#include "doc/doc.h"
#include "engine.h"

pc_status pc_engine_type_new_layer(const pc_engine_session *session,yyjson_val *params,const char *command,pc_text_layer *out,pc_error *err);
const pc_engine_command *pc_engine_type_mask_specs(size_t *count);

/* The command specs for register_modules() (command.c). */
const pc_engine_command *pc_engine_type_specs(size_t *n);

/* ---- cross-module primitives (Rust type_cmds::{...}) --------------------------------------- */

/* type_cmds::norm_text. Normalises line breaks to "\n". *out is a newly allocated buffer of
 * *out_len bytes plus a NUL (free() it), or NULL on OOM. */
char *pc_engine_norm_text(const char *s, size_t len, size_t *out_len);

/* type_cmds::check_text_unlocked. Refuses to change a type layer under Lock All (its own or an
 * enclosing group's), with Photoshop's message. */
pc_status pc_engine_check_text_unlocked(const pc_document *doc, pc_layer_id id, pc_error *err);

/* type_cmds::refresh. Re-renders the layer's pixels and regenerates its PSD TySh data. Best
 * effort: on failure (OOM, no font engine) the layer is left as-is. */
void pc_engine_type_retag(float dpi, pc_text_layer *text);
void pc_engine_type_refresh(const pc_document *doc, pc_text_layer *t);

/* type_cmds::replace_text. Replaces bytes [a, b) of the layer's text with `new` (new_len bytes,
 * line breaks normalised), keeping styles: inserted text takes the style of the character before
 * it (or after it at the start). Never fails destructively; an allocation failure leaves the
 * layer unchanged. */
void pc_engine_replace_text(pc_text_layer *t, size_t a, size_t b, const char *new_, size_t new_len);

/* ---- shared command helpers (type_cmds.c), used by type_extra/styles/spell/caret ---------- */

/* Photoshop's ranges (validator constants). */
#define PC_TYPE_KERN_MIN (-1000.0)
#define PC_TYPE_KERN_MAX 10000.0
#define PC_TYPE_SIZE_MIN_PT 0.1
#define PC_TYPE_SIZE_MAX_PT 1296.0
#define PC_TYPE_TRACKING_MIN (-1000.0)
#define PC_TYPE_TRACKING_MAX 10000.0
#define PC_TYPE_LEADING_MIN_PT 0.01f
#define PC_TYPE_LEADING_MAX_PT 5000.0f
#define PC_TYPE_BASELINE_SHIFT_MAX_PT 5000.0f

/* Byte offset of character index ci in text[0..len) (len when past the end); and the reverse. */
size_t pc_engine_text_byte_at(const char *text, size_t len, size_t ci);
size_t pc_engine_text_char_at(const char *text, size_t len, size_t bi);

/* Validators (check_kerning / check_size_tracking / check_steps). On failure write the reference's
 * message into emsg[cap] and return false. */
bool pc_engine_check_kerning(yyjson_val *p, char *emsg, size_t cap);
bool pc_engine_check_size_tracking(yyjson_val *p, char *emsg, size_t cap);
bool pc_engine_check_steps(yyjson_val *p, char *emsg, size_t cap);

/* Applies character / paragraph style keys from JSON; returns whether any key was present. */
bool pc_engine_apply_char_props(pc_char_style *s, yyjson_val *p);
bool pc_engine_apply_para_props(pc_paragraph_style *s, yyjson_val *p);

/* Applies f to the character style of bytes [a, b) (splitting runs at the edges). */
typedef void (*pc_engine_char_style_mut_fn)(pc_char_style *st, void *ud);
void pc_engine_style_range(pc_text_layer *t, size_t a, size_t b, pc_engine_char_style_mut_fn f, void *ud);
/* Applies f to every paragraph touching bytes [a, b). */
typedef void (*pc_engine_para_style_mut_fn)(pc_paragraph_style *st, void *ud);
void pc_engine_style_paragraphs(pc_text_layer *t, size_t a, size_t b, pc_engine_para_style_mut_fn f, void *ud);

/* The name a type layer gets from its text (first non-empty line, at most 40 chars). Writes into
 * out[cap] (NUL-terminated); returns the byte length written. */
size_t pc_engine_type_layer_name(const char *text, size_t len, char *out, size_t cap);
/* Whether a type layer still carries the name it got from its text. */
bool pc_engine_is_auto_named(const pc_layer *l);

/* Type layers a direct-formatting command acts on: "layer", "layers", else the selected type
 * layers. *out is an owned array (free()) of *n ids. Error ("select a type layer", a bad id, a
 * single non-type target) otherwise. */
pc_status pc_engine_format_targets(const pc_engine_session *s, yyjson_val *p, pc_layer_id **out, size_t *n,
                                   pc_error *err);
pc_status pc_engine_has_format_target(const pc_engine_session *s, pc_error *reason);

/* One history step for a formatting batch: for every format target, lay the per-layer byte range
 * into (a, b) and call f, then re-render. *out_ids (optional; free()) receives the targets. */
typedef pc_status (*pc_engine_text_target_fn)(pc_text_layer *t, size_t a, size_t b, void *ud, pc_error *err);
pc_status pc_engine_with_text_targets(pc_engine_session *s, yyjson_val *p, const char *label,
                                      pc_engine_text_target_fn f, void *ud, pc_layer_id **out_ids,
                                      size_t *nids, pc_error *err);

/* type.info for one layer (shared by type.create / type.edit / type.setStyle). */
pc_status pc_engine_type_info(const pc_engine_session *s, pc_layer_id id, pc_json_value *out, pc_error *err);

#endif /* PC_ENGINE_TYPE_CMDS_H */
