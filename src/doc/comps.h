/* Layer comps and artboards (port of crates/doc/src/comps.rs). Conventions: doc/adjust.h. */
#ifndef PC_DOC_COMPS_H
#define PC_DOC_COMPS_H

#include "color/blend.h"
#include "doc/effects.h"
#include "geom/geom.h"

typedef uint64_t pc_layer_id;
typedef struct pc_document pc_document;
typedef struct pc_layer pc_layer;

typedef struct pc_comp_appearance {
    pc_blend_mode blend;
    float opacity;
    float fill_opacity;
    pc_effects effects;
} pc_comp_appearance;

/* One layer's state in a comp; has_* false = not recorded. */
typedef struct pc_comp_layer_state {
    pc_layer_id layer;
    bool has_visible;
    bool visible;
    bool has_position;
    int32_t position[2];
    bool has_appearance;
    pc_comp_appearance appearance;
} pc_comp_layer_state;

typedef struct pc_layer_comp {
    uint32_t id; /* PSD compID; never 0 (0 is the Last Document State) */
    pc_str name;
    pc_str comment;
    bool apply_visibility;
    bool apply_position;
    bool apply_appearance;
    pc_comp_layer_state *states; /* document walk order */
    size_t nstates;
} pc_layer_comp;

void pc_comp_layer_state_fini(pc_comp_layer_state *s);
pc_status pc_comp_layer_state_clone(const pc_comp_layer_state *s, pc_comp_layer_state *out, pc_error *err);
bool pc_comp_layer_state_eq(const pc_comp_layer_state *a, const pc_comp_layer_state *b);
void pc_layer_comp_fini(pc_layer_comp *c);
pc_status pc_layer_comp_clone(const pc_layer_comp *c, pc_layer_comp *out, pc_error *err);
bool pc_layer_comp_eq(const pc_layer_comp *a, const pc_layer_comp *b);

int32_t pc_layer_comp_captured_info(const pc_layer_comp *c); /* 1 visibility, 2 position, 4 appearance */
void pc_layer_comp_set_captured_info(pc_layer_comp *c, int32_t v);
const pc_comp_layer_state *pc_layer_comp_state(const pc_layer_comp *c, pc_layer_id id);
/* Ids of recorded layers missing from doc (malloc'd; free()). */
pc_status pc_layer_comp_missing_layers(const pc_layer_comp *c, const pc_document *doc, pc_layer_id **out, size_t *n,
                                       pc_error *err);

/* Content bounds used for comp positions (artboard rect; empty for adjustment/fill layers and
 * plain groups). */
pc_rect pc_comp_position_bounds(const pc_layer *l);
bool pc_comp_layer_position(const pc_layer *l, int32_t out[2]);
pc_status pc_comp_capture_states(const pc_document *doc, pc_comp_layer_state **out, size_t *n, pc_error *err);
void pc_comp_layer_states_free(pc_comp_layer_state *s, size_t n);
uint32_t pc_next_comp_id(const pc_document *doc);

typedef enum pc_artboard_bg_kind {
    PC_ARTBOARD_WHITE,
    PC_ARTBOARD_BLACK,
    PC_ARTBOARD_TRANSPARENT,
    PC_ARTBOARD_CUSTOM
} pc_artboard_bg_kind;

typedef struct pc_artboard_background {
    pc_artboard_bg_kind kind;
    pc_color custom; /* CUSTOM only */
} pc_artboard_background;

bool pc_artboard_background_eq(const pc_artboard_background *a, const pc_artboard_background *b);
/* sRGB colour under the contents; false = transparent. */
bool pc_artboard_background_rgba(const pc_artboard_background *b, float out[4]);
int32_t pc_artboard_background_psd_type(const pc_artboard_background *b); /* 1..4 */
const char *pc_artboard_background_name(const pc_artboard_background *b);

typedef struct pc_artboard {
    pc_rect rect;
    pc_artboard_background background;
    pc_str preset; /* "" = custom */
} pc_artboard;

void pc_artboard_init(pc_artboard *out, pc_rect rect); /* white, no preset */
void pc_artboard_fini(pc_artboard *a);
pc_status pc_artboard_clone(const pc_artboard *a, pc_artboard *out, pc_error *err);
bool pc_artboard_eq(const pc_artboard *a, const pc_artboard *b);

/* Top-level artboards bottom to top. */
typedef struct pc_artboard_ref {
    pc_layer_id layer;
    const pc_str *name;
    const pc_artboard *artboard;
} pc_artboard_ref;

pc_status pc_document_artboards(const pc_document *doc, pc_artboard_ref **out, size_t *n, pc_error *err);
bool pc_document_has_artboards(const pc_document *doc);
const pc_layer_comp *pc_document_comp(const pc_document *doc, uint32_t id);
/* The artboard containing layer id (itself or its top-level ancestor). */
bool pc_document_artboard_of(const pc_document *doc, pc_layer_id id, pc_layer_id *out);
const pc_artboard *pc_layer_artboard(const pc_layer *l);
pc_artboard *pc_layer_artboard_mut(pc_layer *l);

#endif
