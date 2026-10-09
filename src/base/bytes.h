/* Explicit-endian loads/stores and a bounds-checked reader for parsing untrusted files.
 *
 * pc_rd is a cursor over a borrowed byte range with a *sticky* error: any read past the end sets
 * `failed`, returns 0 (or NULL), and every later read also fails. Parse a whole structure, then
 * check pc_rd_ok() once. The reader never reads outside [data, data + len).
 *
 * Floats are read via their IEEE 754 bit patterns; the build rejects platforms where float/double
 * aren't IEEE binary32/binary64. */
#ifndef PC_BASE_BYTES_H
#define PC_BASE_BYTES_H

#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "buf.h"
#include "status.h"

_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && sizeof(float) == 4, "float must be IEEE binary32");
_Static_assert(DBL_MANT_DIG == 53 && sizeof(double) == 8, "double must be IEEE binary64");

/* ---- raw loads/stores; caller guarantees the bytes exist ---- */

static inline uint16_t pc_load_be16(const uint8_t *p) { return (uint16_t)((unsigned)p[0] << 8 | p[1]); }
static inline uint16_t pc_load_le16(const uint8_t *p) { return (uint16_t)((unsigned)p[1] << 8 | p[0]); }

static inline uint32_t pc_load_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static inline uint32_t pc_load_le32(const uint8_t *p)
{
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | (uint32_t)p[0];
}

static inline uint64_t pc_load_be64(const uint8_t *p)
{
    return (uint64_t)pc_load_be32(p) << 32 | pc_load_be32(p + 4);
}

static inline uint64_t pc_load_le64(const uint8_t *p)
{
    return (uint64_t)pc_load_le32(p + 4) << 32 | pc_load_le32(p);
}

static inline void pc_store_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void pc_store_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static inline void pc_store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static inline void pc_store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static inline void pc_store_be64(uint8_t *p, uint64_t v) { pc_store_be32(p, (uint32_t)(v >> 32)); pc_store_be32(p + 4, (uint32_t)v); }
static inline void pc_store_le64(uint8_t *p, uint64_t v) { pc_store_le32(p, (uint32_t)v); pc_store_le32(p + 4, (uint32_t)(v >> 32)); }

static inline float pc_f32_from_bits(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }
static inline uint32_t pc_f32_to_bits(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }
static inline double pc_f64_from_bits(uint64_t u) { double d; memcpy(&d, &u, sizeof d); return d; }
static inline uint64_t pc_f64_to_bits(double d) { uint64_t u; memcpy(&u, &d, sizeof u); return u; }

/* ---- bounds-checked reader ---- */

typedef struct pc_rd {
    const uint8_t *data; /* borrowed */
    size_t len;
    size_t pos;
    bool failed;
} pc_rd;

void pc_rd_init(pc_rd *r, const uint8_t *data, size_t len);

static inline bool pc_rd_ok(const pc_rd *r) { return !r->failed; }
static inline size_t pc_rd_remaining(const pc_rd *r) { return r->failed ? 0 : r->len - r->pos; }

/* Return a pointer to the next n bytes and advance, or NULL (and fail) if fewer remain. */
const uint8_t *pc_rd_bytes(pc_rd *r, size_t n);

/* Advance n bytes; fails if fewer remain. */
bool pc_rd_skip(pc_rd *r, size_t n);

/* Move to absolute offset pos (<= len); fails otherwise. */
bool pc_rd_seek(pc_rd *r, size_t pos);

/* A sub-reader over the next n bytes; advances r past them. On failure both readers fail. */
pc_rd pc_rd_sub(pc_rd *r, size_t n);

uint8_t pc_rd_u8(pc_rd *r);
uint16_t pc_rd_be16(pc_rd *r);
uint16_t pc_rd_le16(pc_rd *r);
uint32_t pc_rd_be32(pc_rd *r);
uint32_t pc_rd_le32(pc_rd *r);
uint64_t pc_rd_be64(pc_rd *r);
uint64_t pc_rd_le64(pc_rd *r);
int16_t pc_rd_be16s(pc_rd *r);
int32_t pc_rd_be32s(pc_rd *r);
int64_t pc_rd_be64s(pc_rd *r);
float pc_rd_bef32(pc_rd *r);
double pc_rd_bef64(pc_rd *r);

/* Map an unsigned bit pattern to the two's-complement signed value without
 * implementation-defined conversions. */
int16_t pc_i16_from_bits(uint16_t u);
int64_t pc_i64_from_bits(uint64_t u);

/* ---- appending writers (thin wrappers over pc_buf_append) ---- */

pc_status pc_buf_put_be16(pc_buf *b, uint16_t v, pc_error *err);
pc_status pc_buf_put_le16(pc_buf *b, uint16_t v, pc_error *err);
pc_status pc_buf_put_be32(pc_buf *b, uint32_t v, pc_error *err);
pc_status pc_buf_put_le32(pc_buf *b, uint32_t v, pc_error *err);
pc_status pc_buf_put_be64(pc_buf *b, uint64_t v, pc_error *err);
pc_status pc_buf_put_le64(pc_buf *b, uint64_t v, pc_error *err);

#endif
