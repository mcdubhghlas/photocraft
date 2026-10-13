/* OpenType table readers (see sfnt.h). Offsets and lengths come from the font, so every access
 * goes through rd16/rd32 with an explicit bound; arithmetic on them is done in size_t after
 * range checks, so nothing wraps. */
#include "text/sfnt.h"

#include <string.h>

#include "base/utf8.h"

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)(p[0] << 8) | p[1]);
}
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Bounds-checked big-endian reads: false when [off, off + n) is outside [0, len). */
static bool rd16(const uint8_t *d, size_t len, size_t off, uint16_t *v)
{
    if (off > len || len - off < 2) {
        return false;
    }
    *v = be16(d + off);
    return true;
}
/* An array of n elements of `size` bytes at off fits? (read-fonts reads such arrays as empty
 * when they don't.) */
static bool fits(size_t len, size_t off, size_t n, size_t size)
{
    if (off > len) {
        return false;
    }
    return n <= (len - off) / size;
}

#define TAG_TTCF PC_SFNT_TAG('t', 't', 'c', 'f')

uint32_t pc_sfnt_face_count(const uint8_t *data, size_t len)
{
    pc_sfnt f;
    if (data == NULL || len < 12) {
        return 0;
    }
    if (be32(data) == TAG_TTCF) {
        uint32_t n = be32(data + 8);
        return fits(len, 12, n, 4) ? n : 0;
    }
    return pc_sfnt_open(data, len, 0, &f) ? 1 : 0;
}

static bool open_dir(const uint8_t *data, size_t len, size_t dir, pc_sfnt *out)
{
    uint32_t version;
    uint16_t n = 0;
    size_t i;
    if (dir > len || len - dir < 12) {
        return false;
    }
    version = be32(data + dir);
    if (version != 0x00010000u && version != PC_SFNT_TAG('O', 'T', 'T', 'O') &&
        version != PC_SFNT_TAG('t', 'r', 'u', 'e')) {
        return false;
    }
    n = be16(data + dir + 4);
    out->data = data;
    out->len = len;
    out->dir = dir;
    /* A record array that runs past the end reads as empty (read-fonts' table_records()). */
    out->ntables = fits(len, dir + 12, n, 16) ? n : 0;
    out->sorted = true;
    for (i = 1; i < out->ntables; i++) {
        if (be32(data + dir + 12 + 16 * i) <= be32(data + dir + 12 + 16 * (i - 1))) {
            out->sorted = false;
            break;
        }
    }
    /* The first tag must also be above the all-zero tag. */
    if (out->ntables > 0 && be32(data + dir + 12) == 0) {
        out->sorted = false;
    }
    return true;
}

bool pc_sfnt_open(const uint8_t *data, size_t len, uint32_t index, pc_sfnt *out)
{
    pc_sfnt f;
    if (data == NULL || len < 12) {
        return false;
    }
    if (be32(data) == TAG_TTCF) {
        uint32_t n = be32(data + 8), off;
        if (!fits(len, 12, n, 4) || index >= n) {
            return false;
        }
        off = be32(data + 12 + 4 * (size_t)index);
        if (!open_dir(data, len, off, &f)) {
            return false;
        }
    } else {
        if (index != 0 || !open_dir(data, len, 0, &f)) {
            return false;
        }
    }
    *out = f;
    return true;
}

static bool record_index(const pc_sfnt *f, uint32_t tag, size_t *idx)
{
    const uint8_t *rec = f->data + f->dir + 12;
    size_t i;
    if (f->sorted) {
        size_t lo = 0, hi = f->ntables;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            uint32_t t = be32(rec + 16 * mid);
            if (t < tag) {
                lo = mid + 1;
            } else if (t > tag) {
                hi = mid;
            } else {
                *idx = mid;
                return true;
            }
        }
        return false;
    }
    for (i = 0; i < f->ntables; i++) {
        if (be32(rec + 16 * i) == tag) {
            *idx = i;
            return true;
        }
    }
    return false;
}

bool pc_sfnt_table(const pc_sfnt *f, uint32_t tag, const uint8_t **p, size_t *n)
{
    size_t i;
    uint32_t off, len;
    const uint8_t *rec;
    if (f == NULL || !record_index(f, tag, &i)) {
        return false;
    }
    rec = f->data + f->dir + 12 + 16 * i;
    off = be32(rec + 8);
    len = be32(rec + 12);
    if (off == 0 || off > f->len || f->len - off < len) {
        return false;
    }
    *p = f->data + off;
    *n = len;
    return true;
}

bool pc_sfnt_has_table(const pc_sfnt *f, uint32_t tag)
{
    const uint8_t *p;
    size_t n;
    return pc_sfnt_table(f, tag, &p, &n);
}

bool pc_sfnt_head_flags_offset(const pc_sfnt *f, size_t *off)
{
    const uint8_t *p;
    size_t n;
    if (!pc_sfnt_table(f, PC_SFNT_TAG('h', 'e', 'a', 'd'), &p, &n) || n < 54) {
        return false;
    }
    *off = (size_t)(p - f->data) + 16;
    return true;
}

/* ------------------------------------------------------------------------------------------ */
/* name */

static const uint16_t MAC_ROMAN[128] = {
    196,   197,  199,  201,  209,  214,  220,  225,  224,  226,  228,  227,  229,  231,  233,   232,
    234,   235,  237,  236,  238,  239,  241,  243,  242,  244,  246,  245,  250,  249,  251,   252,
    8224,  176,  162,  163,  167,  8226, 182,  223,  174,  169,  8482, 180,  168,  8800, 198,   216,
    8734,  177,  8804, 8805, 165,  181,  8706, 8721, 8719, 960,  8747, 170,  186,  937,  230,   248,
    191,   161,  172,  8730, 402,  8776, 8710, 171,  187,  8230, 160,  192,  195,  213,  338,   339,
    8211,  8212, 8220, 8221, 8216, 8217, 247,  9674, 255,  376,  8260, 8364, 8249, 8250, 64257, 64258,
    8225,  183,  8218, 8222, 8240, 194,  202,  193,  203,  200,  205,  206,  207,  204,  211,   212,
    63743, 210,  218,  219,  217,  305,  710,  732,  175,  728,  729,  730,  184,  733,  731,   711,
};

/* Language ids skrifa maps to a BCP 47 tag (Mac 0..151, Windows LCIDs); see string.rs. */
static const uint16_t KNOWN_LANG[] = {
    0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x0406, 0x0407, 0x0408, 0x0409, 0x040a, 0x040b, 0x040c, 0x040d,
    0x040e, 0x040f, 0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041a,
    0x041b, 0x041c, 0x041d, 0x041e, 0x041f, 0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    0x0428, 0x0429, 0x042a, 0x042b, 0x042c, 0x042d, 0x042e, 0x042f, 0x0432, 0x0434, 0x0435, 0x0436, 0x0437,
    0x0438, 0x0439, 0x043a, 0x043b, 0x043e, 0x043f, 0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446,
    0x0447, 0x0448, 0x0449, 0x044a, 0x044b, 0x044c, 0x044d, 0x044e, 0x044f, 0x0450, 0x0451, 0x0452, 0x0453,
    0x0454, 0x0456, 0x0457, 0x045a, 0x045b, 0x045d, 0x045e, 0x0461, 0x0462, 0x0463, 0x0464, 0x0465, 0x0468,
    0x046a, 0x046b, 0x046c, 0x046d, 0x046e, 0x046f, 0x0470, 0x0478, 0x047a, 0x047c, 0x047e, 0x0480, 0x0481,
    0x0482, 0x0483, 0x0484, 0x0485, 0x0486, 0x0487, 0x0488, 0x048c, 0x0491, 0x0801, 0x0804, 0x0807, 0x0809,
    0x080a, 0x080c, 0x0810, 0x0813, 0x0814, 0x0816, 0x081a, 0x081d, 0x082c, 0x082e, 0x083b, 0x083c, 0x083e,
    0x0843, 0x0845, 0x0850, 0x085d, 0x085f, 0x086b, 0x0c01, 0x0c04, 0x0c07, 0x0c09, 0x0c0a, 0x0c0c, 0x0c1a,
    0x0c3b, 0x0c6b, 0x1001, 0x1004, 0x1007, 0x1009, 0x100a, 0x100c, 0x101a, 0x103b, 0x1401, 0x1404, 0x1407,
    0x1409, 0x140a, 0x140c, 0x141a, 0x143b, 0x1801, 0x1809, 0x180a, 0x180c, 0x181a, 0x183b, 0x1c01, 0x1c09,
    0x1c0a, 0x1c1a, 0x1c3b, 0x2001, 0x2009, 0x200a, 0x201a, 0x203b, 0x2401, 0x2409, 0x240a, 0x241a, 0x243b,
    0x2801, 0x2809, 0x280a, 0x281a, 0x2c01, 0x2c09, 0x2c0a, 0x2c1a, 0x3001, 0x3009, 0x300a, 0x301a, 0x3401,
    0x3409, 0x340a, 0x3801, 0x380a, 0x3c01, 0x3c0a, 0x4001, 0x4009, 0x400a, 0x4409, 0x440a, 0x4809, 0x480a,
    0x4c0a, 0x500a, 0x540a,
};

static bool known_lang(uint16_t id)
{
    size_t lo = 0, hi = sizeof KNOWN_LANG / sizeof KNOWN_LANG[0];
    if (id <= 151) {
        return id <= 94 || id >= 128;
    }
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (KNOWN_LANG[mid] < id) {
            lo = mid + 1;
        } else if (KNOWN_LANG[mid] > id) {
            hi = mid;
        } else {
            return true;
        }
    }
    return false;
}

typedef struct name_tab {
    const uint8_t *p;
    size_t n;
    uint16_t version, count;
    const uint8_t *strings; /* string storage (may be empty) */
    size_t slen;
    uint16_t ntags; /* version 1 language tag records (0 when cut off) */
    size_t tags;    /* offset of the lang tag records within p */
} name_tab;

static bool name_open(const pc_sfnt *f, name_tab *t)
{
    uint16_t storage;
    if (!pc_sfnt_table(f, PC_SFNT_TAG('n', 'a', 'm', 'e'), &t->p, &t->n) || t->n < 6) {
        return false;
    }
    t->version = be16(t->p);
    t->count = be16(t->p + 2);
    storage = be16(t->p + 4);
    if (!fits(t->n, 6, t->count, 12)) {
        t->count = 0; /* the record array reads as empty */
    }
    if (storage <= t->n) {
        t->strings = t->p + storage;
        t->slen = t->n - storage;
    } else {
        t->strings = t->p;
        t->slen = 0;
    }
    t->ntags = 0;
    t->tags = 0;
    if (t->version == 1) {
        size_t at = 6 + 12 * (size_t)be16(t->p + 2);
        uint16_t nt;
        if (rd16(t->p, t->n, at, &nt) && fits(t->n, at + 2, nt, 4)) {
            t->ntags = nt;
            t->tags = at + 2;
        }
    }
    return true;
}

bool pc_sfnt_has_name(const pc_sfnt *f)
{
    name_tab t;
    return name_open(f, &t);
}

typedef enum name_enc { ENC_UTF16, ENC_MACROMAN, ENC_UNKNOWN } name_enc;

static name_enc encoding(uint16_t platform, uint16_t encid)
{
    if (platform == 0) {
        return ENC_UTF16;
    }
    if (platform == 1 && encid == 0) {
        return ENC_MACROMAN;
    }
    if (platform == 3 && (encid == 0 || encid == 1 || encid == 10)) {
        return ENC_UTF16;
    }
    return ENC_UNKNOWN;
}

/* String bytes of a record: offset 0 counts as 0, the range must lie in the storage. */
static bool record_bytes(const name_tab *t, size_t off, size_t len, const uint8_t **p)
{
    if (off > t->slen || t->slen - off < len) {
        return false;
    }
    *p = t->strings + off;
    return true;
}

/* NameString chars as UTF-8 (read-fonts CharIter). */
static pc_status decode(const uint8_t *s, size_t n, name_enc e, pc_buf *out, pc_error *err)
{
    size_t i = 0;
    pc_status st = PC_OK;
    if (e == ENC_MACROMAN) {
        for (i = 0; i < n && st == PC_OK; i++) {
            uint32_t c = s[i] < 128 ? s[i] : MAC_ROMAN[s[i] - 128];
            st = pc_utf8_put(out, c, err);
        }
        return st;
    }
    if (e != ENC_UTF16) {
        return PC_OK;
    }
    while (i + 2 <= n && st == PC_OK) {
        uint32_t c1 = be16(s + i), c;
        i += 2;
        if (c1 >= 0xD800 && c1 < 0xDC00) {
            if (i + 2 > n) {
                st = pc_utf8_put(out, 0xFFFD, err);
                break;
            }
            uint32_t c2 = be16(s + i);
            if (c2 < 0xDC00 || c2 > 0xDFFF) {
                st = pc_utf8_put(out, 0xFFFD, err); /* c2 is decoded on its own next */
                continue;
            }
            i += 2;
            c = ((c1 & 0x3FF) << 10) + (c2 & 0x3FF) + 0x10000;
        } else {
            c = c1;
        }
        if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        st = pc_utf8_put(out, c, err);
    }
    return st;
}

/* skrifa language rank of a record: 3 = "en-US", 2 = "en", 1 = unknown (None), 0 = other. */
static int skrifa_lang(const name_tab *t, uint16_t platform, uint16_t lang)
{
    if (t->version == 1 && lang >= 0x8000) {
        size_t idx = (size_t)(lang - 0x8000);
        const uint8_t *s;
        char buf[30];
        size_t n = 0, i = 0;
        if (idx >= t->ntags) {
            return 1;
        }
        if (!record_bytes(t, be16(t->p + t->tags + 4 * idx + 2), be16(t->p + t->tags + 4 * idx), &s)) {
            return 1;
        }
        {
            size_t len = be16(t->p + t->tags + 4 * idx);
            /* UTF-16BE, ASCII only, at most 30 characters (Language::from_name_string). */
            while (i + 2 <= len) {
                uint32_t c = be16(s + i);
                i += 2;
                if (c >= 0xD800 && c < 0xDC00) {
                    return 1; /* a surrogate pair or U+FFFD: not ASCII */
                }
                if (c > 0x7F || n == sizeof buf) {
                    return 1;
                }
                buf[n++] = (char)c;
            }
        }
        if (n == 5 && memcmp(buf, "en-US", 5) == 0) {
            return 3;
        }
        if (n == 2 && memcmp(buf, "en", 2) == 0) {
            return 2;
        }
        return 0;
    }
    if (platform != 1 && platform != 3) {
        return 1;
    }
    if (!known_lang(lang)) {
        return 1;
    }
    return lang == 0x0409 ? 3 : lang == 0 ? 2 : 0;
}

bool pc_sfnt_name(const pc_sfnt *f, uint16_t id, pc_sfnt_name_rule rule, pc_buf *out, pc_error *err)
{
    name_tab t;
    size_t i, k = 0, best = SIZE_MAX;
    int best_rank = -1;
    const uint8_t *rec, *s;
    if (!name_open(f, &t)) {
        return false;
    }
    for (i = 0; i < t.count; i++) {
        int rank;
        rec = t.p + 6 + 12 * i;
        if (be16(rec + 6) != id) {
            continue;
        }
        if (rule == PC_SFNT_NAME_FONTIQUE) {
            uint16_t lang = be16(rec + 4);
            if (lang == 0x0409) {
                best = i;
                break;
            }
            if (lang == 0) {
                rank = 2;
            } else if (i == 0) {
                rank = 1;
            } else {
                continue;
            }
        } else {
            int l = skrifa_lang(&t, be16(rec), be16(rec + 4));
            if (l == 3) {
                best = i;
                break;
            }
            if (l == 2) {
                rank = 2;
            } else if (l == 1) {
                rank = 1;
            } else if (k == 0) {
                rank = 0;
            } else {
                k++;
                continue;
            }
            k++;
        }
        if (rank > best_rank) {
            best_rank = rank;
            best = i;
        }
    }
    if (best == SIZE_MAX) {
        return false;
    }
    rec = t.p + 6 + 12 * best;
    if (!record_bytes(&t, be16(rec + 10), be16(rec + 8), &s)) {
        /* fontique: the string is unreadable, so there's no name. skrifa: an empty string. */
        return rule == PC_SFNT_NAME_SKRIFA;
    }
    {
        size_t keep = out->len;
        if (decode(s, be16(rec + 8), encoding(be16(rec), be16(rec + 2)), out, err) != PC_OK) {
            pc_buf_truncate(out, keep);
            return false;
        }
    }
    return true;
}

bool pc_sfnt_each_name(const pc_sfnt *f, uint16_t id, bool (*fn)(void *ctx, const char *s, size_t len),
                       void *ctx)
{
    name_tab t;
    size_t i;
    pc_buf b;
    bool stop = false;
    if (!name_open(f, &t)) {
        return false;
    }
    pc_buf_init(&b);
    for (i = 0; i < t.count && !stop; i++) {
        const uint8_t *rec = t.p + 6 + 12 * i, *s;
        if (be16(rec + 6) != id) {
            continue;
        }
        pc_buf_truncate(&b, 0);
        if (record_bytes(&t, be16(rec + 10), be16(rec + 8), &s) &&
            decode(s, be16(rec + 8), encoding(be16(rec), be16(rec + 2)), &b, NULL) != PC_OK) {
            break; /* out of memory: stop rather than report a wrong name */
        }
        stop = fn(ctx, b.len > 0 ? (const char *)b.data : "", b.len);
    }
    pc_buf_fini(&b);
    return stop;
}

/* The class of a record's skrifa language: None (unknown or unreadable), an empty tag, English
 * (primary subtag "en", any ASCII case) or another language. */
static pc_sfnt_lang lang_class(const name_tab *t, uint16_t platform, uint16_t lang)
{
    if (t->version == 1 && lang >= 0x8000) {
        size_t idx = (size_t)(lang - 0x8000), len, i = 0, n = 0;
        const uint8_t *s;
        char buf[30];
        if (idx >= t->ntags ||
            !record_bytes(t, be16(t->p + t->tags + 4 * idx + 2), be16(t->p + t->tags + 4 * idx), &s)) {
            return PC_SFNT_LANG_NONE;
        }
        len = be16(t->p + t->tags + 4 * idx);
        while (i + 2 <= len) {
            uint32_t c = be16(s + i);
            i += 2;
            if (c > 0x7F || n == sizeof buf) {
                return PC_SFNT_LANG_NONE;
            }
            buf[n++] = (char)c;
        }
        if (n == 0) {
            return PC_SFNT_LANG_EMPTY;
        }
        for (i = 0; i < n && buf[i] != '-' && buf[i] != '_'; i++) {
        }
        return i == 2 && (buf[0] == 'e' || buf[0] == 'E') && (buf[1] == 'n' || buf[1] == 'N')
                   ? PC_SFNT_LANG_ENGLISH
                   : PC_SFNT_LANG_OTHER;
    }
    if ((platform != 1 && platform != 3) || !known_lang(lang)) {
        return PC_SFNT_LANG_NONE;
    }
    /* Every English tag in skrifa's table: Mac 0 ("en") and the Windows LCIDs with primary
     * language 0x09 ("en-US", "en-GB", ...). */
    return lang == 0 || (lang > 151 && (lang & 0x3FF) == 0x09) ? PC_SFNT_LANG_ENGLISH : PC_SFNT_LANG_OTHER;
}

bool pc_sfnt_each_name_lang(const pc_sfnt *f, uint16_t id,
                            bool (*fn)(void *ctx, const char *s, size_t len, pc_sfnt_lang lang), void *ctx)
{
    name_tab t;
    size_t i;
    pc_buf b;
    bool stop = false;
    if (!name_open(f, &t)) {
        return false;
    }
    pc_buf_init(&b);
    for (i = 0; i < t.count && !stop; i++) {
        const uint8_t *rec = t.p + 6 + 12 * i, *s;
        if (be16(rec + 6) != id) {
            continue;
        }
        pc_buf_truncate(&b, 0);
        if (record_bytes(&t, be16(rec + 10), be16(rec + 8), &s) &&
            decode(s, be16(rec + 8), encoding(be16(rec), be16(rec + 2)), &b, NULL) != PC_OK) {
            break; /* out of memory: stop rather than report a wrong name */
        }
        stop =
            fn(ctx, b.len > 0 ? (const char *)b.data : "", b.len, lang_class(&t, be16(rec), be16(rec + 4)));
    }
    pc_buf_fini(&b);
    return stop;
}

/* ------------------------------------------------------------------------------------------ */
/* Attributes, axes, metrics */

static float width_from_class(uint16_t c)
{
    switch (c) {
    case 0:
    case 1:
        return 0.5f;
    case 2:
        return 0.625f;
    case 3:
        return 0.75f;
    case 4:
        return 0.875f;
    case 5:
        return 1.0f;
    case 6:
        return 1.125f;
    case 7:
        return 1.25f;
    case 8:
        return 1.5f;
    default:
        return 2.0f;
    }
}

void pc_sfnt_attrs(const pc_sfnt *f, pc_font_attrs *out)
{
    const uint8_t *p;
    size_t n;
    pc_font_attrs a = {1.0f, PC_SLOPE_NORMAL, false, 0.0f, 400.0f};
    /* OS/2 version 0 is 78 bytes (read-fonts' minimum). */
    if (pc_sfnt_table(f, PC_SFNT_TAG('O', 'S', '/', '2'), &p, &n) && n >= 78) {
        uint16_t sel = be16(p + 62);
        a.width = width_from_class(be16(p + 6));
        if (sel & 1u) {
            a.slope = PC_SLOPE_ITALIC;
        } else if (sel & 0x200u) {
            const uint8_t *q;
            size_t m;
            a.slope = PC_SLOPE_OBLIQUE;
            if (pc_sfnt_table(f, PC_SFNT_TAG('p', 'o', 's', 't'), &q, &m) && m >= 32) {
                /* Fixed 16.16 through f64 to f32, as read-fonts does. */
                int32_t v = (int32_t)be32(q + 4);
                a.has_angle = true;
                a.angle = (float)((double)v / 65536.0);
            }
        }
        a.weight = (float)be16(p + 4);
    } else if (pc_sfnt_table(f, PC_SFNT_TAG('h', 'e', 'a', 'd'), &p, &n) && n >= 54) {
        uint16_t mac = be16(p + 44);
        a.slope = (mac & 2u) ? PC_SLOPE_ITALIC : PC_SLOPE_NORMAL;
        a.weight = (mac & 1u) ? 700.0f : 400.0f;
    }
    *out = a;
}

size_t pc_sfnt_axes(const pc_sfnt *f, pc_font_axis *out, size_t cap)
{
    const uint8_t *p;
    size_t n, i, off, count, size;
    if (!pc_sfnt_table(f, PC_SFNT_TAG('f', 'v', 'a', 'r'), &p, &n) || n < 16) {
        return 0;
    }
    off = be16(p + 4);
    count = be16(p + 8);
    size = be16(p + 10);
    /* read-fonts reads 20-byte records regardless of axisSize; a cut-off array is an error. */
    (void)size;
    if (off == 0 || !fits(n, off, count, 20)) {
        return 0;
    }
    for (i = 0; i < count && i < cap; i++) {
        const uint8_t *r = p + off + 20 * i;
        memcpy(out[i].tag, r, 4);
        out[i].tag[4] = '\0';
        out[i].min = (float)((int32_t)be32(r + 4) / 65536.0);
        out[i].def = (float)((int32_t)be32(r + 8) / 65536.0);
        out[i].max = (float)((int32_t)be32(r + 12) / 65536.0);
    }
    return count;
}

uint16_t pc_sfnt_units_per_em(const pc_sfnt *f)
{
    const uint8_t *p;
    size_t n;
    if (!pc_sfnt_table(f, PC_SFNT_TAG('h', 'e', 'a', 'd'), &p, &n) || n < 54) {
        return 0;
    }
    return be16(p + 18);
}

uint32_t pc_sfnt_num_glyphs(const pc_sfnt *f)
{
    const uint8_t *p;
    size_t n;
    if (!pc_sfnt_table(f, PC_SFNT_TAG('m', 'a', 'x', 'p'), &p, &n) || n < 6) {
        return 0;
    }
    return be16(p + 4);
}

bool pc_sfnt_advance(const pc_sfnt *f, uint32_t gid, int32_t *out)
{
    const uint8_t *hh, *hm;
    size_t nh, nm;
    uint16_t nlong;
    size_t idx;
    if (gid >= pc_sfnt_num_glyphs(f) || !pc_sfnt_table(f, PC_SFNT_TAG('h', 'h', 'e', 'a'), &hh, &nh) ||
        nh < 36 || !pc_sfnt_table(f, PC_SFNT_TAG('h', 'm', 't', 'x'), &hm, &nm)) {
        return false;
    }
    nlong = be16(hh + 34);
    if (nlong == 0) {
        return false;
    }
    idx = gid < nlong ? gid : (size_t)nlong - 1;
    if (!fits(nm, 0, (size_t)idx + 1, 4)) {
        return false;
    }
    *out = be16(hm + 4 * idx);
    return true;
}

bool pc_sfnt_gsub_has_feature(const pc_sfnt *f, uint32_t tag)
{
    const uint8_t *p;
    size_t n, i;
    uint16_t foff, count;
    if (!pc_sfnt_table(f, PC_SFNT_TAG('G', 'S', 'U', 'B'), &p, &n) || n < 10) {
        return false;
    }
    foff = be16(p + 6);
    if (foff == 0 || !rd16(p, n, foff, &count) || !fits(n, (size_t)foff + 2, count, 6)) {
        return false;
    }
    for (i = 0; i < count; i++) {
        if (be32(p + foff + 2 + 6 * i) == tag) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------------------------ */
/* cmap */

/* Subtable at `off` within the cmap table: its bytes run to the end of the table (read-fonts'
 * offset data), and its format must parse (fixed header present). */
static bool subtable(const uint8_t *p, size_t n, uint32_t off, pc_sfnt_cmap *out)
{
    uint16_t format;
    size_t min;
    if (off == 0 || off >= n || !rd16(p, n, off, &format)) {
        return false;
    }
    switch (format) {
    case 0:
        min = 6 + 256;
        break;
    case 2:
        min = 6 + 512;
        break;
    case 4:
        min = 16;
        break;
    case 6:
        min = 10;
        break;
    case 8:
        min = 12 + 8192 + 4;
        break;
    case 10:
        min = 20;
        break;
    case 12:
    case 13:
        min = 16;
        break;
    case 14:
        min = 10;
        break;
    default:
        return false;
    }
    if (n - off < min) {
        return false;
    }
    out->sub = p + off;
    out->len = n - off;
    out->format = format;
    return true;
}

static uint32_t glyph_count_limit(const pc_sfnt *f)
{
    const uint8_t *p;
    size_t n;
    if (!pc_sfnt_table(f, PC_SFNT_TAG('m', 'a', 'x', 'p'), &p, &n) || n < 6) {
        return 65535;
    }
    return be16(p + 4);
}

void pc_sfnt_cmap_skrifa(const pc_sfnt *f, pc_sfnt_cmap *out)
{
    const uint8_t *p;
    size_t n, i;
    int kind = 0; /* 1 BMP, 2 full, 3 symbol */
    memset(out, 0, sizeof *out);
    if (!pc_sfnt_table(f, PC_SFNT_TAG('c', 'm', 'a', 'p'), &p, &n) || n < 4) {
        return;
    }
    {
        uint16_t count = be16(p + 2);
        if (!fits(n, 4, count, 8)) {
            count = 0;
        }
        for (i = count; i-- > 0;) {
            const uint8_t *r = p + 4 + 8 * i;
            uint16_t plat = be16(r), enc = be16(r + 2);
            int k = 0;
            pc_sfnt_cmap c;
            if (plat == 3 && enc == 0) {
                k = 3;
            } else if ((plat == 3 && enc == 10) || (plat == 0 && enc == 4)) {
                k = 2;
            } else if ((plat == 2) || (plat == 0 && enc != 5) || (plat == 3 && enc == 1)) {
                k = 1;
            }
            if (k <= kind || !subtable(p, n, be32(r + 4), &c) || (c.format != 4 && c.format != 12)) {
                continue;
            }
            kind = k;
            *out = c;
            out->symbol = k == 3;
        }
    }
    out->glyph_count = glyph_count_limit(f);
}

/* Format 4 lookup of codepoint cp in segment idx starting at `start` (read-fonts
 * lookup_glyph_id). Returns 0 for none. */
static uint32_t cmap4_lookup(const uint8_t *s, size_t n, size_t segs, uint16_t cp, size_t idx, uint16_t start)
{
    size_t delta_at = 14 + 2 * segs + 2 + 2 * segs + 2 * idx;
    size_t ro_at = 14 + 2 * segs + 2 + 4 * segs + 2 * idx;
    size_t gia = 14 + 2 * segs + 2 + 6 * segs;
    uint16_t delta, ro, gid;
    size_t off, glen;
    if (!rd16(s, n, delta_at, &delta) || !rd16(s, n, ro_at, &ro)) {
        return 0;
    }
    if (ro == 0) {
        return (uint16_t)(cp + delta);
    }
    off = (size_t)ro / 2 + (size_t)(uint16_t)(cp - start);
    off = off > segs - idx ? off - (segs - idx) : 0;
    glen = gia <= n ? (n - gia) / 2 : 0;
    if (off >= glen) {
        return 0;
    }
    gid = be16(s + gia + 2 * off);
    return gid == 0 ? 0 : (uint16_t)(gid + delta);
}

/* Segment arrays are read as empty when cut off; returns the usable segment count. */
static size_t cmap4_segs(const uint8_t *s, size_t n)
{
    size_t segs = be16(s + 6) / 2;
    /* Every array must fit separately: end codes, start codes, deltas, range offsets. */
    if (!fits(n, 14, segs, 2) || !fits(n, 14 + 2 * segs + 2, segs, 2) ||
        !fits(n, 14 + 4 * segs + 2, segs, 2) || !fits(n, 14 + 6 * segs + 2, segs, 2)) {
        return 0;
    }
    return segs;
}

static uint32_t cmap4_map(const uint8_t *s, size_t n, uint32_t cp)
{
    size_t segs = cmap4_segs(s, n), lo = 0, hi;
    if (cp > 0xFFFF) {
        return 0;
    }
    hi = segs;
    while (lo < hi) {
        size_t i = (lo + hi) / 2;
        uint16_t start = be16(s + 14 + 2 * segs + 2 + 2 * i);
        uint16_t end = be16(s + 14 + 2 * i);
        if (cp < start) {
            hi = i;
        } else if (cp > end) {
            lo = i + 1;
        } else {
            return cmap4_lookup(s, n, segs, (uint16_t)cp, i, start);
        }
    }
    return 0;
}

static size_t groups(const uint8_t *s, size_t n)
{
    uint32_t count = be32(s + 12);
    return fits(n, 16, count, 12) ? count : 0;
}

static uint32_t cmap1213_map(const uint8_t *s, size_t n, uint32_t cp, bool constant)
{
    size_t lo = 0, hi = groups(s, n);
    while (lo < hi) {
        size_t i = (lo + hi) / 2;
        const uint8_t *g = s + 16 + 12 * i;
        uint32_t a = be32(g), b = be32(g + 4), gid = be32(g + 8);
        if (cp < a) {
            hi = i;
        } else if (cp > b) {
            lo = i + 1;
        } else {
            return constant ? gid : gid + (cp - a);
        }
    }
    return 0;
}

uint32_t pc_sfnt_cmap_map(const pc_sfnt_cmap *c, uint32_t cp)
{
    uint32_t g = 0;
    if (c == NULL || c->sub == NULL) {
        return 0;
    }
    g = c->format == 4 ? cmap4_map(c->sub, c->len, cp) : cmap1213_map(c->sub, c->len, cp, false);
    if (g == 0 && c->symbol && cp <= 0xFF) {
        g = c->format == 4 ? cmap4_map(c->sub, c->len, cp + 0xF000)
                           : cmap1213_map(c->sub, c->len, cp + 0xF000, false);
    }
    return g;
}

void pc_sfnt_cmap_each(const pc_sfnt_cmap *c, bool (*fn)(void *ctx, uint32_t cp, uint32_t gid), void *ctx)
{
    if (c == NULL || c->sub == NULL) {
        return;
    }
    if (c->format == 4) {
        const uint8_t *s = c->sub;
        size_t n = c->len, segs = cmap4_segs(s, n), ix = 0;
        uint32_t cur, end;
        uint16_t start;
        if (segs == 0) {
            return;
        }
        /* Cmap4Iter: ranges are forced forward (start >= previous end) and the lookup uses the
         * adjusted start, as the reference does. */
        start = be16(s + 14 + 2 * segs + 2);
        cur = start;
        end = (uint32_t)be16(s + 14) + 1;
        for (;;) {
            if (cur < end) {
                uint32_t g = cmap4_lookup(s, n, segs, (uint16_t)cur, ix, start);
                uint32_t cp = cur++;
                if (g != 0 && !fn(ctx, cp, g)) {
                    return;
                }
                continue;
            }
            ix++;
            if (ix >= segs) {
                return;
            }
            {
                uint32_t ns = be16(s + 14 + 2 * segs + 2 + 2 * ix), ne = (uint32_t)be16(s + 14 + 2 * ix) + 1;
                uint32_t prev_end = end;
                cur = ns > prev_end ? ns : prev_end;
                end = ne > prev_end ? ne : prev_end;
                start = (uint16_t)cur;
            }
        }
    } else {
        /* Cmap1213Iter with CmapIterLimits::default_for_font: each group's end is capped by the
         * glyph count and U+10FFFF (exclusive), and a group never starts before the previous
         * one's end. Glyph ids count from the group's own start code. */
        const uint8_t *s = c->sub;
        size_t ng = groups(s, c->len), gi;
        uint64_t cur = 0, end = 0;
        for (gi = 0; gi < ng; gi++) {
            const uint8_t *g = s + 16 + 12 * gi;
            uint32_t a = be32(g), gid = be32(g + 8);
            uint64_t e = (uint64_t)be32(g + 4) + 1;
            uint64_t lim = ((uint64_t)c->glyph_count > gid ? (uint64_t)c->glyph_count - gid : 0) + a;
            if (lim < e) {
                e = lim;
            }
            if (e > 0x10FFFF) {
                e = 0x10FFFF;
            }
            cur = (gi > 0 && a < end) ? end : a;
            end = e;
            while (cur < end) {
                uint32_t cp = (uint32_t)cur++;
                uint32_t gl = gid + (cp - a);
                if (gl != 0 && !fn(ctx, cp, gl)) {
                    return;
                }
            }
        }
    }
}

/* fontique / HarfBuzz preference order. */
bool pc_sfnt_cmap_fontique(const pc_sfnt *f, pc_sfnt_cmap *out)
{
    static const uint16_t ORDER[][2] = {{3, 0}, {3, 10}, {0, 6}, {0, 4}, {3, 1},
                                        {0, 3}, {0, 2},  {0, 1}, {0, 0}, {1, 0}};
    const uint8_t *p;
    size_t n, k, i;
    memset(out, 0, sizeof *out);
    if (!pc_sfnt_table(f, PC_SFNT_TAG('c', 'm', 'a', 'p'), &p, &n) || n < 4) {
        return false;
    }
    {
        uint16_t count = be16(p + 2);
        if (!fits(n, 4, count, 8)) {
            count = 0;
        }
        for (k = 0; k < sizeof ORDER / sizeof ORDER[0]; k++) {
            for (i = 0; i < count; i++) {
                const uint8_t *r = p + 4 + 8 * i;
                pc_sfnt_cmap c;
                if (be16(r) != ORDER[k][0] || be16(r + 2) != ORDER[k][1] ||
                    !subtable(p, n, be32(r + 4), &c)) {
                    continue;
                }
                if (c.format == 0 || c.format == 4 || c.format == 6 || c.format == 10 || c.format == 12 ||
                    c.format == 13) {
                    *out = c;
                    out->symbol = ORDER[k][0] == 3 && ORDER[k][1] == 0;
                    /* Mac Roman is flagged by glyph_count = 0 (unused for these lookups). */
                    out->glyph_count = (ORDER[k][0] == 1 && ORDER[k][1] == 0) ? 0 : 1;
                    return true;
                }
            }
        }
    }
    return false;
}

static uint32_t mac_roman_from_unicode(uint32_t c)
{
    size_t i;
    for (i = 0; i < 128; i++) {
        if (MAC_ROMAN[i] == (uint16_t)c) {
            return (uint32_t)(0x80 + i);
        }
    }
    return 0;
}

uint32_t pc_sfnt_cmap_map_any(const pc_sfnt_cmap *c, uint32_t cp)
{
    const uint8_t *s;
    size_t n;
    uint32_t g = 0, x = cp;
    if (c == NULL || c->sub == NULL) {
        return 0;
    }
    s = c->sub;
    n = c->len;
    if (c->glyph_count == 0 && x > 0x7F) {
        x = mac_roman_from_unicode(x);
    }
    switch (c->format) {
    case 0:
        g = x < 256 ? s[6 + x] : 0;
        break;
    case 6: {
        uint16_t first = be16(s + 6), count = be16(s + 8);
        if (fits(n, 10, count, 2) && x >= first && x - first < count) {
            g = be16(s + 10 + 2 * (x - first));
        }
        break;
    }
    case 10: {
        uint32_t first = be32(s + 12), count = be32(s + 16);
        if (fits(n, 20, count, 2) && x >= first && x - first < count) {
            g = be16(s + 20 + 2 * (size_t)(x - first));
        }
        break;
    }
    case 4:
        g = cmap4_map(s, n, x);
        break;
    case 12:
        g = cmap1213_map(s, n, x, false);
        break;
    case 13:
        g = cmap1213_map(s, n, x, true);
        break;
    default:
        break;
    }
    if (g == 0 && c->symbol && x <= 0xFF) {
        return pc_sfnt_cmap_map_any(c, 0xF000 + x);
    }
    return g;
}
