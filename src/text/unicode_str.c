/* UTF-8 string helpers matching Rust's str methods (see unicode.h). */
#include <string.h>

#include "base/utf8.h"
#include "text/unicode.h"

bool pc_text_is_control(uint32_t cp)
{
    return cp < 0x20 || (cp >= 0x7F && cp < 0xA0);
}

static bool cased(uint32_t cp)
{
    return pc_text_is_lowercase(cp) || pc_text_is_uppercase(cp);
}

/* Next char (U+FFFD for an invalid byte, consuming one byte). */
static uint32_t next_cp(const char *s, size_t len, size_t *i)
{
    uint32_t cp;
    if (!pc_utf8_next((const uint8_t *)s, len, i, &cp)) {
        (*i)++;
        return 0xFFFD;
    }
    return cp;
}

pc_status pc_text_lower(const char *s, size_t len, pc_buf *out, pc_error *err)
{
    size_t i = 0;
    uint32_t prev = 0;
    bool have_prev = false;
    while (i < len) {
        uint32_t cp = next_cp(s, len, &i);
        pc_status st;
        if (cp == 0x130) {
            st = pc_utf8_put(out, 'i', err);
            if (st == PC_OK) {
                st = pc_utf8_put(out, 0x307, err);
            }
        } else if (cp == 0x3A3) {
            /* Final sigma: after a cased letter and not before one. Rust also skips
             * case-ignorable characters on both sides; we don't (no table), see REWRITE_PLAN §7. */
            bool final = false;
            if (have_prev && cased(prev)) {
                size_t j = i;
                final = j >= len || !cased(next_cp(s, len, &j));
            }
            st = pc_utf8_put(out, final ? 0x3C2 : 0x3C3, err);
        } else {
            st = pc_utf8_put(out, pc_text_to_lower1(cp), err);
        }
        if (st != PC_OK) {
            return st;
        }
        prev = cp;
        have_prev = true;
    }
    return PC_OK;
}

pc_status pc_text_ascii_lower(const char *s, size_t len, pc_buf *out, pc_error *err)
{
    size_t i;
    pc_status st = pc_buf_reserve(out, len, err);
    for (i = 0; i < len && st == PC_OK; i++) {
        unsigned char c = (unsigned char)s[i];
        st = pc_buf_append_byte(out, (uint8_t)(c >= 'A' && c <= 'Z' ? c + 32 : c), err);
    }
    return st;
}

bool pc_text_eq_ignore_ascii_case(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t i;
    if (alen != blen) {
        return false;
    }
    for (i = 0; i < alen; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') {
            x = (unsigned char)(x + 32);
        }
        if (y >= 'A' && y <= 'Z') {
            y = (unsigned char)(y + 32);
        }
        if (x != y) {
            return false;
        }
    }
    return true;
}

bool pc_text_contains(const char *hay, size_t hlen, const char *needle)
{
    size_t n = strlen(needle), i;
    if (n == 0) {
        return true;
    }
    for (i = 0; n <= hlen && i <= hlen - n; i++) {
        if (memcmp(hay + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

bool pc_text_starts_with(const char *s, size_t len, const char *prefix)
{
    size_t n = strlen(prefix);
    return n <= len && memcmp(s, prefix, n) == 0;
}

bool pc_text_ends_with(const char *s, size_t len, const char *suffix)
{
    size_t n = strlen(suffix);
    return n <= len && memcmp(s + len - n, suffix, n) == 0;
}

int pc_text_cmp_lower(const char *a, size_t alen, const char *b, size_t blen)
{
    pc_buf x, y;
    int r;
    size_t n;
    pc_buf_init(&x);
    pc_buf_init(&y);
    if (pc_text_lower(a, alen, &x, NULL) != PC_OK || pc_text_lower(b, blen, &y, NULL) != PC_OK) {
        pc_buf_fini(&x);
        pc_buf_fini(&y);
        n = alen < blen ? alen : blen;
        r = n > 0 ? memcmp(a, b, n) : 0;
        return r != 0 ? r : (alen < blen ? -1 : alen > blen);
    }
    n = x.len < y.len ? x.len : y.len;
    r = n > 0 ? memcmp(x.data, y.data, n) : 0;
    if (r == 0) {
        r = x.len < y.len ? -1 : x.len > y.len;
    }
    pc_buf_fini(&x);
    pc_buf_fini(&y);
    return r;
}
