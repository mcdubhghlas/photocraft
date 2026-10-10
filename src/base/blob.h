/* pc_blob: immutable, reference-counted bytes (Rust `Arc<Vec<u8>>`, also `Arc<Vec<f32>>`).
 *
 * Retaining is cheap and thread-safe (atomic count), so cloning a document shares its PSD raw
 * blocks, embedded files and LUTs. The bytes never change after creation. Data is aligned for
 * any scalar type, so a blob may hold an array of floats (len is always in bytes). */
#ifndef PC_BASE_BLOB_H
#define PC_BASE_BLOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "status.h"

typedef struct pc_blob pc_blob;

/* A blob holding a copy of len bytes (data may be NULL when len is 0). */
pc_status pc_blob_new(const void *data, size_t len, pc_blob **out, pc_error *err);
/* A zero-filled blob of len bytes for the creator to fill through pc_blob_data_mut before
 * sharing it. */
pc_status pc_blob_alloc(size_t len, pc_blob **out, pc_error *err);
pc_blob *pc_blob_retain(pc_blob *b); /* NULL-safe; returns b */
void pc_blob_release(pc_blob *b);    /* NULL-safe */

const uint8_t *pc_blob_data(const pc_blob *b); /* NULL-safe: NULL for NULL */
uint8_t *pc_blob_data_mut(pc_blob *b);         /* only before the blob is shared */
size_t pc_blob_len(const pc_blob *b);          /* NULL-safe: 0 for NULL */
/* Same bytes (two NULLs are equal; NULL never equals a blob, even an empty one). */
bool pc_blob_eq(const pc_blob *a, const pc_blob *b);

#endif
