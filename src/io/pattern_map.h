/* Document patterns <-> the PSD Patt / Pat2 / Pat3 global blocks and .pat files (chunk I4:
 * crates/io/src/pattern_map.rs). Thread-safe. */
#ifndef PC_IO_PATTERN_MAP_H
#define PC_IO_PATTERN_MAP_H

#include <stdbool.h>
#include <stddef.h>

#include "base/buf.h"
#include "doc/doc.h"
#include "psd/patterns.h"

/* A PSD pattern as a document pattern (1-bit planes become 8-bit grey, indexed patterns RGB
 * through their palette); *found false for an empty or unusable one. PC_ERR_LIMIT for a size
 * that doesn't fit the address space. */
pc_status pc_io_pattern_from_psd(const pc_psd_pattern *p, bool *found, pc_pattern *out, pc_error *err);
/* A document pattern as a PSD pattern (its own depth and model; other models as RGB). */
pc_status pc_io_pattern_to_psd(const pc_pattern *p, pc_psd_pattern *out, pc_error *err);
void pc_io_patterns_free(pc_pattern *v, size_t n); /* fini each + free; NULL-safe */

/* The patterns stored in doc's preserved global blocks, malformed blocks skipped (*out malloc'd,
 * pc_io_patterns_free; may be NULL when *n is 0). */
pc_status pc_io_patterns_from_global_blocks(const pc_document *doc, pc_pattern **out, size_t *n,
                                            pc_error *err);
/* The global blocks to write: all of doc's preserved global blocks (blobs retained) while their
 * patterns still equal doc's; otherwise the pattern blocks are replaced by one block written from
 * doc's patterns (at the first one's position, appended when there was none and doc has
 * patterns). *out malloc'd; release each data blob, then free. */
pc_status pc_io_export_pattern_blocks(const pc_document *doc, pc_doc_psd_global_block **out, size_t *n,
                                      pc_error *err);
/* A .pat file's patterns; failures keep the psd status with "not a readable .pat file: ...". */
pc_status pc_io_read_pat(const uint8_t *bytes, size_t len, pc_pattern **out, size_t *n, pc_error *err);
/* Patterns as a .pat file (*out replaced). */
pc_status pc_io_write_pat(const pc_pattern *patterns, size_t n, pc_buf *out, pc_error *err);

#endif
