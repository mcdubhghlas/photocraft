/* Synthetic Affinity containers for the tests (port of crates/affinity/src/synth.rs). The stream
 * is written by emitting fields in order (the reference builds an F tree, then encodes it the same
 * way). Not an Affinity exporter. Test-only: allocation failures abort. Zlib / zstd payloads come
 * from zlib / libzstd, so they differ from the reference's bytes; everything else is identical. */
#ifndef PC_TESTS_AFFINITY_SYNTH_H
#define PC_TESTS_AFFINITY_SYNTH_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/compress.h"

static inline void sy_ok(pc_status st)
{
    if (st != PC_OK) {
        (void)fprintf(stderr, "synth: out of memory\n");
        abort();
    }
}

static inline void sy_put(pc_buf *b, const void *p, size_t n) { sy_ok(pc_buf_append(b, p, n, NULL)); }
static inline void sy_byte(pc_buf *b, uint8_t v) { sy_put(b, &v, 1); }

static inline void sy_le(pc_buf *b, uint64_t v, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        sy_byte(b, (uint8_t)(v >> (8 * i)));
    }
}

/* A tag in the stream's byte order (little-endian of the readable big-endian value). */
static inline void sy_tag(pc_buf *b, const char *t)
{
    uint32_t v = (uint32_t)(uint8_t)t[0] << 24 | (uint32_t)(uint8_t)t[1] << 16 | (uint32_t)(uint8_t)t[2] << 8 |
                 (uint32_t)(uint8_t)t[3];
    sy_le(b, v, 4);
}

/* Field head: code, then the tag unless tag is NULL (positional). */
static inline void sy_head(pc_buf *b, uint8_t code, const char *tag)
{
    sy_byte(b, code);
    if (tag != NULL) {
        sy_tag(b, tag);
    }
}

static inline void sy_u8(pc_buf *b, const char *tag, uint8_t v)
{
    sy_head(b, 0x01, tag);
    sy_byte(b, v);
}

static inline void sy_u32(pc_buf *b, const char *tag, uint32_t v)
{
    sy_head(b, 0x03, tag);
    sy_le(b, v, 4);
}

static inline void sy_i32(pc_buf *b, const char *tag, int32_t v)
{
    sy_head(b, 0x07, tag);
    sy_le(b, (uint32_t)v, 4);
}

static inline void sy_f32(pc_buf *b, const char *tag, float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    sy_head(b, 0x09, tag);
    sy_le(b, u, 4);
}

static inline void sy_f64_raw(pc_buf *b, double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    sy_le(b, u, 8);
}

static inline void sy_f64(pc_buf *b, const char *tag, double v)
{
    sy_head(b, 0x0a, tag);
    sy_f64_raw(b, v);
}

static inline void sy_bool(pc_buf *b, const char *tag, bool v)
{
    sy_head(b, 0x29, tag);
    sy_byte(b, v ? 1 : 0);
}

static inline void sy_enum(pc_buf *b, const char *tag, uint16_t id, uint16_t ver)
{
    sy_head(b, 0x2a, tag);
    sy_le(b, id, 2);
    sy_le(b, ver, 2);
}

static inline void sy_str(pc_buf *b, const char *tag, const char *s)
{
    sy_head(b, 0x2b, tag);
    sy_le(b, strlen(s), 4);
    sy_put(b, s, strlen(s));
}

/* f64 vector of 2..6 values. */
static inline void sy_f64s(pc_buf *b, const char *tag, const double *v, size_t n)
{
    size_t i;
    sy_head(b, (uint8_t)(0x22 + n), tag);
    for (i = 0; i < n; i++) {
        sy_f64_raw(b, v[i]);
    }
}

/* Fixed-size struct, 1..64 bytes. */
static inline void sy_struct(pc_buf *b, const char *tag, const void *v, size_t n)
{
    sy_head(b, (uint8_t)(0x34 + n), tag);
    sy_put(b, v, n);
}

/* Array of n fixed-size records of size bytes each (concatenated in recs). */
static inline void sy_records(pc_buf *b, const char *tag, uint16_t size, const void *recs, size_t n)
{
    sy_head(b, 0x80 | 0x2c, tag);
    sy_le(b, n, 4);
    sy_le(b, size, 2);
    sy_put(b, recs, (size_t)size * n);
}

static inline void sy_u8s(pc_buf *b, const char *tag, const uint8_t *v, size_t n)
{
    sy_head(b, 0x80 | 0x01, tag);
    sy_le(b, n, 4);
    sy_put(b, v, n);
}

static inline void sy_entry(pc_buf *b, const char *tag, const char *name)
{
    sy_head(b, 0x33, tag);
    sy_tag(b, "Data");
    sy_le(b, strlen(name), 4);
    sy_put(b, name, strlen(name));
}

/* Shared object definition payload: chain levels but the last carry no fields; then the
 * object's fields follow (end them with sy_end). */
static inline void sy_def_payload(pc_buf *b, uint32_t id, const char *const *chain, size_t nchain)
{
    size_t i;
    sy_byte(b, 1);
    sy_le(b, id, 4);
    for (i = 0; i + 1 < nchain; i++) {
        sy_byte(b, 0);
        sy_tag(b, chain[i]);
        sy_le(b, 0, 2);
        sy_byte(b, 0);
    }
    sy_byte(b, 1);
    sy_tag(b, chain[nchain - 1]);
}

static inline void sy_def(pc_buf *b, const char *tag, uint32_t id, const char *const *chain, size_t nchain)
{
    sy_head(b, 0x31, tag);
    sy_def_payload(b, id, chain, nchain);
}

static inline void sy_ref_payload(pc_buf *b, uint32_t id)
{
    sy_byte(b, 2);
    sy_le(b, id, 4);
}

static inline void sy_ref(pc_buf *b, const char *tag, uint32_t id)
{
    sy_head(b, 0x31, tag);
    sy_ref_payload(b, id);
}

static inline void sy_null(pc_buf *b, const char *tag)
{
    sy_head(b, 0x31, tag);
    sy_byte(b, 0);
}

/* Inline object: fields follow (end with sy_end). */
static inline void sy_obj(pc_buf *b, const char *tag, const char *cls)
{
    sy_head(b, 0x32, tag);
    sy_byte(b, 1);
    sy_tag(b, cls);
    sy_le(b, 0, 2);
}

/* Positional object: untagged fields follow (tag NULL), end with sy_end. */
static inline void sy_pos(pc_buf *b, const char *tag) { sy_head(b, 0x30, tag); }

/* Array of n shared objects / links: follow with n sy_def_payload(+fields+sy_end) or
 * sy_ref_payload. */
static inline void sy_shared(pc_buf *b, const char *tag, size_t n)
{
    sy_head(b, 0x80 | 0x31, tag);
    sy_le(b, n, 4);
}

/* Array of n inline objects of one class: follow with n (sy_objs_item, fields, sy_end). */
static inline void sy_objs(pc_buf *b, const char *tag, const char *cls, size_t n)
{
    sy_head(b, 0x80 | 0x32, tag);
    sy_le(b, n, 4);
    sy_tag(b, cls);
    sy_le(b, 0, 2);
}

static inline void sy_objs_item(pc_buf *b) { sy_byte(b, 1); }

/* Ends a field list. */
static inline void sy_end(pc_buf *b) { sy_byte(b, 0); }

/* Starts a doc.dat stream with root class Pers; emit the root's fields, then sy_end. */
static inline void sy_stream_begin(pc_buf *b)
{
    sy_put(b, "\x00\xffKS", 4);
    sy_le(b, 2, 2);
    sy_tag(b, "Pers");
    sy_le(b, 0, 2);
    sy_le(b, 30, 4);
}

typedef enum sy_method { SY_STORED, SY_ZLIB, SY_ZSTD } sy_method;

typedef struct sy_entry {
    const char *name;
    const uint8_t *data;
    size_t len;
    sy_method method;
} sy_entry_t;

/* A version-12 container with one #FT4 table holding entries, then an optional thumbnail. */
static inline pc_buf sy_container(const sy_entry_t *entries, size_t n, const uint8_t *png, size_t npng)
{
    pc_buf out = PC_BUF_INIT;
    pc_buf records = PC_BUF_INIT;
    size_t i;
    uint64_t fat;
    uint64_t thumb;
    uint8_t head[72];
    memset(head, 0, sizeof head);
    memcpy(head, "\x00\xffKA", 4);
    head[4] = 12;
    head[6] = 4;
    memcpy(head + 8, "nsrP", 4);
    memcpy(head + 12, "#Inf", 4);
    memcpy(head + 64, "Prot", 4);
    sy_put(&out, head, 72);
    for (i = 0; i < n; i++) {
        pc_buf packed = PC_BUF_INIT;
        uint64_t offset = out.len;
        if (entries[i].method == SY_STORED) {
            sy_put(&packed, entries[i].data, entries[i].len);
        } else if (entries[i].method == SY_ZLIB) {
            sy_ok(pc_deflate(PC_Z_ZLIB, PC_Z_FAST, entries[i].data, entries[i].len, &packed, NULL));
        } else {
            sy_ok(pc_zstd_compress(1, entries[i].data, entries[i].len, &packed, NULL));
        }
        sy_put(&out, "#Fil", 4);
        sy_put(&out, packed.data, packed.len);
        sy_le(&records, i + 1, 4);
        sy_byte(&records, 0);
        sy_le(&records, offset, 8);
        sy_le(&records, entries[i].len, 8);
        sy_le(&records, packed.len, 8);
        sy_le(&records, pc_crc32(0, entries[i].data, entries[i].len), 4);
        sy_byte(&records, entries[i].method == SY_STORED ? 0 : entries[i].method == SY_ZLIB ? 1 : 2);
        sy_le(&records, 0, 4);
        sy_le(&records, 0, 4);
        sy_le(&records, strlen(entries[i].name), 2);
        sy_put(&records, entries[i].name, strlen(entries[i].name));
        pc_buf_fini(&packed);
    }
    fat = out.len;
    sy_put(&out, "#FT4", 4);
    sy_le(&out, 0, 8);
    sy_le(&out, 1700000000u, 8);
    {
        uint8_t z[24] = {0};
        sy_put(&out, z, 24);
    }
    sy_le(&out, n, 4);
    {
        uint8_t z[8] = {0};
        sy_put(&out, z, 8);
    }
    sy_le(&out, 0, 2);
    sy_byte(&out, 0);
    sy_put(&out, records.data, records.len);
    pc_buf_fini(&records);
    thumb = out.len;
    for (i = 0; i < 8; i++) {
        out.data[16 + i] = (uint8_t)(fat >> (8 * i));
    }
    if (png != NULL) {
        for (i = 0; i < 8; i++) {
            out.data[24 + i] = (uint8_t)(thumb >> (8 * i));
        }
        sy_put(&out, "\xff\xff\xff\xffThmb", 8);
        sy_le(&out, 1, 4);
        sy_le(&out, npng + 13, 4);
        sy_le(&out, 29, 4);
        sy_le(&out, 0, 4);
        sy_le(&out, npng, 4);
        sy_byte(&out, 1);
        sy_put(&out, png, npng);
    }
    return out;
}

#endif
