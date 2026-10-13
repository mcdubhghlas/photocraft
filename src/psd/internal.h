/* Internal helpers of the psd module: the bounds-checked reader (io.rs Reader), the buffered
 * writer (io.rs WriteExt) and error constructors with the reference's messages. Not public. */
#ifndef PC_PSD_INTERNAL_H
#define PC_PSD_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "psd/types.h"

/* `st = (x); if failed goto fail` for functions with a single cleanup label. */
#define CK(x)                                                                                      \
    do {                                                                                           \
        if ((st = (x)) != PC_OK) {                                                                 \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)

/* ---- errors ---- */

pc_status psd_eof(pc_error *err, size_t offset, size_t needed);
pc_status psd_bad_sig(pc_error *err, const char *expected, const uint8_t found[4]);
#define psd_invalid(err, ...) pc_fail((err), PC_ERR_FORMAT, "invalid data: " __VA_ARGS__)
#define psd_misuse(err, ...) pc_fail((err), PC_ERR_INVALID, "invalid data: " __VA_ARGS__)
#define psd_limit(err, what) pc_fail((err), PC_ERR_LIMIT, "limit exceeded: %s", (what))
#define psd_unsupported(err, ...) pc_fail((err), PC_ERR_UNSUPPORTED, "unsupported: " __VA_ARGS__)
#define psd_decompress(err, ...) pc_fail((err), PC_ERR_FORMAT, "decompression failed: " __VA_ARGS__)
#define psd_nomem(err) pc_fail((err), PC_ERR_NOMEM, "out of memory")
/* Prefixes err's message with "invalid data: <prefix>: " (Rust `map_err(|e| invalid(format!(
 * "{p}: {e}")))`), turning the error into PC_ERR_FORMAT. */
pc_status psd_wrap_invalid(pc_error *err, pc_status st, const char *prefix);

/* Appends a formatted warning (at most 1023 bytes). */
pc_status psd_warn(pc_psd_warnings *w, pc_error *err, const char *fmt, ...);

/* ---- reader ---- */

typedef struct psd_rd {
    const uint8_t *data;
    size_t len;
    size_t pos;
    bool little; /* little-endian integers (TIFF transcoding only) */
} psd_rd;

static inline psd_rd psd_rd_make(const uint8_t *data, size_t len)
{
    psd_rd r = {data, len, 0, false};
    return r;
}
static inline size_t psd_remaining(const psd_rd *r) { return r->len - r->pos; }
static inline bool psd_empty(const psd_rd *r) { return r->pos >= r->len; }
static inline const uint8_t *psd_rest(const psd_rd *r) { return r->data + r->pos; }

pc_status psd_bytes(psd_rd *r, size_t n, const uint8_t **out, pc_error *err);
pc_status psd_bytes_u64(psd_rd *r, uint64_t n, const uint8_t **out, size_t *len, pc_error *err);
pc_status psd_sub(psd_rd *r, uint64_t n, psd_rd *out, pc_error *err);
pc_status psd_skip(psd_rd *r, size_t n, pc_error *err);
pc_status psd_arr4(psd_rd *r, uint8_t out[4], pc_error *err);
pc_status psd_u8(psd_rd *r, uint8_t *v, pc_error *err);
pc_status psd_u16(psd_rd *r, uint16_t *v, pc_error *err);
pc_status psd_i16(psd_rd *r, int16_t *v, pc_error *err);
pc_status psd_u32(psd_rd *r, uint32_t *v, pc_error *err);
pc_status psd_i32(psd_rd *r, int32_t *v, pc_error *err);
pc_status psd_u64(psd_rd *r, uint64_t *v, pc_error *err);
pc_status psd_i64(psd_rd *r, int64_t *v, pc_error *err);
pc_status psd_f64(psd_rd *r, double *v, pc_error *err);
/* A 4-byte length, or 8 bytes when `wide`. */
pc_status psd_len_field(psd_rd *r, bool wide, uint64_t *v, pc_error *err);
/* Err unless count * min_item bytes remain (validates counts before allocating). */
pc_status psd_check_count(const psd_rd *r, uint64_t count, uint64_t min_item, pc_error *err);
/* Copies n bytes into a new owned vector. */
pc_status psd_take_bytes(psd_rd *r, uint64_t n, pc_psd_bytes *out, pc_error *err);
/* Pascal string padded to `align`: the raw name bytes (owned). */
pc_status psd_read_pascal(psd_rd *r, size_t align, pc_psd_bytes *out, pc_error *err);
/* u32 unit count + UTF-16BE units (native uint16 array, owned). */
pc_status psd_read_units(psd_rd *r, uint16_t **units, size_t *n, pc_error *err);

/* ---- writer: first error sticks, later writes are no-ops ---- */

typedef struct psd_w {
    pc_buf b;
    pc_status st;
    pc_error *err;
} psd_w;

void psd_w_init(psd_w *w, pc_error *err);
void psd_w_fini(psd_w *w);
/* Hands the bytes over (status of the writer; on failure frees them). */
pc_status psd_w_finish(psd_w *w, pc_psd_bytes *out);
static inline bool psd_w_ok(const psd_w *w) { return w->st == PC_OK; }
void psd_w_fail(psd_w *w, pc_status st); /* records st if no error yet (err already filled) */
void psd_put(psd_w *w, const void *data, size_t n);
void psd_put_zeros(psd_w *w, size_t n);
void psd_put_u8(psd_w *w, uint8_t v);
void psd_put_u16(psd_w *w, uint16_t v);
void psd_put_i16(psd_w *w, int16_t v);
void psd_put_u32(psd_w *w, uint32_t v);
void psd_put_i32(psd_w *w, int32_t v);
void psd_put_u64(psd_w *w, uint64_t v);
void psd_put_i64(psd_w *w, int64_t v);
void psd_put_f64(psd_w *w, double v);
void psd_put_len(psd_w *w, uint64_t v, bool wide);
size_t psd_begin_len(psd_w *w, bool wide);
void psd_end_len(psd_w *w, size_t at, bool wide);
void psd_write_pascal(psd_w *w, const uint8_t *name, size_t len, size_t align);
void psd_write_units(psd_w *w, const uint16_t *units, size_t n);

/* ---- names ---- */

/* Legacy (Pascal) name: UTF-8 when valid, else Latin-1. NUL-terminated, owned (free()). */
pc_status psd_decode_legacy_name(const uint8_t *b, size_t len, char **out, pc_error *err);
/* Pascal-field encoding of UTF-8 text: non-ASCII and control chars become '?', at most 255. */
pc_status psd_encode_legacy_name(const char *s, pc_psd_bytes *out, pc_error *err);
/* UTF-16 units → UTF-8 (lossy), NUL-terminated, owned. */
pc_status psd_units_to_utf8(const uint16_t *u, size_t n, char **out, pc_error *err);
/* UTF-8 → UTF-16 units (owned); invalid UTF-8 sequences become U+FFFD. */
pc_status psd_utf8_to_units(const char *s, uint16_t **out, size_t *n, pc_error *err);
pc_status psd_utf8n_to_units(const char *s, size_t len, uint16_t **out, size_t *n, pc_error *err);
/* How many of the n UTF-16 units to keep under a cap of max without splitting a surrogate pair
 * (the .aco and .ase writers share it, so the fix cannot drift between them). */
static inline size_t psd_units_capped(const uint16_t *u, size_t n, size_t max)
{
    if (n <= max) {
        return n;
    }
    if (max > 0 && u[max - 1] >= 0xD800u && u[max - 1] <= 0xDBFFu && u[max] >= 0xDC00u && u[max] <= 0xDFFFu) {
        return max - 1;
    }
    return max;
}
/* String::from_utf16_lossy into a pc_psd_str (replaces *out on success). */
pc_status psd_str_from_units(const uint16_t *u, size_t n, pc_psd_str *out, pc_error *err);

#endif
