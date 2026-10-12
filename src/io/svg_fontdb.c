#define _XOPEN_SOURCE 700 /* realpath (D21) */
/* The font database SVG text is shaped with: a port of fontdb 0.23's Database as usvg uses it
 * (crates/io/src/svg.rs fills it with the system fonts; the reference's craft fonts are empty).
 *
 * Faces are listed in fontdb's order: the directories fontconfig names (svg_fontcfg.c; fontdb's
 * fixed list when there is none), each walked depth-first in readdir order with fontdb's
 * symlink rules, every face of every *.ttf/ttc/otf/otc (those exact spellings) file. The bundled
 * fonts (Inter, JetBrains Mono) come last, so that matching and the fallback scan see the
 * system's faces first as the reference does (it has no bundled fonts). PHOTOCRAFT_NO_SYSTEM_FONTS
 * leaves only the bundled fonts (and fontdb's default generic names).
 *
 * A face's attributes come from its tables as fontdb reads them (parse_face_info): families from
 * the typographic family names (else the family names) in Unicode encodings, the English (US)
 * one first; the PostScript name must exist; style, weight and width from OS/2 (italic also from
 * post.italicAngle); monospaced from post.isFixedPitch. Files are read with pread, table by table,
 * while scanning; a face's whole file is read (and kept) only when text uses it.
 *
 * Threads: built once under pthread_once; the byte, HarfBuzz face and cmap caches are filled
 * under one mutex and never freed (they live as long as the process, like fontdb's Arc). */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <hb.h>

#include "base/buf.h"
#include "base/file.h"
#include "io/svg_text.h"
#include "text/fonts.h"
#include "text/sfnt.h"

#define MAX_FACES_PER_FILE 4096u
#define MAX_TABLE ((size_t)16 << 20)
#define MAX_WALK_DEPTH 64u

typedef struct src {
    char *path;          /* NULL: bundled */
    const uint8_t *data; /* bundled data, or the loaded file */
    size_t len;
    bool tried; /* file read attempted */
} src;

struct svf_db {
    src *srcs;
    size_t nsrcs, capsrcs;
    svf_face *faces;
    size_t nfaces, capfaces;
    char *generic[5]; /* serif, sans-serif, cursive, fantasy, monospace */
    pthread_mutex_t mu;
    hb_face_t **hb; /* per face, created on demand */
    uint8_t **cmap; /* per face: the cmap table copy, or NULL */
    size_t *cmap_len;
    bool *cmap_tried;
};

/* ---------------------------------------------------------------- byte readers */

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* A font file being scanned: either in memory or read with pread. */
typedef struct rd {
    int fd;
    const uint8_t *mem;
    size_t size;
} rd;

/* Bytes [off, off + n) into a malloc'd buffer (the caller frees); NULL when out of range. */
static uint8_t *rd_get(const rd *r, size_t off, size_t n)
{
    uint8_t *b;
    size_t got;
    if (off > r->size || n > r->size - off || n > MAX_TABLE) {
        return NULL;
    }
    b = malloc(n > 0 ? n : 1);
    if (b == NULL) {
        return NULL;
    }
    if (r->mem != NULL) {
        memcpy(b, r->mem + off, n);
        return b;
    }
    got = 0;
    while (got < n) {
        ssize_t k = pread(r->fd, b + got, n - got, (off_t)(off + got));
        if (k < 0 && errno == EINTR) {
            continue;
        }
        if (k <= 0) {
            free(b);
            return NULL;
        }
        got += (size_t)k;
    }
    return b;
}

/* RawFace::parse: the table records of face `index`; *recs malloc'd. */
static bool raw_face(const rd *r, uint32_t index, uint8_t **recs, uint16_t *nrecs)
{
    uint8_t *h = rd_get(r, 0, 12);
    size_t off;
    uint32_t magic;
    uint16_t n;
    if (h == NULL && r->size >= 4) {
        h = rd_get(r, 0, 4);
    }
    if (h == NULL) {
        return false;
    }
    magic = be32(h);
    free(h);
    if (magic == 0x74746366u) { /* ttcf */
        uint8_t *c = rd_get(r, 8, 4);
        uint32_t nf;
        uint8_t *o;
        if (c == NULL) {
            return false;
        }
        nf = be32(c);
        free(c);
        if ((uint64_t)nf * 4u > r->size || index >= nf) {
            return false;
        }
        o = rd_get(r, 12 + (size_t)index * 4, 4);
        if (o == NULL) {
            return false;
        }
        off = be32(o);
        free(o);
        if (off < 12 + (size_t)nf * 4) {
            return false; /* checked_sub of the parser offset */
        }
        h = rd_get(r, off, 4);
        if (h == NULL) {
            return false;
        }
        magic = be32(h);
        free(h);
        if (magic != 0x00010000u && magic != 0x74727565u && magic != 0x4F54544Fu) {
            return false;
        }
        off += 4;
    } else if (magic == 0x00010000u || magic == 0x74727565u || magic == 0x4F54544Fu) {
        if (index != 0) {
            return false;
        }
        off = 4;
    } else {
        return false;
    }
    h = rd_get(r, off, 2);
    if (h == NULL) {
        return false;
    }
    n = be16(h);
    free(h);
    *recs = rd_get(r, off + 8, (size_t)n * 16);
    if (*recs == NULL) {
        return false;
    }
    *nrecs = n;
    return true;
}

/* RawFace::table (binary search over the records). */
static uint8_t *raw_table(const rd *r, const uint8_t *recs, uint16_t n, uint32_t tag, size_t *len)
{
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t t = be32(recs + mid * 16);
        if (t == tag) {
            size_t off = be32(recs + mid * 16 + 8);
            size_t l = be32(recs + mid * 16 + 12);
            *len = l;
            return rd_get(r, off, l);
        }
        if (t < tag) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- names (fontdb parse_names) */

static const uint16_t MAC_ROMAN[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5,
    0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4,
    0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6,
    0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265,
    0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF,
    0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5,
    0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044,
    0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9,
    0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

static bool put_utf8(pc_buf *b, uint32_t c)
{
    uint8_t u[4];
    size_t n;
    if (c < 0x80) {
        u[0] = (uint8_t)c;
        n = 1;
    } else if (c < 0x800) {
        u[0] = (uint8_t)(0xC0 | c >> 6);
        u[1] = (uint8_t)(0x80 | (c & 0x3F));
        n = 2;
    } else if (c < 0x10000) {
        u[0] = (uint8_t)(0xE0 | c >> 12);
        u[1] = (uint8_t)(0x80 | (c >> 6 & 0x3F));
        u[2] = (uint8_t)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        u[0] = (uint8_t)(0xF0 | c >> 18);
        u[1] = (uint8_t)(0x80 | (c >> 12 & 0x3F));
        u[2] = (uint8_t)(0x80 | (c >> 6 & 0x3F));
        u[3] = (uint8_t)(0x80 | (c & 0x3F));
        n = 4;
    }
    return pc_buf_append(b, u, n, NULL) == PC_OK;
}

typedef struct nrec {
    uint16_t platform, encoding, language, name_id;
    const uint8_t *s;
    size_t len;
} nrec;

typedef struct ntable {
    const uint8_t *recs;
    size_t count; /* records readable as a NamesIter yields them */
    const uint8_t *storage;
    size_t slen;
} ntable;

static bool ntable_parse(const uint8_t *d, size_t n, ntable *t)
{
    size_t off = 6;
    uint16_t version;
    uint16_t count;
    size_t storage;
    if (n < 6) {
        return false;
    }
    version = be16(d);
    count = be16(d + 2);
    storage = be16(d + 4);
    if (version == 1) {
        uint32_t lt;
        if (n < 8) {
            return false;
        }
        lt = (uint32_t)be16(d + 6) * 4u;
        if (lt > 0xFFFFu) {
            return false;
        }
        off = 8 + lt;
    } else if (version != 0) {
        return false;
    }
    if (off > n || (size_t)count * 12 > n - off) {
        return false;
    }
    t->recs = d + off;
    t->count = count;
    off += (size_t)count * 12;
    if (off < storage) {
        off = storage;
    }
    if (off > n) {
        return false;
    }
    t->storage = d + off;
    t->slen = n - off;
    return true;
}

/* Names::get; false ends the iteration (NamesIter yields None). */
static bool ntable_get(const ntable *t, size_t i, nrec *r)
{
    const uint8_t *p = t->recs + i * 12;
    size_t so;
    r->platform = be16(p);
    if (r->platform > 4) {
        return false;
    }
    r->encoding = be16(p + 2);
    r->language = be16(p + 4);
    r->name_id = be16(p + 6);
    r->len = be16(p + 8);
    so = be16(p + 10);
    if (so > t->slen || r->len > t->slen - so) {
        return false;
    }
    r->s = t->storage + so;
    return true;
}

static bool is_unicode_name(const nrec *r)
{
    return r->platform == 0 || (r->platform == 3 && (r->encoding == 0 || r->encoding == 1));
}

static bool is_mac_roman(const nrec *r)
{
    return r->platform == 1 && r->encoding == 0;
}

static bool is_en_us(const nrec *r)
{
    return (r->platform == 3 && r->language == 0x0409) ||
           (r->platform == 1 && r->encoding == 0 && r->language == 0);
}

/* name_to_unicode into a malloc'd string; NULL for an unsupported encoding or bad UTF-16. */
static char *name_to_unicode(const nrec *r)
{
    pc_buf b;
    size_t i;
    pc_buf_init(&b);
    if (is_unicode_name(r)) {
        size_t n = r->len / 2;
        for (i = 0; i < n; i++) {
            uint32_t c = be16(r->s + i * 2);
            if (c >= 0xD800 && c < 0xDC00) {
                uint32_t lo;
                if (i + 1 >= n) {
                    pc_buf_fini(&b);
                    return NULL;
                }
                lo = be16(r->s + (i + 1) * 2);
                if (lo < 0xDC00 || lo > 0xDFFF) {
                    pc_buf_fini(&b);
                    return NULL;
                }
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            } else if (c >= 0xDC00 && c <= 0xDFFF) {
                pc_buf_fini(&b);
                return NULL;
            }
            if (!put_utf8(&b, c)) {
                pc_buf_fini(&b);
                return NULL;
            }
        }
    } else if (is_mac_roman(r)) {
        for (i = 0; i < r->len; i++) {
            uint8_t c = r->s[i];
            uint32_t u = c < 0x80 ? c : MAC_ROMAN[c - 0x80];
            if (c == 0x11) {
                u = 0x2318;
            } else if (c == 0x12) {
                u = 0x21E7;
            } else if (c == 0x13) {
                u = 0x2325;
            } else if (c == 0x14) {
                u = 0x2303;
            }
            if (!put_utf8(&b, u)) {
                pc_buf_fini(&b);
                return NULL;
            }
        }
    } else {
        return NULL;
    }
    if (pc_buf_append_byte(&b, 0, NULL) != PC_OK) {
        pc_buf_fini(&b);
        return NULL;
    }
    return (char *)pc_buf_take(&b, NULL);
}

typedef struct famlist {
    char **v;
    bool *en;
    size_t n, cap;
} famlist;

static void famlist_fini(famlist *f)
{
    size_t i;
    for (i = 0; i < f->n; i++) {
        free(f->v[i]);
    }
    free(f->v);
    free(f->en);
    memset(f, 0, sizeof *f);
}

static bool famlist_push(famlist *f, char *s, bool en)
{
    if (f->n == f->cap) {
        size_t nc = f->cap < 4 ? 4 : f->cap * 2;
        char **v = realloc(f->v, nc * sizeof *v);
        bool *e;
        if (v == NULL) {
            return false;
        }
        f->v = v;
        e = realloc(f->en, nc * sizeof *e);
        if (e == NULL) {
            return false;
        }
        f->en = e;
        f->cap = nc;
    }
    f->v[f->n] = s;
    f->en[f->n] = en;
    f->n++;
    return true;
}

static bool collect_families(const ntable *t, uint16_t id, famlist *out)
{
    size_t i;
    bool any_en = false;
    nrec r;
    for (i = 0; i < t->count && ntable_get(t, i, &r); i++) {
        if (r.name_id == id && is_unicode_name(&r)) {
            char *s = name_to_unicode(&r);
            if (s != NULL) {
                bool en = is_en_us(&r);
                if (!famlist_push(out, s, en)) {
                    free(s);
                    return false;
                }
                any_en = any_en || en;
            }
        }
    }
    if (!any_en) {
        for (i = 0; i < t->count && ntable_get(t, i, &r); i++) {
            if (r.name_id == id && is_mac_roman(&r)) {
                char *s = name_to_unicode(&r);
                if (s != NULL) {
                    if (!famlist_push(out, s, is_en_us(&r))) {
                        free(s);
                        return false;
                    }
                    break;
                }
            }
        }
    }
    return true;
}

/* parse_names: families (English US first) and whether a PostScript name exists. */
static bool parse_names(const uint8_t *d, size_t n, famlist *fams)
{
    ntable t;
    size_t i;
    nrec r;
    if (!ntable_parse(d, n, &t)) {
        return false;
    }
    if (!collect_families(&t, 16, fams)) {
        return false;
    }
    if (fams->n == 0 && !collect_families(&t, 1, fams)) {
        return false;
    }
    if (fams->n > 1) {
        for (i = 0; i < fams->n; i++) {
            if (fams->en[i]) {
                if (i != 0) {
                    char *s = fams->v[0];
                    bool e = fams->en[0];
                    fams->v[0] = fams->v[i];
                    fams->en[0] = fams->en[i];
                    fams->v[i] = s;
                    fams->en[i] = e;
                }
                break;
            }
        }
    }
    if (fams->n == 0) {
        return false;
    }
    for (i = 0; i < t.count && ntable_get(&t, i, &r); i++) {
        if (r.name_id == 6 && (is_unicode_name(&r) || is_mac_roman(&r))) {
            char *ps = name_to_unicode(&r);
            free(ps);
            return ps != NULL;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- faces */

static bool push_src(svf_db *db, char *path, const uint8_t *data, size_t len, size_t *idx)
{
    if (db->nsrcs == db->capsrcs) {
        size_t nc = db->capsrcs < 64 ? 64 : db->capsrcs * 2;
        src *v = realloc(db->srcs, nc * sizeof *v);
        if (v == NULL) {
            return false;
        }
        db->srcs = v;
        db->capsrcs = nc;
    }
    db->srcs[db->nsrcs].path = path;
    db->srcs[db->nsrcs].data = data;
    db->srcs[db->nsrcs].len = len;
    db->srcs[db->nsrcs].tried = data != NULL;
    *idx = db->nsrcs++;
    return true;
}

static bool push_face(svf_db *db, const svf_face *f)
{
    if (db->nfaces == db->capfaces) {
        size_t nc = db->capfaces < 64 ? 64 : db->capfaces * 2;
        svf_face *v = realloc(db->faces, nc * sizeof *v);
        if (v == NULL) {
            return false;
        }
        db->faces = v;
        db->capfaces = nc;
    }
    db->faces[db->nfaces++] = *f;
    return true;
}

/* parse_face_info for face `index`; false when the face is skipped (or out of memory: *nomem). */
static bool face_info(const rd *r, uint32_t index, svf_face *out, bool *nomem)
{
    uint8_t *recs = NULL;
    uint16_t nrecs = 0;
    uint8_t *t;
    size_t tl = 0;
    famlist fams = {NULL, NULL, 0, 0};
    bool italic;
    if (!raw_face(r, index, &recs, &nrecs)) {
        return false;
    }
    t = raw_table(r, recs, nrecs, PC_SFNT_TAG('n', 'a', 'm', 'e'), &tl);
    if (t == NULL || !parse_names(t, tl, &fams)) {
        free(t);
        free(recs);
        famlist_fini(&fams);
        return false;
    }
    free(t);
    memset(out, 0, sizeof *out);
    out->index = index;
    out->style = SVF_STYLE_NORMAL;
    out->weight = 400;
    out->stretch = 5;
    t = raw_table(r, recs, nrecs, PC_SFNT_TAG('O', 'S', '/', '2'), &tl);
    if (t != NULL && tl >= 2) {
        static const size_t LENS[6] = {78, 86, 96, 96, 96, 100};
        uint16_t v = be16(t);
        if (v <= 5 && tl >= LENS[v]) {
            uint16_t fs = be16(t + 62);
            uint16_t wc = be16(t + 6);
            out->weight = be16(t + 4);
            out->stretch = wc >= 1 && wc <= 9 ? (uint8_t)wc : 5;
            out->style = (fs & 1u) != 0                 ? SVF_STYLE_ITALIC
                         : v >= 4 && (fs & 0x200u) != 0 ? SVF_STYLE_OBLIQUE
                                                        : SVF_STYLE_NORMAL;
        }
    }
    free(t);
    t = raw_table(r, recs, nrecs, PC_SFNT_TAG('p', 'o', 's', 't'), &tl);
    if (t != NULL) {
        out->monospaced = !(tl >= 16 && be32(t + 12) == 0);
        italic = !(tl >= 8 && be32(t + 4) == 0);
    } else {
        out->monospaced = false;
        italic = false;
    }
    free(t);
    free(recs);
    if (out->style == SVF_STYLE_NORMAL && italic) {
        out->style = SVF_STYLE_ITALIC;
    }
    out->families = fams.v;
    out->nfamilies = fams.n;
    free(fams.en);
    (void)nomem;
    return true;
}

static void free_face(svf_face *f)
{
    size_t i;
    for (i = 0; i < f->nfamilies; i++) {
        free(f->families[i]);
    }
    free(f->families);
}

/* load_fonts_from_file / load_font_source: every face of one file or buffer. */
static bool load_source(svf_db *db, char *path, const uint8_t *mem, size_t len)
{
    rd r;
    struct stat st;
    uint32_t nfaces = 1;
    uint32_t i;
    size_t si;
    bool any = false;
    r.fd = -1;
    r.mem = mem;
    r.size = len;
    if (mem == NULL) {
        r.fd = open(path, O_RDONLY | O_CLOEXEC);
        if (r.fd < 0) {
            free(path);
            return true;
        }
        if (fstat(r.fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            (void)close(r.fd);
            free(path);
            return true;
        }
        r.size = (size_t)st.st_size;
    }
    {
        uint8_t *h = rd_get(&r, 0, 12);
        if (h != NULL && be32(h) == 0x74746366u) {
            nfaces = be32(h + 8);
            if (nfaces > MAX_FACES_PER_FILE) {
                nfaces = MAX_FACES_PER_FILE;
            }
        }
        free(h);
    }
    if (!push_src(db, path, mem, len, &si)) {
        if (r.fd >= 0) {
            (void)close(r.fd);
        }
        free(path);
        return false;
    }
    for (i = 0; i < nfaces; i++) {
        svf_face f;
        bool nomem = false;
        if (face_info(&r, i, &f, &nomem)) {
            f.src = si;
            if (!push_face(db, &f)) {
                free_face(&f);
                if (r.fd >= 0) {
                    (void)close(r.fd);
                }
                return false;
            }
            any = true;
        }
    }
    (void)any;
    if (r.fd >= 0) {
        (void)close(r.fd);
    }
    return true;
}

/* ---------------------------------------------------------------- directory walk */

typedef struct seenset {
    char **v;
    size_t n, cap;
} seenset;

static bool seen_has(const seenset *s, const char *p)
{
    size_t i;
    for (i = 0; i < s->n; i++) {
        if (strcmp(s->v[i], p) == 0) {
            return true;
        }
    }
    return false;
}

static bool seen_add(seenset *s, const char *p)
{
    char *d;
    if (s->n == s->cap) {
        size_t nc = s->cap < 64 ? 64 : s->cap * 2;
        char **v = realloc(s->v, nc * sizeof *v);
        if (v == NULL) {
            return false;
        }
        s->v = v;
        s->cap = nc;
    }
    d = malloc(strlen(p) + 1);
    if (d == NULL) {
        return false;
    }
    strcpy(d, p);
    s->v[s->n++] = d;
    return true;
}

static void seen_fini(seenset *s)
{
    size_t i;
    for (i = 0; i < s->n; i++) {
        free(s->v[i]);
    }
    free(s->v);
}

static bool font_extension(const char *path)
{
    static const char *const EXT[8] = {"ttf", "ttc", "TTF", "TTC", "otf", "otc", "OTF", "OTC"};
    const char *name = strrchr(path, '/');
    const char *dot;
    size_t i;
    name = name != NULL ? name + 1 : path;
    dot = strrchr(name, '.');
    if (dot == NULL || dot == name) {
        return false;
    }
    for (i = 0; i < 8; i++) {
        if (strcmp(dot + 1, EXT[i]) == 0) {
            return true;
        }
    }
    return false;
}

typedef struct walk {
    svf_db *db;
    seenset seen;
    size_t files;
    bool nomem;
} walk;

/* Database::load_fonts_dir_impl with Database::canonicalize. */
static void walk_dir(walk *w, const char *dir, unsigned depth)
{
    DIR *d;
    struct dirent *e;
    if (depth > MAX_WALK_DEPTH || w->nomem) {
        return;
    }
    d = opendir(dir);
    if (d == NULL) {
        return;
    }
    while (!w->nomem && w->files < SVF_MAX_FILES && (e = readdir(d)) != NULL) {
        size_t ld;
        size_t ln;
        char *path;
        struct stat ls;
        bool is_file;
        bool is_dir;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        ld = strlen(dir);
        ln = strlen(e->d_name);
        path = malloc(ld + ln + 2);
        if (path == NULL) {
            w->nomem = true;
            break;
        }
        memcpy(path, dir, ld);
        if (ld > 0 && dir[ld - 1] != '/') {
            path[ld++] = '/';
        }
        memcpy(path + ld, e->d_name, ln + 1);
        if (lstat(path, &ls) != 0) {
            free(path);
            continue;
        }
        if (!S_ISLNK(ls.st_mode)) {
            if (w->seen.n > 0) {
                if (seen_has(&w->seen, path)) {
                    free(path);
                    continue;
                }
                if (!seen_add(&w->seen, path)) {
                    w->nomem = true;
                    free(path);
                    break;
                }
            }
            is_file = S_ISREG(ls.st_mode);
            is_dir = S_ISDIR(ls.st_mode);
        } else {
            struct stat st;
            char canon[PATH_MAX];
            if (stat(path, &st) != 0 || realpath(path, canon) == NULL) {
                free(path);
                continue;
            }
            free(path);
            if (seen_has(&w->seen, canon)) {
                continue;
            }
            if (!seen_add(&w->seen, canon)) {
                w->nomem = true;
                break;
            }
            path = malloc(strlen(canon) + 1);
            if (path == NULL) {
                w->nomem = true;
                break;
            }
            strcpy(path, canon);
            is_file = S_ISREG(st.st_mode);
            is_dir = S_ISDIR(st.st_mode);
        }
        if (is_file) {
            if (font_extension(path)) {
                w->files++;
                if (!load_source(w->db, path, NULL, 0)) {
                    w->nomem = true;
                }
            } else {
                free(path);
            }
        } else if (is_dir) {
            walk_dir(w, path, depth + 1);
            free(path);
        } else {
            free(path);
        }
    }
    (void)closedir(d);
}

static char *dupcat(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char *r = malloc(la + lb + 1);
    if (r != NULL) {
        memcpy(r, a, la);
        memcpy(r + la, b, lb + 1);
    }
    return r;
}

static void load_system(svf_db *db)
{
    svf_fc fc;
    walk w;
    size_t i;
    memset(&w, 0, sizeof w);
    w.db = db;
    if (svf_fc_load(&fc)) {
        char **slots[5] = {&fc.serif, &fc.sans, &fc.cursive, &fc.fantasy, &fc.mono};
        for (i = 0; i < 5; i++) {
            if (*slots[i] != NULL) {
                free(db->generic[i]);
                db->generic[i] = *slots[i];
                *slots[i] = NULL;
            }
        }
        for (i = 0; i < fc.ndirs && !w.nomem; i++) {
            walk_dir(&w, fc.dirs[i], 0);
        }
    } else {
        const char *h = getenv("HOME");
        char **slots[5] = {&fc.serif, &fc.sans, &fc.cursive, &fc.fantasy, &fc.mono};
        for (i = 0; i < 5; i++) {
            if (*slots[i] != NULL) {
                free(db->generic[i]);
                db->generic[i] = *slots[i];
                *slots[i] = NULL;
            }
        }
        walk_dir(&w, "/usr/share/fonts/", 0);
        walk_dir(&w, "/usr/local/share/fonts/", 0);
        if (h != NULL) {
            char *p = dupcat(h, "/.fonts");
            if (p != NULL) {
                walk_dir(&w, p, 0);
                free(p);
            }
            p = dupcat(h, "/.local/share/fonts");
            if (p != NULL) {
                walk_dir(&w, p, 0);
                free(p);
            }
        }
    }
    svf_fc_fini(&fc);
    seen_fini(&w.seen);
}

/* ---------------------------------------------------------------- the process-wide database */

static svf_db *g_db;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void build(void)
{
    static const char *const DEF[5] = {"Times New Roman", "Arial", "Comic Sans MS", "Impact", "Courier New"};
    svf_db *db = calloc(1, sizeof *db);
    const pc_bundled_font *bf;
    size_t nb = 0;
    size_t i;
    const char *no = getenv("PHOTOCRAFT_NO_SYSTEM_FONTS");
    if (db == NULL) {
        return;
    }
    if (pthread_mutex_init(&db->mu, NULL) != 0) {
        free(db);
        return;
    }
    for (i = 0; i < 5; i++) {
        db->generic[i] = dupcat(DEF[i], "");
        if (db->generic[i] == NULL) {
            return; /* leaked on purpose: g_db stays NULL */
        }
    }
    if (no == NULL || no[0] == '\0') {
        load_system(db);
    }
    bf = pc_text_bundled_fonts(&nb);
    for (i = 0; i < nb; i++) {
        (void)load_source(db, NULL, bf[i].data, bf[i].len);
    }
    db->hb = calloc(db->nfaces > 0 ? db->nfaces : 1, sizeof *db->hb);
    db->cmap = calloc(db->nfaces > 0 ? db->nfaces : 1, sizeof *db->cmap);
    db->cmap_len = calloc(db->nfaces > 0 ? db->nfaces : 1, sizeof *db->cmap_len);
    db->cmap_tried = calloc(db->nfaces > 0 ? db->nfaces : 1, sizeof *db->cmap_tried);
    if (db->hb == NULL || db->cmap == NULL || db->cmap_len == NULL || db->cmap_tried == NULL) {
        return;
    }
    g_db = db;
}

const svf_db *svf_db_get(void)
{
    (void)pthread_once(&g_once, build);
    return g_db;
}

size_t svf_db_len(const svf_db *db)
{
    return db != NULL ? db->nfaces : 0;
}

const svf_face *svf_db_face(const svf_db *db, size_t id)
{
    return db != NULL && id < db->nfaces ? &db->faces[id] : NULL;
}

const char *svf_db_generic(const svf_db *db, svf_kind kind)
{
    switch (kind) {
    case SVF_SERIF:
        return db->generic[0];
    case SVF_SANS:
        return db->generic[1];
    case SVF_CURSIVE:
        return db->generic[2];
    case SVF_FANTASY:
        return db->generic[3];
    case SVF_MONO:
        return db->generic[4];
    case SVF_NAMED:
    default:
        return "";
    }
}

/* find_best_match over the candidate faces (indices into db->faces); the chosen index. */
static size_t best_match(const svf_db *db, size_t *cand, size_t n, uint16_t weight, uint8_t stretch,
                         svf_style style)
{
    size_t i;
    size_t k;
    uint8_t ms;
    svf_style pref[3];
    svf_style mstyle = SVF_STYLE_NORMAL;
    bool found;
    uint16_t mw;
    /* 4a: font-stretch */
    found = false;
    for (i = 0; i < n; i++) {
        found = found || db->faces[cand[i]].stretch == stretch;
    }
    if (found) {
        ms = stretch;
    } else {
        size_t best = SIZE_MAX;
        int bd = 0;
        bool narrower_first = stretch <= 5;
        for (i = 0; i < n; i++) {
            int s = db->faces[cand[i]].stretch;
            int d = narrower_first ? (int)stretch - s : s - (int)stretch;
            if (d > 0 && (best == SIZE_MAX || d < bd)) {
                best = i;
                bd = d;
            }
        }
        if (best == SIZE_MAX) {
            for (i = 0; i < n; i++) {
                int s = db->faces[cand[i]].stretch;
                int d = narrower_first ? s - (int)stretch : (int)stretch - s;
                if (best == SIZE_MAX || d < bd) {
                    best = i;
                    bd = d;
                }
            }
        }
        ms = db->faces[cand[best]].stretch;
    }
    for (i = 0, k = 0; i < n; i++) {
        if (db->faces[cand[i]].stretch == ms) {
            cand[k++] = cand[i];
        }
    }
    n = k;
    /* 4b: font-style */
    if (style == SVF_STYLE_ITALIC) {
        pref[0] = SVF_STYLE_ITALIC;
        pref[1] = SVF_STYLE_OBLIQUE;
        pref[2] = SVF_STYLE_NORMAL;
    } else if (style == SVF_STYLE_OBLIQUE) {
        pref[0] = SVF_STYLE_OBLIQUE;
        pref[1] = SVF_STYLE_ITALIC;
        pref[2] = SVF_STYLE_NORMAL;
    } else {
        pref[0] = SVF_STYLE_NORMAL;
        pref[1] = SVF_STYLE_OBLIQUE;
        pref[2] = SVF_STYLE_ITALIC;
    }
    found = false;
    for (k = 0; k < 3 && !found; k++) {
        for (i = 0; i < n; i++) {
            if (db->faces[cand[i]].style == pref[k]) {
                mstyle = pref[k];
                found = true;
                break;
            }
        }
    }
    if (!found) {
        return SIZE_MAX;
    }
    for (i = 0, k = 0; i < n; i++) {
        if (db->faces[cand[i]].style == mstyle) {
            cand[k++] = cand[i];
        }
    }
    n = k;
    /* 4c: font-weight */
    found = false;
    for (i = 0; i < n; i++) {
        found = found || db->faces[cand[i]].weight == weight;
    }
    if (found) {
        mw = weight;
    } else {
        bool has500 = false;
        bool has400 = false;
        for (i = 0; i < n; i++) {
            has500 = has500 || db->faces[cand[i]].weight == 500;
            has400 = has400 || db->faces[cand[i]].weight == 400;
        }
        if (weight >= 400 && weight < 450 && has500) {
            mw = 500;
        } else if (weight >= 450 && weight <= 500 && has400) {
            mw = 400;
        } else {
            size_t best = SIZE_MAX;
            int bd = 0;
            bool thinner_first = weight <= 500;
            for (i = 0; i < n; i++) {
                int w = db->faces[cand[i]].weight;
                int d = thinner_first ? (int)weight - w : w - (int)weight;
                if (d >= 0 && (best == SIZE_MAX || d < bd)) {
                    best = i;
                    bd = d;
                }
            }
            if (best == SIZE_MAX) {
                for (i = 0; i < n; i++) {
                    int w = db->faces[cand[i]].weight;
                    int d = thinner_first ? w - (int)weight : (int)weight - w;
                    if (best == SIZE_MAX || d < bd) {
                        best = i;
                        bd = d;
                    }
                }
            }
            if (best == SIZE_MAX) {
                return SIZE_MAX;
            }
            mw = db->faces[cand[best]].weight;
        }
    }
    for (i = 0; i < n; i++) {
        if (db->faces[cand[i]].weight == mw) {
            return cand[i];
        }
    }
    return SIZE_MAX;
}

size_t svf_db_query(const svf_db *db, const svf_family *fams, size_t nfams, uint16_t weight, uint8_t stretch,
                    svf_style style)
{
    size_t *cand;
    size_t f;
    size_t r = SIZE_MAX;
    if (db == NULL || db->nfaces == 0) {
        return SIZE_MAX;
    }
    cand = malloc(db->nfaces * sizeof *cand);
    if (cand == NULL) {
        return SIZE_MAX;
    }
    for (f = 0; f < nfams && r == SIZE_MAX; f++) {
        const char *name = fams[f].kind == SVF_NAMED ? fams[f].name : svf_db_generic(db, fams[f].kind);
        size_t n = 0;
        size_t i;
        for (i = 0; i < db->nfaces; i++) {
            size_t k;
            for (k = 0; k < db->faces[i].nfamilies; k++) {
                if (strcmp(db->faces[i].families[k], name) == 0) {
                    cand[n++] = i;
                    break;
                }
            }
        }
        if (n > 0) {
            r = best_match(db, cand, n, weight, stretch, style);
        }
    }
    free(cand);
    return r;
}

/* The source's bytes, read once (callers hold db->mu). */
static bool src_bytes(svf_db *db, src *s)
{
    if (!s->tried) {
        pc_buf b;
        s->tried = true;
        pc_buf_init(&b);
        if (s->path != NULL && pc_file_read(s->path, SVF_MAX_FILE_BYTES, &b, NULL) == PC_OK && b.len > 0) {
            size_t n = 0;
            s->data = pc_buf_take(&b, &n);
            s->len = n;
        } else {
            pc_buf_fini(&b);
        }
    }
    (void)db;
    return s->data != NULL;
}

bool svf_db_data(const svf_db *cdb, size_t id, const uint8_t **data, size_t *len, uint32_t *index)
{
    svf_db *db = (svf_db *)(uintptr_t)cdb;
    bool ok;
    src *s;
    if (db == NULL || id >= db->nfaces) {
        return false;
    }
    s = &db->srcs[db->faces[id].src];
    (void)pthread_mutex_lock(&db->mu);
    ok = src_bytes(db, s);
    (void)pthread_mutex_unlock(&db->mu);
    if (ok) {
        *data = s->data;
        *len = s->len;
        *index = db->faces[id].index;
    }
    return ok;
}

void *svf_db_hb_face(const svf_db *cdb, size_t id)
{
    svf_db *db = (svf_db *)(uintptr_t)cdb;
    hb_face_t *f = NULL;
    if (db == NULL || id >= db->nfaces) {
        return NULL;
    }
    (void)pthread_mutex_lock(&db->mu);
    if (db->hb[id] == NULL) {
        src *s = &db->srcs[db->faces[id].src];
        if (src_bytes(db, s) && s->len <= UINT_MAX) {
            hb_blob_t *blob =
                hb_blob_create((const char *)s->data, (unsigned)s->len, HB_MEMORY_MODE_READONLY, NULL, NULL);
            hb_face_t *hf = hb_face_create(blob, db->faces[id].index);
            hb_blob_destroy(blob);
            if (hf != hb_face_get_empty()) {
                hb_face_make_immutable(hf);
                db->hb[id] = hf;
            } else {
                hb_face_destroy(hf);
            }
        }
    }
    f = db->hb[id];
    (void)pthread_mutex_unlock(&db->mu);
    return f;
}

/* ttf-parser Face::glyph_index: the first Unicode subtable (in table order) that maps c. */
static bool cmap_has(const uint8_t *cm, size_t n, uint32_t c)
{
    uint16_t count;
    size_t i;
    if (n < 4) {
        return false;
    }
    count = be16(cm + 2);
    for (i = 0; i < count && 4 + i * 8 + 8 <= n; i++) {
        const uint8_t *r = cm + 4 + i * 8;
        uint16_t pid = be16(r);
        uint16_t eid = be16(r + 2);
        size_t off = be32(r + 4);
        pc_sfnt_cmap sub;
        uint16_t format;
        bool unicode;
        if (off + 2 > n) {
            continue;
        }
        format = be16(cm + off);
        unicode =
            pid == 0 || (pid == 3 && eid == 1) || (pid == 3 && eid == 10 && (format == 12 || format == 13));
        if (!unicode) {
            continue;
        }
        sub.sub = cm + off;
        sub.len = n - off;
        sub.format = format;
        sub.symbol = false;
        sub.glyph_count = 65535;
        if (pc_sfnt_cmap_map_any(&sub, c) != 0) {
            return true;
        }
    }
    return false;
}

bool svf_has_char(const svf_db *cdb, size_t id, uint32_t c)
{
    svf_db *db = (svf_db *)(uintptr_t)cdb;
    bool r = false;
    if (db == NULL || id >= db->nfaces) {
        return false;
    }
    (void)pthread_mutex_lock(&db->mu);
    if (!db->cmap_tried[id]) {
        src *s = &db->srcs[db->faces[id].src];
        rd rr;
        uint8_t *recs = NULL;
        uint16_t nrecs = 0;
        struct stat st;
        db->cmap_tried[id] = true;
        rr.fd = -1;
        rr.mem = s->data;
        rr.size = s->len;
        if (s->data == NULL && s->path != NULL) {
            rr.fd = open(s->path, O_RDONLY | O_CLOEXEC);
            rr.size = rr.fd >= 0 && fstat(rr.fd, &st) == 0 && st.st_size > 0 ? (size_t)st.st_size : 0;
        }
        if (rr.size > 0 && raw_face(&rr, db->faces[id].index, &recs, &nrecs)) {
            size_t tl = 0;
            db->cmap[id] = raw_table(&rr, recs, nrecs, PC_SFNT_TAG('c', 'm', 'a', 'p'), &tl);
            db->cmap_len[id] = db->cmap[id] != NULL ? tl : 0;
            free(recs);
        }
        if (rr.fd >= 0) {
            (void)close(rr.fd);
        }
    }
    if (db->cmap[id] != NULL) {
        r = cmap_has(db->cmap[id], db->cmap_len[id], c);
    }
    (void)pthread_mutex_unlock(&db->mu);
    return r;
}
