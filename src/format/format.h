/* photocraft format: the native, lossless `.pcraft` document bundle (port of crates/format).
 *
 *     manifest.json            versioned document tree (C5: manifest.c, convert.c, migrate.c)
 *     tiles/<blake3>.zst       content-addressed 256x256 tiles (zstd, little-endian samples)
 *     blobs/<blake3>.zst       content-addressed binary data (ICC, EXIF, PSD blocks, smart objects)
 *     thumb.png                optional thumbnail (caller-rendered)
 *     composite/preview.png    optional flattened preview (caller-rendered)
 *
 * A bundle is a ZIP archive (STORE entries) or a directory with the same layout. Saving is
 * incremental: a pc_pcraft_writer remembers what it already hashed, compressed and verified, so
 * only new tiles are encoded. Loading treats every byte as hostile (zip.h, store.h).
 *
 * Headers: zip.h (container), atomic.h (crash-safe replace, in base/file.h), read.h (whole-file
 * reads), finite.h (no NaN/inf in a manifest), store.h (bundle writer/loader), autosave.h
 * (background autosave and crash recovery). This header includes them all and adds the
 * one-call API below.
 *
 * ---- The C4/C5 seam (src/format/bridge.h) ----
 * Chunk C4 ported the container, the object store and this API; chunk C5 ports the manifest
 * model (manifest.rs), document conversion (convert.rs), migrations (migrate.rs) and autosave.
 * The store calls C5 only through the functions declared in bridge.h:
 *
 *   pc_format_check_nesting        convert::check_nesting (save: group depth <= PC_MAX_GROUP_DEPTH)
 *   pc_format_manifest_build       Manifest { format_version, generator, document: doc_m(doc, sink),
 *                                  thumbnail, composite } as a yyjson tree in serde field order,
 *                                  NaN/inf kept as reals (the store runs finite.h on it, writes it
 *                                  with pc_json_write(PC_JSON_PRETTY) and checks its depth)
 *   pc_format_manifest_parse       serde_json::from_slice (bounded by the linear depth check
 *                                  instead of serde's 128 levels) + migrate + Manifest decoding
 *   pc_format_manifest_free / pc_format_manifest_thumbnail
 *   pc_format_manifest_to_document convert::Loader::document (fetching tiles/blobs through the
 *                                  store's pc_format_fetch) and the largest id it saw
 *   pc_format_reserve_ids_through  convert::reserve_ids_through
 *
 * In the other direction C5 calls the store's pc_format_sink_tile/blob (hash + collect an object
 * while building the manifest) and pc_format_fetch_tile/blob (load one, verified) from bridge.h,
 * and may use pc_format_is_valid_hash, pc_format_swap_to_le and the error macros below.
 * C5's side lives in manifest.c (the model and its decoding), migrate.c (versions and the
 * parse), convert_save.c (document -> manifest JSON) and convert_load.c (manifest -> document).
 * JSON depth: base's pc_json_parse refuses more than 127 levels (serde's default), but a
 * manifest may nest up to PC_FORMAT_MAX_MANIFEST_DEPTH (367) levels, so the manifest is parsed
 * with pc_json_parse_depth(.., 0, ..) after pc_format_check_manifest_depth has bounded it.
 * Autosave and crash recovery (autosave.rs) are in autosave.h.
 *
 * Errors: the reference's FormatError variants are pc_status codes with the reference's message
 * text (pc_format_error_kind tells them apart). Thread-safe except where an object is shared. */
#ifndef PC_FORMAT_FORMAT_H
#define PC_FORMAT_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "format/atomic.h"
#include "format/autosave.h"
#include "format/finite.h"
#include "format/read.h"
#include "format/store.h"
#include "format/zip.h"

/* File extension of the native format. */
#define PC_FORMAT_EXTENSION "pcraft"

/* ---- errors (FormatError) ----
 *
 *   Corrupt(msg)        PC_ERR_FORMAT       "corrupt .pcraft bundle: msg"
 *   Unsupported(msg)    PC_ERR_UNSUPPORTED  "unsupported .pcraft feature: msg"
 *   TooNew{found, sup}  PC_ERR_UNSUPPORTED  "bundle written by a newer version (format F, this build
 *                                            reads up to S)"
 *   LimitExceeded(msg)  PC_ERR_LIMIT        "limit exceeded: msg"
 *   NonFinite{path}     PC_ERR_INVALID      "`path` is NaN or infinite, which a .pcraft manifest cannot
 *                                            hold; reset that value and save again"
 *   Io(e)               PC_ERR_IO (or NOT_FOUND/NOMEM/INVALID from the I/O layer)  "I/O: e"
 *   Json(e)             PC_ERR_FORMAT       "manifest JSON: e"
 * Out of memory is PC_ERR_NOMEM (the reference aborts). */
typedef enum pc_format_err_kind {
    PC_FORMAT_ERR_NONE,
    PC_FORMAT_ERR_CORRUPT,
    PC_FORMAT_ERR_UNSUPPORTED,
    PC_FORMAT_ERR_TOO_NEW,
    PC_FORMAT_ERR_LIMIT,
    PC_FORMAT_ERR_NON_FINITE,
    PC_FORMAT_ERR_IO,
    PC_FORMAT_ERR_JSON,
    PC_FORMAT_ERR_OTHER /* out of memory, invalid arguments */
} pc_format_err_kind;

/* Which FormatError a failure is (from its status and message prefix). */
pc_format_err_kind pc_format_error_kind(pc_status s, const pc_error *err);

/* Fill err with code and `prefix` + printf text. Use the macros. */
void pc_format_fail_set(pc_error *err, pc_status code, const char *prefix, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

#define pc_format_corrupt(err, ...)                                                                          \
    (pc_format_fail_set((err), PC_ERR_FORMAT, "corrupt .pcraft bundle: ", __VA_ARGS__), PC_ERR_FORMAT)
#define pc_format_unsupported(err, ...)                                                                      \
    (pc_format_fail_set((err), PC_ERR_UNSUPPORTED, "unsupported .pcraft feature: ", __VA_ARGS__),            \
     PC_ERR_UNSUPPORTED)
#define pc_format_limit(err, ...)                                                                            \
    (pc_format_fail_set((err), PC_ERR_LIMIT, "limit exceeded: ", __VA_ARGS__), PC_ERR_LIMIT)
#define pc_format_too_new(err, found, supported)                                                             \
    (pc_format_fail_set((err), PC_ERR_UNSUPPORTED, "",                                                       \
                        "bundle written by a newer version (format %lu, this build reads up to %lu)",        \
                        (unsigned long)(found), (unsigned long)(supported)),                                 \
     PC_ERR_UNSUPPORTED)

/* Prefix err's message with "I/O: " (an I/O failure surfacing as FormatError::Io) and return s.
 * Out of memory keeps its plain message. */
pc_status pc_format_io(pc_status s, pc_error *err);
/* Prefix err's message with "manifest JSON: " and return PC_ERR_FORMAT (out of memory stays
 * PC_ERR_NOMEM). For C5's JSON parse/decode errors. */
pc_status pc_format_json(pc_status s, pc_error *err);

/* ---- one-call API (lib.rs) ---- */

/* True when ZIP bytes contain a `manifest.json` entry (checks the first local header, then the
 * central directory). */
bool pc_format_is_pcraft(const uint8_t *bytes, size_t len);

/* Save to an in-memory ZIP bundle (one-shot; use pc_pcraft_writer for incremental saves).
 * *out is replaced on success. opts may be NULL. */
pc_status pc_format_save_to_bytes(const pc_document *doc, const pc_format_save_opts *opts, pc_buf *out,
                                  pc_error *err);

/* Load a ZIP bundle from memory; *out is initialised on success (pc_document_fini it). */
pc_status pc_format_load_from_bytes(const uint8_t *bytes, size_t len, pc_document *out, pc_error *err);
pc_status pc_format_load_from_bytes_with(const uint8_t *bytes, size_t len, const pc_format_load_opts *opts,
                                         pc_document *out, pc_error *err);

/* Load a directory bundle or a ZIP file. */
pc_status pc_format_load_path(const char *path, pc_document *out, pc_error *err);
pc_status pc_format_load_path_with(const char *path, const pc_format_load_opts *opts, pc_document *out,
                                   pc_error *err);

/* Load from any byte source with the same migration and limits as the path loads
 * (lib::load_bytes_source). */
pc_status pc_format_load_bytes_source(pc_format_byte_source_get get, void *ctx,
                                      const pc_format_load_opts *opts, pc_document *out, pc_error *err);
/* Read and migrate only the manifest (no tiles are decoded). Caller frees *out with
 * pc_format_manifest_free. */
pc_status pc_format_read_manifest(const uint8_t *bytes, size_t len, pc_format_manifest **out, pc_error *err);

/* The embedded thumbnail (PNG bytes), if the manifest names one: *present says whether it does;
 * *out is replaced only when it does. */
pc_status pc_format_read_thumbnail(const uint8_t *bytes, size_t len, bool *present, pc_buf *out,
                                   pc_error *err);

#endif
