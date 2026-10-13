/* The PhotoCraft document model: pure data, no rendering, no UI (port of crates/doc/src/lib.rs).
 * This is the umbrella header for src/doc.
 *
 * Layer order: inside every group (and the root), children[0] is the BOTTOM layer, matching
 * compositing order and the PSD file order. UIs display the list reversed.
 *
 * Tree walks are iterative (explicit stacks), so no depth of nesting can overflow the C stack.
 * Lookups that need a stack (pc_document_layer and friends) use a fixed 128-level buffer and the
 * heap beyond it; if that allocation fails they report "not found". Conventions: doc/adjust.h. */
#ifndef PC_DOC_DOC_H
#define PC_DOC_DOC_H

#include "base/json.h"
#include "doc/adjust.h"
#include "doc/analysis.h"
#include "doc/blend_if.h"
#include "doc/comps.h"
#include "doc/effects.h"
#include "doc/fill.h"
#include "doc/mode.h"
#include "doc/pattern.h"
#include "doc/slices.h"
#include "doc/text.h"
#include "doc/text_styles.h"
#include "doc/variables.h"
#include "doc/vector.h"
#include "doc/video.h"
#include "geom/warp.h"

typedef uint64_t pc_layer_id;
typedef uint64_t pc_doc_id;

/* Identity of the source contents shared by smart-object instances, independent of their bytes:
 * ordinary layer duplicates share it, independent contents get a fresh one. */
typedef uint64_t pc_smart_contents_id;

/* Fresh ids from one process-wide atomic counter shared by layers, documents and smart contents
 * (from 1). */
pc_layer_id pc_layer_id_fresh(void);
pc_doc_id pc_doc_id_fresh(void);
pc_smart_contents_id pc_smart_contents_id_fresh(void);
/* Advance the counter past max (saturating), so loaded ids never collide; never moves back. */
void pc_ensure_ids_above(uint64_t max);

typedef struct pc_locks {
    bool transparency;
    bool pixels;
    bool position;
    bool artboard;
    bool all;
} pc_locks;

pc_locks pc_locks_union(pc_locks a, pc_locks b);
bool pc_locks_eq(pc_locks a, pc_locks b);

/* Layer colour labels, in the reference's declaration (serde) order. */
typedef enum pc_label_color {
    PC_LABEL_NONE,
    PC_LABEL_RED,
    PC_LABEL_ORANGE,
    PC_LABEL_YELLOW,
    PC_LABEL_GREEN,
    PC_LABEL_BLUE,
    PC_LABEL_VIOLET,
    PC_LABEL_GRAY,
    PC_LABEL_SEAFOAM,
    PC_LABEL_INDIGO,
    PC_LABEL_MAGENTA,
    PC_LABEL_FUCHSIA
} pc_label_color;
#define PC_LABEL_COLOR_COUNT 12

/* Display order (None, Red, Orange, Yellow, Green, Seafoam, Blue, Indigo, Magenta, Fuchsia,
 * Violet, Gray). */
extern const pc_label_color PC_LABEL_COLOR_ALL[PC_LABEL_COLOR_COUNT];
const char *pc_label_color_id(pc_label_color c);    /* "none", "seafoam", ... */
const char *pc_label_color_label(pc_label_color c); /* "No Color", "Seafoam", ... */
const char *pc_label_color_name(pc_label_color c);  /* serde name: "None", "Seafoam", ... */
bool pc_label_color_from_id(const char *id, size_t len, pc_label_color *out);

/* A layer mask: grayscale surface whose untouched pixels read as its default (0 hide, 1
 * reveal). */
typedef struct pc_layer_mask {
    pc_surface surface;
    bool enabled;
    bool linked;
    float density;
    float feather;
} pc_layer_mask;

void pc_layer_mask_reveal_all(pc_layer_mask *out); /* never fails */
void pc_layer_mask_hide_all(pc_layer_mask *out);
void pc_layer_mask_fini(pc_layer_mask *m);
pc_status pc_layer_mask_clone(const pc_layer_mask *m, pc_layer_mask *out, pc_error *err);
bool pc_layer_mask_eq(const pc_layer_mask *a, const pc_layer_mask *b);
/* Effective value at a pixel, including density (1 when disabled). */
float pc_layer_mask_value(const pc_layer_mask *m, int32_t x, int32_t y);
/* Effective values over r, row-major, into out of exactly w * h floats (else PC_ERR_INVALID). */
pc_status pc_layer_mask_values_into(const pc_layer_mask *m, pc_rect r, float *out, size_t n, pc_error *err);

/* Deepest legal group nesting (see the reference's MAX_GROUP_DEPTH docs). */
#define PC_MAX_GROUP_DEPTH 100

typedef struct pc_layer pc_layer;

typedef struct pc_group {
    pc_layer *children;
    size_t nchildren;
    bool expanded;
    bool has_artboard; /* a top-level artboard: children clipped to its rect */
    pc_artboard artboard;
} pc_group;

/* Embedded file bytes, or a linked path. */
typedef struct pc_smart_source {
    bool linked;
    pc_str file_name; /* embedded */
    pc_blob *bytes;   /* embedded (NULL = empty) */
    pc_str path;      /* linked */
} pc_smart_source;

/* A non-destructive filter: a command id plus its parameters. */
typedef struct pc_smart_filter {
    pc_str command;
    pc_json_value params;
    pc_blend_mode blend;
    float opacity;
    bool visible;
} pc_smart_filter;

void pc_smart_filter_fini(pc_smart_filter *f);
pc_status pc_smart_filter_clone(const pc_smart_filter *f, pc_smart_filter *out, pc_error *err);
bool pc_smart_filter_eq(const pc_smart_filter *a, const pc_smart_filter *b);

typedef struct pc_smart_object {
    pc_smart_source source;
    pc_affine transform;
    pc_smart_filter *smart_filters;
    size_t nsmart_filters;
    bool has_cache;
    pc_surface cache;
    pc_blob *psd_raw; /* SoLd/PlLd/SoLE block data; NULL = None */
    bool filters_enabled;
    bool has_filter_mask;
    pc_layer_mask filter_mask;
    bool has_warp;
    pc_warp warp;
    bool has_stack_mode;
    pc_stack_mode stack_mode;
    bool has_perspective;
    double perspective[9]; /* row-major 3x3, source -> document */
    pc_smart_contents_id contents_id;
} pc_smart_object;

/* A fresh contents id, no filters, no PSD data, filters enabled. Takes over *source and *cache (moved; cache
 * may be NULL for none). Never fails. */
void pc_smart_object_init(pc_smart_object *out, pc_smart_source *source, pc_affine transform, pc_surface *cache);
void pc_smart_object_fini(pc_smart_object *s);
pc_status pc_smart_object_clone(const pc_smart_object *s, pc_smart_object *out, pc_error *err);
bool pc_smart_object_eq(const pc_smart_object *a, const pc_smart_object *b);

/* Photoshop-rendered pixels for a fill layer, valid while `fill` equals the layer's fill. */
typedef struct pc_fill_cache {
    pc_fill fill;
    pc_surface surface;
} pc_fill_cache;

typedef enum pc_layer_kind {
    PC_LAYER_RASTER,
    PC_LAYER_GROUP,
    PC_LAYER_ADJUSTMENT,
    PC_LAYER_FILL,
    PC_LAYER_TEXT,
    PC_LAYER_SHAPE,
    PC_LAYER_SMART
} pc_layer_kind;

typedef struct pc_layer_content {
    pc_layer_kind kind;
    union {
        pc_surface raster;
        pc_group group;
        pc_adjustment adjustment;
        pc_fill fill;
        pc_text_layer text;
        pc_shape_layer shape;
        pc_smart_object smart;
    } u;
} pc_layer_content;

void pc_layer_content_fini(pc_layer_content *c);
pc_status pc_layer_content_clone(const pc_layer_content *c, pc_layer_content *out, pc_error *err);
bool pc_layer_content_eq(const pc_layer_content *a, const pc_layer_content *b);
const char *pc_layer_content_kind_name(const pc_layer_content *c); /* "Pixel", "Type", "Smart Object" */
const char *pc_layer_content_article(const pc_layer_content *c);   /* "an" for Adjustment, else "a" */

/* A preserved PSD additional-layer-info block. */
typedef struct pc_doc_psd_block {
    uint8_t key[4];
    pc_blob *data;
} pc_doc_psd_block;

/* Adding a field here (or to the structs it holds)? The .pcraft manifest must save it:
 * src/format/convert_save.c pins these structs' sizes and fails to compile until it does. */
struct pc_layer {
    pc_layer_id id;
    pc_str name;
    bool visible;
    pc_locks locks;
    pc_blend_mode blend;
    float opacity;      /* content and effects */
    float fill_opacity; /* content only */
    bool clipped;       /* clipping mask onto the layer below */
    bool has_mask;
    pc_layer_mask mask;
    bool has_vector_mask;
    pc_vector_mask vector_mask;
    pc_effects effects;
    pc_label_color label;
    pc_layer_content content;
    pc_doc_psd_block *psd_blocks;
    size_t npsd_blocks;
    bool has_psd_id; /* PSD lyid, cleared by duplicate */
    uint32_t psd_id;
    bool has_fill_cache;
    pc_fill_cache fill_cache;
    bool has_link_group;
    uint64_t link_group;
    uint32_t excluded_channels; /* bit i set = colour channel i left out of blending */
    pc_blend_if blend_if;
    pc_advanced_blending advanced;
    bool has_video;
    pc_video_data video;
};

/* A visible Normal layer at 100 % with a fresh id, effects enabled, Photoshop's advanced
 * blending defaults; takes over *content (moved on success). */
pc_status pc_layer_init(pc_layer *out, const char *name, pc_layer_content *content, pc_error *err);
/* An empty raster layer. */
pc_status pc_layer_raster(pc_layer *out, const char *name, pc_pixel_format format, pc_error *err);
/* An expanded Pass Through group; takes over the children array (moved on success; free()able
 * memory, may be NULL when n is 0). */
pc_status pc_layer_group(pc_layer *out, const char *name, pc_layer *children, size_t n, pc_error *err);
void pc_layer_fini(pc_layer *l);
pc_status pc_layer_clone(const pc_layer *l, pc_layer *out, pc_error *err);
bool pc_layer_eq(const pc_layer *a, const pc_layer *b);
/* Deep copy with fresh ids throughout and psd_id cleared (Duplicate Layer). */
pc_status pc_layer_duplicate(const pc_layer *l, pc_layer *out, pc_error *err);
bool pc_layer_is_group(const pc_layer *l);
/* Children of a group (NULL and *n = 0 otherwise). */
const pc_layer *pc_layer_children(const pc_layer *l, size_t *n);
/* Pixels of raster content, or the cache of text, shape and smart-object layers; NULL else. */
const pc_surface *pc_layer_surface(const pc_layer *l);
/* Raster content only. */
pc_surface *pc_layer_surface_mut(pc_layer *l);
/* The Background's own properties: an unclipped pixel layer with transparency and position
 * locked. Only the bottom layer of a document with them is the Background (see
 * pc_document_background). */
bool pc_layer_has_background_traits(const pc_layer *l);

/* One guide's own colour, keyed by orientation and position (not index), so code that drops or
 * reorders guides never moves a colour onto another guide. */
typedef struct pc_guide_color {
    bool vertical;
    double position;
    uint8_t rgb[3]; /* sRGB, 0-255 */
} pc_guide_color;

/* Ruler guides in document pixels (f64: ruler units and PSD 1/32 px positions stay exact). */
typedef struct pc_guides {
    double *horizontal;
    size_t nhorizontal;
    double *vertical;
    size_t nvertical;
    /* Guides with their own colour (New Guide > Color); the rest use the preference colour. */
    pc_guide_color *colors;
    size_t ncolors;
} pc_guides;

void pc_guides_fini(pc_guides *g);
pc_status pc_guides_clone(const pc_guides *g, pc_guides *out, pc_error *err);
bool pc_guides_eq(const pc_guides *a, const pc_guides *b);
/* The guide's own colour into rgb (false when it has none). */
bool pc_guides_color(const pc_guides *g, bool vertical, double position, uint8_t rgb[3]);
/* Give the guide at position its own colour (rgb NULL returns it to the preference colour). */
pc_status pc_guides_set_color(pc_guides *g, bool vertical, double position, const uint8_t *rgb,
                              pc_error *err);
/* Drop colours whose guide no longer exists. */
void pc_guides_prune_colors(pc_guides *g);

typedef enum pc_color_indicates { PC_INDICATES_MASKED_AREAS, PC_INDICATES_SELECTED_AREAS } pc_color_indicates;

/* An extra (alpha or spot) channel; also the temporary Quick Mask channel. */
typedef struct pc_alpha_channel {
    pc_str name;
    pc_surface surface;
    bool has_spot;       /* spot colour channel: ink colour and solidity */
    pc_color spot_color;
    float spot_solidity;
    pc_color color; /* overlay colour */
    float opacity;
    pc_color_indicates indicates;
} pc_alpha_channel;

#define PC_ALPHA_CHANNEL_DEFAULT_COLOR ((pc_color){PC_COLOR_MODE_RGB, {1.0f, 0.0f, 0.0f, 0.0f}, 1.0f})

/* Red overlay at 50 % over masked areas; takes over *surface (moved on success). */
pc_status pc_alpha_channel_init(pc_alpha_channel *out, const char *name, pc_surface *surface, pc_error *err);
void pc_alpha_channel_fini(pc_alpha_channel *c);
pc_status pc_alpha_channel_clone(const pc_alpha_channel *c, pc_alpha_channel *out, pc_error *err);
bool pc_alpha_channel_eq(const pc_alpha_channel *a, const pc_alpha_channel *b);
float pc_alpha_channel_overlay_coverage(const pc_alpha_channel *c, float v);

typedef struct pc_doc_psd_resource {
    uint16_t id;
    pc_str name;
    pc_blob *data;
} pc_doc_psd_resource;

typedef struct pc_doc_psd_global_block {
    uint8_t signature[4];
    uint8_t key[4];
    pc_blob *data;
} pc_doc_psd_global_block;

/* A free-form image text entry (PNG tEXt/zTXt/iTXt, TIFF ASCII tags). */
typedef struct pc_doc_text_entry {
    pc_str key;
    pc_str value;
} pc_doc_text_entry;

typedef struct pc_metadata {
    bool has_xmp;
    pc_str xmp;
    pc_blob *exif; /* NULL = None */
    pc_doc_text_entry *text; /* in file order, duplicate keys allowed */
    size_t ntext;
    pc_doc_psd_resource *psd_resources;
    size_t npsd_resources;
    pc_doc_psd_global_block *psd_global_blocks;
    size_t npsd_global_blocks;
} pc_metadata;

void pc_metadata_fini(pc_metadata *m);
pc_status pc_metadata_clone(const pc_metadata *m, pc_metadata *out, pc_error *err);
bool pc_metadata_eq(const pc_metadata *a, const pc_metadata *b);

/* The encoder settings a document's file was opened or saved with (what File > Save and the Save
 * As defaults reuse). Empty for a new document. Qualities are on the codecs' 1-100 scale. */
typedef struct pc_file_encoding {
    bool has_jpeg_quality; /* estimated from the quantisation tables on open */
    uint8_t jpeg_quality;
    bool has_webp_lossless; /* true lossless (VP8L), false lossy at webp_quality */
    bool webp_lossless;
    bool has_webp_quality;
    uint8_t webp_quality;
} pc_file_encoding;

bool pc_file_encoding_eq(const pc_file_encoding *a, const pc_file_encoding *b);
bool pc_file_encoding_is_empty(const pc_file_encoding *e);

/* Adding a field? See the note above struct pc_layer (src/format/convert_save.c). */
struct pc_document {
    pc_doc_id id;
    pc_str name;
    pc_size size;
    float resolution_dpi;
    pc_color_mode mode;
    pc_sample_type depth;
    pc_blob *icc_profile; /* NULL = None */
    pc_layer *layers;     /* bottom to top */
    size_t nlayers;
    pc_alpha_channel *channels;
    size_t nchannels;
    pc_guides guides;
    bool has_selection;
    pc_surface selection;
    pc_metadata metadata;
    pc_global_light global_light;
    pc_named_path *paths; /* top to bottom */
    size_t npaths;
    bool has_work_path;
    pc_path work_path;
    bool has_clipping_path;
    pc_clipping_path clipping_path;
    bool has_quick_mask;
    pc_alpha_channel quick_mask;
    /* Quick Mask was entered with no selection: an untouched white mask leaves no selection. */
    bool quick_mask_unselected;
    pc_pattern *patterns;
    size_t npatterns;
    bool has_color_table;
    pc_color_table color_table;
    bool has_duotone;
    pc_duotone duotone;
    pc_layer_comp *layer_comps;
    size_t nlayer_comps;
    bool has_last_applied_comp;
    uint32_t last_applied_comp;
    bool has_last_document_state;
    pc_layer_comp last_document_state;
    pc_measurement measurement;
    pc_note *notes;
    size_t nnotes;
    pc_text_styles text_styles;
    pc_slices slices;
    pc_variables variables;
    bool has_timeline;
    pc_timeline timeline;
    pc_file_encoding file_encoding; /* how the lossy file was encoded, reused by File > Save */
};

/* An empty document at 72 dpi with a fresh id. */
pc_status pc_document_init(pc_document *out, const char *name, pc_size size, pc_color_mode mode,
                           pc_sample_type depth, pc_error *err);
/* A document with a locked (transparency, position) "Background" layer filled with fill. */
pc_status pc_document_with_background(pc_document *out, const char *name, pc_size size, pc_color_mode mode,
                                      pc_sample_type depth, pc_color fill, pc_error *err);
void pc_document_fini(pc_document *d);
/* Deep copy (tiles and blobs shared, copy-on-write). */
pc_status pc_document_clone(const pc_document *d, pc_document *out, pc_error *err);
bool pc_document_eq(const pc_document *a, const pc_document *b);

/* Pixel format for new raster layers (Indexed/Multichannel -> RGB, Bitmap/Duotone -> Gray). */
pc_pixel_format pc_document_pixel_format(const pc_document *d);
pc_rect pc_document_bounds(const pc_document *d);
/* Nesting depth of the deepest layer (0 = root level; empty groups add nothing). */
pc_status pc_document_max_group_depth(const pc_document *d, size_t *out, pc_error *err);

/* Where a layer lives: indices from the root down. Owned (free with pc_layer_path_fini). */
typedef struct pc_layer_path {
    size_t *idx;
    size_t len;
} pc_layer_path;

void pc_layer_path_fini(pc_layer_path *p);

/* Depth-first walk bottom-to-top: item i is layer items[i].layer at depth items[i].depth,
 * whose path is paths[items[i].path_start .. items[i].path_start + depth + 1). Borrowed layer
 * pointers stay valid until the document is mutated. */
typedef struct pc_walk_item {
    const pc_layer *layer;
    size_t depth;
    size_t path_start;
} pc_walk_item;

typedef struct pc_walk {
    pc_walk_item *items;
    size_t len;
    size_t *paths;
} pc_walk;

pc_status pc_document_walk(const pc_document *d, pc_walk *out, pc_error *err);
void pc_walk_fini(pc_walk *w);
pc_status pc_document_layer_count(const pc_document *d, size_t *out, pc_error *err);

/* PC_ERR_NOT_FOUND ("no layer with id N") when id isn't in the document. */
pc_status pc_document_path_of(const pc_document *d, pc_layer_id id, pc_layer_path *out, pc_error *err);
/* The locks on the layer at path: its own and those of every enclosing group. */
pc_locks pc_document_locks_at(const pc_document *d, const size_t *path, size_t n);
pc_locks pc_document_effective_locks(const pc_document *d, pc_layer_id id);
const pc_layer *pc_document_layer(const pc_document *d, pc_layer_id id);
/* The Background layer, identified structurally (never by name): the bottom root layer when it
 * has pc_layer_has_background_traits; NULL otherwise. */
const pc_layer *pc_document_background(const pc_document *d);
pc_layer *pc_document_background_mut(pc_document *d);
bool pc_document_is_background(const pc_document *d, pc_layer_id id);
pc_layer *pc_document_layer_mut(pc_document *d, pc_layer_id id);
const pc_layer *pc_document_layer_at(const pc_document *d, const size_t *path, size_t n);
pc_layer *pc_document_layer_at_mut(pc_document *d, const size_t *path, size_t n);
/* A link group id no layer uses (max + 1, else the smallest free id from 1; never wraps).
 * *found is false only when every id is taken. */
pc_status pc_document_unused_link_group(const pc_document *d, bool *found, uint64_t *out, pc_error *err);
/* Insert directly above the layer *above (inside its group), or at the top of the root when
 * above is NULL or not found. Takes over *layer (moved on success). */
pc_status pc_document_insert_above(pc_document *d, const pc_layer_id *above, pc_layer *layer, pc_layer_id *out_id,
                                   pc_error *err);
/* Removes the layer and hands it to *out (PC_ERR_NOT_FOUND when absent). */
pc_status pc_document_remove(pc_document *d, pc_layer_id id, pc_layer *out, pc_error *err);
/* Move among siblings by delta; false at the ends or when id is absent. */
bool pc_document_shift(pc_document *d, pc_layer_id id, int32_t delta);
/* Next unused "<base> N". */
pc_status pc_document_next_layer_name(const pc_document *d, const char *base, pc_str *out, pc_error *err);
/* "<root> copy", then "<root> copy 2", ... (root strips an existing " copy" / " copy N"). */
pc_status pc_document_copy_name(const pc_document *d, const char *name, size_t len, pc_str *out, pc_error *err);
bool pc_document_top_layer(const pc_document *d, pc_layer_id *out);

#endif
