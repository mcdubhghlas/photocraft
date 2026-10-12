/* Slices <-> PSD resource 1050 (chunk I4: crates/io/src/slices_map.rs). Failures are out of
 * memory only (an unreadable resource means no slices). Thread-safe. */
#ifndef PC_IO_SLICES_MAP_H
#define PC_IO_SLICES_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "doc/doc.h"
#include "io/core.h"

#define PC_IO_SLICES 1050

/* Document layer id -> PSD layer id (the exporter's map), parallel arrays. */
typedef struct pc_io_layer_id_map {
    const pc_layer_id *ids;
    const uint32_t *psd;
    size_t n;
} pc_io_layer_id_map;

/* What export does with resource 1050 (Option<Option<Vec<u8>>>). */
typedef enum pc_io_slices_action {
    PC_IO_SLICES_KEEP,   /* None: leave the preserved resource as it is */
    PC_IO_SLICES_REMOVE, /* Some(None) */
    PC_IO_SLICES_WRITE   /* Some(Some(bytes)): *out */
} pc_io_slices_action;

/* The slices described by resource 1050 data (raw NULL = absent: no slices) for doc's layers. */
pc_status pc_io_slices_from_psd(const uint8_t *raw, size_t len, const pc_document *doc, pc_slices *out,
                                pc_error *err);
/* Whether doc's slices still match its preserved resource 1050. */
pc_status pc_io_slices_unchanged(const pc_document *doc, bool *out, pc_error *err);
/* Reads the preserved resource 1050 into doc's slices. */
pc_status pc_io_slices_import(pc_document *doc, pc_error *err);
/* Resource 1050 for export (*out replaced only for PC_IO_SLICES_WRITE); a note (Option<String>)
 * is appended to warnings (may be NULL). ids may be NULL (no layers written). */
pc_status pc_io_slices_export_resource(const pc_document *doc, const pc_io_layer_id_map *ids,
                                       pc_io_slices_action *action, pc_buf *out, pc_io_warnings *warnings,
                                       pc_error *err);

#endif
