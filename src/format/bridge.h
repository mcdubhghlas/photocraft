/* Internal interface of the format module: the object store's building blocks, and the seam
 * between the store (chunk C4) and the manifest/document conversion (chunk C5). See format.h's
 * header comment for the division of work. Not for use outside src/format and its tests.
 *
 * Ownership follows REWRITE_PLAN.md §4: out-params are written only on success; objects created
 * with _new are released with _free; borrowed pointers are marked. Not synchronised. */
#ifndef PC_FORMAT_BRIDGE_H
#define PC_FORMAT_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "base/blob.h"
#include "base/buf.h"
#include "base/status.h"
#include "color/color.h"
#include "doc/doc.h"
#include "format/format.h"
#include "raster/surface.h"

/* Bundle paths. */
#define PC_FORMAT_MANIFEST "manifest.json"
#define PC_FORMAT_THUMB "thumb.png"
#define PC_FORMAT_COMPOSITE "composite/preview.png"

/* Deepest JSON nesting a manifest may have: each group level adds three (the layer object, its
 * content object and the `children` array), with headroom for the document and leaf layer
 * fields. The saver checks the same bound, so every bundle it writes loads again. */
#define PC_FORMAT_MAX_MANIFEST_DEPTH (3 * (PC_MAX_GROUP_DEPTH + 1) + 64)

/* "tiles/" or "blobs/" + 64 hex digits + ".zst" + NUL fits. */
enum { PC_FORMAT_OBJECT_PATH_MAX = 80, PC_FORMAT_HASH_HEX = 64 };

/* 64 lower-case hex digits (convert::is_valid_hash). */
bool pc_format_is_valid_hash(const char *h, size_t len);
/* Native-endian samples to little-endian in place (and back: the swap is an involution). A
 * no-op on little-endian hosts and for 8-bit samples. */
void pc_format_swap_to_le(uint8_t *bytes, size_t len, pc_sample_type s);
/* Rejects JSON nested deeper than PC_FORMAT_MAX_MANIFEST_DEPTH with a linear scan (no
 * recursion; brackets inside strings don't count), so a manifest can be parsed without a
 * recursion limit. PC_ERR_LIMIT "limit exceeded: manifest JSON nested deeper than 367 levels". */
pc_status pc_format_check_manifest_depth(const uint8_t *json, size_t len, pc_error *err);
/* pc_atomic_write with failures reported as FormatError::Io ("I/O: ..."). */
pc_status pc_format_write_atomic(const char *path, const void *data, size_t len, pc_error *err);
/* PNG bytes of a preview image (*out replaced); a codec failure is
 * "unsupported .pcraft feature: preview: ...". */
pc_status pc_format_png(const pc_rgba8_image *img, pc_buf *out, pc_error *err);

/* ---- sources: where a bundle's files come from ---- */

typedef struct pc_format_source pc_format_source;

/* A ZIP bundle in memory (bytes borrowed; they must outlive the source). */
pc_status pc_format_source_zip(const uint8_t *bytes, size_t len, pc_format_source **out, pc_error *err);
/* A directory bundle (the path is copied). */
pc_status pc_format_source_dir(const char *root, pc_format_source **out, pc_error *err);
void pc_format_source_free(pc_format_source *src); /* NULL-safe */
/* The file at bundle path `path`, at most `max` bytes (*out replaced). Missing:
 * "corrupt .pcraft bundle: missing `path`"; too big: "limit exceeded: ...". */
pc_status pc_format_source_get(const pc_format_source *src, const char *path, size_t max, pc_buf *out,
                               pc_error *err);

/* manifest.json (at most max bytes) after the depth check, unparsed. */
pc_status pc_format_read_manifest_raw(const pc_format_source *src, size_t max, pc_buf *out, pc_error *err);

/* ---- loading objects (convert::Fetch) ---- */

typedef struct pc_format_fetch pc_format_fetch;

/* Fetches objects from src (borrowed) under opts' limits, caching blobs by hash. */
pc_status pc_format_fetch_new(const pc_format_source *src, const pc_format_load_opts *opts,
                              pc_format_fetch **out, pc_error *err);
void pc_format_fetch_free(pc_format_fetch *f); /* NULL-safe */
/* The canonical (little-endian) bytes of a tile of exactly `len` bytes, decompressed and checked
 * against its hash (*out replaced). Counts toward max_total_bytes before reading. */
pc_status pc_format_fetch_tile(pc_format_fetch *f, const char *hash, size_t len, pc_buf *out, pc_error *err);
/* A blob (at most max_blob_bytes), checked against its hash. *out receives a reference the
 * caller releases; the same blob is shared on later fetches of the same hash. */
pc_status pc_format_fetch_blob(pc_format_fetch *f, const char *hash, pc_blob **out, pc_error *err);

/* ---- saving: collecting objects (convert::Sink) ---- */

/* The writer's tile-hash cache (by tile identity). */
typedef struct pc_format_hash_cache pc_format_hash_cache;

pc_status pc_format_hash_cache_new(pc_format_hash_cache **out, pc_error *err);
void pc_format_hash_cache_free(pc_format_hash_cache *c); /* NULL-safe */
/* Bracket one save's hashing: entries not used between begin and end are dropped at end. */
void pc_format_hash_cache_begin(pc_format_hash_cache *c);
void pc_format_hash_cache_end(pc_format_hash_cache *c);

typedef struct pc_format_sink pc_format_sink;

/* cache (borrowed, may be NULL) remembers tile hashes across saves. */
pc_status pc_format_sink_new(pc_format_hash_cache *cache, pc_format_sink **out, pc_error *err);
void pc_format_sink_free(pc_format_sink *s); /* NULL-safe */
/* Records a tile of format f (retained while the sink or a prepared bundle holds it) and writes
 * its hash (64 hex digits + NUL) into hash. Identical content is stored once. */
pc_status pc_format_sink_tile(pc_format_sink *s, pc_pixel_format f, const pc_raster_tile *tile,
                              char hash[PC_FORMAT_HASH_HEX + 1], pc_error *err);
/* Records a blob (retained). */
pc_status pc_format_sink_blob(pc_format_sink *s, pc_blob *data, char hash[PC_FORMAT_HASH_HEX + 1],
                              pc_error *err);
/* Records an object given as its canonical bytes (copied): a tile (is_tile) or a blob. For
 * re-saving objects read from another bundle. */
pc_status pc_format_sink_bytes(pc_format_sink *s, bool is_tile, const uint8_t *bytes, size_t len,
                               char hash[PC_FORMAT_HASH_HEX + 1], pc_error *err);

/* ---- a save, ready to write ---- */

typedef struct pc_format_object {
    char path[PC_FORMAT_OBJECT_PATH_MAX]; /* "tiles/<hash>.zst" or "blobs/<hash>.zst" */
    bool is_tile;
    pc_raster_tile *tile;  /* a raster tile (one reference), or NULL */
    pc_sample_type sample; /* the tile's sample type (for the little-endian swap) */
    pc_blob *bytes;        /* blob objects, and tiles given as canonical bytes (one reference) */
} pc_format_object;

typedef struct pc_format_preview {
    const char *name; /* PC_FORMAT_THUMB or PC_FORMAT_COMPOSITE (static) */
    pc_buf png;
} pc_format_preview;

typedef struct pc_format_prepared {
    pc_buf manifest;           /* manifest.json bytes */
    pc_format_object *objects; /* sorted by path */
    size_t nobjects;
    pc_format_preview previews[2];
    size_t npreviews;
    pc_format_save_stats stats; /* totals and manifest size */
} pc_format_prepared;

/* Takes the sink's objects (the sink is left empty) and a copy of the manifest bytes. */
pc_status pc_format_prepared_from_sink(pc_format_sink *sink, const uint8_t *manifest, size_t len,
                                       pc_format_prepared *out, pc_error *err);
/* Appends a preview (copied) in the order given (thumbnail first, as the reference). */
pc_status pc_format_prepared_add_preview(pc_format_prepared *p, const char *name, const uint8_t *png,
                                         size_t len, pc_error *err);
void pc_format_prepared_fini(pc_format_prepared *p); /* NULL-safe */
/* The object's canonical bytes: borrowed from the tile/blob, or put in *tmp (big-endian hosts). */
pc_status pc_format_object_content(const pc_format_object *o, pc_buf *tmp, const uint8_t **data, size_t *len,
                                   pc_error *err);

/* The writer's prepare step (convert + finite check + pretty JSON + depth check). */
pc_status pc_pcraft_writer_prepare(pc_pcraft_writer *w, const pc_document *doc,
                                   const pc_format_save_opts *opts, pc_format_prepared *out, pc_error *err);
/* The writer's save steps for a prepared bundle (what save_zip / save_dir do after prepare). */
pc_status pc_pcraft_writer_save_zip_prepared(pc_pcraft_writer *w, const pc_format_prepared *p, pc_buf *out,
                                             pc_format_save_stats *stats, pc_error *err);
pc_status pc_pcraft_writer_save_dir_prepared(pc_pcraft_writer *w, const pc_format_prepared *p,
                                             const char *dir, pc_format_save_stats *stats, pc_error *err);

/* Loading through a source (store::load): manifest, then the document; retries once with fresh
 * ids when the preserved ones are implausibly far ahead. */
pc_status pc_format_load_source(const pc_format_source *src, const pc_format_load_opts *opts,
                                pc_document *out, pc_error *err);
/* store::read_manifest. */
pc_status pc_format_read_manifest_source(const pc_format_source *src, const pc_format_load_opts *opts,
                                         pc_format_manifest **out, pc_error *err);

/* ======== the manifest side (chunk C5: manifest.c, migrate.c, convert_save.c, convert_load.c) ======== */

/* convert::check_nesting: groups nested deeper than PC_MAX_GROUP_DEPTH are refused. */
pc_status pc_format_check_nesting(const pc_document *doc, pc_error *err);

/* The whole Manifest as a yyjson tree owned by jdoc (*out = its root), fields in serde order,
 * NaN/inf kept as reals, every tile and blob passed through sink. thumbnail/composite are the
 * preview paths or NULL. */
pc_status pc_format_manifest_build(const pc_document *doc, const char *thumbnail, const char *composite,
                                   pc_format_sink *sink, yyjson_mut_doc *jdoc, yyjson_mut_val **out,
                                   pc_error *err);

/* Parse (bounded by pc_format_check_manifest_depth, as the store's read is), migrate and decode.
 * JSON errors as pc_format_json; migration and model errors as the reference's. */
pc_status pc_format_manifest_parse(const uint8_t *json, size_t len, pc_format_manifest **out, pc_error *err);

/* convert::Loader::document. *max_id receives the largest id seen (for the id reservation).
 * The manifest's decoded values are moved into the document: afterwards m may only be freed. */
pc_status pc_format_manifest_to_document(pc_format_manifest *m, pc_format_fetch *fetch,
                                         bool preserve_ids, pc_document *out, uint64_t *max_id,
                                         pc_error *err);

/* convert::reserve_ids_through: advance the id counter past max; false when max is implausibly
 * far ahead (the caller then remaps ids). */
bool pc_format_reserve_ids_through(uint64_t max);

#endif
