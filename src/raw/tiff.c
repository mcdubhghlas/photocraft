/* A small, bounds-checked TIFF 6.0 / TIFF-EP structure reader (port of crates/raw/src/tiff.rs).
 * Only what raw decoding needs: typed entry access, the IFD chain, SubIFDs, the EXIF IFD and
 * IFDs embedded elsewhere (maker notes). Every offset is checked; the walk is bounded in IFD and
 * entry count. Entries are decoded from the bytes on demand, so nothing here allocates except
 * the strings. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/mem.h"
#include "base/utf8.h"
#include "raw/internal.h"

size_t rx_type_size(uint16_t typ)
{
    switch (typ) {
    case 1:
    case 2:
    case 6:
    case 7:
        return 1;
    case 3:
    case 8:
        return 2;
    case 4:
    case 9:
    case 11:
    case 13:
        return 4;
    case 5:
    case 10:
    case 12:
        return 8;
    default:
        return 0;
    }
}

static size_t sat_add(size_t a, size_t b) { return a > SIZE_MAX - b ? SIZE_MAX : a + b; }
static size_t sat_mul(size_t a, size_t b) { return a != 0 && b > SIZE_MAX / a ? SIZE_MAX : a * b; }

const uint8_t *rx_bytes(const rx_tiff *t, size_t at, size_t len)
{
    if (at > SIZE_MAX - len || at + len > t->len) {
        return NULL;
    }
    return t->data + at;
}

bool rx_u16_at(const rx_tiff *t, size_t at, uint16_t *v)
{
    const uint8_t *b = rx_bytes(t, at, 2);
    if (b == NULL) {
        return false;
    }
    *v = (uint16_t)(t->le ? (unsigned)(b[0] | b[1] << 8) : (unsigned)(b[0] << 8 | b[1]));
    return true;
}

bool rx_u32_at(const rx_tiff *t, size_t at, uint32_t *v)
{
    const uint8_t *b = rx_bytes(t, at, 4);
    if (b == NULL) {
        return false;
    }
    *v = t->le ? ((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24)
               : ((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | (uint32_t)b[3]);
    return true;
}

static bool u64_at(const rx_tiff *t, size_t at, uint64_t *v)
{
    uint32_t a;
    uint32_t b;
    if (rx_bytes(t, at, 8) == NULL || !rx_u32_at(t, at, &a) || !rx_u32_at(t, at + 4, &b)) {
        return false;
    }
    *v = t->le ? ((uint64_t)b << 32 | a) : ((uint64_t)a << 32 | b);
    return true;
}

bool rx_tiff_new(const uint8_t *data, size_t len, rx_tiff *out)
{
    rx_tiff t;
    uint32_t first;
    if (len < 4) {
        return false;
    }
    if (memcmp(data, "II*\0", 4) == 0 || memcmp(data, "IIRO", 4) == 0 || memcmp(data, "IIRS", 4) == 0 ||
        memcmp(data, "IIU\0", 4) == 0) {
        t.le = true;
    } else if (memcmp(data, "MM\0*", 4) == 0 || memcmp(data, "MMOR", 4) == 0) {
        t.le = false;
    } else {
        return false;
    }
    t.data = data;
    t.len = len;
    t.first_ifd = 0;
    if (!rx_u32_at(&t, 4, &first)) {
        return false;
    }
    t.first_ifd = first;
    *out = t;
    return true;
}

bool rx_ifd_at(const rx_tiff *t, size_t offset, size_t base, rx_ifd *out)
{
    uint16_t n16;
    size_t n;
    size_t i;
    size_t valid = 0;
    if (!rx_u16_at(t, offset, &n16)) {
        return false;
    }
    n = n16;
    if (n == 0 || n > RX_MAX_ENTRIES) {
        return false;
    }
    for (i = 0; i < n; i++) {
        size_t e;
        uint16_t tag;
        uint16_t typ;
        uint32_t count;
        if (offset > SIZE_MAX - (2 + 12 * i)) {
            return false;
        }
        e = offset + 2 + 12 * i;
        if (!rx_u16_at(t, e, &tag) || !rx_u16_at(t, sat_add(e, 2), &typ) || !rx_u32_at(t, sat_add(e, 4), &count)) {
            break;
        }
        if (sat_mul(rx_type_size(typ), count) > 4) {
            uint32_t o;
            if (!rx_u32_at(t, sat_add(e, 8), &o)) {
                return false;
            }
        }
        valid++;
    }
    out->offset = offset;
    out->base = base;
    out->n = valid;
    return true;
}

rx_entry rx_ifd_entry(const rx_tiff *t, const rx_ifd *ifd, size_t i)
{
    rx_entry r;
    size_t e = ifd->offset + 2 + 12 * i;
    uint32_t o = 0;
    r.tag = 0;
    r.typ = 0;
    r.count = 0;
    (void)rx_u16_at(t, e, &r.tag);
    (void)rx_u16_at(t, e + 2, &r.typ);
    (void)rx_u32_at(t, e + 4, &r.count);
    if (sat_mul(rx_type_size(r.typ), r.count) <= 4) {
        r.at = sat_add(e, 8);
    } else {
        (void)rx_u32_at(t, e + 8, &o);
        r.at = sat_add(o, ifd->base);
    }
    return r;
}

bool rx_ifd_get(const rx_tiff *t, const rx_ifd *ifd, uint16_t tag, rx_entry *out)
{
    size_t i;
    for (i = 0; i < ifd->n; i++) {
        uint16_t tg = 0;
        (void)rx_u16_at(t, ifd->offset + 2 + 12 * i, &tg);
        if (tg == tag) {
            *out = rx_ifd_entry(t, ifd, i);
            return true;
        }
    }
    return false;
}

bool rx_ifd_has(const rx_tiff *t, const rx_ifd *ifd, uint16_t tag)
{
    rx_entry e;
    return rx_ifd_get(t, ifd, tag, &e);
}

size_t rx_next_ifd(const rx_tiff *t, const rx_ifd *ifd)
{
    uint32_t v = 0;
    if (!rx_u32_at(t, sat_add(ifd->offset, 2 + 12 * ifd->n), &v)) {
        return 0;
    }
    return v;
}

size_t rx_all_ifds(const rx_tiff *t, rx_ifd out[RX_MAX_IFDS])
{
    /* Every visited IFD pushes at most 1 + 16 + 16 offsets. */
    enum { STACK = 1 + RX_MAX_IFDS * 33 };
    size_t *stack = malloc(STACK * sizeof *stack);
    size_t seen[RX_MAX_IFDS];
    size_t nseen = 0;
    size_t sp = 0;
    size_t nout = 0;
    if (stack == NULL) {
        return 0;
    }
    stack[sp++] = t->first_ifd;
    while (sp > 0) {
        size_t off = stack[--sp];
        size_t k;
        bool dup = false;
        rx_ifd ifd;
        size_t next;
        static const uint16_t SUBS[2] = {RX_SUB_IFDS, RX_EXIF_IFD};
        for (k = 0; k < nseen; k++) {
            dup = dup || seen[k] == off;
        }
        if (off == 0 || dup || nseen >= RX_MAX_IFDS) {
            continue;
        }
        seen[nseen++] = off;
        if (!rx_ifd_at(t, off, 0, &ifd)) {
            continue;
        }
        next = rx_next_ifd(t, &ifd);
        if (next != 0 && sp < STACK) {
            stack[sp++] = next;
        }
        for (k = 0; k < 2; k++) {
            rx_entry e;
            if (rx_ifd_get(t, &ifd, SUBS[k], &e)) {
                uint32_t v[16] = {0};
                size_t n = rx_uints(t, &e, v, 16);
                size_t j;
                if (n > 16) {
                    n = 16;
                }
                /* Reversed so the first SubIFD is visited first. */
                for (j = n; j > 0 && sp < STACK; j--) {
                    stack[sp++] = v[j - 1];
                }
            }
        }
        out[nout++] = ifd;
    }
    free(stack);
    return nout;
}

bool rx_uint(const rx_tiff *t, const rx_entry *e, size_t i, uint32_t *v)
{
    size_t at;
    size_t unit = rx_type_size(e->typ);
    const uint8_t *b;
    uint16_t s;
    uint32_t u;
    if (i >= e->count || (unit != 0 && i > SIZE_MAX / unit) || e->at > SIZE_MAX - i * unit) {
        return false;
    }
    at = e->at + i * unit;
    switch (e->typ) {
    case 1:
    case 7:
        if ((b = rx_bytes(t, at, 1)) == NULL) {
            return false;
        }
        *v = b[0];
        return true;
    case 6:
        if ((b = rx_bytes(t, at, 1)) == NULL || (b[0] & 0x80) != 0) {
            return false;
        }
        *v = b[0];
        return true;
    case 3:
        if (!rx_u16_at(t, at, &s)) {
            return false;
        }
        *v = s;
        return true;
    case 8:
        if (!rx_u16_at(t, at, &s) || (s & 0x8000) != 0) {
            return false;
        }
        *v = s;
        return true;
    case 4:
    case 13:
        return rx_u32_at(t, at, v);
    case 9:
        if (!rx_u32_at(t, at, &u) || (u & 0x80000000u) != 0) {
            return false;
        }
        *v = u;
        return true;
    default:
        return false;
    }
}

bool rx_float(const rx_tiff *t, const rx_entry *e, size_t i, double *v)
{
    size_t at;
    size_t unit = rx_type_size(e->typ);
    const uint8_t *b;
    uint16_t s;
    uint32_t u;
    uint32_t d;
    uint64_t q;
    double r;
    if (i >= e->count || (unit != 0 && i > SIZE_MAX / unit) || e->at > SIZE_MAX - i * unit) {
        return false;
    }
    at = e->at + i * unit;
    switch (e->typ) {
    case 1:
    case 7:
        if ((b = rx_bytes(t, at, 1)) == NULL) {
            return false;
        }
        r = b[0];
        break;
    case 6:
        if ((b = rx_bytes(t, at, 1)) == NULL) {
            return false;
        }
        r = (double)(int8_t)b[0];
        break;
    case 3:
        if (!rx_u16_at(t, at, &s)) {
            return false;
        }
        r = s;
        break;
    case 8:
        if (!rx_u16_at(t, at, &s)) {
            return false;
        }
        r = (double)(int16_t)s;
        break;
    case 4:
    case 13:
        if (!rx_u32_at(t, at, &u)) {
            return false;
        }
        r = u;
        break;
    case 9:
        if (!rx_u32_at(t, at, &u)) {
            return false;
        }
        r = (double)(int32_t)u;
        break;
    case 5:
        if (!rx_u32_at(t, at, &u) || !rx_u32_at(t, sat_add(at, 4), &d) || d == 0) {
            return false;
        }
        r = (double)u / (double)d;
        break;
    case 10:
        if (!rx_u32_at(t, at, &u) || !rx_u32_at(t, sat_add(at, 4), &d) || d == 0) {
            return false;
        }
        r = (double)(int32_t)u / (double)(int32_t)d;
        break;
    case 11: {
        float f;
        if (!rx_u32_at(t, at, &u)) {
            return false;
        }
        memcpy(&f, &u, 4);
        r = (double)f;
        break;
    }
    case 12:
        if (!u64_at(t, at, &q)) {
            return false;
        }
        memcpy(&r, &q, 8);
        break;
    default:
        return false;
    }
    if (!isfinite(r)) {
        return false;
    }
    *v = r;
    return true;
}

size_t rx_uints_n(const rx_tiff *t, const rx_entry *e)
{
    size_t n = e->count < RX_MAX_VALUES ? e->count : RX_MAX_VALUES;
    size_t i;
    for (i = 0; i < n; i++) {
        uint32_t v;
        if (!rx_uint(t, e, i, &v)) {
            return 0;
        }
    }
    return n;
}

size_t rx_floats_n(const rx_tiff *t, const rx_entry *e)
{
    size_t n = e->count < RX_MAX_VALUES ? e->count : RX_MAX_VALUES;
    size_t i;
    for (i = 0; i < n; i++) {
        double v;
        if (!rx_float(t, e, i, &v)) {
            return 0;
        }
    }
    return n;
}

size_t rx_uints(const rx_tiff *t, const rx_entry *e, uint32_t *out, size_t cap)
{
    size_t n = rx_uints_n(t, e);
    size_t i;
    for (i = 0; i < n && i < cap; i++) {
        (void)rx_uint(t, e, i, &out[i]);
    }
    return n;
}

size_t rx_floats(const rx_tiff *t, const rx_entry *e, double *out, size_t cap)
{
    size_t n = rx_floats_n(t, e);
    size_t i;
    for (i = 0; i < n && i < cap; i++) {
        (void)rx_float(t, e, i, &out[i]);
    }
    return n;
}

const uint8_t *rx_raw(const rx_tiff *t, const rx_entry *e, size_t *len)
{
    size_t unit = rx_type_size(e->typ);
    const uint8_t *b;
    if (unit != 0 && e->count > SIZE_MAX / unit) {
        return NULL;
    }
    b = rx_bytes(t, e->at, unit * e->count);
    if (b != NULL) {
        *len = unit * e->count;
    }
    return b;
}

char *rx_ascii(const rx_tiff *t, const rx_entry *e, bool *nomem)
{
    size_t len;
    const uint8_t *b = rx_raw(t, e, &len);
    const uint8_t *z;
    pc_buf s = PC_BUF_INIT;
    size_t start;
    size_t n;
    char *out;
    if (b == NULL) {
        return NULL;
    }
    z = len > 0 ? memchr(b, 0, len) : NULL;
    if (z != NULL) {
        len = (size_t)(z - b);
    }
    if (pc_utf8_lossy(b, len, &s, NULL) != PC_OK) {
        *nomem = true;
        return NULL;
    }
    n = pc_utf8_trim((const char *)s.data, s.len, &start);
    if (n == 0) {
        pc_buf_fini(&s);
        return NULL;
    }
    out = pc_strndup((const char *)s.data + start, n);
    pc_buf_fini(&s);
    if (out == NULL) {
        *nomem = true;
    }
    return out;
}

bool rx_tag_uint(const rx_tiff *t, const rx_ifd *ifd, uint16_t tag, uint32_t *v)
{
    rx_entry e;
    return rx_ifd_get(t, ifd, tag, &e) && rx_uint(t, &e, 0, v);
}

size_t rx_tag_uints(const rx_tiff *t, const rx_ifd *ifd, uint16_t tag, uint32_t *out, size_t cap)
{
    rx_entry e;
    return rx_ifd_get(t, ifd, tag, &e) ? rx_uints(t, &e, out, cap) : 0;
}

size_t rx_tag_floats(const rx_tiff *t, const rx_ifd *ifd, uint16_t tag, double *out, size_t cap)
{
    rx_entry e;
    return rx_ifd_get(t, ifd, tag, &e) ? rx_floats(t, &e, out, cap) : 0;
}

char *rx_tag_ascii(const rx_tiff *t, const rx_ifd *ifd, uint16_t tag, bool *nomem)
{
    rx_entry e;
    return rx_ifd_get(t, ifd, tag, &e) ? rx_ascii(t, &e, nomem) : NULL;
}

char *rx_find_ascii(const rx_tiff *t, const rx_ifd *ifds, size_t n, uint16_t tag, bool *nomem)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char *s = rx_tag_ascii(t, &ifds[i], tag, nomem);
        if (s != NULL || *nomem) {
            return s;
        }
    }
    return NULL;
}

uint16_t rx_orientation(const rx_tiff *t, const rx_ifd *ifd)
{
    uint32_t o;
    if (rx_tag_uint(t, ifd, RX_ORIENTATION, &o) && (uint16_t)o >= 1 && (uint16_t)o <= 8) {
        return (uint16_t)o;
    }
    return 1;
}

/* Rust's Display of a finite f64: the shortest digits that round-trip, never an exponent. */
static void fmt_f64(char *out, size_t cap, double v)
{
    char e[40];
    char digits[24];
    int prec;
    int exp10;
    size_t nd = 0;
    size_t k;
    size_t o = 0;
    bool neg = signbit(v) != 0;
    const char *p;
    if (v == 0.0) {
        (void)snprintf(out, cap, "%s0", neg ? "-" : "");
        return;
    }
    for (prec = 0; prec < 17; prec++) {
        (void)snprintf(e, sizeof e, "%.*e", prec, v);
        if (strtod(e, NULL) == v) {
            break;
        }
    }
    p = e + (neg ? 1 : 0);
    for (; *p != 'e' && *p != '\0'; p++) {
        if (*p >= '0' && *p <= '9' && nd < sizeof digits) {
            digits[nd++] = *p;
        }
    }
    exp10 = *p == 'e' ? atoi(p + 1) : 0;
    while (nd > 1 && digits[nd - 1] == '0') {
        nd--;
    }
#define PUT(c)                                                                                     \
    do {                                                                                           \
        if (o + 1 < cap) {                                                                         \
            out[o++] = (c);                                                                        \
        }                                                                                          \
    } while (0)
    if (neg) {
        PUT('-');
    }
    if (exp10 < 0) {
        PUT('0');
        PUT('.');
        for (k = 1; k < (size_t)-exp10; k++) {
            PUT('0');
        }
        for (k = 0; k < nd; k++) {
            PUT(digits[k]);
        }
    } else {
        size_t ip = (size_t)exp10 + 1;
        for (k = 0; k < ip; k++) {
            PUT(k < nd ? digits[k] : '0');
        }
        if (nd > ip) {
            PUT('.');
            for (k = ip; k < nd; k++) {
                PUT(digits[k]);
            }
        }
    }
#undef PUT
    if (cap > 0) {
        out[o < cap ? o : cap - 1] = '\0';
    }
}

char *rx_dump(const uint8_t *data, size_t len)
{
    rx_tiff t;
    rx_ifd ifds[RX_MAX_IFDS];
    size_t n;
    size_t i;
    pc_buf s = PC_BUF_INIT;
    pc_status st = PC_OK;
    if (!rx_tiff_new(data, len, &t)) {
        return pc_strndup("not a TIFF-structured file", 26);
    }
    n = rx_all_ifds(&t, ifds);
    for (i = 0; i < n && st == PC_OK; i++) {
        size_t k;
        st = pc_buf_appendf(&s, NULL, "IFD @%zu\n", ifds[i].offset);
        for (k = 0; k < ifds[i].n && st == PC_OK; k++) {
            rx_entry e = rx_ifd_entry(&t, &ifds[i], k);
            pc_buf pv = PC_BUF_INIT;
            if (e.typ == 2) {
                bool nomem = false;
                char *a = rx_ascii(&t, &e, &nomem);
                st = pc_buf_append_str(&pv, a != NULL ? a : "", NULL);
                free(a);
            } else {
                size_t m = e.count < 12 ? e.count : 12;
                size_t j;
                for (j = 0; j < m && st == PC_OK; j++) {
                    double f;
                    char num[400];
                    if (rx_float(&t, &e, j, &f)) {
                        fmt_f64(num, sizeof num, f);
                    } else {
                        (void)snprintf(num, sizeof num, "?");
                    }
                    st = pc_buf_appendf(&pv, NULL, "%s%s", j > 0 ? " " : "", num);
                }
                if (st == PC_OK && e.count > m) {
                    st = pc_buf_append_str(&pv, " \xe2\x80\xa6", NULL);
                }
            }
            if (st == PC_OK) {
                st = pc_buf_appendf(&s, NULL, "  %5u (0x%04X) type %2u count %7u @%9zu: %s\n", (unsigned)e.tag,
                                    (unsigned)e.tag, (unsigned)e.typ, (unsigned)e.count, e.at,
                                    pv.len > 0 ? pc_buf_cstr(&pv) : "");
            }
            pc_buf_fini(&pv);
        }
    }
    if (st != PC_OK || pc_buf_append_byte(&s, 0, NULL) != PC_OK) {
        pc_buf_fini(&s);
        return NULL;
    }
    return (char *)pc_buf_take(&s, NULL);
}
