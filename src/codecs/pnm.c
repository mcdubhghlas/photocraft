/* Netpbm: P1-P6 (PBM/PGM/PPM, ASCII and binary), P7 (PAM incl. CMYK/CMYK_ALPHA) and PFM
 * (PF colour / Pf gray float) (port of crates/codecs/src/codecs/pnm.rs). Writing picks the
 * subtype: float -> PFM (gray/RGB), gray -> P5, RGB -> P6, alpha or CMYK -> P7. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "codecs/internal.h"

#define F PC_CF_PNM

typedef struct rd {
    const uint8_t *b;
    size_t n;
    size_t pos;
} rd;

static bool ws(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

static void skip_ws_and_comments(rd *r)
{
    while (r->pos < r->n) {
        uint8_t c = r->b[r->pos];
        if (c == '#') {
            while (r->pos < r->n && r->b[r->pos] != '\n' && r->b[r->pos] != '\r') {
                r->pos++;
            }
        } else if (ws(c)) {
            r->pos++;
        } else {
            break;
        }
    }
}

static pc_status token(rd *r, const uint8_t **t, size_t *len, pc_error *err)
{
    size_t start;
    skip_ws_and_comments(r);
    start = r->pos;
    while (r->pos < r->n && !ws(r->b[r->pos]) && r->b[r->pos] != '#') {
        r->pos++;
    }
    if (start == r->pos) {
        return cx_malformed(err, F, "unexpected end of header");
    }
    *t = r->b + start;
    *len = r->pos - start;
    return PC_OK;
}

/* Rust's u32::from_str: optional '+', then decimal digits. */
static bool parse_u32(const uint8_t *t, size_t n, uint32_t *out)
{
    size_t i = 0;
    uint64_t v = 0;
    if (n > 0 && t[0] == '+') {
        i = 1;
    }
    if (i == n) {
        return false;
    }
    for (; i < n; i++) {
        if (t[i] < '0' || t[i] > '9') {
            return false;
        }
        v = v * 10 + (uint64_t)(t[i] - '0');
        if (v > UINT32_MAX) {
            return false;
        }
    }
    *out = (uint32_t)v;
    return true;
}

static pc_status uint_tok(rd *r, uint32_t *v, pc_error *err)
{
    const uint8_t *t;
    size_t n;
    pc_status st = token(r, &t, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (!parse_u32(t, n, v)) {
        return cx_malformed(err, F, "invalid integer in header");
    }
    return PC_OK;
}

static bool ieq(const uint8_t *t, size_t n, const char *w)
{
    size_t i;
    if (strlen(w) != n) {
        return false;
    }
    for (i = 0; i < n; i++) {
        uint8_t c = t[i];
        if (c >= 'A' && c <= 'Z') {
            c = (uint8_t)(c - 'A' + 'a');
        }
        if (c != (uint8_t)w[i]) {
            return false;
        }
    }
    return true;
}

/* Rust's f32::from_str grammar: [+-]? (inf | infinity | nan | digits[.digits][e[+-]digits] |
 * .digits[e...]), case-insensitive words; then correctly rounded (strtof). */
static bool parse_f32(const uint8_t *t, size_t n, float *out)
{
    char buf[128];
    size_t i = 0;
    size_t digits = 0;
    if (n == 0 || n >= sizeof buf) {
        return false;
    }
    if (t[0] == '+' || t[0] == '-') {
        i = 1;
    }
    if (ieq(t + i, n - i, "inf") || ieq(t + i, n - i, "infinity") || ieq(t + i, n - i, "nan")) {
        memcpy(buf, t, n);
        buf[n] = '\0';
        *out = strtof(buf, NULL);
        return true;
    }
    while (i < n && t[i] >= '0' && t[i] <= '9') {
        i++;
        digits++;
    }
    if (i < n && t[i] == '.') {
        i++;
        while (i < n && t[i] >= '0' && t[i] <= '9') {
            i++;
            digits++;
        }
    }
    if (digits == 0) {
        return false;
    }
    if (i < n && (t[i] == 'e' || t[i] == 'E')) {
        size_t ed = 0;
        i++;
        if (i < n && (t[i] == '+' || t[i] == '-')) {
            i++;
        }
        while (i < n && t[i] >= '0' && t[i] <= '9') {
            i++;
            ed++;
        }
        if (ed == 0) {
            return false;
        }
    }
    if (i != n) {
        return false;
    }
    memcpy(buf, t, n);
    buf[n] = '\0';
    *out = strtof(buf, NULL);
    return true;
}

static pc_status single_ws(rd *r, pc_error *err)
{
    if (r->pos < r->n && ws(r->b[r->pos])) {
        r->pos++;
        return PC_OK;
    }
    return cx_malformed(err, F, "missing whitespace after header");
}

static pc_status sample_for_maxval(uint32_t m, pc_csample *s, pc_error *err)
{
    *s = PC_CS_U8;
    if (m >= 1 && m <= 255) {
        *s = PC_CS_U8;
    } else if (m >= 256 && m <= 65535) {
        *s = PC_CS_U16;
    } else {
        return cx_malformed(err, F, "invalid maxval %u", (unsigned)m);
    }
    return PC_OK;
}

static pc_status sample_count(uint32_t w, uint32_t h, size_t ch, size_t *n, pc_error *err)
{
    size_t px;
    if (!pc_size_mul(w, h, &px) || !pc_size_mul(px, ch, n)) {
        return cx_limit(err, "%ux%ux%zu samples do not fit in memory", (unsigned)w, (unsigned)h, ch);
    }
    return PC_OK;
}

/* An image from raw integer values in [0, maxval]. Takes ownership of v. */
static pc_status from_values(uint32_t w, uint32_t h, pc_clayout l, uint32_t m, uint32_t *v, size_t n, pc_cimage *out,
                             pc_error *err)
{
    pc_csample s;
    uint8_t *d;
    size_t i;
    pc_status st = sample_for_maxval(m, &s, err);
    if (st != PC_OK) {
        free(v);
        return st;
    }
    for (i = 0; i < n; i++) {
        if (v[i] > m) {
            free(v);
            return cx_malformed(err, F, "sample exceeds maxval");
        }
    }
    d = pc_malloc_array(n > 0 ? n : 1, pc_csample_bytes(s));
    if (d == NULL) {
        free(v);
        return cx_nomem(err);
    }
    for (i = 0; i < n; i++) {
        if (s == PC_CS_U8) {
            d[i] = (uint8_t)(m == 255 ? v[i] : (v[i] * 255 + m / 2) / m);
        } else {
            uint16_t x = (uint16_t)(m == 65535 ? v[i] : ((uint64_t)v[i] * 65535 + m / 2) / m);
            memcpy(d + 2 * i, &x, 2);
        }
    }
    free(v);
    return pc_cimage_from_raw(w, h, l, s, d, n * pc_csample_bytes(s), out, err);
}

static pc_status read_binary(const rd *r, size_t n, uint32_t m, uint32_t **out, pc_error *err)
{
    const uint8_t *data = r->b + r->pos;
    size_t have = r->n - r->pos;
    size_t i;
    uint32_t *v;
    size_t need = n;
    if (m >= 256 && !pc_size_mul(n, 2, &need)) {
        return cx_malformed(err, F, "truncated raster");
    }
    if (have < need) {
        return cx_malformed(err, F, "truncated raster");
    }
    v = pc_malloc_array(n > 0 ? n : 1, sizeof *v);
    if (v == NULL) {
        return cx_nomem(err);
    }
    for (i = 0; i < n; i++) {
        v[i] = m < 256 ? data[i] : (uint32_t)cx_be16(data + 2 * i);
    }
    *out = v;
    return PC_OK;
}

static pc_status decode_pam(rd *r, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    bool hw = false;
    bool hh = false;
    bool hd = false;
    bool hm = false;
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t depth = 0;
    uint32_t m = 0;
    pc_buf tt = PC_BUF_INIT;
    pc_clayout layout;
    pc_csample s;
    size_t n;
    uint32_t *v = NULL;
    pc_status st;
    for (;;) {
        const uint8_t *t;
        size_t tn;
        CK(token(r, &t, &tn, err));
        if (tn == 5 && memcmp(t, "WIDTH", 5) == 0) {
            CK(uint_tok(r, &w, err));
            hw = true;
        } else if (tn == 6 && memcmp(t, "HEIGHT", 6) == 0) {
            CK(uint_tok(r, &h, err));
            hh = true;
        } else if (tn == 5 && memcmp(t, "DEPTH", 5) == 0) {
            CK(uint_tok(r, &depth, err));
            hd = true;
        } else if (tn == 6 && memcmp(t, "MAXVAL", 6) == 0) {
            CK(uint_tok(r, &m, err));
            hm = true;
        } else if (tn == 8 && memcmp(t, "TUPLTYPE", 8) == 0) {
            const uint8_t *u;
            size_t un;
            size_t i = 0;
            uint32_t cp;
            CK(token(r, &u, &un, err));
            while (i < un) {
                if (!pc_utf8_next(u, un, &i, &cp)) {
                    st = cx_malformed(err, F, "bad TUPLTYPE");
                    goto fail;
                }
            }
            if (tt.len > 0) {
                CK(pc_buf_append_byte(&tt, ' ', err));
            }
            CK(pc_buf_append(&tt, u, un, err));
        } else if (tn == 6 && memcmp(t, "ENDHDR", 6) == 0) {
            break;
        } else {
            st = cx_malformed(err, F, "unknown PAM header field");
            goto fail;
        }
    }
    CK(single_ws(r, err));
    if (!(hw && hh && hd && hm)) {
        st = cx_malformed(err, F, "incomplete PAM header");
        goto fail;
    }
    {
        const char *t = tt.len > 0 ? (const char *)tt.data : "";
        bool none = tt.len == 0;
#define IS(x) (tt.len == sizeof(x) - 1 && memcmp(t, x, sizeof(x) - 1) == 0)
        if ((IS("BLACKANDWHITE") || IS("GRAYSCALE") || none) && depth == 1) {
            layout = PC_CL_GRAY;
        } else if ((IS("BLACKANDWHITE_ALPHA") || IS("GRAYSCALE_ALPHA") || none) && depth == 2) {
            layout = PC_CL_GRAYA;
        } else if ((IS("RGB") || none) && depth == 3) {
            layout = PC_CL_RGB;
        } else if ((IS("RGB_ALPHA") || none) && depth == 4) {
            layout = PC_CL_RGBA;
        } else if (IS("CMYK") && depth == 4) {
            layout = PC_CL_CMYK;
        } else if (IS("CMYK_ALPHA") && depth == 5) {
            layout = PC_CL_CMYKA;
        } else {
            char q[128];
            pc_utf8_debug_quote(tt.data, tt.len, q, sizeof q);
            st = cx_unsupported(err, F, "PAM tuple type %s with depth %u", q, (unsigned)depth);
            goto fail;
        }
#undef IS
    }
    CK(sample_for_maxval(m, &s, err));
    CK(pc_climits_check(l, w, h, layout, s, err));
    CK(sample_count(w, h, pc_clayout_channels(layout), &n, err));
    CK(read_binary(r, n, m, &v, err));
    pc_buf_fini(&tt);
    return from_values(w, h, layout, m, v, n, out, err);
fail:
    pc_buf_fini(&tt);
    return st;
}

pc_status cx_pnm_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    rd r = {b, n, 2};
    uint8_t kind;
    pc_status st;
    if (n < 2 || b[0] != 'P') {
        return cx_malformed(err, F, "not a Netpbm file");
    }
    kind = b[1];
    if (kind == '1' || kind == '4') {
        uint32_t w;
        uint32_t h;
        size_t cnt;
        pc_buf px = PC_BUF_INIT;
        if ((st = uint_tok(&r, &w, err)) != PC_OK || (st = uint_tok(&r, &h, err)) != PC_OK ||
            (st = pc_climits_check(l, w, h, PC_CL_GRAY, PC_CS_U8, err)) != PC_OK ||
            (st = sample_count(w, h, 1, &cnt, err)) != PC_OK) {
            return st;
        }
        if (kind == '4') {
            size_t row = ((size_t)w + 7) / 8;
            size_t need;
            size_t x;
            size_t y;
            const uint8_t *data;
            uint8_t *d;
            if ((st = single_ws(&r, err)) != PC_OK) {
                return st;
            }
            data = b + r.pos;
            if (!pc_size_mul(row, h, &need) || n - r.pos < need) {
                return cx_malformed(err, F, "truncated raster");
            }
            d = malloc(cnt > 0 ? cnt : 1);
            if (d == NULL) {
                return cx_nomem(err);
            }
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    uint8_t bit = (uint8_t)((data[y * row + x / 8] >> (7 - x % 8)) & 1);
                    d[y * w + x] = bit == 1 ? 0 : 255;
                }
            }
            return pc_cimage_from_raw(w, h, PC_CL_GRAY, PC_CS_U8, d, cnt, out, err);
        }
        while (px.len < cnt) {
            skip_ws_and_comments(&r);
            if (r.pos >= n) {
                pc_buf_fini(&px);
                return cx_malformed(err, F, "truncated raster");
            }
            if (b[r.pos] != '0' && b[r.pos] != '1') {
                pc_buf_fini(&px);
                return cx_malformed(err, F, "invalid PBM digit");
            }
            if ((st = pc_buf_append_byte(&px, b[r.pos] == '0' ? 255 : 0, err)) != PC_OK) {
                pc_buf_fini(&px);
                return st;
            }
            r.pos++;
        }
        if (px.data == NULL) {
            return pc_cimage_from_copy(w, h, PC_CL_GRAY, PC_CS_U8, "", 0, out, err);
        }
        return pc_cimage_from_raw(w, h, PC_CL_GRAY, PC_CS_U8, px.data, px.len, out, err);
    }
    if (kind == '2' || kind == '3' || kind == '5' || kind == '6') {
        uint32_t w;
        uint32_t h;
        uint32_t m;
        pc_clayout layout = kind == '2' || kind == '5' ? PC_CL_GRAY : PC_CL_RGB;
        pc_csample s;
        size_t cnt;
        uint32_t *v = NULL;
        if ((st = uint_tok(&r, &w, err)) != PC_OK || (st = uint_tok(&r, &h, err)) != PC_OK ||
            (st = uint_tok(&r, &m, err)) != PC_OK || (st = sample_for_maxval(m, &s, err)) != PC_OK ||
            (st = pc_climits_check(l, w, h, layout, s, err)) != PC_OK ||
            (st = sample_count(w, h, pc_clayout_channels(layout), &cnt, err)) != PC_OK) {
            return st;
        }
        if (kind == '5' || kind == '6') {
            if ((st = single_ws(&r, err)) != PC_OK || (st = read_binary(&r, cnt, m, &v, err)) != PC_OK) {
                return st;
            }
        } else {
            /* ASCII: the file, not the header, bounds the storage. */
            size_t k = 0;
            size_t cap = 0;
            while (k < cnt) {
                uint32_t x;
                if ((st = uint_tok(&r, &x, err)) != PC_OK) {
                    free(v);
                    return st;
                }
                if (k == cap) {
                    uint32_t *g = pc_grow_array(v, &cap, k + 1, sizeof *v);
                    if (g == NULL) {
                        free(v);
                        return cx_nomem(err);
                    }
                    v = g;
                }
                v[k++] = x;
            }
        }
        return from_values(w, h, layout, m, v, cnt, out, err);
    }
    if (kind == '7') {
        return decode_pam(&r, l, out, err);
    }
    if (kind == 'F' || kind == 'f') {
        pc_clayout layout = kind == 'F' ? PC_CL_RGB : PC_CL_GRAY;
        uint32_t w;
        uint32_t h;
        const uint8_t *t;
        size_t tn;
        float scale;
        size_t nc = pc_clayout_channels(layout);
        size_t cnt;
        size_t need;
        size_t row;
        size_t y;
        size_t i;
        const uint8_t *data;
        float *o;
        bool le;
        if ((st = uint_tok(&r, &w, err)) != PC_OK || (st = uint_tok(&r, &h, err)) != PC_OK ||
            (st = token(&r, &t, &tn, err)) != PC_OK) {
            return st;
        }
        if (!parse_f32(t, tn, &scale) || !isfinite(scale) || scale == 0.0f) {
            return cx_malformed(err, F, "invalid PFM scale");
        }
        if ((st = single_ws(&r, err)) != PC_OK || (st = pc_climits_check(l, w, h, layout, PC_CS_F32, err)) != PC_OK ||
            (st = sample_count(w, h, nc, &cnt, err)) != PC_OK) {
            return st;
        }
        data = b + r.pos;
        if (!pc_size_mul(cnt, 4, &need) || n - r.pos < need) {
            return cx_malformed(err, F, "truncated raster");
        }
        row = (size_t)w * nc;
        le = scale < 0.0f;
        o = pc_malloc_array(cnt > 0 ? cnt : 1, sizeof *o);
        if (o == NULL) {
            return cx_nomem(err);
        }
        for (y = 0; y < h; y++) {
            /* PFM rows are stored bottom-to-top. */
            size_t dst = (h - 1 - y) * row;
            for (i = 0; i < row; i++) {
                const uint8_t *p = data + (y * row + i) * 4;
                uint32_t bits = le ? cx_le32(p) : cx_be32(p);
                memcpy(&o[dst + i], &bits, 4);
            }
        }
        return pc_cimage_from_raw(w, h, layout, PC_CS_F32, (uint8_t *)o, cnt * 4, out, err);
    }
    return cx_malformed(err, F, "unknown Netpbm subtype");
}

pc_status cx_pnm_encode(const pc_cimage *src, cx_plan p, const pc_cencode_opts *o, pc_buf *out, pc_error *err)
{
    pc_cimage tmp;
    const pc_cimage *img;
    cx_w w = {out, PC_OK, err};
    char hdr[256];
    pc_status st = pc_cimage_converted(src, p.layout, p.sample, &tmp, &img, err);
    (void)o;
    if (st != PC_OK) {
        return st;
    }
    if (img->sample == PC_CS_F32) {
        const char *tag;
        size_t row = (size_t)img->width * pc_clayout_channels(img->layout);
        size_t y;
        size_t i;
        if (img->layout == PC_CL_GRAY) {
            tag = "Pf";
        } else if (img->layout == PC_CL_RGB) {
            tag = "PF";
        } else {
            st = cx_encode(err, F, "PFM cannot store %s", pc_clayout_name(img->layout));
            pc_cimage_fini(&tmp);
            return st;
        }
        (void)snprintf(hdr, sizeof hdr, "%s\n%u %u\n-1.0\n", tag, (unsigned)img->width, (unsigned)img->height);
        cx_w_put(&w, hdr, strlen(hdr));
        for (y = img->height; y-- > 0;) {
            for (i = 0; i < row; i++) {
                uint32_t bits;
                memcpy(&bits, img->data + (y * row + i) * 4, 4);
                cx_w_le32(&w, bits);
            }
        }
    } else if (img->sample == PC_CS_U8 || img->sample == PC_CS_U16) {
        unsigned maxval = img->sample == PC_CS_U8 ? 255 : 65535;
        if (img->layout == PC_CL_GRAY) {
            (void)snprintf(hdr, sizeof hdr, "P5\n%u %u\n%u\n", (unsigned)img->width, (unsigned)img->height, maxval);
        } else if (img->layout == PC_CL_RGB) {
            (void)snprintf(hdr, sizeof hdr, "P6\n%u %u\n%u\n", (unsigned)img->width, (unsigned)img->height, maxval);
        } else {
            const char *t = img->layout == PC_CL_GRAYA ? "GRAYSCALE_ALPHA"
                            : img->layout == PC_CL_RGBA ? "RGB_ALPHA"
                            : img->layout == PC_CL_CMYK ? "CMYK"
                                                        : "CMYK_ALPHA";
            (void)snprintf(hdr, sizeof hdr, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH %zu\nMAXVAL %u\nTUPLTYPE %s\nENDHDR\n",
                           (unsigned)img->width, (unsigned)img->height, pc_clayout_channels(img->layout), maxval, t);
        }
        cx_w_put(&w, hdr, strlen(hdr));
        if (img->sample == PC_CS_U8) {
            cx_w_put(&w, img->data, img->len);
        } else {
            size_t i;
            for (i = 0; i < img->len / 2; i++) {
                cx_w_be16(&w, cx_ne16(img->data, i));
            }
        }
    } else {
        st = cx_encode(err, F, "unsupported sample %s", pc_csample_name(img->sample));
        pc_cimage_fini(&tmp);
        return st;
    }
    pc_cimage_fini(&tmp);
    return w.st;
}
