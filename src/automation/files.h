/* File I/O shared by the MCP server and the CLI (port of files.rs): open/save any supported
 * format, with `.pcraft` (ZIP or directory bundle) handled natively. */
#ifndef PC_AUTOMATION_FILES_H
#define PC_AUTOMATION_FILES_H

#include "base/buf.h"
#include "base/status.h"
#include "doc/doc.h"
#include "format/store.h"
#include "io/core.h"

/* Result of opening a file (port of Opened). */
typedef struct pc_auto_opened {
    pc_document document;    /* owned; valid only on success */
    pc_io_warnings warnings; /* owned */
    bool source_read_only;
} pc_auto_opened;

void pc_auto_opened_fini(pc_auto_opened *o); /* NULL-safe */

/* Decode a document from in-memory bytes (named for its extension). */
pc_status pc_auto_open_bytes(const char *name, const uint8_t *bytes, size_t len, pc_auto_opened *out,
                             pc_error *err);
/* Open a document from disk: directory bundles and `.pcraft` ZIPs load natively; everything else
 * goes through photocraft-io (PSD, PNG, …). */
pc_status pc_auto_open_path(const char *path, pc_auto_opened *out, pc_error *err);

/* Save or export `doc` to `path`, choosing the format from the extension (or `format_override`, an
 * extension such as "png"). `writer` (may be NULL) makes repeated `.pcraft` saves incremental.
 * Appends warnings to *warnings (may be NULL). */
pc_status pc_auto_save(const pc_document *doc, const char *path, const char *format_override,
                       const pc_io_export_opts *opts, pc_pcraft_writer *writer, pc_io_warnings *warnings,
                       pc_error *err);
/* Encode for a capability-scoped write: never obtains ambient authority and emits `.pcraft` as a
 * ZIP. *bytes (owned) gets the file; *warnings (may be NULL) gets the notes. */
pc_status pc_auto_save_bytes(const pc_document *doc, const char *name, const char *format_override,
                             const pc_io_export_opts *opts, pc_buf *bytes, pc_io_warnings *warnings,
                             pc_error *err);
/* Flattened document as PNG, scaled to fit `max_side` (0 = full size). *png (owned) gets it. */
pc_status pc_auto_render_png(const pc_document *doc, uint32_t max_side, pc_buf *png, pc_error *err);

#endif /* PC_AUTOMATION_FILES_H */
