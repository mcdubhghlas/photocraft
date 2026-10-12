/* Notes and the measurement scale <-> PSD Anno blocks and resource 1074 (chunk I4:
 * crates/io/src/annotations_map.rs). The reference's Option results are *found / *has flags
 * (other out-params written only when set); pc_buf outputs are replaced on success. Failures
 * are out of memory only. Thread-safe. */
#ifndef PC_IO_ANNOTATIONS_MAP_H
#define PC_IO_ANNOTATIONS_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "doc/doc.h"

#define PC_IO_MEASUREMENT_SCALE 1074

/* A borrowed byte range. */
typedef struct pc_io_byte_span {
    const uint8_t *data;
    size_t len;
} pc_io_byte_span;

/* The notes of an Anno block (*out malloc'd, pc_note_fini each); *found false when it doesn't
 * parse (also when its entry count exceeds its length). Sound entries are skipped. */
pc_status pc_io_parse_anno(const uint8_t *data, size_t len, bool *found, pc_note **out, size_t *n,
                           pc_error *err);
/* An Anno block holding notes plus raw (sound) entries carried over. */
pc_status pc_io_write_anno(const pc_note *notes, size_t n, const pc_io_byte_span *extra, size_t nextra,
                           pc_buf *out, pc_error *err);
/* The scale described by resource 1074 data; *found false when it doesn't parse. */
pc_status pc_io_scale_from_resource(const uint8_t *data, size_t len, bool *found, pc_measurement_scale *out,
                                    pc_error *err);
/* Resource 1074 data for a scale (even-padded). */
pc_status pc_io_write_scale_resource(const pc_measurement_scale *s, pc_buf *out, pc_error *err);

/* The notes in doc's preserved Anno block (*out malloc'd, pc_note_fini each). */
pc_status pc_io_notes_from_blocks(const pc_document *doc, pc_note **out, size_t *n, pc_error *err);
/* An Anno block is present but can't be read. */
bool pc_io_anno_unreadable(const pc_document *doc);
/* The global blocks for export: the raw Anno block while it still describes doc's notes (or
 * doesn't parse and there are no notes), else a regenerated one in its place (appended when new,
 * dropped when the last note is deleted). blocks (a malloc'd array, may be NULL when n is 0) is
 * taken over in every case: *out is the same array, possibly reallocated, with unchanged blocks
 * keeping their blobs; on failure it is released. */
pc_status pc_io_annotation_export_blocks(const pc_document *doc, pc_doc_psd_global_block *blocks, size_t n,
                                         pc_doc_psd_global_block **out, size_t *nout, pc_error *err);
/* The scale stored in resource 1074 (*out initialised when true); false when absent, unreadable
 * or out of memory. */
bool pc_io_raw_scale(const pc_document *doc, pc_measurement_scale *out);
/* A new resource 1074 when the document's scale changed; *has false otherwise. */
pc_status pc_io_fresh_scale_resource(const pc_document *doc, bool *has, pc_buf *out, pc_error *err);
/* Whether the preserved resource id is written back as is. */
bool pc_io_keep_resource(const pc_document *doc, uint16_t id);

#endif
