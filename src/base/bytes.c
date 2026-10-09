#include "bytes.h"

#include "num.h"

void pc_rd_init(pc_rd *r, const uint8_t *data, size_t len)
{
    r->data = data;
    r->len = data != NULL ? len : 0;
    r->pos = 0;
    r->failed = false;
}

const uint8_t *pc_rd_bytes(pc_rd *r, size_t n)
{
    const uint8_t *p;
    if (r->failed || n > r->len - r->pos) {
        r->failed = true;
        return NULL;
    }
    p = r->data + r->pos;
    r->pos += n;
    return p;
}

bool pc_rd_skip(pc_rd *r, size_t n)
{
    return pc_rd_bytes(r, n) != NULL;
}

bool pc_rd_seek(pc_rd *r, size_t pos)
{
    if (r->failed || pos > r->len) {
        r->failed = true;
        return false;
    }
    r->pos = pos;
    return true;
}

pc_rd pc_rd_sub(pc_rd *r, size_t n)
{
    pc_rd sub;
    const uint8_t *p = pc_rd_bytes(r, n);
    pc_rd_init(&sub, p, p != NULL ? n : 0);
    sub.failed = (p == NULL);
    return sub;
}

uint8_t pc_rd_u8(pc_rd *r)
{
    const uint8_t *p = pc_rd_bytes(r, 1);
    return p != NULL ? p[0] : 0;
}

uint16_t pc_rd_be16(pc_rd *r)
{
    const uint8_t *p = pc_rd_bytes(r, 2);
    return p != NULL ? pc_load_be16(p) : 0;
}

uint16_t pc_rd_le16(pc_rd *r)
{
    const uint8_t *p = pc_rd_bytes(r, 2);
    return p != NULL ? pc_load_le16(p) : 0;
}

uint32_t pc_rd_be32(pc_rd *r)
{
    const uint8_t *p = pc_rd_bytes(r, 4);
    return p != NULL ? pc_load_be32(p) : 0;
}

uint32_t pc_rd_le32(pc_rd *r)
{
    const uint8_t *p = pc_rd_bytes(r, 4);
    return p != NULL ? pc_load_le32(p) : 0;
}

uint64_t pc_rd_be64(pc_rd *r)
{
    const uint8_t *p = pc_rd_bytes(r, 8);
    return p != NULL ? pc_load_be64(p) : 0;
}

uint64_t pc_rd_le64(pc_rd *r)
{
    const uint8_t *p = pc_rd_bytes(r, 8);
    return p != NULL ? pc_load_le64(p) : 0;
}

int16_t pc_i16_from_bits(uint16_t u)
{
    if (u <= (uint16_t)INT16_MAX) {
        return (int16_t)u;
    }
    return (int16_t)((int32_t)u - 65536);
}

int64_t pc_i64_from_bits(uint64_t u)
{
    if (u <= (uint64_t)INT64_MAX) {
        return (int64_t)u;
    }
    return (int64_t)(u - 0x8000000000000000u) + INT64_MIN;
}

int16_t pc_rd_be16s(pc_rd *r) { return pc_i16_from_bits(pc_rd_be16(r)); }
int32_t pc_rd_be32s(pc_rd *r) { return pc_i32_from_bits(pc_rd_be32(r)); }
int64_t pc_rd_be64s(pc_rd *r) { return pc_i64_from_bits(pc_rd_be64(r)); }
float pc_rd_bef32(pc_rd *r) { return pc_f32_from_bits(pc_rd_be32(r)); }
double pc_rd_bef64(pc_rd *r) { return pc_f64_from_bits(pc_rd_be64(r)); }

pc_status pc_buf_put_be16(pc_buf *b, uint16_t v, pc_error *err)
{
    uint8_t t[2];
    pc_store_be16(t, v);
    return pc_buf_append(b, t, sizeof t, err);
}

pc_status pc_buf_put_le16(pc_buf *b, uint16_t v, pc_error *err)
{
    uint8_t t[2];
    pc_store_le16(t, v);
    return pc_buf_append(b, t, sizeof t, err);
}

pc_status pc_buf_put_be32(pc_buf *b, uint32_t v, pc_error *err)
{
    uint8_t t[4];
    pc_store_be32(t, v);
    return pc_buf_append(b, t, sizeof t, err);
}

pc_status pc_buf_put_le32(pc_buf *b, uint32_t v, pc_error *err)
{
    uint8_t t[4];
    pc_store_le32(t, v);
    return pc_buf_append(b, t, sizeof t, err);
}

pc_status pc_buf_put_be64(pc_buf *b, uint64_t v, pc_error *err)
{
    uint8_t t[8];
    pc_store_be64(t, v);
    return pc_buf_append(b, t, sizeof t, err);
}

pc_status pc_buf_put_le64(pc_buf *b, uint64_t v, pc_error *err)
{
    uint8_t t[8];
    pc_store_le64(t, v);
    return pc_buf_append(b, t, sizeof t, err);
}
