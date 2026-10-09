/* Decoding follows base64-0.22.1 engine/general_purpose/{decode.rs,decode_suffix.rs} so errors
 * (kind, offset, byte) match the reference exactly. */
#include "base64.h"

#include "num.h"

static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
enum { INVALID = 0xFF, PAD = '=' };

static uint8_t morsel_of(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') { return (uint8_t)(c - 'A'); }
    if (c >= 'a' && c <= 'z') { return (uint8_t)(c - 'a' + 26); }
    if (c >= '0' && c <= '9') { return (uint8_t)(c - '0' + 52); }
    if (c == '+') { return 62; }
    if (c == '/') { return 63; }
    return INVALID;
}

pc_status pc_base64_encode(const uint8_t *data, size_t len, pc_buf *out, pc_error *err)
{
    size_t groups = len / 3 + (len % 3 != 0 ? 1u : 0u);
    size_t need;
    size_t i;
    pc_status s;
    char *p;

    if (!pc_size_mul(groups, 4, &need)) {
        return pc_fail(err, PC_ERR_LIMIT, "base64 output too large");
    }
    s = pc_buf_reserve(out, need, err);
    if (s != PC_OK) {
        return s;
    }
    p = (char *)out->data + out->len;
    for (i = 0; i + 3 <= len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16 | (uint32_t)data[i + 1] << 8 | data[i + 2];
        *p++ = ALPHABET[v >> 18];
        *p++ = ALPHABET[(v >> 12) & 63];
        *p++ = ALPHABET[(v >> 6) & 63];
        *p++ = ALPHABET[v & 63];
    }
    if (len - i == 1) {
        uint32_t v = (uint32_t)data[i] << 16;
        *p++ = ALPHABET[v >> 18];
        *p++ = ALPHABET[(v >> 12) & 63];
        *p++ = PAD;
        *p++ = PAD;
    } else if (len - i == 2) {
        uint32_t v = (uint32_t)data[i] << 16 | (uint32_t)data[i + 1] << 8;
        *p++ = ALPHABET[v >> 18];
        *p++ = ALPHABET[(v >> 12) & 63];
        *p++ = ALPHABET[(v >> 6) & 63];
        *p++ = PAD;
    }
    out->len += need;
    out->data[out->len] = '\0';
    return PC_OK;
}

static pc_status invalid_byte(pc_error *err, size_t index, uint8_t b)
{
    return pc_fail(err, PC_ERR_FORMAT, "Invalid symbol %u, offset %zu.", (unsigned)b, index);
}

pc_status pc_base64_decode(const char *text, size_t len, pc_buf *out, pc_error *err)
{
    const uint8_t *in = (const uint8_t *)text;
    size_t rem = len % 4;
    size_t quads; /* bytes of complete, non-final quads */
    size_t i;
    pc_buf tmp = PC_BUF_INIT;
    pc_status s;
    uint8_t morsels[4] = {0, 0, 0, 0};
    size_t nmorsels = 0;
    size_t npad = 0;
    size_t first_pad = 0;
    uint8_t last_symbol = 0;
    uint32_t num;
    size_t tail_bytes;

    if (rem == 1 && in[len - 1] != PAD && morsel_of(in[len - 1]) == INVALID) {
        return invalid_byte(err, len - 1, in[len - 1]);
    }
    quads = len - rem;
    if (rem == 0) {
        quads = quads >= 4 ? quads - 4 : 0;
    }
    s = pc_buf_reserve(&tmp, quads / 4 * 3 + 3, err);
    if (s != PC_OK) {
        return s;
    }
    for (i = 0; i < quads; i += 4) {
        uint8_t m[4];
        size_t k;
        for (k = 0; k < 4; k++) {
            m[k] = morsel_of(in[i + k]);
            if (m[k] == INVALID) {
                pc_buf_fini(&tmp);
                return invalid_byte(err, i + k, in[i + k]);
            }
        }
        num = (uint32_t)m[0] << 18 | (uint32_t)m[1] << 12 | (uint32_t)m[2] << 6 | m[3];
        tmp.data[tmp.len++] = (uint8_t)(num >> 16);
        tmp.data[tmp.len++] = (uint8_t)(num >> 8);
        tmp.data[tmp.len++] = (uint8_t)num;
    }

    /* The final 1-4 bytes (decode_suffix). */
    for (i = quads; i < len; i++) {
        size_t leftover = i - quads;
        uint8_t b = in[i];
        uint8_t m;
        if (b == PAD) {
            if (leftover < 2) {
                pc_buf_fini(&tmp);
                return invalid_byte(err, i, b);
            }
            if (npad == 0) {
                first_pad = leftover;
            }
            npad++;
            continue;
        }
        if (npad > 0) {
            pc_buf_fini(&tmp);
            return invalid_byte(err, quads + first_pad, PAD);
        }
        last_symbol = b;
        m = morsel_of(b);
        if (m == INVALID) {
            pc_buf_fini(&tmp);
            return invalid_byte(err, i, b);
        }
        morsels[nmorsels++] = m;
    }
    if (len > 0 && nmorsels < 2) {
        pc_buf_fini(&tmp);
        return pc_fail(err, PC_ERR_FORMAT, "Invalid input length: %zu", quads + nmorsels);
    }
    if ((npad + nmorsels) % 4 != 0) {
        pc_buf_fini(&tmp);
        return pc_fail(err, PC_ERR_FORMAT, "Invalid padding");
    }
    tail_bytes = nmorsels * 6 / 8;
    num = (uint32_t)morsels[0] << 26 | (uint32_t)morsels[1] << 20 | (uint32_t)morsels[2] << 14 |
          (uint32_t)morsels[3] << 8;
    if ((num & (UINT32_MAX >> (tail_bytes * 8))) != 0) {
        pc_buf_fini(&tmp);
        return pc_fail(err, PC_ERR_FORMAT, "Invalid last symbol %u, offset %zu.", (unsigned)last_symbol,
                       quads + nmorsels - 1);
    }
    for (i = 0; i < tail_bytes; i++) {
        tmp.data[tmp.len++] = (uint8_t)(num >> 24);
        num <<= 8;
    }
    tmp.data[tmp.len] = '\0';
    pc_buf_fini(out);
    *out = tmp;
    return PC_OK;
}
