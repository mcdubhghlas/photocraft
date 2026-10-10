/* Filter Gallery: the 47 "artistic" filters of Photoshop's gallery (Artistic, Brush Strokes,
 * Distort, Sketch, Stylize and Texture), applied as a stack of effect layers in order (port of
 * crates/algo/src/artistic.rs; the pixel work is artistic_fx.c / artistic_fx2.c).
 *
 * Each filter is described by a parameter notation string (`{"pencilWidth":1..24=4,
 * "texture":"canvas|brick"}`, first choice = default) shared with the engine registry and the
 * dialogs. A pc_algo_gallery_effect is one effect layer: the filter, its values (choice values are
 * indices into the choice list, bools 0/1; keys the notation does not list are kept verbatim) and
 * three straight sRGB RGBA colours. Implemented clean-room from observed behaviour.
 *
 * The FilterParams variant is pc_algo_def_filter_gallery with pc_algo_filter_gallery_params;
 * its label is the single effect's name when the stack has one effect.
 *
 * Ownership: effects own their value keys (fini/clone). Thread safety: tables are immutable;
 * effects are plain data. */
#ifndef PC_ALGO_ARTISTIC_H
#define PC_ALGO_ARTISTIC_H

#include <stdbool.h>
#include <stddef.h>

#include "algo/filter.h"
#include "base/status.h"

typedef enum pc_algo_gallery_filter {
    /* Artistic */
    PC_ALGO_GAL_COLORED_PENCIL,
    PC_ALGO_GAL_CUTOUT,
    PC_ALGO_GAL_DRY_BRUSH,
    PC_ALGO_GAL_FILM_GRAIN,
    PC_ALGO_GAL_FRESCO,
    PC_ALGO_GAL_NEON_GLOW,
    PC_ALGO_GAL_PAINT_DAUBS,
    PC_ALGO_GAL_PALETTE_KNIFE,
    PC_ALGO_GAL_PLASTIC_WRAP,
    PC_ALGO_GAL_POSTER_EDGES,
    PC_ALGO_GAL_ROUGH_PASTELS,
    PC_ALGO_GAL_SMUDGE_STICK,
    PC_ALGO_GAL_SPONGE,
    PC_ALGO_GAL_UNDERPAINTING,
    PC_ALGO_GAL_WATERCOLOR,
    /* Brush Strokes */
    PC_ALGO_GAL_ACCENTED_EDGES,
    PC_ALGO_GAL_ANGLED_STROKES,
    PC_ALGO_GAL_CROSSHATCH,
    PC_ALGO_GAL_DARK_STROKES,
    PC_ALGO_GAL_INK_OUTLINES,
    PC_ALGO_GAL_SPATTER,
    PC_ALGO_GAL_SPRAYED_STROKES,
    PC_ALGO_GAL_SUMI_E,
    /* Distort */
    PC_ALGO_GAL_DIFFUSE_GLOW,
    PC_ALGO_GAL_GLASS,
    PC_ALGO_GAL_OCEAN_RIPPLE,
    /* Sketch (foreground = ink, background = paper) */
    PC_ALGO_GAL_BAS_RELIEF,
    PC_ALGO_GAL_CHALK_CHARCOAL,
    PC_ALGO_GAL_CHARCOAL,
    PC_ALGO_GAL_CHROME,
    PC_ALGO_GAL_CONTE_CRAYON,
    PC_ALGO_GAL_GRAPHIC_PEN,
    PC_ALGO_GAL_HALFTONE_PATTERN,
    PC_ALGO_GAL_NOTE_PAPER,
    PC_ALGO_GAL_PHOTOCOPY,
    PC_ALGO_GAL_PLASTER,
    PC_ALGO_GAL_RETICULATION,
    PC_ALGO_GAL_STAMP,
    PC_ALGO_GAL_TORN_EDGES,
    PC_ALGO_GAL_WATER_PAPER,
    /* Stylize */
    PC_ALGO_GAL_GLOWING_EDGES,
    /* Texture */
    PC_ALGO_GAL_CRAQUELURE,
    PC_ALGO_GAL_GRAIN,
    PC_ALGO_GAL_MOSAIC_TILES,
    PC_ALGO_GAL_PATCHWORK,
    PC_ALGO_GAL_STAINED_GLASS,
    PC_ALGO_GAL_TEXTURIZER,
    PC_ALGO_GALLERY_FILTERS
} pc_algo_gallery_filter;

_Static_assert(PC_ALGO_GALLERY_FILTERS == 47, "the gallery has 47 filters");

/* The gallery's folders, in order. */
extern const char *const pc_algo_gallery_categories[6];
/* Photoshop's default Neon Glow colour (a saturated sky blue). */
extern const float pc_algo_neon_default[4];

/* Camel-case key ("coloredPencil"), engine command id ("filter.gallery.coloredPencil"), display
 * name, folder and parameter notation. "" for an out-of-range filter. */
const char *pc_algo_gallery_filter_key(pc_algo_gallery_filter f);
const char *pc_algo_gallery_filter_command_id(pc_algo_gallery_filter f);
const char *pc_algo_gallery_filter_name(pc_algo_gallery_filter f);
const char *pc_algo_gallery_filter_category(pc_algo_gallery_filter f);
const char *pc_algo_gallery_filter_params_doc(pc_algo_gallery_filter f);
/* Looks a filter up by key, command id or display name (ASCII case-insensitive). */
bool pc_algo_gallery_filter_from_key(const char *k, size_t len, pc_algo_gallery_filter *out);
/* Whether the filter reads the foreground / background colours. */
bool pc_algo_gallery_filter_uses_colours(pc_algo_gallery_filter f);
/* The enum's serde descriptor ("GalleryFilter", the keys). */
extern const pc_algo_enum_desc PC_ALGO_ENUM_GALLERY_FILTER;

#define PC_ALGO_GALLERY_MAX_PARAMS 12
#define PC_ALGO_GALLERY_MAX_CHOICES 12
#define PC_ALGO_GALLERY_NAME_MAX 32

typedef enum pc_algo_gallery_param_kind {
    PC_ALGO_GALLERY_RANGE,
    PC_ALGO_GALLERY_CHOICE, /* the first choice is the default; stored as the index */
    PC_ALGO_GALLERY_BOOL
} pc_algo_gallery_param_kind;

/* One numeric / choice / bool parameter (colours are not listed; they are effect fields). */
typedef struct pc_algo_gallery_param {
    char key[PC_ALGO_GALLERY_NAME_MAX];
    pc_algo_gallery_param_kind kind;
    float min, max, def; /* RANGE */
    bool flag;           /* BOOL default */
    size_t nchoices;     /* CHOICE */
    char choices[PC_ALGO_GALLERY_MAX_CHOICES][PC_ALGO_GALLERY_NAME_MAX];
} pc_algo_gallery_param;

/* Parses the notation of f into out[0..cap); returns the number of parameters. */
size_t pc_algo_gallery_params(pc_algo_gallery_filter f, pc_algo_gallery_param *out, size_t cap);

/* One stored value (key owned). */
typedef struct pc_algo_gallery_value {
    char *key;
    size_t key_len;
    float v;
} pc_algo_gallery_value;

/* One effect layer. `values` is sorted by key bytes. */
typedef struct pc_algo_gallery_effect {
    pc_algo_gallery_filter filter;
    pc_algo_gallery_value *values;
    size_t nvalues;
    float foreground[4], background[4], color[4];
} pc_algo_gallery_effect;

/* GalleryEffect::new: defaults, black ink on white paper, the neon colour. */
pc_status pc_algo_gallery_effect_new(pc_algo_gallery_filter f, pc_algo_gallery_effect *out, pc_error *err);
void pc_algo_gallery_effect_fini(pc_algo_gallery_effect *e); /* NULL-safe */
pc_status pc_algo_gallery_effect_clone(const pc_algo_gallery_effect *e, pc_algo_gallery_effect *out,
                                       pc_error *err);
/* Inserts or replaces a stored value verbatim. */
pc_status pc_algo_gallery_effect_put(pc_algo_gallery_effect *e, const char *key, size_t len, float v,
                                     pc_error *err);
/* set: a numeric value clamped to its range, a choice by index (rounded, clamped), or a bool;
 * unknown keys are ignored. */
pc_status pc_algo_gallery_effect_set(pc_algo_gallery_effect *e, const char *key, float v, pc_error *err);
/* set_choice: a choice by name (ASCII case-insensitive); *ok false when not one of the choices. */
pc_status pc_algo_gallery_effect_set_choice(pc_algo_gallery_effect *e, const char *key, const char *name,
                                            bool *ok, pc_error *err);
/* A numeric value (the default when unset). */
float pc_algo_gallery_effect_get(const pc_algo_gallery_effect *e, const char *key);
bool pc_algo_gallery_effect_flag(const pc_algo_gallery_effect *e, const char *key);
/* The chosen name of a choice parameter, written to buf ("" when key is not a choice). */
const char *pc_algo_gallery_effect_choice(const pc_algo_gallery_effect *e, const char *key,
                                          char buf[PC_ALGO_GALLERY_NAME_MAX]);

/* serde of one effect (a map; missing fields take their defaults). */
pc_status pc_algo_gallery_effect_from_json(yyjson_val *v, pc_algo_gallery_effect *out, pc_error *err);
yyjson_mut_val *pc_algo_gallery_effect_to_json(yyjson_mut_doc *d, const pc_algo_gallery_effect *e);

/* ---- the FilterParams variant ---- */

typedef struct pc_algo_gallery_effects {
    pc_algo_gallery_effect *v;
    size_t n;
} pc_algo_gallery_effects;

typedef struct pc_algo_filter_gallery_params {
    pc_algo_gallery_effects effects;
} pc_algo_filter_gallery_params;

extern const pc_algo_filter_def pc_algo_def_filter_gallery;

/* Total reach of a stack of effects (the variant's halo radius). */
int32_t pc_algo_filter_gallery_reach(const pc_algo_gallery_effects *effects);

#endif
