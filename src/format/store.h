/* .pcraft object storage: the incremental writer and the load options (port of
 * crates/format/src/store.rs; the loader's entry points are in format.h).
 *
 * Saving collects every tile and blob the document references by the BLAKE3 hash of its
 * uncompressed bytes (tiles hashed over little-endian samples), so identical content is stored
 * once. A ZIP save writes manifest.json, the previews, then the objects in path order. A directory
 * save reuses existing objects only after checking them, rewrites missing or damaged ones,
 * writes the manifest atomically last, then removes unreferenced objects.
 *
 * What the directory cache trusts: a directory save reuses an existing object file only after
 * decompressing it and checking its content. The writer then remembers the file's signature
 * (length, modification time, inode, device and change time) and skips the full check on later
 * saves while it is unchanged. A file damaged in place without changing its signature would be
 * trusted, so each save also re-verifies a rolling share of the cached objects, oldest-checked
 * first, up to the re-verify budget. Over enough saves every object is read again.
 *
 * A pc_pcraft_writer is not synchronised: one per open document, one save at a time. */
#ifndef PC_FORMAT_STORE_H
#define PC_FORMAT_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "doc/doc.h"
#include "raster/pixel.h"

/* Manifest version written by this build (manifest::FORMAT_VERSION). */
#define PC_FORMAT_VERSION 1u

/* The parsed, migrated manifest (manifest.rs; src/format/manifest.h has the model). */
typedef struct pc_format_manifest pc_format_manifest;
void pc_format_manifest_free(pc_format_manifest *m); /* NULL-safe */
/* Its format_version and preview paths (borrowed; NULL when absent). */
uint32_t pc_format_manifest_format_version(const pc_format_manifest *m);
const char *pc_format_manifest_thumbnail(const pc_format_manifest *m);
const char *pc_format_manifest_composite(const pc_format_manifest *m);

/* Previews to embed (the format cannot render; callers pass them). NULL = none. Borrowed. */
typedef struct pc_format_save_opts {
    const pc_rgba8_image *thumbnail;
    const pc_rgba8_image *composite;
} pc_format_save_opts;

/* Limits applied while loading untrusted bundles. */
typedef struct pc_format_load_opts {
    size_t max_manifest_bytes; /* size of manifest.json (256 MiB) */
    size_t max_blob_bytes;     /* one decompressed blob (1 GiB) */
    uint64_t max_total_bytes;  /* all decompressed tile + blob bytes (16 GiB) */
    bool preserve_ids;         /* keep document and layer ids (the id counter is advanced past them) */
    /* Longest canvas side the manifest may declare (PC_FORMAT_MAX_CANVAS_SIDE, the PSB limit). */
    uint32_t max_canvas_side;
    /* Most canvas pixels it may declare (default: the full 300000 x 300000 PSB canvas). Callers
     * handling untrusted files (previewers, batch jobs) may lower it: the compositor, thumbnails
     * and display work in proportion to the area. Loading itself allocates only stored tiles. */
    uint64_t max_canvas_pixels;
} pc_format_load_opts;

#define PC_FORMAT_MAX_CANVAS_SIDE 300000u
#define PC_FORMAT_MAX_CANVAS_PIXELS ((uint64_t)PC_FORMAT_MAX_CANVAS_SIDE * PC_FORMAT_MAX_CANVAS_SIDE)

pc_format_load_opts pc_format_load_opts_default(void);

/* A bundle's files from anywhere (a directory behind a capability handle, an archive, a test
 * fixture): fill *out (replacing it) with the file at bundle path `path`, at most max bytes, or
 * fail with the FormatError conventions (missing: corrupt "missing `path`"; too big: limit). */
typedef pc_status (*pc_format_byte_source_get)(void *ctx, const char *path, size_t max, pc_buf *out,
                                               pc_error *err);

/* Read one opened bundle entry from fd under its max budget (store::read_entry). len_hint (the
 * stat length) is a hint only: the bound is enforced on the read, so a stream that yields more
 * than max is "limit exceeded: `path` exceeds max bytes". *out replaced on success. */
pc_status pc_format_read_entry(int fd, uint64_t len_hint, size_t max, const char *path, pc_buf *out,
                               pc_error *err);

/* What a save did. */
typedef struct pc_format_save_stats {
    size_t tiles_total;   /* distinct tiles referenced by the document */
    size_t tiles_written; /* tiles compressed (ZIP) or written (directory) */
    size_t tiles_reused;  /* tiles reused from the previous save */
    size_t blobs_total;
    size_t blobs_written;
    size_t objects_removed; /* unreferenced objects removed (directory bundles) */
    size_t manifest_bytes;
} pc_format_save_stats;

/* Compressed bytes of already-verified objects re-read per directory save by default. */
#define PC_FORMAT_DEFAULT_REVERIFY_BUDGET ((uint64_t)16 * 1024 * 1024)

typedef struct pc_pcraft_writer pc_pcraft_writer;

pc_status pc_pcraft_writer_new(pc_pcraft_writer **out, pc_error *err);
void pc_pcraft_writer_free(pc_pcraft_writer *w); /* NULL-safe */

/* How many compressed bytes of already-verified objects each directory save re-reads (oldest-
 * checked first; at least one object when non-zero). 0 trusts unchanged signatures entirely;
 * UINT64_MAX re-verifies everything on every save. */
void pc_pcraft_writer_set_reverify_budget(pc_pcraft_writer *w, uint64_t bytes);

/* Save as a ZIP bundle in memory (*out replaced on success). opts and stats may be NULL. */
pc_status pc_pcraft_writer_save_zip(pc_pcraft_writer *w, const pc_document *doc,
                                    const pc_format_save_opts *opts, pc_buf *out, pc_format_save_stats *stats,
                                    pc_error *err);

/* Save into a directory bundle (created if missing). */
pc_status pc_pcraft_writer_save_dir(pc_pcraft_writer *w, const pc_document *doc, const char *dir,
                                    const pc_format_save_opts *opts, pc_format_save_stats *stats,
                                    pc_error *err);

/* Save to `path`: a directory bundle if it is an existing directory or ends with '/' (or '\'),
 * otherwise a ZIP file written atomically. */
pc_status pc_pcraft_writer_save_path(pc_pcraft_writer *w, const pc_document *doc, const char *path,
                                     const pc_format_save_opts *opts, pc_format_save_stats *stats,
                                     pc_error *err);

#endif
