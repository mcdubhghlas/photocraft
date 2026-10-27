/* Resolution in EXIF, XMP and Photoshop resources, and the EXIF ColorSpace tag (port of
 * crates/codecs/src/resolution.rs and exif_color.rs; see resolution.h). Implemented from TIFF
 * 6.0 section 8, EXIF 2.3 and the Adobe Photoshop File Formats Specification. */
#include "codecs/resolution.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "codecs/internal.h"
#include "codecs/orientation.h"

#define TAG_X 282
#define TAG_Y 283
#define TAG_UNIT 296
#define TYPE_SHORT 3
#define TYPE_LONG 4
#define TYPE_RATIONAL 5
#define CM_PER_INCH 2.54
#define RESOLUTION_INFO 0x03ED
#define MAX_PPI 1000000.0
#define SAME_PPI 1e-3

/* ---- Rust number parsing ---- */

static bool ieq(const char *s, size_t n, const char *word)
{
    size_t i;
    if (strlen(word) != n) {
        return false;
    }
    for (i = 0; i < n; i++) {
        char c = s[i];
        if ((c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c) != word[i]) {
            return false;
        }
    }
    return true;
}

bool cx_parse_f64(const char *s, size_t n, double *out)
{
    size_t i = 0;
    size_t digits = 0;
    bool neg = false;
    char small[64];
    char *buf;
    double v;
    if (n > 0 && (s[0] == '+' || s[0] == '-')) {
        neg = s[0] == '-';
        i = 1;
    }
    if (ieq(s + i, n - i, "inf") || ieq(s + i, n - i, "infinity")) {
        *out = neg ? -INFINITY : INFINITY;
        return true;
    }
    if (ieq(s + i, n - i, "nan")) {
        *out = NAN;
        return true;
    }
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        i++;
        digits++;
    }
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            i++;
            digits++;
        }
    }
    if (digits == 0) {
        return false;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t e = 0;
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) {
            i++;
        }
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            i++;
            e++;
        }
        if (e == 0) {
            return false;
        }
    }
    if (i != n) {
        return false;
    }
    /* The text is now plain ASCII digits, sign, point and exponent: strtod rounds it correctly. */
    buf = n < sizeof small ? small : malloc(n + 1);
    if (buf == NULL) {
        return false;
    }
    memcpy(buf, s, n);
    buf[n] = '\0';
    v = strtod(buf, NULL);
    if (buf != small) {
        free(buf);
    }
    *out = v;
    return true;
}

bool cx_parse_i64(const char *s, size_t n, int64_t *out)
{
    size_t i = 0;
    bool neg = false;
    uint64_t mag = 0;
    uint64_t lim;
    if (n > 0 && (s[0] == '+' || s[0] == '-')) {
        neg = s[0] == '-';
        i = 1;
    }
    if (i == n) {
        return false;
    }
    lim = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    for (; i < n; i++) {
        unsigned d;
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        d = (unsigned)(s[i] - '0');
        if (mag > (lim - d) / 10) {
            return false;
        }
        mag = mag * 10 + d;
    }
    *out = neg ? (mag == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)mag) : (int64_t)mag;
    return true;
}

/* ---- helpers ---- */

static bool ppi_ok(double v)
{
    return isfinite(v) && v > 0.0 && v <= MAX_PPI;
}

static bool ppi_pair(double x, double y, float *ox, float *oy)
{
    if (!ppi_ok(x) || !ppi_ok(y)) {
        return false;
    }
    *ox = (float)x;
    *oy = (float)y;
    return true;
}

static bool same(float ax, float ay, float bx, float by)
{
    return fabs((double)ax - (double)bx) < SAME_PPI && fabs((double)ay - (double)by) < SAME_PPI;
}

/* ppi as a TIFF RATIONAL: a whole number over 1 when it is one, else over 1000. */
static void rational(float ppi, uint32_t *num, uint32_t *den)
{
    double p = (double)ppi;
    if (fabs(p - round(p)) < SAME_PPI) {
        *num = pc_f64_to_u32(round(p));
        *den = 1;
    } else {
        *num = pc_f64_to_u32(round(p * 1000.0));
        *den = 1000;
    }
}

/* ---- EXIF IFD0 ---- */

typedef struct field {
    size_t entry; /* offset of the entry */
    uint16_t tag;
    uint16_t ty;
    uint64_t count;
    bool has_value; /* the value bytes are of a type we read and lie inside the block */
    size_t value;
} field;

typedef struct ifd {
    cx_tif t;
    cx_ifd0 d;
    field *f; /* every resolution entry, duplicates included */
    size_t nf;
    size_t cap;
} ifd;

/* Where an entry's SHORT or RATIONAL value bytes are: inline when they fit, else at the offset
 * it holds; false for other types or when they leave the block. */
static bool value_at(const cx_tif *t, size_t e, bool big, uint16_t ty, uint64_t count, size_t *at)
{
    uint64_t unit = ty == TYPE_SHORT ? 2 : ty == TYPE_RATIONAL ? 8 : 0;
    uint64_t size;
    size_t field_at;
    uint64_t inline_len = big ? 8 : 4;
    uint64_t off;
    if (unit == 0 || count > UINT64_MAX / unit) {
        return false;
    }
    size = unit * count;
    if (!pc_size_add(e, big ? 12 : 8, &field_at)) {
        return false;
    }
    if (size <= inline_len) {
        off = field_at;
    } else if (!cx_tif_rd(t, field_at, big ? 8 : 4, &off)) {
        return false;
    }
    if (off > SIZE_MAX || size > SIZE_MAX || off + size < off || off + size > t->n) {
        return false;
    }
    *at = (size_t)off;
    return true;
}

static void ifd_fini(ifd *i)
{
    free(i->f);
}

/* IFD0's resolution entries. false when IFD0 does not parse or memory runs out. */
static bool ifd_parse(const uint8_t *b, size_t n, ifd *out)
{
    size_t k;
    memset(out, 0, sizeof *out);
    out->t.b = b;
    out->t.n = n;
    if (!cx_tif_ifd0(&out->t, &out->d)) {
        return false;
    }
    for (k = 0; k < out->d.count; k++) {
        size_t e = out->d.first + k * out->d.entry;
        uint64_t tag;
        uint64_t ty;
        uint64_t count;
        field *g;
        if (!cx_tif_rd(&out->t, e, 2, &tag)) {
            ifd_fini(out);
            return false;
        }
        if (tag != TAG_X && tag != TAG_Y && tag != TAG_UNIT) {
            continue;
        }
        if (!cx_tif_rd(&out->t, e + 2, 2, &ty) || !cx_tif_rd(&out->t, e + 4, out->d.big ? 8 : 4, &count) ||
            (g = pc_grow_array(out->f, &out->cap, out->nf + 1, sizeof *g)) == NULL) {
            ifd_fini(out);
            return false;
        }
        out->f = g;
        g = &out->f[out->nf++];
        g->entry = e;
        g->tag = (uint16_t)tag;
        g->ty = (uint16_t)ty;
        g->count = count;
        g->value = 0;
        g->has_value = value_at(&out->t, e, out->d.big, g->ty, count, &g->value);
    }
    return true;
}

static const field *first_of(ifd *i, uint16_t tag)
{
    size_t k;
    for (k = 0; k < i->nf; k++) {
        if (i->f[k].tag == tag) {
            return &i->f[k];
        }
    }
    return NULL;
}

static bool f_short(const ifd *i, const field *f, uint16_t *v)
{
    uint64_t x;
    if (f->ty != TYPE_SHORT || f->count != 1 || !f->has_value || !cx_tif_rd(&i->t, f->value, 2, &x)) {
        return false;
    }
    *v = (uint16_t)x;
    return true;
}

static bool f_rational(const ifd *i, const field *f, double *v)
{
    uint64_t num;
    uint64_t den;
    if (f->ty != TYPE_RATIONAL || f->count != 1 || !f->has_value || !cx_tif_rd(&i->t, f->value, 4, &num) ||
        !cx_tif_rd(&i->t, f->value + 4, 4, &den) || den == 0) {
        return false;
    }
    *v = (double)num / (double)den;
    return true;
}

static bool resolution_of(ifd *i, float *x, float *y)
{
    const field *u = first_of(i, TAG_UNIT);
    const field *fx;
    const field *fy;
    uint16_t unit = 2;
    double scale;
    double vx;
    double vy;
    if (u != NULL && !f_short(i, u, &unit)) {
        return false;
    }
    if (unit == 2) {
        scale = 1.0;
    } else if (unit == 3) {
        scale = CM_PER_INCH;
    } else {
        return false;
    }
    fx = first_of(i, TAG_X);
    fy = first_of(i, TAG_Y);
    if (fx == NULL || !f_rational(i, fx, &vx) || fy == NULL || !f_rational(i, fy, &vy)) {
        return false;
    }
    return ppi_pair(vx * scale, vy * scale, x, y);
}

bool pc_exif_resolution(const uint8_t *exif, size_t n, float *x, float *y)
{
    size_t off = cx_exif_body(exif, n);
    ifd i;
    bool ok;
    if (!ifd_parse(exif + off, n - off, &i)) {
        return false;
    }
    ok = resolution_of(&i, x, y);
    ifd_fini(&i);
    return ok;
}

static void put(bool le, uint8_t *at, uint64_t v, size_t k)
{
    size_t j;
    for (j = 0; j < k; j++) {
        at[le ? j : k - 1 - j] = (uint8_t)(v >> (8 * j));
    }
}

/* Removes the IFD0 entries flagged in `gone` (parallel to the entry index): the later entries
 * move up, the count and next pointer follow, the freed bytes at the end are zeroed. */
static void remove_entries(uint8_t *b, size_t n, const ifd *i, const bool *gone)
{
    size_t eb = i->d.entry;
    size_t next_bytes = i->d.big ? 8 : 4;
    size_t count_bytes = i->d.big ? 8 : 2;
    size_t table_end = i->d.first + i->d.count * eb;
    size_t len = table_end - i->d.first + next_bytes;
    uint8_t *old;
    size_t k;
    size_t o = 0;
    size_t kept = 0;
    if (table_end + next_bytes > n || i->d.first < count_bytes) {
        return;
    }
    old = malloc(len > 0 ? len : 1);
    if (old == NULL) {
        return;
    }
    memcpy(old, b + i->d.first, len);
    for (k = 0; k < i->d.count; k++) {
        if (!gone[k]) {
            memmove(b + i->d.first + o, old + k * eb, eb);
            o += eb;
            kept++;
        }
    }
    memcpy(b + i->d.first + o, old + i->d.count * eb, next_bytes);
    o += next_bytes;
    memset(b + i->d.first + o, 0, len - o);
    put(i->t.le, b + i->d.first - count_bytes, kept, count_bytes);
    free(old);
}

/* One fresh IFD0 entry (12 bytes classic, 20 BigTIFF): tag, type, count 1 and either an inline
 * SHORT (the resolution unit) or a RATIONAL's offset. Mirrors resolution_entry in the reference. */
static void res_entry(bool le, bool big, uint16_t tag, uint64_t value, uint8_t *out)
{
    size_t value_at = big ? 12 : 8;
    memset(out, 0, big ? 20 : 12);
    put(le, out, tag, 2);
    put(le, out + 2, tag == TAG_UNIT ? TYPE_SHORT : TYPE_RATIONAL, 2);
    if (big) {
        put(le, out + 4, 1, 8);
    } else {
        put(le, out + 4, 1, 4);
    }
    if (tag == TAG_UNIT) {
        put(le, out + value_at, value, 2);
    } else {
        put(le, out + value_at, value, big ? 8 : 4);
    }
}

static size_t align_up2(size_t v, size_t a)
{
    return v + ((a - v % a) % a);
}

typedef struct built_entry {
    uint16_t tag;
    uint8_t bytes[20];
} built_entry;

static int built_cmp(const void *a, const void *b)
{
    uint16_t ta = ((const built_entry *)a)->tag;
    uint16_t tb = ((const built_entry *)b)->tag;
    return ta < tb ? -1 : ta > tb ? 1 : 0;
}

/* Append a replacement IFD0 carrying fresh X/Y resolution and inch unit, keeping every other
 * entry's original bytes (their value offsets stay valid, since the old data is not moved). *out
 * NULL when it cannot be built (left to the caller to treat as unchanged). Port of
 * append_resolution_ifd. */
static pc_status append_resolution_ifd(const ifd *i, const uint8_t *body, size_t bn, float x, float y,
                                       uint8_t **out, size_t *out_len, pc_error *err)
{
    bool big = i->d.big;
    size_t eb = i->d.entry;
    size_t count_bytes = big ? 8 : 2;
    size_t next_bytes = big ? 8 : 4;
    size_t alignment = big ? 8 : 2;
    size_t value_width = big ? 8 : 4;
    built_entry *ents;
    size_t ne = 0;
    size_t count;
    size_t new_ifd;
    size_t table_end;
    size_t next_at;
    size_t rational_at;
    size_t y_rational_at;
    size_t total;
    size_t idx;
    uint32_t xn;
    uint32_t xd;
    uint32_t yn;
    uint32_t yd;
    uint8_t *o;
    size_t w;
    *out = NULL;
    *out_len = 0;
    if (i->d.count > (SIZE_MAX / sizeof *ents) - 3) {
        return PC_OK;
    }
    ents = calloc(i->d.count + 3, sizeof *ents);
    if (ents == NULL) {
        return cx_nomem(err);
    }
    for (idx = 0; idx < i->d.count; idx++) {
        size_t entry;
        uint64_t tag;
        if (!pc_size_mul(idx, eb, &entry) || !pc_size_add(entry, i->d.first, &entry) ||
            entry + eb > bn || !cx_tif_rd(&i->t, entry, 2, &tag)) {
            free(ents);
            return PC_OK;
        }
        if (tag == TAG_X || tag == TAG_Y || tag == TAG_UNIT) {
            continue;
        }
        ents[ne].tag = (uint16_t)tag;
        memcpy(ents[ne].bytes, body + entry, eb);
        ne++;
    }
    count = ne + 3;
    if (!big && count > 0xFFFF) {
        free(ents);
        return PC_OK;
    }
    new_ifd = align_up2(bn, alignment);
    if (!pc_size_mul(count, eb, &table_end) || !pc_size_add(table_end, new_ifd + count_bytes, &table_end) ||
        !pc_size_mul(i->d.count, eb, &next_at) || !pc_size_add(next_at, i->d.first, &next_at) ||
        next_at + next_bytes > bn) {
        free(ents);
        return PC_OK;
    }
    rational_at = align_up2(table_end + next_bytes, big ? 8 : 4);
    y_rational_at = rational_at + 8;
    total = y_rational_at + 8;
    if (!big && (new_ifd > UINT32_MAX || total > UINT32_MAX)) {
        free(ents);
        return PC_OK;
    }
    res_entry(i->t.le, big, TAG_X, rational_at, ents[ne].bytes);
    ents[ne].tag = TAG_X;
    res_entry(i->t.le, big, TAG_Y, y_rational_at, ents[ne + 1].bytes);
    ents[ne + 1].tag = TAG_Y;
    res_entry(i->t.le, big, TAG_UNIT, 2, ents[ne + 2].bytes);
    ents[ne + 2].tag = TAG_UNIT;
    qsort(ents, count, sizeof *ents, built_cmp);
    o = calloc(total > 0 ? total : 1, 1);
    if (o == NULL) {
        free(ents);
        return cx_nomem(err);
    }
    memcpy(o, body, bn);
    w = new_ifd;
    put(i->t.le, o + w, count, count_bytes);
    w += count_bytes;
    for (idx = 0; idx < count; idx++) {
        memcpy(o + w, ents[idx].bytes, eb);
        w += eb;
    }
    memcpy(o + w, body + next_at, next_bytes);
    rational(x, &xn, &xd);
    rational(y, &yn, &yd);
    put(i->t.le, o + rational_at, xn, 4);
    put(i->t.le, o + rational_at + 4, xd, 4);
    put(i->t.le, o + y_rational_at, yn, 4);
    put(i->t.le, o + y_rational_at + 4, yd, 4);
    put(i->t.le, o + (big ? 8 : 4), new_ifd, value_width);
    free(ents);
    *out = o;
    *out_len = total;
    return PC_OK;
}

/* The minimal valid little-endian TIFF block an empty EXIF synthesises from (header + an empty
 * IFD0 and its zero next-IFD pointer), byte-for-byte the reference's b"II*\0\x08\0\0\0\0\0\0\0\0\0\0". */
static const uint8_t MINIMAL_TIFF[15] = {'I', 'I', 0x2A, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static pc_status exif_with_resolution_inner(const uint8_t *exif, size_t n, float x, float y,
                                            bool add_missing, uint8_t **out, size_t *out_len, pc_error *err)
{
    size_t off = cx_exif_body(exif, n);
    const uint8_t *body = exif + off;
    size_t bn = n - off;
    ifd i;
    float rx;
    float ry;
    uint8_t *c;
    bool *gone = NULL;
    bool any_gone = false;
    size_t k;
    *out = NULL;
    *out_len = 0;
    if (!ppi_ok((double)x) || !ppi_ok((double)y)) {
        return PC_OK;
    }
    if (n == 0) {
        if (!add_missing) {
            return PC_OK;
        }
        /* An image with DPI but no source EXIF still needs a valid EXIF block for a format that
         * cannot carry resolution in its own header (CMYK JPEG has no valid JFIF marker). */
        return exif_with_resolution_inner(MINIMAL_TIFF, sizeof MINIMAL_TIFF, x, y, true, out, out_len, err);
    }
    if (!ifd_parse(body, bn, &i)) {
        return PC_OK;
    }
    if (resolution_of(&i, &rx, &ry) && same(rx, ry, x, y)) {
        ifd_fini(&i);
        return PC_OK;
    }
    if (add_missing) {
        const field *fx = first_of(&i, TAG_X);
        const field *fy = first_of(&i, TAG_Y);
        double tmp;
        bool missing = (fx == NULL || !f_rational(&i, fx, &tmp)) || (fy == NULL || !f_rational(&i, fy, &tmp));
        if (missing) {
            uint8_t *nb = NULL;
            size_t nbn = 0;
            pc_status st = append_resolution_ifd(&i, body, bn, x, y, &nb, &nbn, err);
            ifd_fini(&i);
            if (st != PC_OK || nb == NULL) {
                return st;
            }
            c = malloc(off + nbn > 0 ? off + nbn : 1);
            if (c == NULL) {
                free(nb);
                return cx_nomem(err);
            }
            memcpy(c, exif, off);
            memcpy(c + off, nb, nbn);
            free(nb);
            *out = c;
            *out_len = off + nbn;
            return PC_OK;
        }
    }
    if (i.nf == 0) {
        ifd_fini(&i);
        return PC_OK;
    }
    c = malloc(n > 0 ? n : 1);
    gone = calloc(i.d.count > 0 ? i.d.count : 1, sizeof *gone);
    if (c == NULL || gone == NULL) {
        free(c);
        free(gone);
        ifd_fini(&i);
        return cx_nomem(err);
    }
    memcpy(c, exif, n);
    for (k = 0; k < i.nf; k++) {
        const field *f = &i.f[k];
        uint16_t sv;
        double rv;
        uint8_t bytes[8];
        size_t blen;
        if (f->tag == TAG_UNIT && f_short(&i, f, &sv)) {
            put(i.t.le, bytes, 2, 2);
            blen = 2;
        } else if ((f->tag == TAG_X || f->tag == TAG_Y) && f_rational(&i, f, &rv)) {
            uint32_t num;
            uint32_t den;
            rational(f->tag == TAG_X ? x : y, &num, &den);
            put(i.t.le, bytes, num, 4);
            put(i.t.le, bytes + 4, den, 4);
            blen = 8;
        } else {
            gone[(f->entry - i.d.first) / i.d.entry] = true;
            any_gone = true;
            continue;
        }
        /* In range: value_at checked the bytes of the one value lie inside the block. */
        memcpy(c + off + f->value, bytes, blen);
    }
    if (any_gone) {
        remove_entries(c + off, bn, &i, gone);
    }
    free(gone);
    ifd_fini(&i);
    if (memcmp(c, exif, n) == 0) {
        free(c);
        return PC_OK;
    }
    *out = c;
    *out_len = n;
    return PC_OK;
}

pc_status pc_exif_with_resolution(const uint8_t *exif, size_t n, float x, float y, uint8_t **out,
                                  size_t *out_len, pc_error *err)
{
    return exif_with_resolution_inner(exif, n, x, y, false, out, out_len, err);
}

pc_status pc_exif_with_resolution_adding(const uint8_t *exif, size_t n, float x, float y, uint8_t **out,
                                         size_t *out_len, pc_error *err)
{
    return exif_with_resolution_inner(exif, n, x, y, true, out, out_len, err);
}

/* ---- Photoshop resources ---- */

bool pc_photoshop_resolution(const uint8_t *res, size_t n, float *x, float *y)
{
    size_t i = 0;
    while (i < n) {
        size_t name;
        size_t size_at;
        size_t size;
        size_t data;
        uint16_t id;
        if (n - i < 7) {
            return false;
        }
        id = cx_be16(res + i + 4);
        name = ((size_t)res[i + 6] + 2) & ~(size_t)1;
        size_at = i + 6 + name;
        if (size_at > n || n - size_at < 4) {
            return false;
        }
        size = cx_be32(res + size_at);
        data = size_at + 4;
        if (memcmp(res + i, "8BIM", 4) == 0 && id == RESOLUTION_INFO) {
            if (n - data < size || size < 16) {
                return false;
            }
            return ppi_pair((double)cx_be32(res + data) / 65536.0, (double)cx_be32(res + data + 8) / 65536.0,
                            x, y);
        }
        if (n - data < size) {
            /* the next resource would start past the end: the loop condition ends it */
            return false;
        }
        i = data + size + size % 2;
    }
    return false;
}

/* ---- XMP ---- */

#define XMP_X "tiff:XResolution"
#define XMP_Y "tiff:YResolution"
#define XMP_UNIT "tiff:ResolutionUnit"

/* Skips Unicode white space (str::trim_start) from s[i]. */
static size_t skip_ws(const char *s, size_t n, size_t i)
{
    while (i < n) {
        size_t j = i;
        uint32_t cp;
        if (!pc_utf8_next((const uint8_t *)s, n, &j, &cp) || !pc_unicode_is_space(cp)) {
            break;
        }
        i = j;
    }
    return i;
}

static const char *find_mem(const char *s, size_t n, const char *key, size_t kl)
{
    size_t i;
    for (i = 0; kl <= n && i + kl <= n; i++) {
        if (memcmp(s + i, key, kl) == 0) {
            return s + i;
        }
    }
    return NULL;
}

typedef struct range {
    size_t a;
    size_t b;
} range;

/* Calls fn for each value range of property key (key="v", key='v' or <key>v</key>), in order;
 * stops early when fn returns false. */
static void xmp_values(const char *xmp, size_t n, const char *key, bool (*fn)(void *, range), void *ctx)
{
    size_t kl = strlen(key);
    size_t from = 0;
    const char *at;
    while ((at = find_mem(xmp + from, n - from, key, kl)) != NULL) {
        size_t k = (size_t)(at - xmp);
        size_t rest;
        size_t r;
        size_t value;
        char close;
        const char *end;
        range rg;
        char prev;
        from = k + kl;
        /* An attribute or an opening tag of exactly this name (not </key>, not xkey). */
        prev = k > 0 ? xmp[k - 1] : '\0';
        if (!(k > 0 &&
              (prev == '<' || prev == ' ' || prev == '\t' || prev == '\n' || prev == '\f' || prev == '\r'))) {
            continue;
        }
        rest = from;
        r = skip_ws(xmp, n, rest);
        if (r < n && xmp[r] == '=') {
            r = skip_ws(xmp, n, r + 1);
            if (r < n && (xmp[r] == '"' || xmp[r] == '\'')) {
                close = xmp[r];
                value = r + 1;
            } else {
                continue;
            }
        } else if (rest < n && xmp[rest] == '>') {
            value = rest + 1;
            close = '<';
        } else {
            continue;
        }
        end = memchr(xmp + value, close, n - value);
        if (end == NULL) {
            continue;
        }
        rg.a = value;
        rg.b = (size_t)(end - xmp);
        if (!fn(ctx, rg)) {
            return;
        }
    }
}

static bool take_first(void *ctx, range r)
{
    range *out = ctx;
    *out = r;
    return false;
}

static bool first_value(const char *xmp, size_t n, const char *key, range *out)
{
    out->a = SIZE_MAX;
    out->b = 0;
    xmp_values(xmp, n, key, take_first, out);
    return out->a != SIZE_MAX;
}

/* An XMP rational ("300/1") or plain number (Unicode white space around each part is ignored). */
static bool xmp_number(const char *s, size_t n, double *out)
{
    size_t start;
    size_t len = pc_utf8_trim(s, n, &start);
    const char *t = s + start;
    const char *slash = memchr(t, '/', len);
    if (slash != NULL) {
        size_t nl = (size_t)(slash - t);
        size_t ns;
        size_t ds;
        size_t nlen = pc_utf8_trim(t, nl, &ns);
        size_t dlen = pc_utf8_trim(slash + 1, len - nl - 1, &ds);
        double num;
        double den;
        if (!cx_parse_f64(t + ns, nlen, &num) || !cx_parse_f64(slash + 1 + ds, dlen, &den) || den == 0.0) {
            return false;
        }
        *out = num / den;
        return true;
    }
    return cx_parse_f64(t, len, out);
}

bool pc_xmp_resolution(const char *xmp, size_t n, float *x, float *y)
{
    range r;
    double unit = 2.0;
    double scale;
    double vx;
    double vy;
    if (first_value(xmp, n, XMP_UNIT, &r) && !xmp_number(xmp + r.a, r.b - r.a, &unit)) {
        return false;
    }
    if (unit == 2.0) {
        scale = 1.0;
    } else if (unit == 3.0) {
        scale = CM_PER_INCH;
    } else {
        return false;
    }
    if (!first_value(xmp, n, XMP_X, &r) || !xmp_number(xmp + r.a, r.b - r.a, &vx) ||
        !first_value(xmp, n, XMP_Y, &r) || !xmp_number(xmp + r.a, r.b - r.a, &vy)) {
        return false;
    }
    return ppi_pair(vx * scale, vy * scale, x, y);
}

typedef struct edit {
    size_t a;
    size_t b;
    int which; /* 0 x, 1 y, 2 unit */
    size_t seq;
} edit;

typedef struct edits {
    edit *v;
    size_t n;
    size_t cap;
    int which;
    bool oom;
} edits;

static bool add_edit(void *ctx, range r)
{
    edits *e = ctx;
    edit *g = pc_grow_array(e->v, &e->cap, e->n + 1, sizeof *e->v);
    if (g == NULL) {
        e->oom = true;
        return false;
    }
    e->v = g;
    e->v[e->n].a = r.a;
    e->v[e->n].b = r.b;
    e->v[e->n].which = e->which;
    e->v[e->n].seq = e->n;
    e->n++;
    return true;
}

static int edit_cmp(const void *pa, const void *pb)
{
    const edit *a = pa;
    const edit *b = pb;
    if (a->a != b->a) {
        return a->a < b->a ? -1 : 1;
    }
    return a->seq < b->seq ? -1 : a->seq > b->seq;
}

pc_status pc_xmp_with_resolution(const char *xmp, size_t n, float x, float y, char **out, size_t *out_len,
                                 pc_error *err)
{
    static const char *const KEYS[3] = {XMP_X, XMP_Y, XMP_UNIT};
    char text[3][32];
    float rx;
    float ry;
    edits e = {NULL, 0, 0, 0, false};
    pc_buf o = PC_BUF_INIT;
    size_t at = 0;
    size_t k;
    pc_status st = PC_OK;
    *out = NULL;
    *out_len = 0;
    if (!ppi_ok((double)x) || !ppi_ok((double)y) ||
        (pc_xmp_resolution(xmp, n, &rx, &ry) && same(rx, ry, x, y))) {
        return PC_OK;
    }
    for (k = 0; k < 2; k++) {
        uint32_t num;
        uint32_t den;
        rational(k == 0 ? x : y, &num, &den);
        (void)snprintf(text[k], sizeof text[k], "%u/%u", (unsigned)num, (unsigned)den);
    }
    (void)snprintf(text[2], sizeof text[2], "2");
    for (k = 0; k < 3; k++) {
        e.which = (int)k;
        xmp_values(xmp, n, KEYS[k], add_edit, &e);
    }
    if (e.oom) {
        free(e.v);
        return cx_nomem(err);
    }
    if (e.n == 0) {
        return PC_OK;
    }
    qsort(e.v, e.n, sizeof *e.v, edit_cmp);
    for (k = 0; k < e.n && st == PC_OK; k++) {
        if (e.v[k].a < at) {
            continue; /* overlapping ranges only come from malformed markup: keep the first */
        }
        st = pc_buf_append(&o, xmp + at, e.v[k].a - at, err);
        if (st == PC_OK) {
            st = pc_buf_append(&o, text[e.v[k].which], strlen(text[e.v[k].which]), err);
        }
        at = e.v[k].b;
    }
    if (st == PC_OK) {
        st = pc_buf_append(&o, xmp + at, n - at, err);
    }
    if (st == PC_OK) {
        st = pc_buf_append(&o, "", 1, err);
    }
    free(e.v);
    if (st != PC_OK) {
        pc_buf_fini(&o);
        return st;
    }
    if (o.len - 1 == n && memcmp(o.data, xmp, n) == 0) {
        pc_buf_fini(&o);
        return PC_OK;
    }
    *out_len = o.len - 1;
    *out = (char *)o.data;
    return PC_OK;
}

/* ---- export ---- */

/* exif with IFD0's next-IFD pointer (IFD1, the embedded thumbnail) cleared; NULL *out when it
 * has none or does not parse. */
static pc_status without_thumbnail(const uint8_t *exif, size_t n, uint8_t **out, pc_error *err)
{
    size_t off = cx_exif_body(exif, n);
    cx_tif t = {exif + off, n - off, false};
    cx_ifd0 d;
    size_t next;
    size_t width;
    size_t k;
    bool zero = true;
    *out = NULL;
    if (!cx_tif_ifd0(&t, &d)) {
        return PC_OK;
    }
    next = off + d.first + d.count * d.entry;
    width = d.big ? 8 : 4;
    for (k = 0; k < width; k++) {
        zero = zero && exif[next + k] == 0;
    }
    if (zero) {
        return PC_OK;
    }
    *out = malloc(n);
    if (*out == NULL) {
        return cx_nomem(err);
    }
    memcpy(*out, exif, n);
    memset(*out + next, 0, width);
    return PC_OK;
}

static pc_status export_exif_inner(const uint8_t *exif, size_t n, const float *dpi, bool add_missing,
                                   uint8_t **out, size_t *out_len, pc_error *err)
{
    uint8_t *up = NULL;
    size_t un = 0;
    uint8_t *thumb = NULL;
    uint8_t *res = NULL;
    size_t rn = 0;
    const uint8_t *cur;
    size_t cn;
    pc_status st;
    *out = NULL;
    *out_len = 0;
    if ((st = pc_upright_exif(exif, n, &up, &un, err)) != PC_OK) {
        return st;
    }
    cur = up != NULL ? up : exif;
    cn = up != NULL ? un : n;
    if ((st = without_thumbnail(cur, cn, &thumb, err)) != PC_OK) {
        free(up);
        return st;
    }
    if (thumb != NULL) {
        free(up);
        up = thumb;
        un = cn;
        cur = up;
    }
    if (dpi != NULL) {
        st = add_missing ? pc_exif_with_resolution_adding(cur, cn, dpi[0], dpi[1], &res, &rn, err)
                         : pc_exif_with_resolution(cur, cn, dpi[0], dpi[1], &res, &rn, err);
        if (st != PC_OK) {
            free(up);
            return st;
        }
        if (res != NULL) {
            free(up);
            *out = res;
            *out_len = rn;
            return PC_OK;
        }
    }
    *out = up;
    *out_len = up != NULL ? cn : 0;
    return PC_OK;
}

pc_status pc_export_exif(const uint8_t *exif, size_t n, const float *dpi, uint8_t **out, size_t *out_len,
                         pc_error *err)
{
    return export_exif_inner(exif, n, dpi, false, out, out_len, err);
}

pc_status pc_export_exif_adding_missing(const uint8_t *exif, size_t n, const float *dpi, uint8_t **out,
                                        size_t *out_len, pc_error *err)
{
    return export_exif_inner(exif, n, dpi, true, out, out_len, err);
}

pc_status pc_export_xmp(const char *xmp, size_t n, const float *dpi, char **out, size_t *out_len,
                        pc_error *err)
{
    char *up = NULL;
    size_t un = 0;
    char *res = NULL;
    size_t rn = 0;
    pc_status st;
    *out = NULL;
    *out_len = 0;
    if ((st = pc_upright_xmp(xmp, n, &up, &un, err)) != PC_OK) {
        return st;
    }
    if (dpi != NULL) {
        st = pc_xmp_with_resolution(up != NULL ? up : xmp, up != NULL ? un : n, dpi[0], dpi[1], &res, &rn,
                                    err);
        if (st != PC_OK) {
            free(up);
            return st;
        }
        if (res != NULL) {
            free(up);
            *out = res;
            *out_len = rn;
            return PC_OK;
        }
    }
    *out = up;
    *out_len = un;
    return PC_OK;
}

bool cx_resolution_from_metadata(const pc_cmeta *m, float *x, float *y)
{
    if (m->xmp.has && pc_xmp_resolution((const char *)m->xmp.data, m->xmp.len, x, y)) {
        return true;
    }
    return m->exif.has && pc_exif_resolution(m->exif.data, m->exif.len, x, y);
}

/* ---- EXIF ColorSpace ---- */

#define TAG_EXIF_IFD 0x8769
#define TAG_COLOR_SPACE 0xA001
#define TYPE_IFD 13

/* Offset of the first entry tagged tag among count entries from first. */
static bool find_tag(const cx_tif *t, size_t first, size_t count, size_t entry, uint16_t tag, size_t *at)
{
    size_t k;
    for (k = 0; k < count; k++) {
        size_t e;
        uint64_t v;
        if (k > (SIZE_MAX - first) / entry) {
            return false;
        }
        e = first + k * entry;
        if (cx_tif_rd(t, e, 2, &v) && v == tag) {
            *at = e;
            return true;
        }
    }
    return false;
}

bool pc_exif_color_space(const uint8_t *exif, size_t n, uint16_t *out)
{
    size_t off = cx_exif_body(exif, n);
    cx_tif t = {exif + off, n - off, false};
    cx_ifd0 d;
    size_t ptr;
    size_t e;
    size_t first;
    uint64_t v;
    uint64_t ty;
    uint64_t sub;
    uint64_t count;
    if (!cx_tif_ifd0(&t, &d) || d.big || !find_tag(&t, d.first, d.count, d.entry, TAG_EXIF_IFD, &ptr)) {
        return false;
    }
    if (!cx_tif_rd(&t, ptr + 2, 2, &ty) || (ty != TYPE_LONG && ty != TYPE_IFD) ||
        !cx_tif_rd(&t, ptr + 4, 4, &v) || v != 1 || !cx_tif_rd(&t, ptr + 8, 4, &sub) || sub < 8 ||
        sub > SIZE_MAX - 2) {
        return false;
    }
    if (!cx_tif_rd(&t, (size_t)sub, 2, &count)) {
        return false;
    }
    first = (size_t)sub + 2;
    /* The whole table must be present. */
    if (count * 12 > t.n || first > t.n - count * 12) {
        return false;
    }
    if (!find_tag(&t, first, (size_t)count, 12, TAG_COLOR_SPACE, &e) || !cx_tif_rd(&t, e + 4, 4, &v) ||
        v != 1 || !cx_tif_rd(&t, e + 2, 2, &ty)) {
        return false;
    }
    if (ty == TYPE_SHORT && cx_tif_rd(&t, e + 8, 2, &v)) {
        *out = (uint16_t)v;
        return true;
    }
    if (ty == TYPE_LONG && cx_tif_rd(&t, e + 8, 4, &v) && v <= 0xFFFF) {
        *out = (uint16_t)v;
        return true;
    }
    return false;
}
