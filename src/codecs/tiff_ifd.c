#include "codecs/tiff_ifd.h"

#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "codecs/internal.h"

#define MAX_ENTRIES 65535u
#define MAX_SUB_DEPTH 4
#define MAX_SUBIFDS 256u

size_t tiff_type_size(uint16_t ty)
{
    switch (ty) {
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
    case 16:
    case 17:
    case 18:
        return 8;
    default:
        return 0;
    }
}

static const uint8_t *slice(const tiff_file *f, uint64_t at, uint64_t len)
{
    if (at > f->n || len > f->n - at) {
        return NULL;
    }
    return f->b + at;
}

static uint64_t rd(const tiff_file *f, const uint8_t *p, size_t k)
{
    uint64_t v = 0;
    size_t i;
    for (i = 0; i < k; i++) {
        v = (v << 8) | (f->le ? p[k - 1 - i] : p[i]);
    }
    return v;
}

bool tiff_u16_at(const tiff_file *f, uint64_t at, uint16_t *v)
{
    const uint8_t *p = slice(f, at, 2);
    if (p == NULL) {
        return false;
    }
    *v = (uint16_t)rd(f, p, 2);
    return true;
}

bool tiff_u32_at(const tiff_file *f, uint64_t at, uint32_t *v)
{
    const uint8_t *p = slice(f, at, 4);
    if (p == NULL) {
        return false;
    }
    *v = (uint32_t)rd(f, p, 4);
    return true;
}

bool tiff_u64_at(const tiff_file *f, uint64_t at, uint64_t *v)
{
    const uint8_t *p = slice(f, at, 8);
    if (p == NULL) {
        return false;
    }
    *v = rd(f, p, 8);
    return true;
}

bool tiff_file_parse(const uint8_t *b, size_t n, tiff_file *f)
{
    uint16_t version;
    if (n < 2) {
        return false;
    }
    if (b[0] == 'I' && b[1] == 'I') {
        f->le = true;
    } else if (b[0] == 'M' && b[1] == 'M') {
        f->le = false;
    } else {
        return false;
    }
    f->b = b;
    f->n = n;
    f->big = false;
    f->first_ifd = 0;
    if (!tiff_u16_at(f, 2, &version)) {
        return false;
    }
    if (version == 42) {
        uint32_t o;
        if (!tiff_u32_at(f, 4, &o)) {
            return false;
        }
        f->first_ifd = o;
    } else if (version == 43) {
        uint16_t a;
        uint16_t z;
        /* Bytesize of offsets (always 8) and a reserved zero. */
        if (!tiff_u16_at(f, 4, &a) || !tiff_u16_at(f, 6, &z) || a != 8 || z != 0 || !tiff_u64_at(f, 8, &f->first_ifd)) {
            return false;
        }
        f->big = true;
    } else {
        return false;
    }
    return true;
}

uint64_t tiff_header_len(const tiff_file *f) { return f->big ? 16 : 8; }

static bool add64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (b > UINT64_MAX - a) {
        return false;
    }
    *out = a + b;
    return true;
}

bool tiff_dir_read(const tiff_file *f, uint64_t at, tiff_dir *out)
{
    uint64_t n;
    uint64_t first;
    uint64_t entry = f->big ? 20 : 12;
    uint64_t inl = f->big ? 8 : 4;
    uint64_t next_at;
    uint64_t next;
    uint64_t i;
    tiff_entry *v;
    size_t k = 0;
    if (at < tiff_header_len(f)) {
        return false;
    }
    if (f->big) {
        if (!tiff_u64_at(f, at, &n) || !add64(at, 8, &first)) {
            return false;
        }
    } else {
        uint16_t c;
        if (!tiff_u16_at(f, at, &c) || !add64(at, 2, &first)) {
            return false;
        }
        n = c;
    }
    if (n > MAX_ENTRIES || !add64(first, n * entry, &next_at)) {
        return false;
    }
    if (f->big) {
        if (!tiff_u64_at(f, next_at, &next)) {
            return false;
        }
    } else {
        uint32_t x;
        if (!tiff_u32_at(f, next_at, &x)) {
            return false;
        }
        next = x;
    }
    v = n > 0 ? pc_malloc_array((size_t)n, sizeof *v) : NULL;
    if (n > 0 && v == NULL) {
        return false;
    }
    for (i = 0; i < n; i++) {
        uint64_t e = first + i * entry;
        uint16_t tag;
        uint16_t ty;
        uint64_t count;
        uint64_t field = e + (f->big ? 12 : 8);
        size_t size;
        if (!tiff_u16_at(f, e, &tag) || !tiff_u16_at(f, e + 2, &ty)) {
            free(v);
            return false;
        }
        if (f->big) {
            if (!tiff_u64_at(f, e + 4, &count)) {
                free(v);
                return false;
            }
        } else {
            uint32_t c;
            if (!tiff_u32_at(f, e + 4, &c)) {
                free(v);
                return false;
            }
            count = c;
        }
        /* Unknown types are skipped, as the specification says. */
        size = tiff_type_size(ty);
        if (size == 0) {
            continue;
        }
        v[k].tag = tag;
        v[k].ty = ty;
        v[k].count = count;
        if (count <= UINT64_MAX / size && count * size <= inl) {
            v[k].value_at = field;
        } else if (f->big) {
            if (!tiff_u64_at(f, field, &v[k].value_at)) {
                free(v);
                return false;
            }
        } else {
            uint32_t o;
            if (!tiff_u32_at(f, field, &o)) {
                free(v);
                return false;
            }
            v[k].value_at = o;
        }
        k++;
    }
    out->at = at;
    out->entries = v;
    out->n = k;
    out->next = next;
    out->next_at = next_at;
    return true;
}

void tiff_dir_fini(tiff_dir *d)
{
    if (d != NULL) {
        free(d->entries);
        d->entries = NULL;
        d->n = 0;
    }
}

const tiff_entry *tiff_dir_get(const tiff_dir *d, uint16_t tag)
{
    size_t i;
    for (i = 0; i < d->n; i++) {
        if (d->entries[i].tag == tag) {
            return &d->entries[i];
        }
    }
    return NULL;
}

const uint8_t *tiff_data(const tiff_file *f, const tiff_entry *e, size_t *len)
{
    size_t size = tiff_type_size(e->ty);
    const uint8_t *p;
    if (size == 0 || e->count > UINT64_MAX / size) {
        return NULL;
    }
    p = slice(f, e->value_at, e->count * size);
    if (p != NULL) {
        *len = (size_t)(e->count * size);
    }
    return p;
}

bool tiff_uint(const tiff_file *f, const tiff_entry *e, uint64_t i, uint64_t *v)
{
    size_t size = tiff_type_size(e->ty);
    uint64_t at;
    const uint8_t *p;
    if (i >= e->count || size == 0 || i > UINT64_MAX / size || !add64(e->value_at, i * size, &at)) {
        return false;
    }
    switch (e->ty) {
    case 1:
    case 3:
    case 4:
    case 13:
    case 16:
    case 18:
        p = slice(f, at, size);
        if (p == NULL) {
            return false;
        }
        *v = size == 1 ? p[0] : rd(f, p, size);
        return true;
    default:
        return false;
    }
}

bool tiff_tag_uint(const tiff_file *f, const tiff_dir *d, uint16_t tag, uint64_t *v)
{
    const tiff_entry *e = tiff_dir_get(d, tag);
    return e != NULL && tiff_uint(f, e, 0, v);
}

bool tiff_tag_uints(const tiff_file *f, const tiff_dir *d, uint16_t tag, uint64_t max, uint64_t **out, size_t *n)
{
    const tiff_entry *e = tiff_dir_get(d, tag);
    uint64_t k;
    uint64_t cnt;
    uint64_t *v;
    if (e == NULL) {
        return false;
    }
    cnt = e->count < max ? e->count : max;
    v = malloc(cnt > 0 ? (size_t)cnt * sizeof *v : 1);
    if (v == NULL) {
        return false;
    }
    for (k = 0; k < cnt; k++) {
        if (!tiff_uint(f, e, k, &v[k])) {
            free(v);
            return false;
        }
    }
    *out = v;
    *n = (size_t)cnt;
    return true;
}

bool tiff_tag_f64(const tiff_file *f, const tiff_dir *d, uint16_t tag, double *v)
{
    const tiff_entry *e = tiff_dir_get(d, tag);
    uint64_t u;
    if (e == NULL || e->count == 0) {
        return false;
    }
    switch (e->ty) {
    case 5: {
        uint32_t num;
        uint32_t den;
        if (!tiff_u32_at(f, e->value_at, &num) || !tiff_u32_at(f, e->value_at + 4, &den) || den == 0) {
            return false;
        }
        *v = (double)num / (double)den;
        return true;
    }
    case 11: {
        uint32_t bits;
        float x;
        if (!tiff_u32_at(f, e->value_at, &bits)) {
            return false;
        }
        memcpy(&x, &bits, 4);
        *v = (double)x;
        return true;
    }
    case 12: {
        uint64_t bits;
        if (!tiff_u64_at(f, e->value_at, &bits)) {
            return false;
        }
        memcpy(v, &bits, 8);
        return true;
    }
    default:
        if (!tiff_uint(f, e, 0, &u)) {
            return false;
        }
        *v = (double)u;
        return true;
    }
}

const uint8_t *tiff_tag_bytes(const tiff_file *f, const tiff_dir *d, uint16_t tag, size_t *len)
{
    const tiff_entry *e = tiff_dir_get(d, tag);
    if (e == NULL || !(e->ty == 1 || e->ty == 2 || e->ty == 6 || e->ty == 7)) {
        return NULL;
    }
    return tiff_data(f, e, len);
}

uint16_t tiff_orientation_of(const tiff_file *f, const tiff_dir *d)
{
    const tiff_entry *e = tiff_dir_get(d, TAG_ORIENTATION);
    uint64_t v;
    if (e == NULL || e->count != 1 || (e->ty != 3 && e->ty != 4) || !tiff_uint(f, e, 0, &v) || v < 1 || v > 8) {
        return 1;
    }
    return (uint16_t)v;
}

/* ---- pages ---- */

static pc_tiff_page describe(const tiff_file *f, const tiff_dir *d, bool has_parent, size_t parent)
{
    pc_tiff_page p;
    uint64_t newk = 0;
    uint64_t oldk = 1;
    uint64_t v;
    memset(&p, 0, sizeof p);
    (void)tiff_tag_uint(f, d, TAG_NEW_SUBFILE_TYPE, &newk);
    (void)tiff_tag_uint(f, d, TAG_SUBFILE_TYPE, &oldk);
    p.kind = (newk & 4) != 0 ? PC_TIFF_MASK : ((newk & 1) != 0 || oldk == 2) ? PC_TIFF_REDUCED : PC_TIFF_PAGE;
    p.ifd_offset = d->at;
    p.has_parent = has_parent;
    p.parent = parent;
    p.width = tiff_tag_uint(f, d, TAG_WIDTH, &v) && v <= UINT32_MAX ? (uint32_t)v : 0;
    p.height = tiff_tag_uint(f, d, TAG_HEIGHT, &v) && v <= UINT32_MAX ? (uint32_t)v : 0;
    p.samples_per_pixel = tiff_tag_uint(f, d, TAG_SAMPLES, &v) && v <= UINT16_MAX ? (uint16_t)v : 1;
    p.bits_per_sample = tiff_tag_uint(f, d, TAG_BITS, &v) && v <= UINT16_MAX ? (uint16_t)v : 1;
    p.sample_format = tiff_tag_uint(f, d, TAG_SAMPLE_FORMAT, &v) && v <= UINT16_MAX ? (uint16_t)v : 1;
    p.has_photometric = tiff_tag_uint(f, d, TAG_PHOTOMETRIC, &v) && v <= UINT16_MAX;
    p.photometric = p.has_photometric ? (uint16_t)v : 0;
    p.compression = tiff_tag_uint(f, d, TAG_COMPRESSION, &v) && v <= UINT16_MAX ? (uint16_t)v : 1;
    p.tiled = tiff_dir_get(d, TAG_TILE_OFFSETS) != NULL;
    p.planar = tiff_tag_uint(f, d, TAG_PLANAR, &v) && v == 2;
    return p;
}

typedef struct walk {
    const tiff_file *f;
    uint64_t *seen; /* sorted */
    size_t nseen;
    pc_tiff_page *pages;
    size_t n;
    size_t cap;
    bool complete;
    bool oom;
    /* Directory entries that may still be parsed. Real directory tables are disjoint, so their
     * total size is bounded by the file; overlapping tables (a hostile file) would otherwise make
     * every one of thousands of directories cost a full 65,535-entry parse. */
    uint64_t entry_budget;
} walk;

/* The declared entry count of the directory at `at`, before its table is parsed. */
static bool entry_count(const tiff_file *f, uint64_t at, uint64_t *n)
{
    uint16_t v16;
    if (at < tiff_header_len(f)) {
        return false;
    }
    if (f->big) {
        return tiff_u64_at(f, at, n);
    }
    if (!tiff_u16_at(f, at, &v16)) {
        return false;
    }
    *n = v16;
    return true;
}

/* Inserts at into the sorted seen-set; false when it was already there. */
static bool seen_insert(walk *w, uint64_t at)
{
    size_t lo = 0;
    size_t hi = w->nseen;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (w->seen[mid] == at) {
            return false;
        }
        if (w->seen[mid] < at) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    memmove(w->seen + lo + 1, w->seen + lo, (w->nseen - lo) * sizeof *w->seen);
    w->seen[lo] = at;
    w->nseen++;
    return true;
}

/* SubIFD offsets of a directory (tag 330 as LONG, IFD, LONG8 or IFD8). */
static size_t sub_ifds(const tiff_file *f, const tiff_dir *d, uint64_t *out)
{
    const tiff_entry *e = tiff_dir_get(d, TAG_SUB_IFDS);
    uint64_t i;
    size_t k = 0;
    if (e == NULL || !(e->ty == 4 || e->ty == 13 || e->ty == 16 || e->ty == 18)) {
        return 0;
    }
    for (i = 0; i < e->count && i < MAX_SUBIFDS; i++) {
        uint64_t v;
        if (!tiff_uint(f, e, i, &v)) {
            break;
        }
        if (v != 0) {
            out[k++] = v;
        }
    }
    return k;
}

static void chain(walk *w, uint64_t at, bool has_parent, size_t parent, int depth)
{
    while (at != 0) {
        tiff_dir d;
        size_t index;
        pc_tiff_page *g;
        uint64_t count;
        if (w->n >= TIFF_MAX_IFDS || !seen_insert(w, at)) {
            w->complete = false;
            return;
        }
        if (!entry_count(w->f, at, &count) || count > w->entry_budget) {
            w->complete = false;
            return;
        }
        w->entry_budget -= count;
        if (!tiff_dir_read(w->f, at, &d)) {
            w->complete = false;
            return;
        }
        g = pc_grow_array(w->pages, &w->cap, w->n + 1, sizeof *g);
        if (g == NULL) {
            tiff_dir_fini(&d);
            w->oom = true;
            w->complete = false;
            return;
        }
        w->pages = g;
        index = w->n;
        w->pages[w->n++] = describe(w->f, &d, has_parent, parent);
        if (depth < MAX_SUB_DEPTH) {
            uint64_t subs[MAX_SUBIFDS];
            size_t ns = sub_ifds(w->f, &d, subs);
            size_t i;
            for (i = 0; i < ns; i++) {
                chain(w, subs[i], true, index, depth + 1);
            }
        }
        at = d.next;
        tiff_dir_fini(&d);
    }
}

pc_status tiff_info_of(const tiff_file *f, pc_tiff_info *out, pc_error *err)
{
    walk w;
    memset(&w, 0, sizeof w);
    w.f = f;
    w.complete = true;
    w.entry_budget = (uint64_t)f->n / 12 + 64;
    w.seen = malloc((TIFF_MAX_IFDS + 1) * sizeof *w.seen);
    if (w.seen == NULL) {
        return cx_nomem(err);
    }
    chain(&w, f->first_ifd, false, 0, 0);
    free(w.seen);
    if (w.oom) {
        free(w.pages);
        return cx_nomem(err);
    }
    if (w.n == 0) {
        free(w.pages);
        return cx_malformed(err, PC_CF_TIFF, "the first image directory is missing or cut off");
    }
    out->big_tiff = f->big;
    out->little_endian = f->le;
    out->pages = w.pages;
    out->npages = w.n;
    out->complete = w.complete;
    return PC_OK;
}

void pc_tiff_info_fini(pc_tiff_info *i)
{
    if (i != NULL) {
        free(i->pages);
        memset(i, 0, sizeof *i);
    }
}

bool pc_tiff_info_default_page(const pc_tiff_info *info, size_t *page)
{
    size_t i;
    for (i = 0; i < info->npages; i++) {
        const pc_tiff_page *p = &info->pages[i];
        if (!p->has_parent && p->kind == PC_TIFF_PAGE && p->width > 0 && p->height > 0) {
            *page = i;
            return true;
        }
    }
    if (info->npages > 0) {
        *page = 0;
        return true;
    }
    return false;
}

size_t pc_tiff_info_page_count(const pc_tiff_info *info)
{
    size_t i;
    size_t c = 0;
    for (i = 0; i < info->npages; i++) {
        c += !info->pages[i].has_parent && info->pages[i].kind == PC_TIFF_PAGE;
    }
    return c;
}

pc_status pc_ctiff_info(const uint8_t *b, size_t n, pc_tiff_info *out, pc_error *err)
{
    tiff_file f;
    if (!tiff_file_parse(b, n, &f)) {
        return cx_malformed(err, PC_CF_TIFF, "not a TIFF or BigTIFF header");
    }
    return tiff_info_of(&f, out, err);
}

/* ---- EXIF directories of a TIFF stream (port of codecs::tiff_exif, used by the TIFF codec and
 * the camera-raw importer) ---- */

#define EXIF_TAG_EXIF_IFD 34665u
#define EXIF_TAG_GPS_IFD 34853u
#define EXIF_TAG_INTEROP_IFD 40965u
#define EXIF_TAG_PIXEL_X 40962u
#define EXIF_TAG_PIXEL_Y 40963u
#define EXIF_MAX_FIELDS 1024u
#define EXIF_MAX_BYTES (4u * 1024u * 1024u)

/* IFD0's descriptive tags: ImageDescription, Make, Model, Software, DateTime, Artist, Copyright. */
static const uint16_t EXIF_IFD0_TAGS[7] = {270, 271, 272, 305, 306, 315, 33432};

static bool exif_is_ifd0_tag(uint16_t t)
{
    for (size_t i = 0; i < 7; i++) {
        if (EXIF_IFD0_TAGS[i] == t) {
            return true;
        }
    }
    return false;
}

size_t cx_exif_element(uint16_t ty)
{
    switch (ty) {
    case 3:  /* SHORT */
    case 8:  /* SSHORT */
        return 2;
    case 4:  /* LONG */
    case 5:  /* RATIONAL (two LONGs) */
    case 9:  /* SLONG */
    case 10: /* SRATIONAL */
    case 11: /* FLOAT */
    case 13: /* IFD */
        return 4;
    case 12: /* DOUBLE */
    case 16: /* LONG8 */
    case 17: /* SLONG8 */
    case 18: /* IFD8 */
        return 8;
    default: /* BYTE, ASCII, UNDEFINED, SBYTE, ... */
        return 1;
    }
}

void cx_exif_reorder(uint16_t ty, const uint8_t *value, size_t vlen, bool from_le, bool to_le, uint8_t *out)
{
    size_t e = cx_exif_element(ty);
    if (from_le == to_le || e == 1) {
        memcpy(out, value, vlen);
        return;
    }
    for (size_t i = 0; i < vlen; i += e) {
        size_t m = vlen - i < e ? vlen - i : e;
        for (size_t k = 0; k < m; k++) {
            out[i + k] = value[i + m - 1 - k];
        }
    }
}

void cx_exif_dirs_fini(cx_exif_dirs *d)
{
    if (d == NULL) {
        return;
    }
    for (size_t i = 0; i < d->n_ifd0; i++) {
        free(d->ifd0[i].val);
    }
    for (size_t i = 0; i < d->n_exif; i++) {
        free(d->exif[i].val);
    }
    for (size_t i = 0; i < d->n_gps; i++) {
        free(d->gps[i].val);
    }
    free(d->ifd0);
    free(d->exif);
    free(d->gps);
    memset(d, 0, sizeof *d);
}

/* The entries of d that pass the filters, each with an owned copy of its value. budget is the
 * value bytes still allowed (shared across the stream's directories); an entry whose value does
 * not fit is skipped so later, smaller entries can still be kept. Returns a malloc'd array (NULL
 * with *n = 0 when empty, or on OOM with *oom set). */
static cx_exif_field *exif_collect(const tiff_file *f, const tiff_dir *d, size_t *budget, bool only_ifd0,
                                   bool drop_pixel, size_t *n, bool *oom)
{
    cx_exif_field *out;
    size_t m = 0;
    *n = 0;
    out = pc_malloc_array(EXIF_MAX_FIELDS, sizeof *out);
    if (out == NULL) {
        *oom = true;
        return NULL;
    }
    for (size_t i = 0; i < d->n && m < EXIF_MAX_FIELDS; i++) {
        const tiff_entry *e = &d->entries[i];
        size_t vlen = 0;
        const uint8_t *v;
        uint8_t *copy;
        if (e->tag == EXIF_TAG_EXIF_IFD || e->tag == EXIF_TAG_GPS_IFD || e->tag == EXIF_TAG_INTEROP_IFD) {
            continue;
        }
        if (only_ifd0 && !exif_is_ifd0_tag(e->tag)) {
            continue;
        }
        if (drop_pixel && (e->tag == EXIF_TAG_PIXEL_X || e->tag == EXIF_TAG_PIXEL_Y)) {
            continue;
        }
        if (e->count > UINT32_MAX) {
            continue;
        }
        v = tiff_data(f, e, &vlen);
        if (v == NULL || vlen > *budget) {
            continue;
        }
        copy = pc_malloc(vlen > 0 ? vlen : 1);
        if (copy == NULL) {
            *oom = true;
            for (size_t k = 0; k < m; k++) {
                free(out[k].val);
            }
            free(out);
            *n = 0;
            return NULL;
        }
        memcpy(copy, v, vlen);
        *budget -= vlen;
        out[m].tag = e->tag;
        out[m].ty = e->ty;
        out[m].count = (uint32_t)e->count;
        out[m].val = copy;
        out[m].vlen = vlen;
        m++;
    }
    if (m == 0) {
        free(out);
        return NULL;
    }
    *n = m;
    return out;
}

static bool exif_read_file(const tiff_file *f, bool drop_pixel, cx_exif_dirs *out)
{
    tiff_dir ifd0;
    size_t budget;
    uint64_t at;
    bool oom = false;
    memset(out, 0, sizeof *out);
    if (f->big) {
        return false; /* an EXIF block is classic TIFF only */
    }
    if (!tiff_dir_read(f, f->first_ifd, &ifd0)) {
        return false;
    }
    out->le = f->le;
    budget = f->n < EXIF_MAX_BYTES ? f->n : EXIF_MAX_BYTES;
    /* The small descriptive directories first, so a huge maker note cannot crowd out the GPS
     * position: IFD0's descriptive tags, then GPS, then Exif. */
    out->ifd0 = exif_collect(f, &ifd0, &budget, true, false, &out->n_ifd0, &oom);
    if (tiff_tag_uint(f, &ifd0, EXIF_TAG_GPS_IFD, &at) && at != ifd0.at) {
        tiff_dir gd;
        if (tiff_dir_read(f, at, &gd)) {
            out->gps = exif_collect(f, &gd, &budget, false, false, &out->n_gps, &oom);
            tiff_dir_fini(&gd);
        }
    }
    if (tiff_tag_uint(f, &ifd0, EXIF_TAG_EXIF_IFD, &at) && at != ifd0.at) {
        tiff_dir ed;
        if (tiff_dir_read(f, at, &ed)) {
            out->exif = exif_collect(f, &ed, &budget, false, drop_pixel, &out->n_exif, &oom);
            tiff_dir_fini(&ed);
        }
    }
    tiff_dir_fini(&ifd0);
    if (oom) {
        cx_exif_dirs_fini(out);
        return false;
    }
    return true;
}

bool cx_exif_read(const uint8_t *b, size_t n, bool drop_pixel_dims, cx_exif_dirs *out)
{
    tiff_file f;
    memset(out, 0, sizeof *out);
    if (!tiff_file_parse(b, n, &f)) {
        return false;
    }
    return exif_read_file(&f, drop_pixel_dims, out);
}

bool cx_exif_has_directories(const cx_exif_dirs *d)
{
    return d->n_exif > 0 || d->n_gps > 0;
}

static int exif_cmp(const void *a, const void *b)
{
    uint16_t ta = ((const cx_exif_field *)a)->tag;
    uint16_t tb = ((const cx_exif_field *)b)->tag;
    return ta < tb ? -1 : (ta > tb ? 1 : 0);
}

static size_t exif_pad2(size_t x) { return (x + 1) & ~(size_t)1; }

/* Byte size of a directory: table + next pointer + out-of-line (even-padded) values. */
static size_t exif_dir_size(const cx_exif_field *fs, size_t n)
{
    size_t s = 2 + 12 * n + 4;
    for (size_t i = 0; i < n; i++) {
        if (fs[i].vlen > 4) {
            s += exif_pad2(fs[i].vlen);
        }
    }
    return s;
}

/* Writes one directory (sorted by tag) and its out-of-line values at cursor *at in o, keeping the
 * stream's byte order le (values are copied verbatim; fs is sorted in place). */
static void exif_write_dir(uint8_t *o, size_t *at, bool le, cx_exif_field *fs, size_t n)
{
    size_t start = *at;
    size_t extra = start + 2 + 12 * n + 4;
    size_t p = start;
    qsort(fs, n, sizeof *fs, exif_cmp);
    if (le) {
        pc_store_le16(o + p, (uint16_t)n);
    } else {
        pc_store_be16(o + p, (uint16_t)n);
    }
    p += 2;
    for (size_t i = 0; i < n; i++) {
        if (le) {
            pc_store_le16(o + p, fs[i].tag);
            pc_store_le16(o + p + 2, fs[i].ty);
            pc_store_le32(o + p + 4, fs[i].count);
        } else {
            pc_store_be16(o + p, fs[i].tag);
            pc_store_be16(o + p + 2, fs[i].ty);
            pc_store_be32(o + p + 4, fs[i].count);
        }
        if (fs[i].vlen <= 4) {
            memset(o + p + 8, 0, 4);
            memcpy(o + p + 8, fs[i].val, fs[i].vlen);
        } else {
            if (le) {
                pc_store_le32(o + p + 8, (uint32_t)extra);
            } else {
                pc_store_be32(o + p + 8, (uint32_t)extra);
            }
            extra += exif_pad2(fs[i].vlen);
        }
        p += 12;
    }
    memset(o + p, 0, 4); /* next IFD = 0 */
    p += 4;
    for (size_t i = 0; i < n; i++) {
        if (fs[i].vlen > 4) {
            memcpy(o + p, fs[i].val, fs[i].vlen);
            p += fs[i].vlen;
            if (fs[i].vlen & 1) {
                o[p++] = 0;
            }
        }
    }
    *at = p;
}

pc_status cx_exif_block(const cx_exif_dirs *d, pc_buf *out, pc_error *err)
{
    bool le = d->le;
    cx_exif_field *f0;
    cx_exif_field *fe = NULL;
    cx_exif_field *fg = NULL;
    uint8_t ptr_exif[4] = {0, 0, 0, 0};
    uint8_t ptr_gps[4] = {0, 0, 0, 0};
    size_t n0 = d->n_ifd0;
    size_t at0 = 8;
    size_t at_exif;
    size_t at_gps;
    size_t total;
    uint8_t *buf;
    size_t cur;
    pc_status st;
    /* IFD0 fields plus room for the two sub-directory pointers (shallow copies share val). */
    f0 = pc_malloc_array(n0 + 2, sizeof *f0);
    if (f0 == NULL) {
        return cx_nomem(err);
    }
    if (n0 > 0) {
        memcpy(f0, d->ifd0, n0 * sizeof *f0);
    }
    if (d->n_exif > 0) {
        fe = pc_malloc_array(d->n_exif, sizeof *fe);
        if (fe == NULL) {
            free(f0);
            return cx_nomem(err);
        }
        memcpy(fe, d->exif, d->n_exif * sizeof *fe);
        f0[n0].tag = (uint16_t)EXIF_TAG_EXIF_IFD;
        f0[n0].ty = 4;
        f0[n0].count = 1;
        f0[n0].val = ptr_exif;
        f0[n0].vlen = 4;
        n0++;
    }
    if (d->n_gps > 0) {
        fg = pc_malloc_array(d->n_gps, sizeof *fg);
        if (fg == NULL) {
            free(fe);
            free(f0);
            return cx_nomem(err);
        }
        memcpy(fg, d->gps, d->n_gps * sizeof *fg);
        f0[n0].tag = (uint16_t)EXIF_TAG_GPS_IFD;
        f0[n0].ty = 4;
        f0[n0].count = 1;
        f0[n0].val = ptr_gps;
        f0[n0].vlen = 4;
        n0++;
    }
    /* Layout: header, then IFD0, Exif, GPS, each followed by its out-of-line values. */
    at_exif = at0 + exif_dir_size(f0, n0);
    at_gps = at_exif + (d->n_exif > 0 ? exif_dir_size(d->exif, d->n_exif) : 0);
    if (le) {
        pc_store_le32(ptr_exif, (uint32_t)at_exif);
        pc_store_le32(ptr_gps, (uint32_t)at_gps);
    } else {
        pc_store_be32(ptr_exif, (uint32_t)at_exif);
        pc_store_be32(ptr_gps, (uint32_t)at_gps);
    }
    total = at_gps + (d->n_gps > 0 ? exif_dir_size(d->gps, d->n_gps) : 0);
    buf = pc_malloc(total > 0 ? total : 1);
    if (buf == NULL) {
        free(fg);
        free(fe);
        free(f0);
        return cx_nomem(err);
    }
    memset(buf, 0, total);
    buf[0] = le ? 'I' : 'M';
    buf[1] = le ? 'I' : 'M';
    if (le) {
        pc_store_le16(buf + 2, 42);
        pc_store_le32(buf + 4, (uint32_t)at0);
    } else {
        pc_store_be16(buf + 2, 42);
        pc_store_be32(buf + 4, (uint32_t)at0);
    }
    cur = at0;
    exif_write_dir(buf, &cur, le, f0, n0);
    if (d->n_exif > 0) {
        exif_write_dir(buf, &cur, le, fe, d->n_exif);
    }
    if (d->n_gps > 0) {
        exif_write_dir(buf, &cur, le, fg, d->n_gps);
    }
    st = pc_buf_append(out, buf, cur, err);
    free(buf);
    free(fg);
    free(fe);
    free(f0);
    return st;
}

pc_status pc_raw_exif(const uint8_t *b, size_t n, pc_buf *out, bool *found, pc_error *err)
{
    tiff_file f;
    cx_exif_dirs d;
    pc_status st;
    *found = false;
    /* Classic TIFF (42/43) via the shared parser, or a CR2 (II, magic 0x55) header. */
    if (!tiff_file_parse(b, n, &f)) {
        uint16_t magic;
        tiff_file probe = {0};
        probe.b = b;
        probe.n = n;
        probe.le = true;
        if (n < 8 || b[0] != 'I' || b[1] != 'I' || !tiff_u16_at(&probe, 2, &magic) || magic != 0x55) {
            return PC_OK;
        }
        memset(&f, 0, sizeof f);
        f.b = b;
        f.n = n;
        f.le = true;
        f.big = false;
        {
            uint32_t first = 0;
            if (!tiff_u32_at(&f, 4, &first)) {
                return PC_OK;
            }
            f.first_ifd = first;
        }
    }
    /* A camera raw: keep IFD0's descriptive tags and the Exif/GPS directories, dropping the Exif
     * directory's sensor/preview PixelXDimension/PixelYDimension. */
    if (!exif_read_file(&f, true, &d)) {
        return PC_OK;
    }
    if (d.n_exif == 0 && d.n_gps == 0 && d.n_ifd0 == 0) {
        cx_exif_dirs_fini(&d);
        return PC_OK;
    }
    st = cx_exif_block(&d, out, err);
    cx_exif_dirs_fini(&d);
    if (st == PC_OK) {
        *found = true;
    }
    return st;
}
