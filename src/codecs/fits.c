/* FITS (Flexible Image Transport System, FITS Standard 4.0; port of
 * crates/codecs/src/codecs/fits.rs, own code per REWRITE_PLAN.md D24): the image in the
 * primary HDU, or the first IMAGE extension when the primary HDU holds no data.
 *
 * Reading: BITPIX 8, 16, 32, 64, -32, -64 with BSCALE/BZERO and integer BLANK; NAXIS3 = 3 opens
 * as RGB, other plane counts open the first plane as gray with a MorePages warning; rows are
 * bottom-to-top unless ROWORDER = 'TOP-DOWN'. Values that fit (unsigned 8-bit, integers in
 * 0..=65535) stay exact as U8/U16; everything else becomes F32 (up to 2: already normalised;
 * up to 65535: scaled by 65535; else by the maximum); NaN and blank pixels read as 0. Other
 * header cards are kept as text (strings unquoted; COMMENT/HISTORY by their text; HIERARCH by
 * the long name).
 *
 * Writing: gray or RGB, U8 (BITPIX 8), U16 (BITPIX 16, BZERO 32768) or F32 (BITPIX -32),
 * bottom-to-top with ROWORDER = 'BOTTOM-UP', plus the text cards. Tile-compressed files (fpack
 * ZIMAGE tables) are recognised and refused. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "codecs/internal.h"

#define F PC_CF_FITS
#define BLOCK 2880
#define CARD 80
#define MAX_HDUS 1000

/* Keywords that describe the data layout: written from the image, never kept as text. */
static const char *const STRUCTURAL[] = {"SIMPLE",   "BITPIX",  "NAXIS",    "EXTEND",  "BZERO",  "BSCALE",
                                         "BLANK",    "END",     "XTENSION", "PCOUNT",  "GCOUNT", "ROWORDER",
                                         "CHECKSUM", "DATASUM", "DATAMIN",  "DATAMAX", "ZIMAGE", NULL};

enum { V_NONE, V_STR, V_RAW };

typedef struct card {
    char key[CARD + 1]; /* the keyword (HIERARCH: its long name) */
    size_t key_len;
    int kind;           /* V_NONE: commentary (val holds its text) */
    char val[CARD + 1]; /* a string unquoted and unescaped, or a raw value as written */
    size_t val_len;
    bool hierarch;
} card;

typedef struct header {
    card *cards;
    size_t n;
    size_t cap;
    size_t data_start;
} header;

static void header_fini(header *h)
{
    free(h->cards);
    memset(h, 0, sizeof *h);
}

static bool is_ws(char c)
{
    return c == ' ';
} /* the only white space after the ASCII filter */

static void trim(const char *s, size_t n, size_t *a, size_t *b)
{
    size_t i = 0;
    while (i < n && is_ws(s[i])) {
        i++;
    }
    while (n > i && is_ws(s[n - 1])) {
        n--;
    }
    *a = i;
    *b = n;
}

static void set(char *dst, size_t *len, const char *s, size_t n)
{
    memcpy(dst, s, n);
    dst[n] = '\0';
    *len = n;
}

/* A value field: a quoted string ('' escapes a quote; trailing blanks dropped) or a raw value up
 * to its comment, trimmed. */
static void parse_value(const char *f, size_t n, card *c)
{
    size_t i = 0;
    while (i < n && is_ws(f[i])) {
        i++;
    }
    if (i < n && f[i] == '\'') {
        size_t o = 0;
        i++;
        while (i < n) {
            if (f[i] == '\'') {
                if (i + 1 < n && f[i + 1] == '\'') {
                    c->val[o++] = '\'';
                    i += 2;
                    continue;
                }
                break;
            }
            c->val[o++] = f[i++];
        }
        while (o > 0 && is_ws(c->val[o - 1])) {
            o--;
        }
        c->val[o] = '\0';
        c->val_len = o;
        c->kind = V_STR;
    } else {
        const char *slash = memchr(f + i, '/', n - i);
        size_t end = slash != NULL ? (size_t)(slash - f) : n;
        size_t a;
        size_t b;
        trim(f + i, end - i, &a, &b);
        set(c->val, &c->val_len, f + i + a, b - a);
        c->kind = V_RAW;
    }
}

static void parse_card(const uint8_t *raw, card *c)
{
    char s[CARD + 1];
    size_t k;
    size_t a;
    size_t b;
    /* Header bytes are ASCII 32-126 by the standard; anything else is replaced, never trusted. */
    for (k = 0; k < CARD; k++) {
        s[k] = raw[k] >= 32 && raw[k] <= 126 ? (char)raw[k] : '?';
    }
    s[CARD] = '\0';
    memset(c, 0, sizeof *c);
    trim(s, 8, &a, &b);
    set(c->key, &c->key_len, s, b); /* trim_end only */
    if (c->key_len == 8 && memcmp(c->key, "HIERARCH", 8) == 0) {
        const char *eq = memchr(s + 9, '=', CARD - 9);
        if (eq != NULL) {
            size_t kl = (size_t)(eq - (s + 9));
            trim(s + 9, kl, &a, &b);
            set(c->key, &c->key_len, s + 9 + a, b - a);
            parse_value(eq + 1, (size_t)(s + CARD - (eq + 1)), c);
            c->hierarch = true;
            return;
        }
    }
    if (s[8] == '=' && s[9] == ' ') {
        parse_value(s + 10, CARD - 10, c);
        return;
    }
    c->kind = V_NONE;
    k = CARD;
    while (k > 8 && is_ws(s[k - 1])) {
        k--;
    }
    set(c->val, &c->val_len, s + 8, k - 8);
}

static const card *get(const header *h, const char *key)
{
    size_t i;
    size_t kl = strlen(key);
    for (i = 0; i < h->n; i++) {
        const card *c = &h->cards[i];
        if (!c->hierarch && c->key_len == kl && memcmp(c->key, key, kl) == 0) {
            return c->kind == V_NONE ? NULL : c;
        }
    }
    return NULL;
}

static bool hfloat(const header *h, const char *key, double *out)
{
    const card *c = get(h, key);
    char t[CARD + 1];
    size_t i;
    double v;
    if (c == NULL || c->kind != V_RAW) {
        return false;
    }
    /* Fortran-style D exponents are allowed in FITS. */
    for (i = 0; i < c->val_len; i++) {
        t[i] = c->val[i] == 'D' || c->val[i] == 'd' ? 'E' : c->val[i];
    }
    if (!cx_parse_f64(t, c->val_len, &v) || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

static bool hint(const header *h, const char *key, int64_t *out)
{
    const card *c = get(h, key);
    double f;
    if (c == NULL || c->kind != V_RAW) {
        return false;
    }
    if (cx_parse_i64(c->val, c->val_len, out)) {
        return true;
    }
    if (hfloat(h, key, &f) && f == trunc(f) && fabs(f) < 9.0e15) {
        *out = (int64_t)f;
        return true;
    }
    return false;
}

static bool hlogical(const header *h, const char *key)
{
    const card *c = get(h, key);
    return c != NULL && c->kind == V_RAW && c->val_len == 1 && c->val[0] == 'T';
}

/* A string value, trimmed; NULL when absent or not a string. */
static const char *hstring(const header *h, const char *key, size_t *len)
{
    const card *c = get(h, key);
    size_t a;
    size_t b;
    if (c == NULL || c->kind != V_STR) {
        return NULL;
    }
    trim(c->val, c->val_len, &a, &b);
    *len = b - a;
    return c->val + a;
}

static pc_status parse_header(const uint8_t *b, size_t n, size_t start, header *out, pc_error *err)
{
    header h;
    size_t pos = start;
    memset(&h, 0, sizeof h);
    for (;;) {
        const uint8_t *raw;
        card *g;
        size_t k;
        bool end = true;
        if (pos > SIZE_MAX - CARD) {
            header_fini(&h);
            return cx_malformed(err, F, "header too long");
        }
        if (pos + CARD > n) {
            header_fini(&h);
            return cx_malformed(err, F, "header ends before END");
        }
        raw = b + pos;
        pos += CARD;
        if (memcmp(raw, "END", 3) == 0) {
            for (k = 3; k < CARD; k++) {
                end = end && raw[k] == ' ';
            }
            if (end) {
                break;
            }
        }
        g = pc_grow_array(h.cards, &h.cap, h.n + 1, sizeof *g);
        if (g == NULL) {
            header_fini(&h);
            return cx_nomem(err);
        }
        h.cards = g;
        parse_card(raw, &h.cards[h.n++]);
    }
    h.data_start = (pos + BLOCK - 1) / BLOCK * BLOCK;
    if (h.data_start < pos) {
        header_fini(&h);
        return cx_malformed(err, F, "header too long");
    }
    *out = h;
    return PC_OK;
}

/* The axis lengths (NAXIS1, NAXIS2, ...): a new array of *n entries (NULL when 0). */
static pc_status axes(const header *h, uint64_t **ax, size_t *n, pc_error *err)
{
    int64_t na;
    int64_t i;
    uint64_t *v;
    *ax = NULL;
    *n = 0;
    if (!hint(h, "NAXIS", &na)) {
        return cx_malformed(err, F, "missing NAXIS");
    }
    if (na < 0 || na > 999) {
        return cx_malformed(err, F, "invalid NAXIS %lld", (long long)na);
    }
    if (na == 0) {
        return PC_OK;
    }
    v = malloc((size_t)na * sizeof *v);
    if (v == NULL) {
        return cx_nomem(err);
    }
    for (i = 1; i <= na; i++) {
        char key[32];
        int64_t x;
        (void)snprintf(key, sizeof key, "NAXIS%lld", (long long)i);
        if (!hint(h, key, &x) || x < 0) {
            free(v);
            return cx_malformed(err, F, "missing or negative NAXIS%lld", (long long)i);
        }
        v[i - 1] = (uint64_t)x;
    }
    *ax = v;
    *n = (size_t)na;
    return PC_OK;
}

static pc_status naxes(const header *h, size_t *n, pc_error *err)
{
    uint64_t *ax;
    pc_status st = axes(h, &ax, n, err);
    free(ax);
    return st;
}

static uint64_t abs_bits(int64_t bitpix)
{
    return bitpix < 0 ? (uint64_t)0 - (uint64_t)bitpix : (uint64_t)bitpix;
}

/* Bytes in the data unit (before padding to a block). */
static pc_status data_len(const header *h, int64_t bitpix, uint64_t *out, pc_error *err)
{
    uint64_t *ax;
    size_t n;
    size_t i;
    int64_t pcount = 0;
    int64_t gcount = 1;
    uint64_t elems = 1;
    uint64_t bytes = abs_bits(bitpix) / 8;
    pc_status st = axes(h, &ax, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n == 0) {
        *out = 0;
        return PC_OK;
    }
    (void)hint(h, "PCOUNT", &pcount);
    (void)hint(h, "GCOUNT", &gcount);
    if (pcount < 0) {
        free(ax);
        return cx_malformed(err, F, "negative PCOUNT");
    }
    if (gcount < 0) {
        free(ax);
        return cx_malformed(err, F, "negative GCOUNT");
    }
    for (i = 0; i < n; i++) {
        if (ax[i] != 0 && elems > UINT64_MAX / ax[i]) {
            free(ax);
            return cx_malformed(err, F, "data size overflows");
        }
        elems *= ax[i];
    }
    free(ax);
    if (elems > UINT64_MAX - (uint64_t)pcount) {
        return cx_malformed(err, F, "data size overflows");
    }
    elems += (uint64_t)pcount;
    if ((gcount != 0 && elems > UINT64_MAX / (uint64_t)gcount)) {
        return cx_malformed(err, F, "data size overflows");
    }
    elems *= (uint64_t)gcount;
    if (bytes != 0 && elems > UINT64_MAX / bytes) {
        return cx_malformed(err, F, "data size overflows");
    }
    *out = elems * bytes;
    return PC_OK;
}

static int64_t bitpix_or8(const header *h)
{
    int64_t v = 8;
    (void)hint(h, "BITPIX", &v);
    return v;
}

/* The image HDU: the primary one when it has at least two axes, else the first IMAGE
 * extension (*primary then holds the primary header, for its descriptive cards). */
static pc_status find_image(const uint8_t *b, size_t n, header *img, header *primary, bool *has_primary,
                            pc_error *err)
{
    header p;
    size_t na;
    bool compressed = false;
    size_t cur_start;
    uint64_t cur_len;
    size_t k;
    pc_status st = parse_header(b, n, 0, &p, err);
    *has_primary = false;
    if (st != PC_OK) {
        return st;
    }
    if (get(&p, "SIMPLE") == NULL) {
        header_fini(&p);
        return cx_malformed(err, F, "missing SIMPLE");
    }
    if ((st = naxes(&p, &na, err)) != PC_OK) {
        header_fini(&p);
        return st;
    }
    if (na >= 2) {
        *img = p;
        return PC_OK;
    }
    cur_start = p.data_start;
    if ((st = data_len(&p, bitpix_or8(&p), &cur_len, err)) != PC_OK) {
        header_fini(&p);
        return st;
    }
    for (k = 0; k < MAX_HDUS; k++) {
        uint64_t blocks = cur_len / BLOCK + (cur_len % BLOCK != 0);
        uint64_t len = blocks > UINT64_MAX / BLOCK ? UINT64_MAX : blocks * BLOCK;
        size_t next;
        header h;
        const char *kind;
        size_t kl = 0;
        if (len > SIZE_MAX || cur_start > SIZE_MAX - (size_t)len) {
            header_fini(&p);
            return cx_malformed(err, F, "data size overflows");
        }
        next = cur_start + (size_t)len;
        if (next >= n) {
            break;
        }
        if ((st = parse_header(b, n, next, &h, err)) != PC_OK) {
            header_fini(&p);
            return st;
        }
        kind = hstring(&h, "XTENSION", &kl);
        if (kind != NULL && kl == 5 && memcmp(kind, "IMAGE", 5) == 0) {
            if ((st = naxes(&h, &na, err)) != PC_OK) {
                header_fini(&h);
                header_fini(&p);
                return st;
            }
            if (na >= 2) {
                *img = h;
                *primary = p;
                *has_primary = true;
                return PC_OK;
            }
        }
        if (kind != NULL && kl == 8 && memcmp(kind, "BINTABLE", 8) == 0 && hlogical(&h, "ZIMAGE")) {
            compressed = true;
        }
        st = data_len(&h, bitpix_or8(&h), &cur_len, err);
        cur_start = h.data_start;
        header_fini(&h);
        if (st != PC_OK) {
            header_fini(&p);
            return st;
        }
    }
    header_fini(&p);
    if (compressed) {
        return cx_unsupported(
            err, F, "tile-compressed FITS (fpack) is not supported; decompress it with funpack first");
    }
    return cx_malformed(err, F, "the file holds no image");
}

typedef struct phys {
    const uint8_t *data;
    int64_t bitpix;
    size_t bps;
    double bscale;
    double bzero;
    bool has_blank;
    int64_t blank;
} phys;

/* The physical value of sample i (planar order); false for a blank or non-finite float pixel. */
static bool physical(const phys *p, size_t i, double *out)
{
    const uint8_t *s = p->data + i * p->bps;
    int64_t raw;
    double v;
    switch (p->bitpix) {
    case 8:
        raw = s[0];
        break;
    case 16:
        raw = (int16_t)cx_be16(s);
        break;
    case 32:
        raw = (int32_t)cx_be32(s);
        break;
    case 64:
        raw = (int64_t)(((uint64_t)cx_be32(s) << 32) | cx_be32(s + 4));
        break;
    case -32: {
        uint32_t bits = cx_be32(s);
        float f;
        memcpy(&f, &bits, 4);
        v = (double)f * p->bscale + p->bzero;
        if (!isfinite(v)) {
            return false;
        }
        *out = v;
        return true;
    }
    default: {
        uint64_t bits = ((uint64_t)cx_be32(s) << 32) | cx_be32(s + 4);
        double f;
        memcpy(&f, &bits, 8);
        v = f * p->bscale + p->bzero;
        if (!isfinite(v)) {
            return false;
        }
        *out = v;
        return true;
    }
    }
    if (p->has_blank && raw == p->blank) {
        return false;
    }
    *out = (double)raw * p->bscale + p->bzero;
    return true;
}

static bool is_naxis(const char *key, size_t n)
{
    size_t i;
    if (n < 5 || memcmp(key, "NAXIS", 5) != 0) {
        return false;
    }
    for (i = 5; i < n; i++) {
        if (key[i] < '0' || key[i] > '9') {
            return false;
        }
    }
    return true;
}

static bool is_structural(const char *key, size_t n)
{
    size_t i;
    for (i = 0; STRUCTURAL[i] != NULL; i++) {
        if (strlen(STRUCTURAL[i]) == n && memcmp(STRUCTURAL[i], key, n) == 0) {
            return true;
        }
    }
    return false;
}

static pc_status add_cards(const header *h, pc_cmeta *m, pc_error *err)
{
    size_t i;
    pc_status st = PC_OK;
    for (i = 0; i < h->n && st == PC_OK; i++) {
        const card *c = &h->cards[i];
        size_t a;
        size_t b;
        if (!c->hierarch && (is_structural(c->key, c->key_len) || is_naxis(c->key, c->key_len))) {
            continue;
        }
        if (c->kind != V_NONE) {
            st = pc_cmeta_add_text(m, c->key, c->key_len, c->val, c->val_len, err);
            continue;
        }
        trim(c->val, c->val_len, &a, &b);
        if (c->key_len == 0 && a == b) {
            continue;
        }
        st = pc_cmeta_add_text(m, c->key_len == 0 ? "COMMENT" : c->key, c->key_len == 0 ? 7 : c->key_len,
                               c->val + a, b - a, err);
    }
    return st;
}

pc_status cx_fits_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    header h;
    header primary;
    bool has_primary = false;
    int64_t bitpix;
    uint64_t *ax = NULL;
    size_t na = 0;
    uint64_t planes = 1;
    pc_clayout layout;
    size_t nplanes;
    phys p;
    bool integral;
    pc_csample cand;
    size_t plane_len;
    size_t count;
    size_t need;
    uint32_t w;
    uint32_t hgt;
    bool top_down = false;
    size_t i;
    pc_cimage img;
    pc_status st;
    size_t rl = 0;
    const char *ro;
    pc_cimage_init(&img);
    memset(&primary, 0, sizeof primary);
    if ((st = find_image(b, n, &h, &primary, &has_primary, err)) != PC_OK) {
        return st;
    }
    if (!hint(&h, "BITPIX", &bitpix)) {
        st = cx_malformed(err, F, "missing BITPIX");
        goto done;
    }
    if (!(bitpix == 8 || bitpix == 16 || bitpix == 32 || bitpix == 64 || bitpix == -32 || bitpix == -64)) {
        st = cx_malformed(err, F, "invalid BITPIX %lld", (long long)bitpix);
        goto done;
    }
    if ((st = axes(&h, &ax, &na, err)) != PC_OK) {
        goto done;
    }
    if (na < 2) {
        st = cx_malformed(err, F, "the image has fewer than two axes");
        goto done;
    }
    if (ax[0] > UINT32_MAX) {
        st = cx_limit(err, "width %llu", (unsigned long long)ax[0]);
        goto done;
    }
    if (ax[1] > UINT32_MAX) {
        st = cx_limit(err, "height %llu", (unsigned long long)ax[1]);
        goto done;
    }
    w = (uint32_t)ax[0];
    hgt = (uint32_t)ax[1];
    for (i = 2; i < na; i++) {
        if (ax[i] != 0 && planes > UINT64_MAX / ax[i]) {
            st = cx_malformed(err, F, "too many planes");
            goto done;
        }
        planes *= ax[i];
    }
    if (na > 2 && ax[2] == 3 && planes == 3) {
        layout = PC_CL_RGB;
        nplanes = 3;
    } else {
        layout = PC_CL_GRAY;
        nplanes = 1;
    }
    memset(&p, 0, sizeof p);
    p.bitpix = bitpix;
    p.bscale = 1.0;
    p.bzero = 0.0;
    (void)hfloat(&h, "BSCALE", &p.bscale);
    (void)hfloat(&h, "BZERO", &p.bzero);
    p.has_blank = bitpix > 0 && hint(&h, "BLANK", &p.blank);
    integral = p.bscale == 1.0 && p.bzero == trunc(p.bzero);
    if (bitpix == 8 && integral && p.bzero == 0.0) {
        cand = PC_CS_U8;
    } else if ((bitpix == 8 || bitpix == 16) && integral) {
        cand = PC_CS_U16;
    } else {
        cand = PC_CS_F32;
    }
    if ((st = pc_climits_check(l, w, hgt, layout, cand, err)) != PC_OK) {
        goto done;
    }
    p.bps = (size_t)(abs_bits(bitpix) / 8);
    if (!pc_size_mul(w, hgt, &plane_len)) {
        st = cx_limit(err, "%ux%u does not fit in memory", (unsigned)w, (unsigned)hgt);
        goto done;
    }
    if (!pc_size_mul(plane_len, nplanes, &count)) {
        st = cx_limit(err, "image does not fit in memory");
        goto done;
    }
    if (!pc_size_mul(count, p.bps, &need) || !pc_size_add(need, h.data_start, &need)) {
        st = cx_malformed(err, F, "data size overflows");
        goto done;
    }
    if (need > n) {
        st = cx_malformed(err, F, "truncated data");
        goto done;
    }
    p.data = b + h.data_start;
    ro = hstring(&h, "ROWORDER", &rl);
    if (ro != NULL && rl == 8) {
        static const char TD[] = "top-down";
        size_t k;
        top_down = true;
        for (k = 0; k < 8; k++) {
            char c = ro[k] >= 'A' && ro[k] <= 'Z' ? (char)(ro[k] - 'A' + 'a') : ro[k];
            top_down = top_down && c == TD[k];
        }
    }
    {
        /* planar, bottom-up (by default) -> interleaved, top-down */
#define DEST(ii)                                                                                             \
    (((top_down ? ((ii) % plane_len) / w : hgt - 1 - ((ii) % plane_len) / w) * (size_t)w +                   \
      ((ii) % plane_len) % w) *                                                                              \
         nplanes +                                                                                           \
     (ii) / plane_len)
        bool done16 = false;
        if (cand == PC_CS_U8) {
            uint8_t *o = malloc(count);
            if (o == NULL) {
                st = cx_nomem(err);
                goto done;
            }
            for (i = 0; i < count; i++) {
                double v = 0.0;
                (void)physical(&p, i, &v);
                o[DEST(i)] = pc_f64_to_u8(v);
            }
            st = pc_cimage_from_raw(w, hgt, layout, PC_CS_U8, o, count, &img, err);
            done16 = true;
        } else if (cand == PC_CS_U16) {
            uint16_t *o = malloc(count * 2);
            bool fits16 = true;
            if (o == NULL) {
                st = cx_nomem(err);
                goto done;
            }
            for (i = 0; i < count; i++) {
                double v = 0.0;
                (void)physical(&p, i, &v);
                if (!(v >= 0.0 && v <= 65535.0)) {
                    fits16 = false; /* signed data with negative values: fall back to float */
                    break;
                }
                o[DEST(i)] = (uint16_t)v;
            }
            if (fits16) {
                st = pc_cimage_from_raw(w, hgt, layout, PC_CS_U16, (uint8_t *)o, count * 2, &img, err);
                done16 = true;
            } else {
                free(o);
            }
        }
        if (st == PC_OK && !done16) {
            float *o;
            double max = 0.0;
            double white;
            if (!pc_size_mul(count, 4, &need) || (o = malloc(need)) == NULL) {
                st = cx_nomem(err);
                goto done;
            }
            for (i = 0; i < count; i++) {
                double v;
                if (physical(&p, i, &v)) {
                    max = fmax(max, v);
                }
            }
            white = max <= 2.0 ? 1.0 : max <= 65535.0 ? 65535.0 : max;
            for (i = 0; i < count; i++) {
                double v = 0.0;
                (void)physical(&p, i, &v);
                o[DEST(i)] = pc_f64_to_f32(v / white);
            }
            st = pc_cimage_from_raw(w, hgt, layout, PC_CS_F32, (uint8_t *)o, need, &img, err);
        }
#undef DEST
    }
    if (st != PC_OK) {
        goto done;
    }
    if (planes > nplanes) {
        pc_cwarning wn;
        memset(&wn, 0, sizeof wn);
        wn.kind = PC_CW_MORE_PAGES;
        wn.has_total = planes <= UINT32_MAX;
        wn.total = wn.has_total ? (uint32_t)planes : 0;
        if ((st = pc_cimage_add_warning(&img, wn, err)) != PC_OK) {
            goto done;
        }
    }
    if (has_primary && (st = add_cards(&primary, &img.meta, err)) != PC_OK) {
        goto done;
    }
    if ((st = add_cards(&h, &img.meta, err)) != PC_OK) {
        goto done;
    }
    *out = img;
    pc_cimage_init(&img);
done:
    pc_cimage_fini(&img);
    free(ax);
    header_fini(&h);
    if (has_primary) {
        header_fini(&primary);
    }
    return st;
}

/* ---- writing ---- */

/* The text with every character outside ' '..'~' (a UTF-8 scalar, or an invalid byte) as '?'. */
static pc_status ascii(const char *s, size_t n, pc_buf *out, pc_error *err)
{
    size_t i = 0;
    pc_status st = PC_OK;
    while (i < n && st == PC_OK) {
        size_t j = i;
        uint32_t cp;
        char c;
        if (!pc_utf8_next((const uint8_t *)s, n, &j, &cp)) {
            j = i + 1;
            cp = 0xFFFD;
        }
        c = cp >= ' ' && cp <= '~' ? (char)cp : '?';
        st = pc_buf_append(out, &c, 1, err);
        i = j;
    }
    return st;
}

/* A FITS literal (number or logical) that can be written unquoted. */
static bool literal(const char *v, size_t n)
{
    char small[64] = {0};
    char *t;
    size_t i;
    double d;
    bool ok;
    if (n == 1 && (v[0] == 'T' || v[0] == 'F')) {
        return true;
    }
    for (i = 0; i < n; i++) {
        if (v[i] == '\0' || strchr("0123456789+-.EeDd", v[i]) == NULL) {
            return false;
        }
    }
    t = n < sizeof small ? small : malloc(n);
    if (t == NULL) {
        return false;
    }
    for (i = 0; i < n; i++) {
        t[i] = v[i] == 'D' || v[i] == 'd' ? 'E' : v[i];
    }
    ok = cx_parse_f64(t, n, &d) && isfinite(d);
    if (t != small) {
        free(t);
    }
    return ok;
}

/* '...' with '' escapes, cut to fit room (keeping the closing quote), padded to 8 characters. */
static pc_status quoted(const char *v, size_t n, size_t room, pc_buf *out, pc_error *err)
{
    pc_buf a = PC_BUF_INIT;
    size_t i;
    size_t len = 1;
    pc_status st = ascii(v, n, &a, err);
    if (st == PC_OK) {
        st = pc_buf_append(out, "'", 1, err);
    }
    for (i = 0; i < a.len && st == PC_OK; i++) {
        size_t add = a.data[i] == '\'' ? 2 : 1;
        if (len + add + 1 > room) {
            break;
        }
        st = pc_buf_append(out, a.data[i] == '\'' ? "''" : (const char *)&a.data[i], add, err);
        len += add;
    }
    while (len < 9 && st == PC_OK) {
        st = pc_buf_append(out, " ", 1, err);
        len++;
    }
    if (st == PC_OK) {
        st = pc_buf_append(out, "'", 1, err);
    }
    pc_buf_fini(&a);
    return st;
}

typedef struct writer {
    pc_buf *out;
    pc_status st;
    pc_error *err;
} writer;

static void wcard(writer *w, const char *text, size_t n)
{
    pc_buf a = PC_BUF_INIT;
    size_t len;
    static const char SP[CARD + 1] =
        "                                                                                ";
    if (w->st != PC_OK) {
        return;
    }
    w->st = ascii(text, n, &a, w->err);
    len = a.len < CARD ? a.len : CARD;
    if (w->st == PC_OK) {
        w->st = pc_buf_append(w->out, a.data != NULL ? a.data : (const uint8_t *)"", len, w->err);
    }
    if (w->st == PC_OK) {
        w->st = pc_buf_append(w->out, SP, CARD - len, w->err);
    }
    pc_buf_fini(&a);
}

/* "{key:<8}" + rest into one card. */
static void wkeyed(writer *w, const char *key, size_t kl, const char *rest, size_t rn)
{
    pc_buf t = PC_BUF_INIT;
    size_t chars = 0;
    size_t i = 0;
    if (w->st != PC_OK) {
        return;
    }
    while (i < kl) {
        uint32_t cp;
        size_t j = i;
        if (!pc_utf8_next((const uint8_t *)key, kl, &j, &cp)) {
            j = i + 1;
        }
        i = j;
        chars++;
    }
    w->st = pc_buf_append(&t, key, kl, w->err);
    while (w->st == PC_OK && chars < 8) {
        w->st = pc_buf_append(&t, " ", 1, w->err);
        chars++;
    }
    if (w->st == PC_OK) {
        w->st = pc_buf_append(&t, rest, rn, w->err);
    }
    if (w->st == PC_OK) {
        wcard(w, (const char *)t.data, t.len);
    }
    pc_buf_fini(&t);
}

static void wvalue(writer *w, const char *key, size_t kl, const char *v, size_t vn)
{
    pc_buf t = PC_BUF_INIT;
    if (w->st != PC_OK) {
        return;
    }
    w->st = pc_buf_append(&t, "= ", 2, w->err);
    if (literal(v, vn)) {
        size_t pad = vn < 20 ? 20 - vn : 0;
        while (w->st == PC_OK && pad-- > 0) {
            w->st = pc_buf_append(&t, " ", 1, w->err);
        }
        if (w->st == PC_OK) {
            w->st = pc_buf_append(&t, v, vn, w->err);
        }
    } else if (w->st == PC_OK) {
        w->st = quoted(v, vn, CARD - 10, &t, w->err);
    }
    if (w->st == PC_OK) {
        wkeyed(w, key, kl, (const char *)t.data, t.len);
    }
    pc_buf_fini(&t);
}

static void wvalue_c(writer *w, const char *key, const char *v)
{
    wvalue(w, key, strlen(key), v, strlen(v));
}

static void wcommentary(writer *w, const char *key, const char *text, size_t n)
{
    pc_buf a = PC_BUF_INIT;
    size_t i;
    if (w->st != PC_OK) {
        return;
    }
    w->st = ascii(text, n, &a, w->err);
    if (w->st == PC_OK && a.len == 0) {
        wcard(w, key, strlen(key));
    }
    for (i = 0; w->st == PC_OK && i < a.len; i += CARD - 8) {
        size_t k = a.len - i < CARD - 8 ? a.len - i : CARD - 8;
        wkeyed(w, key, strlen(key), (const char *)a.data + i, k);
    }
    pc_buf_fini(&a);
}

/* A standard keyword: 1-8 characters from A-Z 0-9 _ -. */
static bool standard_key(const char *k, size_t n)
{
    size_t i;
    if (n < 1 || n > 8) {
        return false;
    }
    for (i = 0; i < n; i++) {
        char c = k[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

/* body holds all of v (it was not cut short to fit the card). */
static bool quoted_whole(const char *v, size_t vn, const pc_buf *body, const pc_buf *av)
{
    size_t a = 0;
    size_t b = body->len;
    size_t i;
    size_t o = 0;
    size_t an = av->len;
    char *t;
    bool eq;
    if (literal(v, vn)) {
        return true;
    }
    while (a < b && body->data[a] == '\'') {
        a++;
    }
    while (b > a && body->data[b - 1] == '\'') {
        b--;
    }
    while (b > a && body->data[b - 1] == ' ') {
        b--;
    }
    t = malloc(b - a + 1);
    if (t == NULL) {
        return false;
    }
    for (i = a; i < b; i++) {
        if (body->data[i] == '\'' && i + 1 < b && body->data[i + 1] == '\'') {
            t[o++] = '\'';
            i++;
        } else {
            t[o++] = (char)body->data[i];
        }
    }
    while (an > 0 && av->data[an - 1] == ' ') {
        an--;
    }
    eq = o == an && (o == 0 || memcmp(t, av->data, o) == 0);
    free(t);
    return eq;
}

static void wtext(writer *w, const char *key, size_t kl, const char *v, size_t vn)
{
    bool commentary = false;
    char upper[8];
    if (w->st != PC_OK) {
        return;
    }
    if (kl == 7) {
        size_t i;
        for (i = 0; i < 7; i++) {
            upper[i] = key[i] >= 'a' && key[i] <= 'z' ? (char)(key[i] - 'a' + 'A') : key[i];
        }
        commentary = memcmp(upper, "COMMENT", 7) == 0 || memcmp(upper, "HISTORY", 7) == 0;
    }
    if (commentary) {
        char k[8];
        memcpy(k, upper, 7);
        k[7] = '\0';
        wcommentary(w, k, v, vn);
    } else if (standard_key(key, kl)) {
        if (!is_structural(key, kl) && !is_naxis(key, kl)) {
            wvalue(w, key, kl, v, vn);
        }
    } else {
        /* Long or mixed-case names: the HIERARCH convention when the card fits, else a comment. */
        pc_buf name = PC_BUF_INIT;
        pc_buf head = PC_BUF_INIT;
        pc_buf body = PC_BUF_INIT;
        pc_buf av = PC_BUF_INIT;
        bool lit = literal(v, vn);
        bool fits_key;
        size_t a;
        size_t b;
        w->st = ascii(key, kl, &name, w->err);
        if (w->st == PC_OK) {
            w->st = ascii(v, vn, &av, w->err);
        }
        if (w->st == PC_OK) {
            w->st = pc_buf_append(&head, "HIERARCH ", 9, w->err);
        }
        if (w->st == PC_OK) {
            w->st =
                pc_buf_append(&head, name.data != NULL ? name.data : (const uint8_t *)"", name.len, w->err);
        }
        if (w->st == PC_OK) {
            w->st = pc_buf_append(&head, " = ", 3, w->err);
        }
        {
            const uint8_t *nd = name.data != NULL ? name.data : (const uint8_t *)"";
            size_t nl = name.data != NULL ? name.len : 0;
            trim((const char *)nd, nl, &a, &b);
            fits_key = (nl == 0 || memchr(nd, '=', nl) == NULL) && a < b;
        }
        if (w->st == PC_OK) {
            w->st = lit ? pc_buf_append(&body, v, vn, w->err)
                        : quoted(v, vn, head.len < CARD ? CARD - head.len : 0, &body, w->err);
        }
        if (w->st == PC_OK) {
            if (fits_key && head.len + body.len <= CARD && quoted_whole(v, vn, &body, &av)) {
                w->st = pc_buf_append(&head, body.data, body.len, w->err);
                if (w->st == PC_OK) {
                    wcard(w, (const char *)head.data, head.len);
                }
            } else {
                pc_buf c = PC_BUF_INIT;
                w->st = pc_buf_append(&c, key, kl, w->err);
                if (w->st == PC_OK) {
                    w->st = pc_buf_append(&c, " = ", 3, w->err);
                }
                if (w->st == PC_OK) {
                    w->st = pc_buf_append(&c, v, vn, w->err);
                }
                if (w->st == PC_OK) {
                    wcommentary(w, "COMMENT", (const char *)c.data, c.len);
                }
                pc_buf_fini(&c);
            }
        }
        pc_buf_fini(&name);
        pc_buf_fini(&head);
        pc_buf_fini(&body);
        pc_buf_fini(&av);
    }
}

static void pad_to_block(writer *w, uint8_t fill)
{
    while (w->st == PC_OK && w->out->len % BLOCK != 0) {
        size_t k = BLOCK - w->out->len % BLOCK;
        uint8_t z[256];
        memset(z, fill, sizeof z);
        w->st = pc_buf_append(w->out, z, k < sizeof z ? k : sizeof z, w->err);
    }
}

pc_status cx_fits_encode(const pc_cimage *src, cx_plan pl, const pc_cencode_opts *o, pc_buf *out,
                         pc_error *err)
{
    pc_cimage tmp;
    const pc_cimage *img;
    size_t nc;
    int bitpix;
    char num[24];
    writer w;
    pc_buf b = PC_BUF_INIT;
    size_t wu;
    size_t hu;
    size_t p;
    size_t x;
    size_t y;
    size_t bytes;
    pc_status st = pc_cimage_converted(src, pl.layout, pl.sample, &tmp, &img, err);
    if (st != PC_OK) {
        return st;
    }
    if (img->layout == PC_CL_GRAY) {
        nc = 1;
    } else if (img->layout == PC_CL_RGB) {
        nc = 3;
    } else {
        pc_cimage_fini(&tmp);
        return cx_encode(err, F, "FITS cannot store %s", pc_clayout_name(img->layout));
    }
    if (img->sample == PC_CS_U8) {
        bitpix = 8;
    } else if (img->sample == PC_CS_U16) {
        bitpix = 16;
    } else if (img->sample == PC_CS_F32) {
        bitpix = -32;
    } else {
        pc_cimage_fini(&tmp);
        return cx_encode(err, F, "unsupported sample %s", pc_csample_name(img->sample));
    }
    w.out = &b;
    w.st = PC_OK;
    w.err = err;
    wvalue_c(&w, "SIMPLE", "T");
    (void)snprintf(num, sizeof num, "%d", bitpix);
    wvalue_c(&w, "BITPIX", num);
    wvalue_c(&w, "NAXIS", nc == 3 ? "3" : "2");
    (void)snprintf(num, sizeof num, "%u", (unsigned)img->width);
    wvalue_c(&w, "NAXIS1", num);
    (void)snprintf(num, sizeof num, "%u", (unsigned)img->height);
    wvalue_c(&w, "NAXIS2", num);
    if (nc == 3) {
        wvalue_c(&w, "NAXIS3", "3");
    }
    if (bitpix == 16) {
        wvalue_c(&w, "BZERO", "32768");
        wvalue_c(&w, "BSCALE", "1");
    }
    wvalue_c(&w, "ROWORDER", "BOTTOM-UP");
    if (o->embed_metadata) {
        size_t i;
        for (i = 0; i < img->meta.ntext; i++) {
            wtext(&w, img->meta.text[i].key, img->meta.text[i].key_len, img->meta.text[i].value,
                  img->meta.text[i].value_len);
        }
    }
    wcard(&w, "END", 3);
    pad_to_block(&w, ' ');
    /* Planar, bottom row first. */
    wu = img->width;
    hu = img->height;
    bytes = (size_t)(bitpix < 0 ? -bitpix : bitpix) / 8;
    if (w.st == PC_OK) {
        size_t total;
        if (!pc_size_mul3(wu, hu, nc * bytes, &total)) {
            w.st = cx_nomem(err);
        } else {
            w.st = pc_buf_reserve(&b, total, err);
        }
    }
    for (p = 0; p < nc && w.st == PC_OK; p++) {
        for (y = 0; y < hu; y++) {
            for (x = 0; x < wu; x++) {
                size_t i = ((hu - 1 - y) * wu + x) * nc + p;
                uint8_t v[4];
                if (bitpix == 8) {
                    v[0] = img->data[i];
                } else if (bitpix == 16) {
                    uint16_t s = cx_ne16(img->data, i);
                    uint16_t u = (uint16_t)(s ^ 0x8000u); /* (v - 32768) as i16, big-endian */
                    v[0] = (uint8_t)(u >> 8);
                    v[1] = (uint8_t)u;
                } else {
                    uint32_t bits;
                    memcpy(&bits, img->data + 4 * i, 4);
                    v[0] = (uint8_t)(bits >> 24);
                    v[1] = (uint8_t)(bits >> 16);
                    v[2] = (uint8_t)(bits >> 8);
                    v[3] = (uint8_t)bits;
                }
                memcpy(b.data + b.len, v, bytes);
                b.len += bytes;
            }
        }
        b.data[b.len] = '\0';
    }
    pad_to_block(&w, 0);
    pc_cimage_fini(&tmp);
    if (w.st != PC_OK) {
        pc_buf_fini(&b);
        return w.st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}
