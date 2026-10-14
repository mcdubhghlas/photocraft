/* DPX (SMPTE 268M Digital Picture Exchange): the 1664-byte generic header, the 384-byte industry
 * header, then the image data of the first element. Decode and encode of descriptor 6 (luma),
 * 50 (RGB) or 51 (RGBA), unsigned, unencoded, at 8/10/12/16 bits, both byte orders. Port of
 * crates/codecs/src/codecs/dpx.rs. */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/rfmt.h"
#include "codecs/internal.h"

#define F PC_CF_DPX

enum {
    HEADER_LEN = 2048,
    GENERIC_LEN = 1664,
    INDUSTRY_LEN = 384,
    ELEMENT0 = 780,
    ELEMENT_LEN = 72,
    MAX_ELEMENTS = 8,
    OFF_CREATION_TIME = 136,
    LEN_CREATION_TIME = 24,
    OFF_CREATOR = 160,
    LEN_CREATOR = 100,
    OFF_PROJECT = 260,
    LEN_PROJECT = 200,
    OFF_COPYRIGHT = 460,
    LEN_COPYRIGHT = 200,
    OFF_ORIENTATION = 768,
    OFF_FRAME_POSITION = 1712,
    OFF_FRAME_RATE = 1724,
    OFF_TIME_CODE = 1920
};

#define UNDEF32 0xFFFFFFFFu
#define UNDEF8 0xFFu

/* Text metadata keys of the header fields kept through a DPX -> document -> DPX round trip. */
#define KEY_CREATOR "DPX:Creator"
#define KEY_PROJECT "DPX:Project"
#define KEY_COPYRIGHT "DPX:Copyright"
#define KEY_CREATION_TIME "DPX:CreationTime"
#define KEY_TRANSFER "DPX:Transfer"
#define KEY_COLORIMETRIC "DPX:Colorimetric"
#define KEY_FRAME_POSITION "DPX:FramePosition"
#define KEY_FRAME_RATE "DPX:FrameRate"
#define KEY_TIME_CODE "DPX:TimeCode"

static const char *const KEYS[9] = {KEY_CREATOR,     KEY_PROJECT,        KEY_COPYRIGHT,
                                    KEY_CREATION_TIME, KEY_TRANSFER,     KEY_COLORIMETRIC,
                                    KEY_FRAME_POSITION, KEY_FRAME_RATE,  KEY_TIME_CODE};

bool cx_dpx_is_header_key(const char *key)
{
    size_t i;
    for (i = 0; i < 9; i++) {
        if (strcmp(key, KEYS[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- byte-order reads ---- */

typedef enum { ORDER_BIG, ORDER_LITTLE } order_t;

static bool rd_order(const uint8_t *b, size_t n, order_t *o)
{
    if (n < 4) {
        return false;
    }
    if (memcmp(b, "SDPX", 4) == 0) {
        *o = ORDER_BIG;
        return true;
    }
    if (memcmp(b, "XPDS", 4) == 0) {
        *o = ORDER_LITTLE;
        return true;
    }
    return false;
}

static bool rd_u16(const uint8_t *b, size_t n, order_t o, size_t at, uint16_t *v)
{
    if (n < 2 || at > n - 2) {
        return false;
    }
    *v = o == ORDER_BIG ? cx_be16(b + at) : cx_le16(b + at);
    return true;
}

static bool rd_u32(const uint8_t *b, size_t n, order_t o, size_t at, uint32_t *v)
{
    if (n < 4 || at > n - 4) {
        return false;
    }
    *v = o == ORDER_BIG ? cx_be32(b + at) : cx_le32(b + at);
    return true;
}

static bool rd_byte(const uint8_t *b, size_t n, size_t at, uint8_t *v)
{
    if (at >= n) {
        return false;
    }
    *v = b[at];
    return true;
}

/* A 32-bit field that may be undefined (all ones). */
static bool rd_defined_u32(const uint8_t *b, size_t n, order_t o, size_t at, uint32_t *v)
{
    return rd_u32(b, n, o, at, v) && *v != UNDEF32;
}

/* ---- header string fields ---- */

static bool is_ws(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

/* The printable, trimmed ASCII string at (at, len), written NUL-terminated into out (cap bytes,
 * must be > len); false (and out untouched) when empty, undefined or out of range. */
static bool hdr_string(const uint8_t *b, size_t n, size_t at, size_t len, char *out, size_t *out_len)
{
    size_t end;
    size_t i;
    size_t k = 0;
    size_t start;
    size_t stop;
    if (len == 0 || at > n || n - at < len) {
        return false;
    }
    end = len;
    for (i = 0; i < len; i++) {
        if (b[at + i] == 0) {
            end = i;
            break;
        }
    }
    for (i = 0; i < end; i++) {
        uint8_t c = b[at + i];
        if (c >= 32 && c <= 126) {
            out[k++] = (char)c;
        }
    }
    start = 0;
    stop = k;
    while (start < stop && out[start] == ' ') {
        start++;
    }
    while (stop > start && out[stop - 1] == ' ') {
        stop--;
    }
    if (stop == start) {
        return false;
    }
    if (start > 0) {
        memmove(out, out + start, stop - start);
    }
    out[stop - start] = '\0';
    *out_len = stop - start;
    return true;
}

/* A SMPTE time code (BCD HHMMSSFF) as "HH:MM:SS:FF", or "0x........" when it is not valid BCD. */
static void format_time_code(uint32_t v, char out[16])
{
    unsigned d[8];
    int i;
    bool bcd = true;
    for (i = 0; i < 8; i++) {
        d[i] = (v >> ((7 - i) * 4)) & 0xF;
        if (d[i] > 9) {
            bcd = false;
        }
    }
    if (bcd) {
        (void)snprintf(out, 16, "%u%u:%u%u:%u%u:%u%u", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    } else {
        (void)snprintf(out, 16, "0x%08x", (unsigned)v);
    }
}

/* The inverse of format_time_code; false for anything else. */
static bool parse_time_code(const char *s, uint32_t *out)
{
    size_t len = strlen(s);
    uint32_t acc = 0;
    size_t g;
    if (len > 2 && s[0] == '0' && s[1] == 'x') {
        char *end = NULL;
        unsigned long v;
        if (s[2] == '\0') {
            return false;
        }
        errno = 0;
        v = strtoul(s + 2, &end, 16);
        if (end == NULL || *end != '\0' || errno != 0 || v > UNDEF32) {
            return false;
        }
        /* reject a leading sign or 0x prefix inside (strtoul accepts neither here anyway) */
        for (g = 2; s[g] != '\0'; g++) {
            char c = s[g];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                return false;
            }
        }
        *out = (uint32_t)v;
        return true;
    }
    if (len != 11) {
        return false;
    }
    for (g = 0; g < 4; g++) {
        size_t p = g * 3;
        if (g > 0 && s[p - 1] != ':') {
            return false;
        }
        if (s[p] < '0' || s[p] > '9' || s[p + 1] < '0' || s[p + 1] > '9') {
            return false;
        }
        acc = (acc << 4) | (uint32_t)(s[p] - '0');
        acc = (acc << 4) | (uint32_t)(s[p + 1] - '0');
    }
    *out = acc;
    return true;
}

/* ---- widening / narrowing of 10- and 12-bit code values ---- */

/* round(c * 65535 / max) in exact integer arithmetic. */
static uint16_t widen(uint16_t c, uint64_t max)
{
    uint64_t v = ((uint64_t)c * 2 * 65535 + max) / (2 * max);
    return (uint16_t)(v < 65535 ? v : 65535);
}

/* The inverse of widen: round(v * max / 65535). */
static uint16_t narrow(uint16_t v, uint64_t max)
{
    return (uint16_t)(((uint64_t)v * 2 * max + 65535) / (2 * 65535));
}

/* ---- line geometry and unpacking ---- */

/* Bytes in one stored line of `samples` samples, padded to a 32-bit word; false on overflow. */
static bool line_bytes(size_t samples, uint8_t bits, uint16_t packing, size_t *out)
{
    size_t words;
    if (bits == 10 && (packing == 1 || packing == 2)) {
        words = samples / 3 + (samples % 3 != 0);
    } else if (bits == 12 && (packing == 1 || packing == 2)) {
        words = samples / 2 + (samples % 2 != 0);
    } else {
        size_t total;
        if (samples != 0 && samples > SIZE_MAX / bits) {
            return false;
        }
        total = samples * bits;
        words = total / 32 + (total % 32 != 0);
    }
    if (words > SIZE_MAX / 4) {
        return false;
    }
    *out = words * 4;
    return true;
}

/* Code values of one stored line into out[0..out_len]; the line slice is (line, line_n). */
static pc_status unpack_line(const uint8_t *line, size_t line_n, order_t o, uint8_t bits, uint16_t packing,
                             uint16_t *out, size_t out_len, pc_error *err)
{
    size_t i;
    if (bits == 8) {
        if (line_n < out_len) {
            return cx_malformed(err, F, "truncated image data");
        }
        for (i = 0; i < out_len; i++) {
            out[i] = line[i];
        }
    } else if (bits == 16) {
        for (i = 0; i < out_len; i++) {
            if (!rd_u16(line, line_n, o, i * 2, &out[i])) {
                return cx_malformed(err, F, "truncated image data");
            }
        }
    } else if (bits == 10 && (packing == 1 || packing == 2)) {
        unsigned base = packing == 1 ? 22u : 20u;
        for (i = 0; i < out_len; i++) {
            uint32_t word;
            unsigned shift = base - 10u * (unsigned)(i % 3);
            if (!rd_u32(line, line_n, o, i / 3 * 4, &word)) {
                return cx_malformed(err, F, "truncated image data");
            }
            out[i] = (uint16_t)((word >> shift) & 0x3FF);
        }
    } else if (bits == 12 && (packing == 1 || packing == 2)) {
        unsigned shift = packing == 1 ? 4u : 0u;
        for (i = 0; i < out_len; i++) {
            uint16_t half;
            if (!rd_u16(line, line_n, o, i * 2, &half)) {
                return cx_malformed(err, F, "truncated image data");
            }
            out[i] = (uint16_t)((half >> shift) & 0xFFF);
        }
    } else {
        /* Packing 0: one bit stream through the line's 32-bit words, high bits first. */
        uint64_t mask = ((uint64_t)1 << bits) - 1;
        uint64_t acc = 0;
        unsigned have = 0;
        size_t word = 0;
        for (i = 0; i < out_len; i++) {
            while (have < (unsigned)bits) {
                uint32_t w;
                if (!rd_u32(line, line_n, o, word * 4, &w)) {
                    return cx_malformed(err, F, "truncated image data");
                }
                acc = (acc << 32) | (uint64_t)w;
                word++;
                have += 32;
            }
            have -= (unsigned)bits;
            out[i] = (uint16_t)((acc >> have) & mask);
        }
    }
    return PC_OK;
}

/* ---- parsed header ---- */

typedef struct dpx_header {
    order_t order;
    uint32_t width;
    uint32_t height;
    uint16_t elements;
    bool signed_data;
    uint8_t descriptor;
    uint8_t transfer;
    uint8_t colorimetric;
    uint8_t bits;
    uint16_t packing;
    uint16_t encoding;
    size_t data_offset;
    size_t eol_padding;
} dpx_header;

static pc_status parse(const uint8_t *b, size_t n, dpx_header *h, pc_error *err)
{
    order_t o;
    uint32_t image_offset;
    uint16_t orient;
    uint16_t elements;
    uint32_t width;
    uint32_t height;
    uint32_t sign;
    uint8_t descriptor;
    uint8_t transfer;
    uint8_t colorimetric;
    uint8_t bits;
    uint16_t packing;
    uint16_t encoding;
    uint32_t data_field;
    uint32_t eol_field;
    size_t e = ELEMENT0;
    if (!rd_order(b, n, &o)) {
        return cx_malformed(err, F, "missing SDPX/XPDS magic number");
    }
    if (!rd_u32(b, n, o, 4, &image_offset)) {
        return cx_malformed(err, F, "the header ends before the image data offset");
    }
    if (!rd_u16(b, n, o, OFF_ORIENTATION, &orient)) {
        return cx_malformed(err, F, "the header ends before the orientation");
    }
    if (!rd_u16(b, n, o, OFF_ORIENTATION + 2, &elements)) {
        return cx_malformed(err, F, "the header ends before the element count");
    }
    if (elements < 1 || elements > MAX_ELEMENTS) {
        return cx_malformed(err, F, "invalid number of image elements %u", (unsigned)elements);
    }
    if (!rd_u32(b, n, o, 772, &width)) {
        return cx_malformed(err, F, "the header ends before the line length");
    }
    if (!rd_u32(b, n, o, 776, &height)) {
        return cx_malformed(err, F, "the header ends before the line count");
    }
    if (!rd_u32(b, n, o, e, &sign) || !rd_byte(b, n, e + 20, &descriptor) || !rd_byte(b, n, e + 21, &transfer) ||
        !rd_byte(b, n, e + 22, &colorimetric) || !rd_byte(b, n, e + 23, &bits) ||
        !rd_u16(b, n, o, e + 24, &packing) || !rd_u16(b, n, o, e + 26, &encoding) ||
        !rd_u32(b, n, o, e + 28, &data_field) || !rd_u32(b, n, o, e + 32, &eol_field)) {
        return cx_malformed(err, F, "the header ends before image element 1");
    }
    h->order = o;
    h->width = width;
    h->height = height;
    h->elements = elements;
    h->signed_data = sign == 1;
    h->descriptor = descriptor;
    h->transfer = transfer;
    h->colorimetric = colorimetric;
    h->bits = bits;
    h->packing = packing;
    h->encoding = encoding;
    h->data_offset = (data_field == 0 || data_field == UNDEF32) ? (size_t)image_offset : (size_t)data_field;
    h->eol_padding = eol_field == UNDEF32 ? 0 : (size_t)eol_field;
    return PC_OK;
}

uint16_t cx_dpx_orientation(const uint8_t *b, size_t n)
{
    order_t o;
    uint16_t v;
    if (!rd_order(b, n, &o) || !rd_u16(b, n, o, OFF_ORIENTATION, &v)) {
        return 1;
    }
    switch (v) {
    case 1:
        return 2;
    case 2:
        return 4;
    case 3:
        return 3;
    case 4:
        return 5;
    case 5:
        return 6;
    case 6:
        return 8;
    case 7:
        return 7;
    default:
        return 1;
    }
}

/* ---- header text metadata ---- */

static pc_status put_string_field(pc_cmeta *m, const uint8_t *b, size_t n, const char *key, size_t at, size_t len,
                                   pc_error *err)
{
    char buf[256];
    size_t sl;
    if (hdr_string(b, n, at, len, buf, &sl)) {
        return pc_cmeta_add_text(m, key, strlen(key), buf, sl, err);
    }
    return PC_OK;
}

static pc_status header_text(pc_cmeta *m, const uint8_t *b, size_t n, order_t o, const dpx_header *h, pc_error *err)
{
    char num[PC_RUST_F64_TEXT_MAX];
    uint32_t v;
    pc_status st;
    if ((st = put_string_field(m, b, n, KEY_CREATOR, OFF_CREATOR, LEN_CREATOR, err)) != PC_OK ||
        (st = put_string_field(m, b, n, KEY_PROJECT, OFF_PROJECT, LEN_PROJECT, err)) != PC_OK ||
        (st = put_string_field(m, b, n, KEY_COPYRIGHT, OFF_COPYRIGHT, LEN_COPYRIGHT, err)) != PC_OK ||
        (st = put_string_field(m, b, n, KEY_CREATION_TIME, OFF_CREATION_TIME, LEN_CREATION_TIME, err)) != PC_OK) {
        return st;
    }
    if (h->transfer != UNDEF8) {
        (void)snprintf(num, sizeof num, "%u", (unsigned)h->transfer);
        if ((st = pc_cmeta_add_text(m, KEY_TRANSFER, strlen(KEY_TRANSFER), num, strlen(num), err)) != PC_OK) {
            return st;
        }
    }
    if (h->colorimetric != UNDEF8) {
        (void)snprintf(num, sizeof num, "%u", (unsigned)h->colorimetric);
        if ((st = pc_cmeta_add_text(m, KEY_COLORIMETRIC, strlen(KEY_COLORIMETRIC), num, strlen(num), err)) != PC_OK) {
            return st;
        }
    }
    /* The industry header is there when the generic header says so and the file holds it. */
    if (rd_u32(b, n, o, 28, &v) && v != 0 && v != UNDEF32 && n >= HEADER_LEN) {
        if (rd_defined_u32(b, n, o, OFF_FRAME_POSITION, &v)) {
            (void)snprintf(num, sizeof num, "%u", (unsigned)v);
            if ((st = pc_cmeta_add_text(m, KEY_FRAME_POSITION, strlen(KEY_FRAME_POSITION), num, strlen(num), err)) !=
                PC_OK) {
                return st;
            }
        }
        if (rd_defined_u32(b, n, o, OFF_FRAME_RATE, &v)) {
            float fr;
            memcpy(&fr, &v, 4);
            if (isfinite(fr)) {
                /* Rust's f32 Display: the shortest decimal that reads back as fr. */
                char e[64];
                int prec;
                for (prec = 0; prec < 9; prec++) {
                    (void)snprintf(e, sizeof e, "%.*e", prec, (double)fr);
                    if (strtof(e, NULL) == fr) {
                        break;
                    }
                }
                (void)pc_rust_display_f64(strtod(e, NULL), num);
                if ((st = pc_cmeta_add_text(m, KEY_FRAME_RATE, strlen(KEY_FRAME_RATE), num, strlen(num), err)) !=
                    PC_OK) {
                    return st;
                }
            }
        }
        if (rd_defined_u32(b, n, o, OFF_TIME_CODE, &v)) {
            char tc[16];
            format_time_code(v, tc);
            if ((st = pc_cmeta_add_text(m, KEY_TIME_CODE, strlen(KEY_TIME_CODE), tc, strlen(tc), err)) != PC_OK) {
                return st;
            }
        }
    }
    return PC_OK;
}

/* ---- decode ---- */

pc_status cx_dpx_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    dpx_header h = {0}; /* zero-init: parse() fills it, but keep -O1 -Wmaybe-uninitialized quiet */
    pc_status st;
    pc_clayout layout;
    pc_csample sample;
    size_t nc;
    size_t per_line;
    size_t height;
    size_t data_avail;
    const uint8_t *data;
    size_t line_len;
    size_t stride;
    size_t need;
    size_t count;
    uint16_t *codes;
    uint8_t *pixels;
    size_t y;
    bool packing_ok;
    st = parse(b, n, &h, err);
    if (st != PC_OK) {
        return st;
    }
    switch (h.descriptor) {
    case 6:
        layout = PC_CL_GRAY;
        break;
    case 50:
        layout = PC_CL_RGB;
        break;
    case 51:
        layout = PC_CL_RGBA;
        break;
    default:
        return cx_unsupported(err, F, "descriptor %u is not supported (only 6 luma, 50 RGB and 51 RGBA)",
                              (unsigned)h.descriptor);
    }
    if (h.signed_data) {
        return cx_unsupported(err, F, "data sign 1 (signed samples) is not supported");
    }
    if (h.encoding != 0) {
        return cx_unsupported(err, F, "encoding %u (run-length encoded data) is not supported",
                              (unsigned)h.encoding);
    }
    switch (h.bits) {
    case 8:
    case 16:
        packing_ok = h.packing == 0;
        break;
    case 10:
        packing_ok = h.packing <= 2;
        break;
    case 12:
        packing_ok = h.packing <= 1;
        break;
    default:
        return cx_unsupported(err, F, "bit size %u is not supported (only 8, 10, 12 and 16)", (unsigned)h.bits);
    }
    if (!packing_ok) {
        return cx_unsupported(err, F, "packing %u is not supported at %u bits", (unsigned)h.packing,
                              (unsigned)h.bits);
    }
    sample = h.bits == 8 ? PC_CS_U8 : PC_CS_U16;
    st = pc_climits_check(l, h.width, h.height, layout, sample, err);
    if (st != PC_OK) {
        return st;
    }
    nc = pc_clayout_channels(layout);
    if (nc != 0 && (size_t)h.width > SIZE_MAX / nc) {
        return cx_limit(err, "line too long");
    }
    per_line = (size_t)h.width * nc;
    height = h.height;
    if (h.data_offset > n) {
        return cx_malformed(err, F, "the image data offset is past the end of the file");
    }
    data = b + h.data_offset;
    data_avail = n - h.data_offset;
    if (!line_bytes(per_line, h.bits, h.packing, &line_len)) {
        return cx_limit(err, "image too large");
    }
    if (line_len > SIZE_MAX - h.eol_padding) {
        return cx_limit(err, "image too large");
    }
    stride = line_len + h.eol_padding;
    /* Some writers pack 8- and 16-bit lines without padding them to a word; take that layout when
     * the file is too short for padded lines but holds the unpadded ones. */
    if ((h.bits == 8 || h.bits == 16) && h.eol_padding == 0) {
        size_t tight = per_line * (h.bits / 8);
        if (line_len != tight) {
            bool ok = stride == 0 || height - 1 <= SIZE_MAX / stride;
            if (ok) {
                size_t a = stride * (height - 1);
                ok = a <= SIZE_MAX - line_len && (a + line_len) <= data_avail;
            }
            if (!ok) {
                line_len = tight;
                stride = tight;
            }
        }
    }
    if (stride != 0 && height - 1 > SIZE_MAX / stride) {
        return cx_limit(err, "image too large");
    }
    need = stride * (height - 1);
    if (need > SIZE_MAX - line_len) {
        return cx_limit(err, "image too large");
    }
    need += line_len;
    if (need > data_avail) {
        return cx_malformed(err, F, "truncated image data");
    }
    if (per_line != 0 && height > SIZE_MAX / per_line) {
        return cx_limit(err, "image too large");
    }
    count = per_line * height;
    codes = malloc((count > 0 ? count : 1) * sizeof *codes);
    if (codes == NULL) {
        return cx_nomem(err);
    }
    for (y = 0; y < height; y++) {
        size_t start = y * stride;
        st = unpack_line(data + start, data_avail - start, h.order, h.bits, h.packing, codes + y * per_line,
                         per_line, err);
        if (st != PC_OK) {
            free(codes);
            return st;
        }
    }
    if (h.bits == 8) {
        size_t i;
        pixels = malloc(count > 0 ? count : 1);
        if (pixels == NULL) {
            free(codes);
            return cx_nomem(err);
        }
        for (i = 0; i < count; i++) {
            pixels[i] = (uint8_t)codes[i];
        }
        st = pc_cimage_from_raw(h.width, h.height, layout, PC_CS_U8, pixels, count, out, err);
    } else {
        size_t i;
        uint64_t max = ((uint64_t)1 << h.bits) - 1;
        pixels = malloc((count > 0 ? count : 1) * 2);
        if (pixels == NULL) {
            free(codes);
            return cx_nomem(err);
        }
        for (i = 0; i < count; i++) {
            uint16_t v = h.bits == 16 ? codes[i] : widen(codes[i], max);
            memcpy(pixels + i * 2, &v, 2);
        }
        st = pc_cimage_from_raw(h.width, h.height, layout, PC_CS_U16, pixels, count * 2, out, err);
    }
    free(codes);
    if (st != PC_OK) {
        return st;
    }
    if (h.elements > 1) {
        pc_cwarning w;
        memset(&w, 0, sizeof w);
        w.kind = PC_CW_MORE_ELEMENTS;
        w.total = h.elements;
        st = pc_cimage_add_warning(out, w, err);
        if (st != PC_OK) {
            pc_cimage_fini(out);
            return st;
        }
    }
    if (h.transfer != UNDEF8 || h.colorimetric != UNDEF8) {
        pc_cwarning w = pc_cwarning_dpx(h.transfer, h.colorimetric);
        st = pc_cimage_add_warning(out, w, err);
        if (st != PC_OK) {
            pc_cimage_fini(out);
            return st;
        }
    }
    st = header_text(&out->meta, b, n, h.order, &h, err);
    if (st != PC_OK) {
        pc_cimage_fini(out);
        return st;
    }
    return PC_OK;
}

/* ---- encode ---- */

pc_csample cx_dpx_plan_sample(pc_dpx_bits bits, pc_csample src)
{
    switch (bits) {
    case PC_DPX_BITS_8:
        return PC_CS_U8;
    case PC_DPX_BITS_AUTO:
        return src == PC_CS_U8 ? PC_CS_U8 : PC_CS_U16;
    default:
        return PC_CS_U16;
    }
}

/* Bits written for `sample` when the options pick none: 8 for U8, 16 otherwise. */
static uint8_t bits_for(pc_dpx_bits opt, pc_csample sample)
{
    switch (opt) {
    case PC_DPX_BITS_8:
        return 8;
    case PC_DPX_BITS_10:
        return 10;
    case PC_DPX_BITS_12:
        return 12;
    case PC_DPX_BITS_16:
        return 16;
    default:
        return sample == PC_CS_U8 ? 8 : 16;
    }
}

static void w8(uint8_t *h, size_t at, uint8_t v) { h[at] = v; }
static void w16(uint8_t *h, size_t at, uint16_t v)
{
    h[at] = (uint8_t)(v >> 8);
    h[at + 1] = (uint8_t)v;
}
static void w32(uint8_t *h, size_t at, uint32_t v)
{
    h[at] = (uint8_t)(v >> 24);
    h[at + 1] = (uint8_t)(v >> 16);
    h[at + 2] = (uint8_t)(v >> 8);
    h[at + 3] = (uint8_t)v;
}
static void wundef(uint8_t *h, size_t from, size_t to)
{
    if (to > from) {
        memset(h + from, 0xFF, to - from);
    }
}

static size_t utf8_len(uint8_t c)
{
    if (c < 0x80) {
        return 1;
    }
    if ((c >> 5) == 0x6) {
        return 2;
    }
    if ((c >> 4) == 0xE) {
        return 3;
    }
    if ((c >> 3) == 0x1E) {
        return 4;
    }
    return 1;
}

/* An ASCII string field, cut to leave room for a terminating NUL (non-ASCII char -> '?'). */
static void wr_string(uint8_t *h, size_t at, size_t len, const char *s, size_t slen)
{
    size_t cap = len > 0 ? len - 1 : 0;
    size_t w = 0;
    size_t i = 0;
    while (i < slen && w < cap) {
        uint8_t c = (uint8_t)s[i];
        h[at + w++] = (c >= 0x20 && c <= 0x7e) ? c : (uint8_t)'?';
        i += utf8_len(c);
    }
}

/* The trimmed non-empty value of `key` in the image's text metadata (NULL when absent, empty or
 * metadata is not embedded). *slen is the trimmed length. */
static const char *keep_text(const pc_cimage *img, const char *key, bool embed, size_t *slen)
{
    size_t i;
    if (!embed) {
        return NULL;
    }
    for (i = 0; i < img->meta.ntext; i++) {
        if (strcmp(img->meta.text[i].key, key) == 0) {
            const char *v = img->meta.text[i].value;
            size_t a = 0;
            size_t e = img->meta.text[i].value_len;
            while (a < e && is_ws((uint8_t)v[a])) {
                a++;
            }
            while (e > a && is_ws((uint8_t)v[e - 1])) {
                e--;
            }
            if (e == a) {
                return NULL;
            }
            *slen = e - a;
            return v + a;
        }
    }
    return NULL;
}

/* Rust u8::from_str: decimal digits, 0..=255; false otherwise. */
static bool parse_u8(const char *s, size_t slen, uint8_t *out)
{
    unsigned v = 0;
    size_t i;
    if (slen == 0) {
        return false;
    }
    for (i = 0; i < slen; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (unsigned)(s[i] - '0');
        if (v > 255) {
            return false;
        }
    }
    *out = (uint8_t)v;
    return true;
}

/* Rust u32::from_str. */
static bool parse_u32(const char *s, size_t slen, uint32_t *out)
{
    uint64_t v = 0;
    size_t i;
    if (slen == 0) {
        return false;
    }
    for (i = 0; i < slen; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (uint64_t)(s[i] - '0');
        if (v > UNDEF32) {
            return false;
        }
    }
    *out = (uint32_t)v;
    return true;
}

/* Rust f32::from_str, finite only. */
static bool parse_f32_finite(const char *s, size_t slen, float *out)
{
    char buf[64];
    char *end = NULL;
    float v;
    if (slen == 0 || slen >= sizeof buf) {
        return false;
    }
    memcpy(buf, s, slen);
    buf[slen] = '\0';
    errno = 0;
    v = strtof(buf, &end);
    if (end != buf + slen || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

/* The DPX transfer / colorimetric byte from a text field (undefined when absent or invalid). */
static uint8_t code_byte(const pc_cimage *img, const char *key, bool embed)
{
    size_t slen = 0;
    const char *v = keep_text(img, key, embed, &slen);
    uint8_t out;
    if (v != NULL && parse_u8(v, slen, &out)) {
        return out;
    }
    return UNDEF8;
}

/* Appends one line's code values to out, padded to a 32-bit word. */
static pc_status pack_line(cx_w *w, const uint16_t *codes, size_t count, uint8_t bits)
{
    size_t written;
    size_t pad;
    size_t i;
    if (bits == 8) {
        for (i = 0; i < count; i++) {
            cx_w_byte(w, (uint8_t)codes[i]);
        }
        written = count;
    } else if (bits == 10) {
        for (i = 0; i < count; i += 3) {
            uint32_t word = 0;
            size_t j;
            for (j = 0; j < 3 && i + j < count; j++) {
                word |= (uint32_t)(codes[i + j] & 0x3FF) << (22 - 10 * (unsigned)j);
            }
            cx_w_be32(w, word);
        }
        written = (count / 3 + (count % 3 != 0)) * 4;
    } else if (bits == 12) {
        for (i = 0; i < count; i++) {
            cx_w_be16(w, (uint16_t)((codes[i] & 0xFFF) << 4));
        }
        written = count * 2;
    } else {
        for (i = 0; i < count; i++) {
            cx_w_be16(w, codes[i]);
        }
        written = count * 2;
    }
    pad = (written % 4 != 0) ? 4 - written % 4 : 0;
    for (i = 0; i < pad; i++) {
        cx_w_byte(w, 0);
    }
    return w->st;
}

pc_status cx_dpx_encode(const pc_cimage *img, cx_plan p, const pc_cencode_opts *o, pc_buf *out, pc_error *err)
{
    pc_cimage tmp;
    const pc_cimage *src;
    pc_status st;
    uint8_t descriptor;
    uint8_t bits;
    size_t nc;
    size_t count;
    size_t per_line;
    size_t data_len;
    size_t line_len;
    uint16_t *codes;
    uint8_t h[HEADER_LEN];
    size_t e = ELEMENT0;
    bool embed = o->embed_metadata;
    cx_w w;
    uint32_t total;
    size_t i;
    const char *s;
    size_t sl;
    uint32_t pu32;
    float pf32;
    st = pc_cimage_converted(img, p.layout, p.sample, &tmp, &src, err);
    if (st != PC_OK) {
        pc_cimage_fini(&tmp);
        return st;
    }
    switch (src->layout) {
    case PC_CL_GRAY:
        descriptor = 6;
        break;
    case PC_CL_RGB:
        descriptor = 50;
        break;
    case PC_CL_RGBA:
        descriptor = 51;
        break;
    default:
        pc_cimage_fini(&tmp);
        return cx_encode(err, F, "DPX cannot store %s", pc_clayout_name(src->layout));
    }
    bits = bits_for(o->dpx_bits, src->sample);
    nc = pc_clayout_channels(src->layout);
    count = pc_cimage_sample_count(src);
    codes = calloc(count > 0 ? count : 1, sizeof *codes);
    if (codes == NULL) {
        pc_cimage_fini(&tmp);
        return cx_nomem(err);
    }
    if (src->sample == PC_CS_U8 && bits == 8) {
        for (i = 0; i < count; i++) {
            codes[i] = src->data[i];
        }
    } else if (src->sample == PC_CS_U16 && bits == 16) {
        for (i = 0; i < count; i++) {
            codes[i] = cx_ne16(src->data, i);
        }
    } else if (src->sample == PC_CS_U16 && (bits == 10 || bits == 12)) {
        uint64_t max = ((uint64_t)1 << bits) - 1;
        for (i = 0; i < count; i++) {
            codes[i] = narrow(cx_ne16(src->data, i), max);
        }
    } else {
        free(codes);
        st = cx_encode(err, F, "cannot write %s samples at %u bits", pc_csample_name(src->sample), (unsigned)bits);
        pc_cimage_fini(&tmp);
        return st;
    }
    if ((size_t)src->width > (nc != 0 ? SIZE_MAX / nc : SIZE_MAX)) {
        free(codes);
        pc_cimage_fini(&tmp);
        return cx_encode(err, F, "image too large");
    }
    per_line = (size_t)src->width * nc;
    if (per_line == 0 || count != per_line * (size_t)src->height) {
        free(codes);
        pc_cimage_fini(&tmp);
        return cx_encode(err, F, "the image has the wrong number of samples");
    }
    if (!line_bytes(per_line, bits, (bits == 10 || bits == 12) ? 1 : 0, &line_len) ||
        line_len > SIZE_MAX / (size_t)src->height) {
        free(codes);
        pc_cimage_fini(&tmp);
        return cx_encode(err, F, "image too large");
    }
    data_len = line_len * (size_t)src->height;
    if (data_len > (size_t)UNDEF32 - HEADER_LEN) {
        free(codes);
        pc_cimage_fini(&tmp);
        return cx_encode(err, F, "image too large for a DPX file (4 GiB)");
    }
    total = (uint32_t)(HEADER_LEN + data_len);

    memset(h, 0, sizeof h);
    /* Generic file header. */
    memcpy(h, "SDPX", 4);
    w32(h, 4, HEADER_LEN);
    wr_string(h, 8, 8, "V2.0", 4);
    w32(h, 16, total);
    w32(h, 20, 1);
    w32(h, 24, GENERIC_LEN);
    w32(h, 28, INDUSTRY_LEN);
    w32(h, 32, 0);
    s = keep_text(src, KEY_CREATION_TIME, embed, &sl);
    if (s != NULL) {
        wr_string(h, OFF_CREATION_TIME, LEN_CREATION_TIME, s, sl);
    }
    s = keep_text(src, KEY_CREATOR, embed, &sl);
    if (s != NULL) {
        wr_string(h, OFF_CREATOR, LEN_CREATOR, s, sl);
    } else {
        wr_string(h, OFF_CREATOR, LEN_CREATOR, "PhotoCraft", 10);
    }
    s = keep_text(src, KEY_PROJECT, embed, &sl);
    if (s != NULL) {
        wr_string(h, OFF_PROJECT, LEN_PROJECT, s, sl);
    }
    s = keep_text(src, KEY_COPYRIGHT, embed, &sl);
    if (s != NULL) {
        wr_string(h, OFF_COPYRIGHT, LEN_COPYRIGHT, s, sl);
    }
    w32(h, 660, UNDEF32);
    /* Image information: default orientation, one element. */
    w16(h, OFF_ORIENTATION, 0);
    w16(h, OFF_ORIENTATION + 2, 1);
    w32(h, 772, src->width);
    w32(h, 776, src->height);
    w32(h, e, 0);
    w32(h, e + 4, 0);
    w32(h, e + 8, UNDEF32);
    {
        uint32_t maxval = ((uint32_t)1 << bits) - 1;
        w32(h, e + 12, maxval < 65535 ? maxval : 65535);
    }
    w32(h, e + 16, UNDEF32);
    w8(h, e + 20, descriptor);
    w8(h, e + 21, code_byte(src, KEY_TRANSFER, embed));
    w8(h, e + 22, code_byte(src, KEY_COLORIMETRIC, embed));
    w8(h, e + 23, bits);
    w16(h, e + 24, (bits == 10 || bits == 12) ? 1 : 0);
    w16(h, e + 26, 0);
    w32(h, e + 28, HEADER_LEN);
    w32(h, e + 32, 0);
    w32(h, e + 36, 0);
    wundef(h, e + ELEMENT_LEN, e + ELEMENT_LEN * MAX_ELEMENTS);
    wundef(h, 1408, 1432);
    wundef(h, 1620, 1644);
    wundef(h, OFF_FRAME_POSITION, 1732);
    wundef(h, OFF_TIME_CODE, 1972);
    s = keep_text(src, KEY_FRAME_POSITION, embed, &sl);
    if (s != NULL && parse_u32(s, sl, &pu32)) {
        w32(h, OFF_FRAME_POSITION, pu32);
    }
    s = keep_text(src, KEY_FRAME_RATE, embed, &sl);
    if (s != NULL && parse_f32_finite(s, sl, &pf32)) {
        uint32_t bitsf;
        memcpy(&bitsf, &pf32, 4);
        w32(h, OFF_FRAME_RATE, bitsf);
    }
    s = keep_text(src, KEY_TIME_CODE, embed, &sl);
    if (s != NULL) {
        char tc[32];
        uint32_t tcv;
        if (sl < sizeof tc) {
            memcpy(tc, s, sl);
            tc[sl] = '\0';
            if (parse_time_code(tc, &tcv)) {
                w32(h, OFF_TIME_CODE, tcv);
            }
        }
    }

    w.b = out;
    w.st = PC_OK;
    w.err = err;
    cx_w_put(&w, h, HEADER_LEN);
    for (i = 0; w.st == PC_OK && i < (size_t)src->height; i++) {
        st = pack_line(&w, codes + i * per_line, per_line, bits);
        if (st != PC_OK) {
            break;
        }
    }
    free(codes);
    pc_cimage_fini(&tmp);
    return w.st;
}
