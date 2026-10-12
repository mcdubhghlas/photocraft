/* PSD export helpers shared by the Multichannel writer (I1) and the PSD exporter (I3): version
 * choice, the encoded-size estimate and the channel-name resources (ports of the matching
 * functions of crates/io/src/psd_export.rs; I3 calls these instead of porting them again).
 * Internal to io. Thread-safe (no shared state). */
#ifndef PC_IO_PSD_COMMON_H
#define PC_IO_PSD_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/str.h"
#include "doc/doc.h"
#include "psd/types.h"

/* A PSD file may not exceed 2 GiB. */
#define PC_IO_PSD_MAX_FILE_SIZE ((uint64_t)2 * 1024 * 1024 * 1024)

/* PSB when forced, when a side exceeds 30000 px, or when the estimate says the file won't fit. */
pc_psd_version pc_io_psd_version(const pc_document *doc, bool force_psb, bool size_exceeds_limit);
/* The estimated encoded size (raw planes, worst-case RLE overhead, row tables, blocks and
 * resources); *ok false when it overflows u64 (the reference's None). Fails only on NOMEM. */
pc_status pc_io_estimate_psd_size(const pc_document *doc, bool *ok, uint64_t *out, pc_error *err);
/* The estimate is unknown or above PC_IO_PSD_MAX_FILE_SIZE. */
pc_status pc_io_psd_size_exceeds_limit(const pc_document *doc, bool *out, pc_error *err);

/* Resource 1045: each name as u32 count + UTF-16BE units, NUL-terminated (count includes it). */
pc_status pc_io_unicode_names_resource(const pc_str *names, size_t n, pc_buf *out, pc_error *err);
/* Resource 1006: Pascal names (printable ASCII kept, everything else '?', at most 255). */
pc_status pc_io_pascal_names_resource(const pc_str *names, size_t n, pc_buf *out, pc_error *err);

#endif
