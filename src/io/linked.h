/* Embedded smart-object files in the PSD global lnk2 / lnk3 / lnkD blocks (port of
 * crates/io/src/linked.rs).
 *
 * PSD import keeps a placed layer's source as a linked path holding its Idnt uuid; the file bytes
 * stay inside the preserved global block (so PSD export stays byte-stable). This finds the bytes
 * for a uuid when a smart object has to be re-rendered from its source.
 *
 * Layout (Adobe PSD spec, "Linked Layer"): items, each u64 length + item data padded to 4. Item:
 * type (liFD data / liFE external / liFA alias), version (1-7), Pascal uuid, Unicode file name,
 * file type, creator, u64 data length, open-descriptor flag (+ versioned descriptor), then for
 * liFD the raw file bytes. Malformed data is skipped, never an error (only out of memory is).
 * Thread-safe (no shared state). */
#ifndef PC_IO_LINKED_H
#define PC_IO_LINKED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/str.h"
#include "doc/doc.h"
#include "io/core.h"

/* An embedded file found in a linked-layer block. */
typedef struct pc_io_linked_file {
    pc_str uuid;      /* lossy UTF-8 of the Pascal id */
    pc_str file_name; /* lossy UTF-16, trailing U+0000 trimmed */
    uint8_t *bytes;
    size_t len;
} pc_io_linked_file;

void pc_io_linked_file_fini(pc_io_linked_file *f); /* NULL-safe */

typedef struct pc_io_linked_files {
    pc_io_linked_file *v;
    size_t n;
    size_t cap;
} pc_io_linked_files;

void pc_io_linked_files_fini(pc_io_linked_files *l); /* NULL-safe */

/* Every embedded (liFD) file in one linked-layer block's data; *out initialised on success. */
pc_status pc_io_parse_linked_files(const uint8_t *data, size_t len, pc_io_linked_files *out, pc_error *err);
/* The embedded file for smart-object uuid among the document's preserved PSD global blocks
 * (lnk2 / lnk3 / lnkD, in order). *found false (out untouched) when there is none or uuid is
 * empty. */
pc_status pc_io_find_linked_file(const pc_metadata *meta, const char *uuid, size_t uuid_len, bool *found,
                                 pc_io_linked_file *out, pc_error *err);
/* The four-character file type Photoshop records for an embedded file, from its contents. */
void pc_io_linked_file_type(const uint8_t *bytes, size_t len, uint8_t out[4]);
/* One liFD item (version 7, no open descriptor, empty child id, no modification time,
 * unlocked), length-prefixed and padded to 4; appended to *out. */
pc_status pc_io_encode_linked_file(const pc_io_linked_file *f, pc_buf *out, pc_error *err);

/* Decides whether rebuild keeps the item with this uuid. */
typedef bool (*pc_io_keep_uuid)(void *ctx, const char *uuid, size_t len);

/* Rewrites a linked-layer block: keeps the items keep accepts (and anything unreadable), then
 * appends add[0..nadd). *changed false (out untouched) when nothing would change. */
pc_status pc_io_rebuild_linked_block(const uint8_t *data, size_t len, pc_io_keep_uuid keep, void *ctx,
                                     const pc_io_linked_file *add, size_t nadd, bool *changed, pc_buf *out,
                                     pc_error *err);
/* The uuids of every item in a linked-layer block; *out initialised on success. */
pc_status pc_io_linked_block_uuids(const uint8_t *data, size_t len, pc_io_strs *out, pc_error *err);

/* Copies one linked item under a new UUID (its embedded/external/alias payload preserved),
 * appending the rebuilt item to out. *found false (out untouched) when uuid is absent or the item
 * is malformed. */
pc_status pc_io_copy_linked_item(const uint8_t *data, size_t len, const char *uuid, size_t uuid_len,
                                 const char *new_uuid, size_t new_uuid_len, bool *found, pc_buf *out,
                                 pc_error *err);

#endif
