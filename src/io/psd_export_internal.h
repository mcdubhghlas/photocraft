/* The PSD exporter's state, shared by psd_export.c (records, blocks, resources, the file) and
 * psd_export_smart.c (smart objects: placed-layer blocks, embedded files, filter caches). Port of
 * the Ex / SmartOut structs of crates/io/src/psd_export.rs. Not part of the io API. */
#ifndef PC_IO_PSD_EXPORT_INTERNAL_H
#define PC_IO_PSD_EXPORT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "doc/doc.h"
#include "io/core.h"
#include "io/linked.h"
#include "io/psd_export.h"
#include "io/smart_map.h"
#include "psd/file.h"
#include "raster/surface.h"

/* How deep embedded documents are exported inside each other before a smart object is written
 * as pixels instead. */
#define PC_IO_MAX_NESTING 8u

/* An embedded smart-object source as written to lnk2. */
typedef struct pc_io_ex_source {
    pc_str uuid;
    double size[2]; /* pixel size of the source document */
    double dpi;
} pc_io_ex_source;

/* One embed() result, by contents identity and content hash (ok false = the reason it can't be
 * embedded). Equal bytes in independent objects stay independent. */
typedef struct pc_io_ex_converted {
    pc_smart_contents_id contents;
    uint8_t key[32];
    bool ok;
    pc_io_ex_source src;
    char *reason;
} pc_io_ex_converted;

/* A nested document decoded while converting a .pcraft source, by uuid. */
typedef struct pc_io_ex_doc {
    char *uuid;
    pc_document doc;
} pc_io_ex_doc;

/* A source composite (document pixel format, source space) by uuid; has false = unreadable. */
typedef struct pc_io_ex_composite {
    char *uuid;
    bool has;
    pc_surface surface;
    pc_rect bounds;
} pc_io_ex_composite;

/* Smart-object state collected while emitting layers, written into the global blocks at the end
 * (SmartOut). */
typedef struct pc_io_ex_smart {
    uint32_t depth;                         /* nesting depth of this export */
    const pc_doc_psd_global_block *globals; /* the document's preserved global blocks (borrowed) */
    size_t nglobals;
    pc_io_strs known;       /* uuids of the files in the preserved linked-layer blocks */
    pc_io_strs used;        /* uuids the layers reference */
    bool prune_ok;          /* unreferenced preserved files may be dropped */
    pc_io_linked_files add; /* files to embed */
    pc_io_ex_converted *converted;
    size_t nconverted;
    size_t capconverted;
    pc_io_ex_doc *docs;
    size_t ndocs;
    size_t capdocs;
    pc_io_ex_composite *composites;
    size_t ncomposites;
    size_t capcomposites;
    pc_psd_fx_item *old_fx; /* preserved FEid items */
    size_t nold_fx;
    size_t capold_fx;
    pc_io_strs keep_fx;     /* placed ids whose preserved FEid item stays */
    pc_psd_fx_item *new_fx; /* regenerated FEid items */
    size_t nnew_fx;
    size_t capnew_fx;
    /* Each placed layer has its own id, even when it duplicates an imported layer. */
    pc_io_strs placed_used;
    /* Original instances reserve their imported placement regardless of layer order
     * (placed id -> layer id) and their contents (contents id -> layer id). */
    struct pc_io_placed_owner {
        char *placed;
        uint64_t layer;
    } *placed_owners;
    size_t nplaced_owners;
    size_t capplaced_owners;
    struct pc_io_unplaced_owner {
        pc_smart_contents_id contents;
        uint64_t layer;
    } *unplaced_owners;
    size_t nunplaced_owners;
    size_t capunplaced_owners;
    /* The imported identity that keeps each original linked UUID (path -> contents id). */
    struct pc_io_linked_contents {
        char *path;
        pc_smart_contents_id contents;
    } *linked_contents;
    size_t nlinked_contents;
    size_t caplinked_contents;
    /* Independent copies of preserved linked items, keyed by (path, contents id). */
    struct pc_io_linked_copy {
        char *path;
        pc_smart_contents_id contents;
        uint8_t signature[4];
        uint8_t key[4];
        pc_blob *data;
    } *linked_copies;
    size_t nlinked_copies;
    size_t caplinked_copies;
} pc_io_ex_smart;

/* u64 -> u32 open-addressing map (document layer id -> PSD layer id). */
typedef struct pc_io_ex_idmap {
    uint64_t *keys;
    uint32_t *vals;
    bool *used;
    size_t cap; /* power of two */
} pc_io_ex_idmap;

typedef struct pc_io_ex {
    const pc_document *doc;
    pc_pixel_format fmt;
    float dpi;
    pc_pixel_format mask_fmt;
    size_t cc;
    bool cmyk;
    pc_psd_version version;
    uint32_t next_id;
    pc_io_ex_idmap layer_ids;
    /* Generated Txt2 object number of each type layer (layer id -> index, stored as u32); a
     * preserved TySh keeps its own TextIndex, others get the next free number. */
    pc_io_ex_idmap text_index;
    pc_rect canvas;
    pc_psd_layer *records;
    size_t nrecords;
    size_t caprecords;
    /* link_group of each record in emit order (has false = none; dividers have none). */
    bool *link_has;
    uint64_t *link_group;
    size_t caplinks;
    pc_io_warnings warnings;
    bool comps; /* layer comps to regenerate into each layer's cmls */
    pc_io_ex_smart smart;
} pc_io_ex;

/* ---- psd_export.c ---- */

/* The PSD id of a layer, if assigned. */
bool pc_io_ex_idmap_get(const pc_io_ex_idmap *m, uint64_t key, uint32_t *out);
pc_status pc_io_ex_idmap_init(pc_io_ex_idmap *m, size_t n, pc_error *err);
void pc_io_ex_idmap_fini(pc_io_ex_idmap *m);
/* Inserts or overwrites (HashMap::insert); returns whether the key was new. */
bool pc_io_ex_idmap_put(pc_io_ex_idmap *m, uint64_t key, uint32_t val);
/* Emits the records of layers (bottom to top) and, recursively, of group children (depth
 * bounded: PC_ERR_LIMIT past 128 levels). */
pc_status pc_io_ex_emit(pc_io_ex *ex, const pc_layer *layers, size_t n, size_t depth, pc_error *err);
/* (clamp(v, 0, 1) * 255).round() as u8 */
uint8_t pc_io_q255(float v);
/* legacy_name: printable ASCII kept, every other char '?', at most 255 bytes. *out replaced. */
pc_status pc_io_legacy_name(const char *s, size_t len, pc_psd_bytes *out, pc_error *err);

/* document_to_psd_nested: the exporter at nesting depth `depth`. Warnings appended. */
pc_status pc_io_document_to_psd_nested(const pc_document *doc, const pc_io_psd_export_opts *opts,
                                       uint32_t depth, pc_psd_file *out, pc_io_warnings *warnings,
                                       pc_error *err);

/* ---- resource helpers shared with multichannel_map.c (psd_export_file.c) ---- */

/* A growable list of image resources. */
typedef struct pc_io_res_list {
    pc_psd_resource *v;
    size_t n;
    size_t cap;
} pc_io_res_list;

/* Appends a resource (id, data); pc_io_res_add_named also sets its legacy name. */
pc_status pc_io_res_add(pc_io_res_list *r, uint16_t id, const void *data, size_t len, pc_error *err);
pc_status pc_io_res_add_named(pc_io_res_list *r, uint16_t id, const pc_str *name, const void *data,
                              size_t len, pc_error *err);
/* The grid-and-guides (1032) resource bytes for doc (Photoshop writes it in every file). */
pc_status pc_io_guides_resource(const pc_document *doc, pc_buf *out, pc_error *err);
/* Saved paths (2000+), the work path (1025) and the clipping path name (2999), appended to res. */
pc_status pc_io_path_resources(pc_io_res_list *res, const pc_document *doc, pc_io_warnings *warnings,
                               pc_error *err);
/* Whether an imported (unmodelled) resource still describes the document and is written back. */
bool pc_io_keep_imported_resource(const pc_document *doc, uint16_t id, const uint8_t *data, size_t len);
/* A document-level (global) tagged block: key/data, Photoshop's 4-byte padding, signature kept. */
pc_status pc_io_global_block(const uint8_t signature[4], const uint8_t key[4], const uint8_t *data,
                             size_t len, pc_psd_block *out, pc_error *err);
/* The signature a kept tagged block is written with: 8B64 (PSB-only) becomes 8BIM in a PSD. */
void pc_io_block_signature(const uint8_t sig[4], pc_psd_version version, uint8_t out[4]);

/* ---- psd_export_smart.c ---- */

/* SmartOut::new. */
pc_status pc_io_ex_smart_init(pc_io_ex_smart *s, const pc_document *doc, uint32_t depth, pc_error *err);
void pc_io_ex_smart_fini(pc_io_ex_smart *s);
/* SmartOut::finish: the global blocks (taken over) with linked files and filter caches brought
 * up to date. *out malloc'd (release each data blob). */
pc_status pc_io_ex_smart_finish(pc_io_ex_smart *s, pc_doc_psd_global_block *blocks, size_t n,
                                pc_doc_psd_global_block **out, size_t *nout, pc_error *err);
/* The placed-layer blocks of a smart object (Ex::smart_blocks). tmpl (the parsed psd_raw) may be
 * NULL. */
pc_status pc_io_ex_smart_blocks(pc_io_ex *ex, const pc_layer *l, const pc_smart_object *sm,
                                const pc_io_placed *tmpl, pc_io_raw_blocks *raw, pc_error *err);
/* Replaces the data of key in raw, else inserts it before `before` (NULL = append); takes *data. */
pc_status pc_io_ex_upsert(pc_io_raw_blocks *raw, const char key[4], uint8_t **data, size_t len,
                          const char *before, pc_error *err);
/* Whether set s holds p. */
bool pc_io_ex_strs_has(const pc_io_strs *s, const char *p, size_t len);

#endif
