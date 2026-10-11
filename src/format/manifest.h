/* The `manifest.json` model (port of crates/format/src/manifest.rs) and its migrations
 * (migrate.rs). Internal to src/format: the public view is format.h / store.h.
 *
 * The manifest is a serde model separate from the document model on purpose: the on-disk
 * schema is versioned and migrated independently of in-memory refactors. Binary payloads never
 * live in JSON: surfaces name tiles (`tiles/<blake3>.zst`) and other binary data names blobs
 * (`blobs/<blake3>.zst`) by the BLAKE3 hash of their uncompressed (little-endian) bytes.
 *
 * Reading (manifest.c) decodes these structs with doc's descriptor-driven serde
 * (doc/json_util.h), so field order, defaults and error texts are serde_json::from_value's.
 * Doc types are embedded as they are (pc_adjustment, pc_effect, ...); conversion (convert.c)
 * moves them into the pc_document. Writing (convert.c) builds the JSON tree straight from the
 * document in the same field order, so the model structs are never built on save.
 *
 * Each pc_mf_* struct mirrors one Rust struct field for field; `has_x` flags are Option fields.
 * Strings are pc_str, vectors are pointer + count. Not synchronised. */
#ifndef PC_FORMAT_MANIFEST_H
#define PC_FORMAT_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "base/status.h"
#include "base/str.h"
#include "doc/doc.h"
#include "format/store.h"

/* Writer identification: the reference writes `photocraft-format {CARGO_PKG_VERSION}`; the C
 * writer keeps the reference's version so both writers produce identical manifests. */
#define PC_FORMAT_GENERATOR "photocraft-format 0.5.0"

typedef struct pc_mf_tile {
    int32_t tx;
    int32_t ty;
    pc_str hash;
} pc_mf_tile;

typedef struct pc_mf_surface {
    pc_pixel_format format;
    pc_str def; /* default pixel: hex of its little-endian encoded bytes */
    pc_mf_tile *tiles;
    size_t ntiles;
} pc_mf_surface;

typedef struct pc_mf_mask {
    pc_mf_surface surface;
    bool enabled;
    bool linked;
    float density;
    float feather;
} pc_mf_mask;

typedef struct pc_mf_effects {
    bool enabled;
    pc_effect *items;
    size_t nitems;
    bool has_psd_raw;
    pc_str psd_raw;
    bool has_reference;
    double reference[2];
} pc_mf_effects;

typedef struct pc_mf_fill_cache {
    pc_fill fill;
    pc_mf_surface surface;
} pc_mf_fill_cache;

typedef struct pc_mf_video {
    pc_mf_surface *frames;
    size_t nframes;
    pc_video_source source;
    float fps;
    bool show_altered;
} pc_mf_video;

/* (4-char key as hex, blob) */
typedef struct pc_mf_psd_block {
    pc_str key;
    pc_str hash;
} pc_mf_psd_block;

typedef enum pc_mf_source_kind { PC_MF_EMBEDDED, PC_MF_LINKED } pc_mf_source_kind;

typedef struct pc_mf_smart_source {
    pc_mf_source_kind kind;
    pc_str file_name; /* embedded */
    pc_str blob;      /* embedded */
    pc_str path;      /* linked */
} pc_mf_smart_source;

typedef struct pc_mf_layer pc_mf_layer;

/* Vec<LayerM>. */
typedef struct pc_mf_layer_list {
    pc_mf_layer *v;
    size_t n;
} pc_mf_layer_list;

/* ContentM: one flat struct for all variants (only the fields of `kind` are meaningful; fields
 * two variants share, like `transform` or `cache`, are stored once). kind uses pc_layer_kind's
 * values, which follow the reference's variant order. */
typedef struct pc_mf_content {
    pc_layer_kind kind;
    /* raster */
    pc_mf_surface surface;
    /* group */
    pc_mf_layer_list children;
    bool expanded;
    bool has_artboard;
    pc_artboard artboard;
    /* adjustment */
    pc_adjustment adjustment;
    /* fill (required) and shape (has_fill: Option) */
    bool has_fill;
    pc_fill fill;
    /* text */
    pc_str text;
    pc_str font_family;
    float size_pt;
    pc_color color;
    pc_affine transform; /* text, smart */
    bool has_cache;      /* text, shape, smart */
    pc_mf_surface cache;
    bool has_psd_raw; /* text, shape, smart */
    pc_str psd_raw;
    pc_text_run *runs;
    size_t nruns;
    pc_paragraph_run *paragraphs;
    size_t nparagraphs;
    pc_text_shape shape;
    pc_orientation orientation;
    pc_antialias antialias;
    bool has_text_warp;
    pc_text_warp text_warp;
    /* shape */
    pc_path path;
    bool has_stroke;
    pc_shape_stroke stroke;
    bool has_live;
    pc_live_shape live;
    /* smart */
    pc_mf_smart_source source;
    pc_smart_filter *smart_filters;
    size_t nsmart_filters;
    bool filters_enabled;
    bool has_filter_mask;
    pc_mf_mask filter_mask;
    bool has_warp;
    pc_warp warp;
    bool has_stack_mode;
    pc_stack_mode stack_mode;
    bool has_perspective;
    double perspective[9];
} pc_mf_content;

struct pc_mf_layer {
    uint64_t id;
    pc_str name;
    bool visible;
    pc_locks locks;
    pc_blend_mode blend;
    float opacity;
    float fill_opacity;
    bool clipped;
    bool has_mask;
    pc_mf_mask mask;
    bool has_vector_mask;
    pc_vector_mask vector_mask;
    pc_mf_effects effects;
    pc_label_color label;
    pc_mf_content content;
    pc_mf_psd_block *psd_blocks;
    size_t npsd_blocks;
    bool has_psd_id;
    uint32_t psd_id;
    bool has_fill_cache;
    pc_mf_fill_cache fill_cache;
    bool has_link_group;
    uint64_t link_group;
    uint32_t excluded_channels;
    pc_blend_if blend_if;
    pc_advanced_blending advanced;
    bool has_video;
    pc_mf_video video;
};

typedef struct pc_mf_spot {
    pc_color color;
    float solidity;
} pc_mf_spot;

typedef struct pc_mf_channel {
    pc_str name;
    pc_mf_surface surface;
    bool has_spot;
    pc_mf_spot spot;
    pc_color color;
    float opacity;
    pc_color_indicates indicates;
} pc_mf_channel;

typedef struct pc_mf_pattern {
    pc_str id;
    pc_str name;
    uint32_t width;
    uint32_t height;
    pc_mf_surface surface;
} pc_mf_pattern;

typedef struct pc_mf_named_path {
    pc_str name;
    pc_path path;
    bool has_psd_raw;
    pc_str psd_raw;
} pc_mf_named_path;

typedef struct pc_mf_appearance {
    pc_blend_mode blend;
    float opacity;
    float fill_opacity;
    pc_mf_effects effects;
} pc_mf_appearance;

typedef struct pc_mf_comp_state {
    uint64_t layer;
    bool has_visible;
    bool visible;
    bool has_position;
    int32_t position[2];
    bool has_appearance;
    pc_mf_appearance appearance;
} pc_mf_comp_state;

typedef struct pc_mf_comp {
    uint32_t id;
    pc_str name;
    pc_str comment;
    bool apply_visibility;
    bool apply_position;
    bool apply_appearance;
    pc_mf_comp_state *states;
    size_t nstates;
} pc_mf_comp;

/* (id, name, blob) */
typedef struct pc_mf_resource {
    uint16_t id;
    pc_str name;
    pc_str hash;
} pc_mf_resource;

/* (signature hex, key hex, blob) */
typedef struct pc_mf_global_block {
    pc_str signature;
    pc_str key;
    pc_str hash;
} pc_mf_global_block;

typedef struct pc_mf_metadata {
    bool has_xmp;
    pc_str xmp;
    bool has_exif;
    pc_str exif;
    pc_mf_resource *psd_resources;
    size_t npsd_resources;
    pc_mf_global_block *psd_global_blocks;
    size_t npsd_global_blocks;
} pc_mf_metadata;

typedef struct pc_mf_doc {
    uint64_t id;
    pc_str name;
    pc_size size;
    float resolution_dpi;
    pc_color_mode mode;
    pc_sample_type depth;
    bool has_icc_profile;
    pc_str icc_profile;
    pc_mf_layer_list layers; /* bottom to top */
    pc_mf_channel *channels;
    size_t nchannels;
    pc_guides guides;
    bool has_selection;
    pc_mf_surface selection;
    pc_mf_metadata metadata;
    pc_global_light global_light;
    pc_mf_named_path *paths;
    size_t npaths;
    bool has_work_path;
    pc_path work_path;
    bool has_clipping_path;
    pc_clipping_path clipping_path;
    bool has_quick_mask;
    pc_mf_channel quick_mask;
    pc_mf_pattern *patterns;
    size_t npatterns;
    bool has_color_table;
    pc_color_table color_table;
    bool has_duotone;
    pc_duotone duotone;
    pc_mf_comp *layer_comps;
    size_t nlayer_comps;
    bool has_last_applied_comp;
    uint32_t last_applied_comp;
    bool has_last_document_state;
    pc_mf_comp last_document_state;
    pc_variables variables;
    bool has_timeline;
    pc_timeline timeline;
    pc_measurement measurement;
    pc_note *notes;
    size_t nnotes;
    pc_text_styles text_styles;
    pc_slices slices;
} pc_mf_doc;

struct pc_format_manifest {
    uint32_t format_version;
    pc_str generator;
    pc_mf_doc document;
    bool has_thumbnail;
    pc_str thumbnail;
    bool has_composite;
    pc_str composite;
};

/* ---- migrations (migrate.rs) ----
 *
 * A step upgrades the raw JSON (a mutable yyjson tree) from version n to n + 1. steps[i]
 * upgrades version i + 1 to i + 2; the build's list is empty while version 1 is the only one. */
typedef pc_status (*pc_format_migrate_step)(yyjson_mut_doc *doc, yyjson_mut_val *root, pc_error *err);

/* The `format_version` of root as migrate_with reads it: "manifest has no format_version",
 * "format_version out of range" (above u32), "format_version 0 is invalid" (all Corrupt), and
 * TooNew when it is above target. */
pc_status pc_format_manifest_version(yyjson_val *root, uint32_t target, uint32_t *found, pc_error *err);
pc_status pc_format_manifest_version_mut(yyjson_mut_val *root, uint32_t target, uint32_t *found,
                                         pc_error *err);
/* migrate_with: upgrades root in place to target with steps, setting format_version after each
 * step; *found receives the version it started from. A missing step is Unsupported
 * "no migration from format N". */
pc_status pc_format_migrate_with(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                 const pc_format_migrate_step *steps, size_t nsteps, uint32_t target,
                                 uint32_t *found, pc_error *err);

/* ---- decoding ---- */

/* Decodes a migrated manifest (store::read_manifest after the parse): the document without
 * its layers first, then the layers one at a time, group children before their group. Errors
 * are serde's messages (PC_ERR_INVALID; the caller adds "manifest JSON: "). *out is a fresh
 * allocation the caller frees with pc_format_manifest_free. */
pc_status pc_format_manifest_decode(yyjson_val *root, pc_format_manifest **out, pc_error *err);

/* Clears a pc_mf_layer (recursively, children included) and zeroes it. */
void pc_mf_layer_fini(pc_mf_layer *l);

#endif
