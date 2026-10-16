/* Preset panels data model and built-in presets (port of crates/engine/src/presets, E2b).
 *
 * Window > Gradients, Patterns, Styles, Shapes, Tool Presets, Clone Source and Swatches each own a
 * set of named presets in named folders (groups). The engine holds that data so every frontend
 * (GUI, CLI, MCP) sees and edits the same sets through `<kind>.presets.*` / `swatches.*` /
 * `cloneSource.*` commands. The built-in sets are our own designs (clean-room).
 *
 * This header is internal to the engine crate (not engine.h). It defines the PresetState model,
 * the generic group machinery the panel commands share (find / unique name / group index /
 * group-edit actions, matching the Rust `Group<T>` + `Named` trait), the gradient, swatch, style,
 * shape, tool-preset and clone-source types, and the built-in-data constructors. Persistence
 * (`to_json` / `load_presets_json`) lives on the Session.
 *
 * Never crashes on any params or a corrupt preferences blob (REWRITE_PLAN.md rule 9). */
#ifndef PC_ENGINE_PRESETS_H
#define PC_ENGINE_PRESETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "base/json.h"
#include "base/mem.h"
#include "base/status.h"
#include "color/blend.h"
#include "doc/effects.h"
#include "doc/vector.h"

/* ---- generic group machinery (Rust Group<T> + Named) -------------------------------------- */

/* One folder of presets: a name and a contiguous array of `n` items (each `ops.size` bytes). */
typedef struct pc_preset_group {
    char *name;   /* owned */
    void *items;  /* owned contiguous array, ops.size stride; may be NULL when n == 0 */
    size_t n;
    size_t cap;
} pc_preset_group;

/* A growable vector of groups. */
typedef struct pc_preset_groups {
    pc_preset_group *v;
    size_t n;
    size_t cap;
} pc_preset_groups;

/* Operations on one item kind (the Rust Named trait plus clone/fini). set_name takes a borrowed
 * string and stores an owned copy (false on OOM). clone deep-copies src into an uninitialised dst. */
typedef struct pc_preset_item_ops {
    size_t size;
    const char *(*get_name)(const void *item);
    bool (*set_name)(void *item, const char *name);
    void (*fini)(void *item);
    pc_status (*clone)(const void *src, void *dst, pc_error *err);
} pc_preset_item_ops;

void pc_preset_groups_init(pc_preset_groups *g);
void pc_preset_groups_fini(pc_preset_groups *g, const pc_preset_item_ops *ops);
pc_status pc_preset_groups_clone(const pc_preset_groups *src, pc_preset_groups *out,
                                 const pc_preset_item_ops *ops, pc_error *err);
/* Append a new empty group named `name` (owned copy). Returns the new group, or NULL on OOM. */
pc_preset_group *pc_preset_groups_push(pc_preset_groups *g, const char *name, pc_error *err);
/* Append one item (bitwise-moved from *item, which must not be freed afterwards) to group gi. */
pc_status pc_preset_group_push(pc_preset_groups *g, size_t gi, const void *item,
                               const pc_preset_item_ops *ops, pc_error *err);

/* Locate a preset by name (optionally within group), as (group index, item index). */
bool pc_preset_find(const pc_preset_groups *g, const char *name, const char *group,
                    const pc_preset_item_ops *ops, size_t *gi, size_t *ii);
/* `base`, or `base 2`, `base 3`... unique across every group (start = 2). Caller frees. */
char *pc_preset_unique_name(const pc_preset_groups *g, const char *base, const pc_preset_item_ops *ops);
/* Unique group name: `base`, or `base 1`, `base 2`... (start = 1). Caller frees. */
char *pc_preset_unique_group(const pc_preset_groups *g, const char *base);
/* Index of `group` (created at the end when missing), or the first group (created as "Presets"
 * when there is none, when group is NULL). Returns SIZE_MAX on OOM. */
size_t pc_preset_group_index(pc_preset_groups *g, const char *group, pc_error *err);

/* The generic group commands shared by every preset kind: rename / delete / move / newGroup /
 * renameGroup / deleteGroup. Writes the serde result into *out on success. */
pc_status pc_preset_edit_groups(pc_preset_groups *g, const char *action, yyjson_val *p,
                                const char *cmd, const pc_preset_item_ops *ops, pc_json_value *out,
                                pc_error *err);

/* Params doc string for the generic group commands. */
extern const char *const PC_PRESET_GROUP_EDIT_PARAMS;
/* The same text as a compile-time literal, for `params` in a static command-spec table. */
#define GROUP_EDIT_PARAMS_DOC                                                                      \
    "{\"action\":\"rename|delete|move|newGroup|renameGroup|deleteGroup\",\"preset\":name|[names] "  \
    "(rename/delete/move),\"group\":name? (narrows the lookup; the group for "                      \
    "renameGroup/deleteGroup),\"name\":str (rename: new name; newGroup/renameGroup: group "         \
    "name),\"to\":group (move),\"index\":n? (move)}"

/* ---- gradients ---------------------------------------------------------------------------- */

typedef enum pc_stop_color_kind {
    PC_STOP_FOREGROUND = 0,
    PC_STOP_BACKGROUND = 1,
    PC_STOP_RGB = 2
} pc_stop_color_kind;

typedef struct pc_stop_color {
    pc_stop_color_kind kind;
    float rgb[3]; /* RGB only */
} pc_stop_color;

typedef struct pc_gradient_cstop {
    float loc; /* 0..1 */
    pc_stop_color color;
} pc_gradient_cstop;

typedef struct pc_gradient_ostop {
    float loc;     /* 0..1 */
    float opacity; /* 0..1 */
} pc_gradient_ostop;

typedef struct pc_gradient_preset {
    char *name; /* owned */
    pc_gradient_cstop *stops;
    size_t nstops;
    pc_gradient_ostop *opacity;
    size_t nopacity;
} pc_gradient_preset;

void pc_gradient_preset_fini(pc_gradient_preset *g);
pc_status pc_gradient_preset_clone(const pc_gradient_preset *src, pc_gradient_preset *out, pc_error *err);
pc_status pc_gradient_preset_fg_to_bg(pc_gradient_preset *out, pc_error *err);
const pc_preset_item_ops *pc_gradient_preset_ops(void);
/* Built-in gradient groups. */
pc_status pc_gradients_builtin(pc_preset_groups *out, pc_error *err);
/* Serialise a gradient to the command JSON ({name,stops,transparency}). */
yyjson_mut_val *pc_gradient_preset_to_json(yyjson_mut_doc *d, const pc_gradient_preset *g);
/* Parse `{"stops":[...],"transparency":[...]}` into *out (name owned copy). */
pc_status pc_gradient_from_params(const char *name, yyjson_val *p, const char *cmd,
                                  pc_gradient_preset *out, pc_error *err);
/* Replace *g's transparency stops from `p["transparency"]` when present. */
pc_status pc_gradient_with_transparency(pc_gradient_preset *g, yyjson_val *p, const char *cmd, pc_error *err);
/* Apply a gradient.presets.stop edit to a copy of *g. Writes *out (finalise with
 * pc_gradient_preset_fini) on success. fg/bg are straight RGBA. */
pc_status pc_gradient_apply_preset_stop(const pc_gradient_preset *g, yyjson_val *p, const float fg[4],
                                        const float bg[4], const char *cmd, pc_gradient_preset *out,
                                        pc_error *err);

/* ---- swatches ----------------------------------------------------------------------------- */

/* Swatches panel limits (swatch_cmds): most swatches, most groups, longest name (characters). */
#define PC_SWATCH_MAX_SWATCHES 100000
#define PC_SWATCH_MAX_GROUPS 10000
#define PC_SWATCH_MAX_NAME 255

typedef enum pc_swatch_model {
    PC_SWATCH_RGB = 0,
    PC_SWATCH_HSB,
    PC_SWATCH_CMYK,
    PC_SWATCH_LAB,
    PC_SWATCH_GRAY,
    PC_SWATCH_BOOK
} pc_swatch_model;

typedef struct pc_swatch_color {
    pc_swatch_model model;
    float v[4];              /* RGB/HSB (3), CMYK (4), LAB (3), GRAY (1) */
    uint16_t book_space;     /* BOOK only */
    uint16_t book_values[4]; /* BOOK only */
} pc_swatch_color;

typedef enum pc_swatch_kind {
    PC_SWATCH_PROCESS = 0,
    PC_SWATCH_GLOBAL,
    PC_SWATCH_SPOT
} pc_swatch_kind;

typedef struct pc_swatch {
    char *name; /* owned */
    pc_swatch_color color;
    pc_swatch_kind kind;
} pc_swatch;

void pc_swatch_fini(pc_swatch *s);
pc_status pc_swatch_clone(const pc_swatch *src, pc_swatch *out, pc_error *err);
const pc_preset_item_ops *pc_swatch_ops(void);
pc_status pc_swatches_builtin(pc_preset_groups *out, pc_error *err);
const char *pc_swatch_model_id(pc_swatch_model m);
const char *pc_swatch_kind_id(pc_swatch_kind k);
bool pc_swatch_kind_parse(const char *s, pc_swatch_kind *out);
/* HSB (hue degrees) to RGB in the same space. */
void pc_swatch_hsb_to_rgb(const float hsb[3], float out[3]);
/* Parse a command colour (`"#rrggbb"` or {model,values}); false with *msg set on error. */
bool pc_swatch_color_from_param(yyjson_val *v, pc_swatch_color *out, char msg[256]);
/* Color Picker display units. *out gets `n` values; returns n. */
size_t pc_swatch_color_display_values(const pc_swatch_color *c, double out[5]);
/* {model,values} JSON. */
yyjson_mut_val *pc_swatch_color_to_json(yyjson_mut_doc *d, const pc_swatch_color *c);
/* `#rrggbb` of an RGB working-space colour into buf[8]. */
void pc_swatch_hex(const float rgb[3], char buf[8]);

/* A swatch -> RGB working-space converter through the CMS (built once per library change). */
typedef struct pc_swatch_conv pc_swatch_conv;
struct pc_engine_session;
pc_swatch_conv *pc_swatch_conv_new(const struct pc_engine_session *s); /* session working spaces */
pc_swatch_conv *pc_swatch_conv_builtin(void);                          /* sRGB / coated / sGray */
void pc_swatch_conv_free(pc_swatch_conv *c);
void pc_swatch_conv_rgb(const pc_swatch_conv *c, const pc_swatch_color *col, float out[3]);

/* ---- styles ------------------------------------------------------------------------------- */

typedef struct pc_style_preset {
    char *name; /* owned */
    pc_effect *effects;
    size_t neffects;
    bool has_blend;
    pc_blend_mode blend;
    bool has_fill_opacity;
    float fill_opacity; /* 0..1 */
} pc_style_preset;

void pc_style_preset_fini(pc_style_preset *s);
pc_status pc_style_preset_clone(const pc_style_preset *src, pc_style_preset *out, pc_error *err);
const pc_preset_item_ops *pc_style_preset_ops(void);
pc_status pc_styles_builtin(pc_preset_groups *out, pc_error *err);
yyjson_mut_val *pc_style_preset_to_json(yyjson_mut_doc *d, const pc_style_preset *s);

/* ---- shapes ------------------------------------------------------------------------------- */

typedef struct pc_shape_preset {
    char *name; /* owned */
    pc_path path;
} pc_shape_preset;

void pc_shape_preset_fini(pc_shape_preset *s);
pc_status pc_shape_preset_clone(const pc_shape_preset *src, pc_shape_preset *out, pc_error *err);
const pc_preset_item_ops *pc_shape_preset_ops(void);
pc_status pc_shapes_builtin(pc_preset_groups *out, pc_error *err);
/* Parse the built-in shape path language (M/L/C/Q/Z/O and `!`). false with *msg set on error. */
bool pc_shape_parse_path(const char *src, pc_path *out, char msg[256]);
/* Map path's control bounds into rect=[x,y,w,h]; keep preserves aspect (centred). */
pc_status pc_shape_fit(const pc_path *path, const double rect[4], bool keep, pc_path *out, pc_error *err);

/* ---- tool presets ------------------------------------------------------------------------- */

#define PC_TOOL_PRESETS_MAX 1000

typedef struct pc_tool_preset {
    char *name; /* owned */
    char *tool; /* owned */
    pc_json_value options;
} pc_tool_preset;

void pc_tool_preset_fini(pc_tool_preset *t);
pc_status pc_tool_preset_clone(const pc_tool_preset *src, pc_tool_preset *out, pc_error *err);
pc_status pc_tool_presets_builtin(pc_tool_preset **out, size_t *n, pc_error *err);
const pc_preset_item_ops *pc_tool_preset_ops(void);
#define PC_PRESET_MAX_NAME 255 /* longest preset name (brush_preset_cmds::MAX_NAME) */

/* ---- clone sources ------------------------------------------------------------------------ */

#define PC_CLONE_SLOTS 5

typedef struct pc_clone_source {
    bool has_source;
    double source[2];
    bool has_anchor;
    double anchor[2];
    bool has_layer;
    uint64_t layer;
    double scale[2]; /* W, H percent */
    double rotation; /* degrees CCW */
    bool flip_h, flip_v, linked;
} pc_clone_source;

typedef struct pc_clone_overlay {
    bool show;
    float opacity; /* percent */
    bool clipped, auto_hide, invert;
    int blend; /* 0 normal, 1 darken, 2 lighten, 3 difference */
} pc_clone_overlay;

typedef struct pc_clone_sources {
    pc_clone_source slots[PC_CLONE_SLOTS];
    size_t active;
    pc_clone_overlay overlay;
} pc_clone_sources;

void pc_clone_source_default(pc_clone_source *c);
void pc_clone_sources_default(pc_clone_sources *c);
const char *pc_clone_overlay_blend_name(int blend);
bool pc_clone_overlay_blend_parse(const char *s, int *out);

/* ---- PresetState -------------------------------------------------------------------------- */

typedef struct pc_preset_state {
    pc_preset_groups gradients;     /* pc_gradient_preset */
    pc_gradient_preset gradient;    /* the Gradient tool's current gradient */
    pc_preset_groups styles;        /* pc_style_preset */
    pc_preset_groups shapes;        /* pc_shape_preset */
    pc_preset_groups pattern_groups;/* char* (pattern ids) */
    bool has_pattern;
    char *pattern;                  /* the selected pattern id, or NULL */
    pc_tool_preset *tool_presets;
    size_t ntool_presets;
    size_t cap_tool_presets;
    pc_clone_sources clone;
    pc_json_value layer_defaults;   /* object: effect kind -> params */
    pc_preset_groups swatches;      /* pc_swatch */
    /* E3: File > New named configurations (document.presets.*) and the Recent list
     * (document.recent.*). The struct layout and all logic live in document_preset_cmds.c; this
     * state holds owned arrays released by pc_engine_document_presets_fini. */
    struct pc_document_preset *documents;
    size_t ndocuments;
    struct pc_document_settings *recent_documents;
    size_t nrecent;
    uint64_t rev;
} pc_preset_state;

/* E3 (document_preset_cmds.c): release the File > New document presets / recent list of a preset
 * state (NULL-safe). Called from pc_preset_state_fini so the E2b state owner need not know the
 * struct layout. */
void pc_engine_document_presets_fini(pc_preset_state *st);
/* E3: serialise the File > New presets/recent into the mutable object `root` of doc `d` (keys
 * "documents" and "recent_documents"); and load them back from a saved blob (a malformed entry is
 * dropped, the rest kept). Declared here so presets_persist.c (E2b) can call them without the
 * DocumentSettings layout. `d` and `root` are a mutable yyjson doc and object; `v` is a parsed
 * yyjson value. */
void pc_engine_document_presets_to_json(const pc_preset_state *st, struct yyjson_mut_doc *d,
                                        struct yyjson_mut_val *root);
void pc_engine_document_presets_load(pc_preset_state *st, struct yyjson_val *v);

/* Default (built-in) PresetState. On OOM leaves a valid but possibly partial state (never crashes). */
void pc_preset_state_init(pc_preset_state *st);
void pc_preset_state_fini(pc_preset_state *st);

/* The "String" item ops (pattern-group ids). */
const pc_preset_item_ops *pc_preset_string_ops(void);

#endif /* PC_ENGINE_PRESETS_H */
