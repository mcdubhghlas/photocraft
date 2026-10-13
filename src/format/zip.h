/* Minimal ZIP container (port of crates/format/src/zip.rs).
 *
 * We write STORE entries (the payloads are already zstd-compressed) and read STORE or DEFLATE
 * entries (so a bundle that was unzipped and re-zipped by another tool still loads). No ZIP64,
 * no encryption, no multi-disk archives.
 *
 * The writer's bytes are identical to the reference's for the same entries (fixed 1980-01-01
 * timestamp, UTF-8 flag, version 2.0). The reader treats the archive as hostile: every offset is
 * checked before use, an entry's data must lie inside the archive, sizes are capped by the
 * caller's `max`, DEFLATE output stops at max + 1 bytes, and sizes and CRC-32 are verified.
 * Errors carry the reference's FormatError text (format.h). Not synchronised. */
#ifndef PC_FORMAT_ZIP_H
#define PC_FORMAT_ZIP_H

#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "base/str.h"

/* CRC-32 (IEEE) of data (zip::crc32). */
uint32_t pc_zip_crc32(const uint8_t *data, size_t len);

/* ---- writing ---- */

typedef struct pc_zip_writer {
    /* private */
    pc_buf out;
    pc_buf central;
    uint16_t count;
} pc_zip_writer;

void pc_zip_writer_init(pc_zip_writer *w);
void pc_zip_writer_fini(pc_zip_writer *w); /* NULL-safe */

/* Append a STORE entry. Unsupported: the 65536th entry, an entry or archive over 4 GiB, a name
 * over 65535 bytes (the reference writes a corrupt length there). On failure the writer is
 * unchanged. */
pc_status pc_zip_writer_add(pc_zip_writer *w, const char *name, size_t name_len, const uint8_t *data,
                            size_t len, pc_error *err);

/* Append the central directory and end record; *out is replaced with the archive and the writer
 * is left empty (reusable). On failure nothing changes. */
pc_status pc_zip_writer_finish(pc_zip_writer *w, pc_buf *out, pc_error *err);

/* ---- reading ---- */

typedef struct pc_zip_entry {
    pc_str name; /* the central-directory name, invalid UTF-8 replaced like from_utf8_lossy */
    uint16_t method;
    uint32_t crc;
    size_t compressed;
    size_t uncompressed;
    size_t data_offset;
} pc_zip_entry;

/* A read-only view of an archive in memory. The bytes are borrowed and must outlive it. */
typedef struct pc_zip_reader {
    const uint8_t *bytes;
    size_t len;
    pc_zip_entry *entries; /* in central-directory order; treat as read-only */
    size_t count;
    /* private: name -> first entry with that name (open addressing; slot value index + 1, 0 =
     * empty), so pc_zip_find is O(1) (a bundle reads one entry per tile). NULL falls back to a
     * scan. */
    size_t *index;
    size_t index_cap;
} pc_zip_reader;

/* Parse the central directory (*r is initialised on success only). */
pc_status pc_zip_reader_open(pc_zip_reader *r, const uint8_t *bytes, size_t len, pc_error *err);
void pc_zip_reader_close(pc_zip_reader *r); /* NULL-safe */

/* The first entry named exactly `name` (len bytes), or NULL. Borrowed. */
const pc_zip_entry *pc_zip_find(const pc_zip_reader *r, const char *name, size_t len);

/* An entry's payload (decompressed), at most `max` bytes, size- and CRC-checked. *out is replaced
 * on success. An entry whose data lies outside this archive (e.g. one from another reader) is
 * corrupt "zip entry `name` out of range". */
pc_status pc_zip_read(const pc_zip_reader *r, const pc_zip_entry *e, size_t max, pc_buf *out, pc_error *err);
/* pc_zip_read of the first entry named `name` (NUL-terminated); a missing one is
 * "corrupt .pcraft bundle: missing `name`". */
pc_status pc_zip_read_by_name(const pc_zip_reader *r, const char *name, size_t max, pc_buf *out,
                              pc_error *err);

#endif
