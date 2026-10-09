#include "utf8.h"

bool pc_unicode_is_space(uint32_t c)
{
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

/* Length of the valid prefix of a sequence starting at s[0] (Unicode "maximal subpart"), and
 * whether it is a complete scalar value. */
static size_t valid_prefix(const uint8_t *s, size_t len, bool *complete, uint32_t *cp)
{
    uint8_t b = s[0];
    size_t need;
    uint8_t lo = 0x80;
    uint8_t hi = 0xBF;
    uint32_t c;
    size_t k;
    *complete = false;
    if (b < 0x80) {
        *complete = true;
        *cp = b;
        return 1;
    }
    if (b >= 0xC2 && b <= 0xDF) {
        need = 1;
        c = b & 0x1Fu;
    } else if (b >= 0xE0 && b <= 0xEF) {
        need = 2;
        c = b & 0x0Fu;
        if (b == 0xE0) { lo = 0xA0; }
        if (b == 0xED) { hi = 0x9F; }
    } else if (b >= 0xF0 && b <= 0xF4) {
        need = 3;
        c = b & 0x07u;
        if (b == 0xF0) { lo = 0x90; }
        if (b == 0xF4) { hi = 0x8F; }
    } else {
        return 0; /* not a lead byte */
    }
    for (k = 1; k <= need; k++) {
        uint8_t cc;
        if (k >= len) {
            return k; /* truncated */
        }
        cc = s[k];
        if (k == 1 ? (cc < lo || cc > hi) : (cc < 0x80 || cc > 0xBF)) {
            return k;
        }
        c = c << 6 | (cc & 0x3Fu);
    }
    *complete = true;
    *cp = c;
    return need + 1;
}

bool pc_utf8_next(const uint8_t *s, size_t len, size_t *i, uint32_t *cp)
{
    bool complete;
    uint32_t c = 0;
    size_t n;
    if (*i >= len) {
        return false;
    }
    n = valid_prefix(s + *i, len - *i, &complete, &c);
    if (!complete) {
        return false;
    }
    *i += n;
    *cp = c;
    return true;
}

pc_status pc_utf8_put(pc_buf *out, uint32_t cp, pc_error *err)
{
    uint8_t t[4];
    size_t n;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        cp = 0xFFFD;
    }
    if (cp < 0x80) {
        t[0] = (uint8_t)cp;
        n = 1;
    } else if (cp < 0x800) {
        t[0] = (uint8_t)(0xC0 | cp >> 6);
        t[1] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        t[0] = (uint8_t)(0xE0 | cp >> 12);
        t[1] = (uint8_t)(0x80 | (cp >> 6 & 0x3F));
        t[2] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        t[0] = (uint8_t)(0xF0 | cp >> 18);
        t[1] = (uint8_t)(0x80 | (cp >> 12 & 0x3F));
        t[2] = (uint8_t)(0x80 | (cp >> 6 & 0x3F));
        t[3] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 4;
    }
    return pc_buf_append(out, t, n, err);
}

pc_status pc_utf8_lossy(const uint8_t *bytes, size_t len, pc_buf *out, pc_error *err)
{
    size_t i = 0;
    size_t start = out->len;
    pc_status s = pc_buf_reserve(out, len, err);
    while (s == PC_OK && i < len) {
        bool complete;
        uint32_t cp = 0;
        size_t n = valid_prefix(bytes + i, len - i, &complete, &cp);
        if (complete) {
            s = pc_buf_append(out, bytes + i, n, err);
            i += n;
        } else {
            s = pc_utf8_put(out, 0xFFFD, err);
            i += n == 0 ? 1 : n;
        }
    }
    if (s != PC_OK) {
        pc_buf_truncate(out, start);
    }
    return s;
}

pc_status pc_utf16be_lossy(const uint8_t *be, size_t n, pc_buf *out, pc_error *err)
{
    size_t i = 0;
    size_t start = out->len;
    pc_status s = PC_OK;
    while (s == PC_OK && i < n) {
        uint32_t u = (uint32_t)be[2 * i] << 8 | be[2 * i + 1];
        uint32_t cp = u;
        i++;
        if (u >= 0xD800 && u <= 0xDBFF) {
            uint32_t lo = i < n ? ((uint32_t)be[2 * i] << 8 | be[2 * i + 1]) : 0;
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            } else {
                cp = 0xFFFD;
            }
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            cp = 0xFFFD;
        }
        s = pc_utf8_put(out, cp, err);
    }
    if (s != PC_OK) {
        pc_buf_truncate(out, start);
    }
    return s;
}

size_t pc_utf8_trim(const char *s, size_t len, size_t *start)
{
    const uint8_t *u = (const uint8_t *)s;
    size_t a = 0;
    size_t b = len;
    /* Leading white space. */
    while (a < len) {
        size_t j = a;
        uint32_t cp;
        if (!pc_utf8_next(u, len, &j, &cp) || !pc_unicode_is_space(cp)) {
            break;
        }
        a = j;
    }
    /* Trailing: step back over one scalar at a time. */
    while (b > a) {
        size_t j = b - 1;
        size_t k;
        uint32_t cp;
        while (j > a && (u[j] & 0xC0u) == 0x80u) {
            j--;
        }
        k = j;
        if (!pc_utf8_next(u, b, &k, &cp) || k != b || !pc_unicode_is_space(cp)) {
            break;
        }
        b = j;
    }
    *start = a;
    return b - a;
}
